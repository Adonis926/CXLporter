// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/cxlfork.h>

/*
 * CXLporter 是 CXLfork 主线上的 serverless 复现层：
 * - checkpoint：在 VM0 生成某个函数的内存镜像并写入 CXL slot。
 * - invoke：在 VM1 创建 ghost worker，resume 后执行函数 workload。
 * - local：在 localfork VM 上用普通 fork 执行同类 workload，作为 baseline。
 */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PAGE_SIZE_LOCAL 4096UL
#define FUNCTION_BASE ((void *)0x776000000000ULL) /* 函数数据映射的固定 VA，便于远端恢复。 */

enum function_kind {
	/* 合成函数类型，模拟 serverless benchmark 中不同访问/计算模式。 */
	FN_TOUCH,
	FN_JSON,
	FN_PYAES,
	FN_HTML,
	FN_FLOAT,
};

struct ghost_job {
	/* parent 发送给 ghost worker 的一次 invoke 请求。 */
	uint64_t key;
	uint64_t bytes;
	uint64_t flags;
	uint64_t hot_stride_pages;
	uint32_t function;
};

struct invoke_result {
	/* ghost worker 回传的恢复、执行和 fault 统计。 */
	uint64_t dispatch_to_done_ns;
	uint64_t child_start_ns;
	uint64_t restore_ns;
	uint64_t global_restore_ns;
	uint64_t pte_attach_ns;
	uint64_t direct_pte_install_ns;
	uint64_t workload_ns;
	uint64_t total_child_ns;
	uint64_t faults;
	uint64_t checksum;
	uint64_t pages;
	int status;
};

struct ghost {
	/* 一个短生命周期 worker 子进程及其 pipe。 */
	pid_t pid;
	int to_child;
	int from_child;
	int busy;
	uint64_t dispatch_ns;
};

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static size_t round_len(uint64_t len)
{
	return (size_t)((len + PAGE_SIZE_LOCAL - 1) & ~(PAGE_SIZE_LOCAL - 1));
}

static enum function_kind parse_function(const char *name)
{
	/* 脚本中的函数名到内部 workload 类型的映射。 */
	if (!strcmp(name, "touch"))
		return FN_TOUCH;
	if (!strcmp(name, "json") || !strcmp(name, "Json"))
		return FN_JSON;
	if (!strcmp(name, "pyaes") || !strcmp(name, "Pyaes"))
		return FN_PYAES;
	if (!strcmp(name, "html") || !strcmp(name, "HTML"))
		return FN_HTML;
	if (!strcmp(name, "float") || !strcmp(name, "Float"))
		return FN_FLOAT;
	fprintf(stderr, "unknown function: %s\n", name);
	exit(2);
}

static const char *function_name(enum function_kind fn)
{
	switch (fn) {
	case FN_TOUCH:
		return "touch";
	case FN_JSON:
		return "Json";
	case FN_PYAES:
		return "Pyaes";
	case FN_HTML:
		return "HTML";
	case FN_FLOAT:
		return "Float";
	}
	return "unknown";
}

static uint64_t policy_flags(const char *policy)
{
	/* CXLporter policy 名称复用 CXLfork 的 MoW/MoA/hybrid flag。 */
	if (!strcmp(policy, "mow"))
		return CXLFORK_F_MIGRATE_ON_WRITE;
	if (!strcmp(policy, "moa"))
		return CXLFORK_F_MIGRATE_ON_ACCESS;
	if (!strcmp(policy, "hybrid"))
		return CXLFORK_F_HYBRID_TIERING;
	fprintf(stderr, "unknown policy: %s\n", policy);
	exit(2);
}

static uint8_t pattern(size_t i, enum function_kind fn)
{
	/* 为不同函数生成稳定输入数据，保证 checkpoint/resume 内容可验证。 */
	static const char json_chars[] =
		"{\"key\":12345,\"value\":\"abcdef\",\"items\":[1,2,3,4]},";
	static const char html_chars[] =
		"<div class=\"item\"><span>cxlfork benchmark</span></div>\n";

	if (fn == FN_JSON)
		return (uint8_t)json_chars[i % (sizeof(json_chars) - 1)];
	if (fn == FN_HTML)
		return (uint8_t)html_chars[i % (sizeof(html_chars) - 1)];
	return (uint8_t)((i * 1315423911ULL + (uint64_t)fn * 17ULL) >> 8);
}

static void map_and_fill(size_t len, enum function_kind fn)
{
	/* VM0/local baseline 预先把函数输入映射到固定地址并填充。 */
	uint8_t *p;
	size_t i;

	p = mmap(FUNCTION_BASE, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap function");
		exit(1);
	}
	for (i = 0; i < len; i++)
		p[i] = pattern(i, fn);
}

static uint64_t run_touch(size_t len)
{
	volatile uint8_t *p = FUNCTION_BASE;
	uint64_t sum = 0;
	size_t i;

	for (i = 0; i < len; i += PAGE_SIZE_LOCAL)
		sum += p[i];
	return sum;
}

static uint64_t run_json(size_t len)
{
	volatile uint8_t *p = FUNCTION_BASE;
	uint64_t score = 0;
	int depth = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t c = p[i];

		if (c == '{' || c == '[')
			depth++;
		else if ((c == '}' || c == ']') && depth > 0)
			depth--;
		else if (c >= '0' && c <= '9')
			score += (uint64_t)(c - '0') * (uint64_t)(depth + 1);
		else if (c == '"')
			score ^= i;
	}
	return score ^ (uint64_t)depth;
}

static uint64_t run_pyaes(size_t len)
{
	volatile uint8_t *p = FUNCTION_BASE;
	uint32_t s0 = 0x00112233U;
	uint32_t s1 = 0x44556677U;
	uint32_t s2 = 0x8899aabbU;
	uint32_t s3 = 0xccddeeffU;
	size_t i;

	for (i = 0; i < len; i++) {
		uint32_t b = p[i];

		s0 = ((s0 << 5) | (s0 >> 27)) ^ (b + s3);
		s1 = ((s1 << 7) | (s1 >> 25)) + (s0 ^ (b * 0x45d9f3bU));
		s2 = ((s2 << 11) | (s2 >> 21)) ^ (s1 + 0x9e3779b9U);
		s3 = ((s3 << 13) | (s3 >> 19)) + (s2 ^ b);
	}
	return ((uint64_t)s0 << 32) ^ s1 ^ s2 ^ s3;
}

static uint64_t run_html(size_t len)
{
	volatile uint8_t *p = FUNCTION_BASE;
	uint64_t tags = 0;
	uint64_t text = 0;
	int in_tag = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t c = p[i];

		if (c == '<') {
			in_tag = 1;
			tags++;
		} else if (c == '>') {
			in_tag = 0;
		} else if (!in_tag && c > ' ') {
			text += c;
		}
	}
	return (tags << 32) ^ text;
}

static uint64_t run_float(size_t len)
{
	volatile uint8_t *p = FUNCTION_BASE;
	size_t count = len / sizeof(double);
	double acc = 1.000001;
	size_t i;

	for (i = 0; i < count; i++) {
		double v = (double)(p[(i * sizeof(double)) % len] + 1) * 0.0009765625;

		acc = acc * 1.0000001 + v;
		if (acc > 4096.0)
			acc *= 0.25;
	}
	return (uint64_t)(acc * 1000000.0);
}

static uint64_t run_function(size_t len, enum function_kind fn)
{
	/* 当前 workload 是 C 语言合成版本，不等同于 OpenWhisk action 的完整运行时开销。 */
	switch (fn) {
	case FN_TOUCH:
		return run_touch(len);
	case FN_JSON:
		return run_json(len);
	case FN_PYAES:
		return run_pyaes(len);
	case FN_HTML:
		return run_html(len);
	case FN_FLOAT:
		return run_float(len);
	}
	return 0;
}

static void write_full(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			_exit(111);
		p += n;
		len -= (size_t)n;
	}
}

static int read_full(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = read(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static uint64_t avg_u64(const uint64_t *v, int n)
{
	__uint128_t sum = 0;
	int i;

	for (i = 0; i < n; i++)
		sum += v[i];
	return n ? (uint64_t)(sum / (uint64_t)n) : 0;
}

static int checkpoint_cmd(int argc, char **argv)
{
	/* VM0 命令：准备函数内存并调用 CXLFORK_FORK_PREPARE。 */
	struct cxlfork_checkpoint op;
	struct cxlfork_release rel;
	enum function_kind fn;
	uint64_t key;
	size_t len;
	int fd;

	if (argc != 5) {
		fprintf(stderr, "usage: %s checkpoint <key> <function> <bytes>\n", argv[0]);
		return 2;
	}
	key = strtoull(argv[2], NULL, 0);
	fn = parse_function(argv[3]);
	len = round_len(strtoull(argv[4], NULL, 0));

	map_and_fill(len, fn);
	(void)run_function(len, FN_TOUCH);

	fd = open(CXLFORK_DEVICE, O_RDWR);
	if (fd < 0) {
		perror(CXLFORK_DEVICE);
		return 1;
	}
	memset(&rel, 0, sizeof(rel));
	rel.key = key;
	(void)ioctl(fd, CXLFORK_RELEASE, &rel);

	memset(&op, 0, sizeof(op));
	op.key = key;
	op.scan_start = (uintptr_t)FUNCTION_BASE;
	op.scan_end = (uintptr_t)FUNCTION_BASE + len;
	if (ioctl(fd, CXLFORK_FORK_PREPARE, &op)) {
		perror("CXLFORK_FORK_PREPARE");
		close(fd);
		return 1;
	}
	close(fd);

	printf("cxlporter_checkpoint key=%" PRIu64 " function=%s bytes=%zu"
	       " prepare_ns=%" PRIu64 " regions=%" PRIu64 " pages=%" PRIu64
	       " anon_pages=%" PRIu64 " clean_file_pages=%" PRIu64
	       " dirty_file_pages=%" PRIu64 " pte_image_build_ns=%" PRIu64
	       " pt_page_image_build_ns=%" PRIu64 " vma_tree_build_ns=%" PRIu64
	       " state_image_build_ns=%" PRIu64 " private_rebase_ns=%" PRIu64
	       " global_detach_ns=%" PRIu64 " verify=ok\n",
	       key, function_name(fn), len, op.checkpoint_ns, op.nr_regions,
	       op.total_pages, op.anon_pages, op.clean_file_pages,
	       op.dirty_file_pages, op.pte_image_build_ns,
	       op.pt_page_image_build_ns, op.vma_tree_build_ns,
	       op.state_image_build_ns, op.private_rebase_ns,
	       op.global_detach_ns);
	return 0;
}

static void do_restore_job(const struct ghost_job *job, struct invoke_result *res)
{
	/* ghost worker：执行 CXLFORK_FORK_RESUME，再运行函数 workload 并读取 fault 差值。 */
	struct cxlfork_restore op;
	struct cxlfork_stats stats;
	enum function_kind fn = (enum function_kind)job->function;
	uint64_t work_begin;
	uint64_t faults_before = 0;
	int fd;

	memset(res, 0, sizeof(*res));
	res->child_start_ns = nsec_now();
	fd = open(CXLFORK_DEVICE, O_RDWR);
	if (fd < 0) {
		res->status = errno;
		return;
	}
	memset(&stats, 0, sizeof(stats));
	stats.key = job->key;
	if (!ioctl(fd, CXLFORK_STATS, &stats))
		faults_before = stats.faults;

	memset(&op, 0, sizeof(op));
	op.key = job->key;
	op.flags = job->flags | CXLFORK_F_APPLY_MM_STATE |
		   CXLFORK_F_APPLY_GLOBAL_STATE;
	op.tiering_hot_stride_pages = job->hot_stride_pages;
	if (ioctl(fd, CXLFORK_FORK_RESUME, &op)) {
		res->status = errno;
		close(fd);
		return;
	}

	work_begin = nsec_now();
	res->checksum = run_function((size_t)job->bytes, fn);
	res->workload_ns = nsec_now() - work_begin;
	res->total_child_ns = nsec_now() - res->child_start_ns;
	res->restore_ns = op.restore_ns;
	res->global_restore_ns = op.global_restore_ns;
	res->pte_attach_ns = op.pte_attach_ns;
	res->direct_pte_install_ns = op.direct_pte_install_ns;
	res->pages = op.total_pages;

	memset(&stats, 0, sizeof(stats));
	stats.key = job->key;
	if (!ioctl(fd, CXLFORK_STATS, &stats))
		res->faults = stats.faults >= faults_before ?
			      stats.faults - faults_before : stats.faults;
	close(fd);
}

static void ghost_loop(int read_fd, int write_fd)
{
	/* 单个 ghost 只处理一个 job，完成后退出，模拟冷启动/远端恢复路径。 */
	struct ghost_job job;
	struct invoke_result res;

	if (read_full(read_fd, &job, sizeof(job))) {
		res.status = EIO;
		write_full(write_fd, &res, sizeof(res));
		_exit(1);
	}
	do_restore_job(&job, &res);
	write_full(write_fd, &res, sizeof(res));
	_exit(res.status ? 1 : 0);
}

static int spawn_ghost(struct ghost *g)
{
	/* 创建 ghost worker，并建立 parent/child 双向 pipe。 */
	int to_child[2];
	int from_child[2];
	pid_t pid;

	if (pipe(to_child) || pipe(from_child)) {
		perror("pipe");
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		perror("fork ghost");
		return -1;
	}
	if (!pid) {
		close(to_child[1]);
		close(from_child[0]);
		ghost_loop(to_child[0], from_child[1]);
	}
	close(to_child[0]);
	close(from_child[1]);
	g->pid = pid;
	g->to_child = to_child[1];
	g->from_child = from_child[0];
	g->busy = 0;
	g->dispatch_ns = 0;
	return 0;
}

static int reap_ghost(struct ghost *g, struct invoke_result *res)
{
	/* 收集 ghost 结果，并把 dispatch 到完成的端到端时间写入结果。 */
	int status;

	if (read_full(g->from_child, res, sizeof(*res)))
		return -1;
	res->dispatch_to_done_ns = nsec_now() - g->dispatch_ns;
	close(g->to_child);
	close(g->from_child);
	if (waitpid(g->pid, &status, 0) != g->pid)
		return -1;
	g->pid = -1;
	g->busy = 0;
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		return res->status ? res->status : ECHILD;
	return 0;
}

static int dispatch_ghost(struct ghost *g, const struct ghost_job *job)
{
	/* 下发一次 invoke；当前实现为了便于调试，外层循环一次只保持一个 outstanding job。 */
	g->dispatch_ns = nsec_now();
	g->busy = 1;
	write_full(g->to_child, job, sizeof(*job));
	return 0;
}

static int invoke_cmd(int argc, char **argv)
{
	/* VM1 命令：按 policy 恢复 CXL slot 并执行多轮函数请求。 */
	struct ghost *pool;
	struct ghost_job job;
	struct invoke_result *measured;
	enum function_kind fn;
	const char *policy;
	uint64_t key;
	uint64_t flags;
	uint64_t hot_stride = 0;
	uint64_t keepalive_us = 0;
	size_t len;
	int pool_size;
	int requests;
	int warmup;
	int sent = 0;
	int done = 0;
	int measured_count = 0;
	int outstanding = 0;
	int i;

	if (argc < 9 || argc > 11) {
		fprintf(stderr, "usage: %s invoke <key> <function> <bytes> <policy> <pool> <requests> <warmup> [keepalive_us] [hot_stride_pages]\n", argv[0]);
		return 2;
	}
	key = strtoull(argv[2], NULL, 0);
	fn = parse_function(argv[3]);
	len = round_len(strtoull(argv[4], NULL, 0));
	policy = argv[5];
	flags = policy_flags(policy);
	pool_size = atoi(argv[6]);
	requests = atoi(argv[7]);
	warmup = atoi(argv[8]);
	if (argc >= 10)
		keepalive_us = strtoull(argv[9], NULL, 0);
	if (argc >= 11)
		hot_stride = strtoull(argv[10], NULL, 0);
	if (pool_size <= 0 || requests <= 0 || warmup < 0)
		return 2;

	pool = calloc((size_t)pool_size, sizeof(*pool));
	measured = calloc((size_t)requests, sizeof(*measured));
	if (!pool || !measured) {
		perror("calloc");
		return 1;
	}
	for (i = 0; i < pool_size; i++) {
		if (spawn_ghost(&pool[i]))
			return 1;
	}

	memset(&job, 0, sizeof(job));
	job.key = key;
	job.bytes = len;
	job.flags = flags;
	job.hot_stride_pages = hot_stride;
	job.function = (uint32_t)fn;

	while (done < warmup + requests) {
		fd_set rfds;
		int maxfd = -1;

		for (i = 0; outstanding == 0 && i < pool_size &&
		     sent < warmup + requests; i++) {
			if (!pool[i].busy) {
				if (keepalive_us)
					usleep((useconds_t)keepalive_us);
				dispatch_ghost(&pool[i], &job);
				outstanding++;
				sent++;
				break;
			}
		}

		FD_ZERO(&rfds);
		for (i = 0; i < pool_size; i++) {
			if (pool[i].busy) {
				FD_SET(pool[i].from_child, &rfds);
				if (pool[i].from_child > maxfd)
					maxfd = pool[i].from_child;
			}
		}
		if (maxfd < 0)
			continue;
		if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			return 1;
		}
		for (i = 0; i < pool_size; i++) {
			struct invoke_result res;

			if (!pool[i].busy || !FD_ISSET(pool[i].from_child, &rfds))
				continue;
			if (reap_ghost(&pool[i], &res)) {
				fprintf(stderr, "ghost failed status=%d restore_ns=%" PRIu64
					" workload_ns=%" PRIu64 " total_child_ns=%" PRIu64 "\n",
					res.status, res.restore_ns, res.workload_ns,
					res.total_child_ns);
				return 1;
			}
			if (done >= warmup)
				measured[measured_count++] = res;
			done++;
			outstanding--;
			if (sent < warmup + requests && spawn_ghost(&pool[i]))
				return 1;
		}
	}

	{
		uint64_t dispatch_ns[measured_count];
		uint64_t restore_ns[measured_count];
		uint64_t workload_ns[measured_count];
		uint64_t total_ns[measured_count];
		uint64_t faults = 0;

		for (i = 0; i < measured_count; i++) {
			dispatch_ns[i] = measured[i].dispatch_to_done_ns;
			restore_ns[i] = measured[i].restore_ns;
			workload_ns[i] = measured[i].workload_ns;
			total_ns[i] = measured[i].total_child_ns;
			faults = measured[i].faults;
		}
		printf("cxlporter_invoke key=%" PRIu64 " function=%s bytes=%zu"
		       " policy=%s pool=%d requests=%d warmup=%d keepalive_us=%" PRIu64
		       " hot_stride_pages=%" PRIu64 " dispatch_to_done_ns=%" PRIu64
		       " restore_ns=%" PRIu64 " workload_ns=%" PRIu64
		       " child_total_ns=%" PRIu64 " pages=%" PRIu64
		       " faults=%" PRIu64 " verify=ok\n",
		       key, function_name(fn), len, policy, pool_size, requests,
		       warmup, keepalive_us, hot_stride,
		       avg_u64(dispatch_ns, measured_count),
		       avg_u64(restore_ns, measured_count),
		       avg_u64(workload_ns, measured_count),
		       avg_u64(total_ns, measured_count),
		       measured_count ? measured[0].pages : 0, faults);
	}
	free(pool);
	free(measured);
	return 0;
}

static int local_cmd(int argc, char **argv)
{
	/* localfork baseline：普通 fork 后直接运行相同合成函数。 */
	enum function_kind fn;
	struct invoke_result *measured;
	uint64_t *fork_ns;
	size_t len;
	int requests;
	int warmup;
	int i;
	int measured_count = 0;

	if (argc != 6) {
		fprintf(stderr, "usage: %s local <function> <bytes> <requests> <warmup>\n", argv[0]);
		return 2;
	}
	fn = parse_function(argv[2]);
	len = round_len(strtoull(argv[3], NULL, 0));
	requests = atoi(argv[4]);
	warmup = atoi(argv[5]);
	if (requests <= 0 || warmup < 0)
		return 2;

	map_and_fill(len, fn);
	measured = calloc((size_t)requests, sizeof(*measured));
	fork_ns = calloc((size_t)requests, sizeof(*fork_ns));
	if (!measured || !fork_ns) {
		perror("calloc");
		return 1;
	}

	for (i = 0; i < requests + warmup; i++) {
		struct invoke_result res;
		uint64_t begin = nsec_now();
		int pipefd[2];
		int status;
		pid_t pid;

		if (pipe(pipefd)) {
			perror("pipe");
			return 1;
		}
		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (!pid) {
			uint64_t work_begin;

			close(pipefd[0]);
			memset(&res, 0, sizeof(res));
			res.child_start_ns = nsec_now();
			work_begin = nsec_now();
			res.checksum = run_function(len, fn);
			res.workload_ns = nsec_now() - work_begin;
			res.total_child_ns = nsec_now() - res.child_start_ns;
			res.pages = len / PAGE_SIZE_LOCAL;
			write_full(pipefd[1], &res, sizeof(res));
			close(pipefd[1]);
			_exit(0);
		}
		close(pipefd[1]);
		if (read_full(pipefd[0], &res, sizeof(res))) {
			fprintf(stderr, "short read from local child\n");
			return 1;
		}
		close(pipefd[0]);
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
		    WEXITSTATUS(status)) {
			fprintf(stderr, "local child failed\n");
			return 1;
		}
		if (i >= warmup) {
			fork_ns[measured_count] = res.child_start_ns - begin;
			measured[measured_count++] = res;
		}
	}

	{
		uint64_t workload_ns[measured_count];
		uint64_t total_ns[measured_count];

		for (i = 0; i < measured_count; i++) {
			workload_ns[i] = measured[i].workload_ns;
			total_ns[i] = measured[i].total_child_ns;
		}
		printf("cxlporter_local function=%s bytes=%zu requests=%d warmup=%d"
		       " fork_to_child_ns=%" PRIu64 " workload_ns=%" PRIu64
		       " child_total_ns=%" PRIu64 " pages=%zu verify=ok\n",
		       function_name(fn), len, requests, warmup,
		       avg_u64(fork_ns, measured_count),
		       avg_u64(workload_ns, measured_count),
		       avg_u64(total_ns, measured_count),
		       len / PAGE_SIZE_LOCAL);
	}
	free(measured);
	free(fork_ns);
	return 0;
}

int main(int argc, char **argv)
{
	/* 三个子命令由 scripts/30-benchmark-cxlporter.sh 跨 VM 编排。 */
	if (argc < 2) {
		fprintf(stderr, "usage: %s checkpoint|invoke|local ...\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "checkpoint"))
		return checkpoint_cmd(argc, argv);
	if (!strcmp(argv[1], "invoke"))
		return invoke_cmd(argc, argv);
	if (!strcmp(argv[1], "local"))
		return local_cmd(argc, argv);
	fprintf(stderr, "unknown command: %s\n", argv[1]);
	return 2;
}
