// SPDX-License-Identifier: MIT
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/cxlfork.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PAGE_BYTES 4096UL
#define FUNCTION_BASE ((void *)0x776000000000ULL)
#define MAX_REGISTRY 16
#define MAX_WORKERS 32
#define MAX_PENDING 1024
#define MAX_LINE 8192
#define MAX_NAME 32

enum function_kind {
	FN_TOUCH,
	FN_JSON,
	FN_PYAES,
	FN_HTML,
	FN_FLOAT,
};

enum request_op {
	OP_INVALID,
	OP_PING,
	OP_REGISTER,
	OP_INVOKE,
};

enum request_mode {
	MODE_INVOKE,
	MODE_RESTORE_ONLY,
	MODE_COLD,
};

struct registry_entry {
	bool used;
	char function[MAX_NAME];
	uint64_t key;
	uint64_t generation;
	uint64_t bytes;
	uint64_t page_checksum;
	uint64_t workload_checksum;
};

struct control_request {
	enum request_op op;
	char id[80];
	char function[MAX_NAME];
	char policy[16];
	char validation[16];
	enum request_mode mode;
	uint64_t key;
	uint64_t generation;
	uint64_t bytes;
	uint64_t page_checksum;
	uint64_t workload_checksum;
};

struct worker_job {
	char id[80];
	char function[MAX_NAME];
	uint32_t fn;
	uint32_t mode;
	uint64_t key;
	uint64_t generation;
	uint64_t bytes;
	uint64_t page_checksum;
	uint64_t workload_checksum;
	uint64_t restore_flags;
	uint64_t dispatch_ns;
};

struct worker_result {
	int32_t status;
	uint32_t verify;
	uint64_t worker_wakeup_ns;
	uint64_t restore_ioctl_ns;
	uint64_t workload_ns;
	uint64_t worker_total_ns;
	uint64_t result_checksum;
	uint64_t page_checksum;
	uint64_t restore_id;
	uint64_t total_pages;
	struct cxlfork_restore_stats_v2 stats;
};

struct worker {
	pid_t pid;
	int fd;
	int client_fd;
	bool busy;
	uint64_t enqueue_ns;
	uint64_t dispatch_ns;
	struct worker_job job;
};

struct pending_request {
	int client_fd;
	uint64_t enqueue_ns;
	struct worker_job job;
};

struct pending_queue {
	struct pending_request entries[MAX_PENDING];
	size_t head;
	size_t count;
};

static volatile sig_atomic_t stop_requested;

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void stop_handler(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static size_t round_len(uint64_t bytes)
{
	return (size_t)((bytes + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1));
}

static int write_full(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
	unsigned char *p = buf;

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

static int read_line(int fd, char *line, size_t capacity)
{
	size_t used = 0;

	while (used + 1 < capacity) {
		char c;
		ssize_t n = read(fd, &c, 1);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		if (c == '\n') {
			line[used] = '\0';
			return 0;
		}
		if (c == '\r')
			continue;
		line[used++] = c;
	}
	errno = EMSGSIZE;
	return -1;
}

static enum function_kind parse_function(const char *name)
{
	if (!strcmp(name, "touch"))
		return FN_TOUCH;
	if (!strcmp(name, "json"))
		return FN_JSON;
	if (!strcmp(name, "pyaes"))
		return FN_PYAES;
	if (!strcmp(name, "html"))
		return FN_HTML;
	if (!strcmp(name, "float"))
		return FN_FLOAT;
	return (enum function_kind)-1;
}

static uint8_t pattern(size_t i, enum function_kind fn)
{
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

static void *map_and_fill(size_t len, enum function_kind fn)
{
	uint8_t *p;
	size_t i;

	p = mmap(FUNCTION_BASE, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED)
		return MAP_FAILED;
	(void)madvise(p, len, MADV_NOHUGEPAGE);
	for (i = 0; i < len; i++)
		p[i] = pattern(i, fn);
	return p;
}

static uint64_t run_touch(size_t len)
{
	volatile uint8_t *p = FUNCTION_BASE;
	uint64_t sum = 0;
	size_t i;

	for (i = 0; i < len; i += PAGE_BYTES)
		sum = (sum * 1315423911ULL) ^ p[i];
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
	uint32_t s0 = 0x00112233U, s1 = 0x44556677U;
	uint32_t s2 = 0x8899aabbU, s3 = 0xccddeeffU;
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
	uint64_t tags = 0, text = 0;
	bool in_tag = false;
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t c = p[i];

		if (c == '<') {
			in_tag = true;
			tags++;
		} else if (c == '>') {
			in_tag = false;
		} else if (!in_tag && c > ' ') {
			text += c;
		}
	}
	return (tags << 32) ^ text;
}

static uint64_t run_float(size_t len)
{
	volatile uint8_t *p = FUNCTION_BASE;
	double acc = 1.000001;
	size_t i;

	for (i = 0; i < len; i++) {
		acc = acc * 1.0000001 + (double)(p[i] + 1) * 0.0009765625;
		if (acc > 4096.0)
			acc *= 0.25;
	}
	return (uint64_t)(acc * 1000000.0);
}

static uint64_t run_function(size_t len, enum function_kind fn)
{
	switch (fn) {
	case FN_TOUCH: return run_touch(len);
	case FN_JSON: return run_json(len);
	case FN_PYAES: return run_pyaes(len);
	case FN_HTML: return run_html(len);
	case FN_FLOAT: return run_float(len);
	}
	return 0;
}

static void init_v2(void *ptr, size_t size)
{
	struct cxlfork_v2_header *header = ptr;

	memset(ptr, 0, size);
	header->size = (uint32_t)size;
	header->abi_version = CXLFORK_ABI_VERSION_V2;
}

static int query_caps_fd(int fd, struct cxlfork_query_caps_v2 *caps)
{
	const uint64_t required = CXLFORK_CAP_V2_ABI |
		CXLFORK_CAP_CONSISTENT_SINGLE_THREAD |
		CXLFORK_CAP_RELIABLE_SLOT_GENERATION |
		CXLFORK_CAP_RESTORE_FAST_PATH;

	init_v2(caps, sizeof(*caps));
	if (ioctl(fd, CXLFORK_QUERY_CAPS_V2, caps))
		return -errno;
	if ((caps->capabilities & required) != required)
		return -EOPNOTSUPP;
	return 0;
}

static uint64_t policy_flags(const char *policy)
{
	if (!strcmp(policy, "readonly"))
		return CXLFORK_F_MAP_READONLY;
	if (!strcmp(policy, "mow"))
		return CXLFORK_F_MIGRATE_ON_WRITE;
	if (!strcmp(policy, "moa"))
		return CXLFORK_F_MIGRATE_ON_ACCESS;
	if (!strcmp(policy, "hybrid"))
		return CXLFORK_F_HYBRID_TIERING;
	return UINT64_MAX;
}

static void do_worker_job(const struct worker_job *job,
			  struct worker_result *result)
{
	struct cxlfork_region regions[CXLFORK_MAX_REGIONS];
	struct cxlfork_resume_v2 op;
	struct cxlfork_call_stats_v2 call;
	enum function_kind fn = (enum function_kind)job->fn;
	uint64_t begin = nsec_now();
	uint64_t workload_begin;
	int fd = -1;

	memset(result, 0, sizeof(*result));
	result->worker_wakeup_ns = begin - job->dispatch_ns;

	if (job->mode == MODE_COLD) {
		if (map_and_fill((size_t)job->bytes, fn) == MAP_FAILED) {
			result->status = errno;
			return;
		}
		workload_begin = nsec_now();
		result->result_checksum = run_function((size_t)job->bytes, fn);
		result->workload_ns = nsec_now() - workload_begin;
		result->verify = result->result_checksum == job->workload_checksum;
		result->status = result->verify ? 0 : EILSEQ;
		result->worker_total_ns = nsec_now() - begin;
		return;
	}

	fd = open(CXLFORK_DEVICE, O_RDWR);
	if (fd < 0) {
		result->status = errno;
		return;
	}
	init_v2(&op, sizeof(op));
	op.key = job->key;
	op.generation = job->generation;
	op.flags = job->restore_flags;
	op.regions_ptr = (uintptr_t)regions;
	op.region_capacity = CXLFORK_MAX_REGIONS;
	workload_begin = nsec_now();
	if (ioctl(fd, CXLFORK_RESUME_V2, &op)) {
		result->status = errno;
		close(fd);
		return;
	}
	result->restore_ioctl_ns = nsec_now() - workload_begin;
	result->page_checksum = op.page_checksum;
	result->restore_id = op.restore_id;
	result->total_pages = op.total_pages;
	result->stats = op.stats;

	if (job->mode == MODE_INVOKE) {
		workload_begin = nsec_now();
		result->result_checksum = run_function((size_t)job->bytes, fn);
		result->workload_ns = nsec_now() - workload_begin;
	}

	init_v2(&call, sizeof(call));
	call.key = job->key;
	call.generation = job->generation;
	call.restore_id = op.restore_id;
	if (!ioctl(fd, CXLFORK_GET_CALL_STATS_V2, &call))
		result->stats = call.stats;

	result->verify = op.page_checksum == job->page_checksum;
	if (job->mode == MODE_INVOKE)
		result->verify = result->verify &&
			result->result_checksum == job->workload_checksum;
	result->status = result->verify ? 0 : EILSEQ;
	result->worker_total_ns = nsec_now() - begin;
	close(fd);
}

static void worker_main(int fd)
{
	struct worker_job job;
	struct worker_result result;

	if (read_full(fd, &job, sizeof(job)))
		_exit(111);
	do_worker_job(&job, &result);
	if (write_full(fd, &result, sizeof(result)))
		_exit(112);
	close(fd);
	_exit(result.status ? 1 : 0);
}

static int spawn_worker(struct worker *worker)
{
	int sv[2];
	pid_t pid;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv))
		return -1;
	pid = fork();
	if (pid < 0) {
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	if (!pid) {
		int fd;

		if (sv[1] != 3)
			dup2(sv[1], 3);
		for (fd = 4; fd < 1024; fd++)
			close(fd);
		worker_main(3);
	}
	close(sv[1]);
	memset(worker, 0, sizeof(*worker));
	worker->pid = pid;
	worker->fd = sv[0];
	worker->client_fd = -1;
	return 0;
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno || !*text || *end)
		return -1;
	*value = (uint64_t)parsed;
	return 0;
}

static int copy_field(char *dst, size_t capacity, const char *src)
{
	if (strlen(src) >= capacity)
		return -1;
	strcpy(dst, src);
	return 0;
}

static int parse_request(char *line, struct control_request *request)
{
	char *save = NULL;
	char *token;

	memset(request, 0, sizeof(*request));
	strcpy(request->policy, "mow");
	strcpy(request->validation, "fast");
	request->mode = MODE_INVOKE;

	token = strtok_r(line, " ", &save);
	if (!token || strcmp(token, "CXLP/1"))
		return -1;
	token = strtok_r(NULL, " ", &save);
	if (!token)
		return -1;
	if (!strcmp(token, "PING")) request->op = OP_PING;
	else if (!strcmp(token, "REGISTER")) request->op = OP_REGISTER;
	else if (!strcmp(token, "INVOKE")) request->op = OP_INVOKE;
	else return -1;

	while ((token = strtok_r(NULL, " ", &save))) {
		char *equal = strchr(token, '=');
		const char *key;
		const char *value;

		if (!equal)
			return -1;
		*equal = '\0';
		key = token;
		value = equal + 1;
		if (!strcmp(key, "id")) {
			if (copy_field(request->id, sizeof(request->id), value)) return -1;
		} else if (!strcmp(key, "function")) {
			if (copy_field(request->function, sizeof(request->function), value)) return -1;
		} else if (!strcmp(key, "policy")) {
			if (copy_field(request->policy, sizeof(request->policy), value)) return -1;
		} else if (!strcmp(key, "validation")) {
			if (copy_field(request->validation, sizeof(request->validation), value)) return -1;
		} else if (!strcmp(key, "mode")) {
			if (!strcmp(value, "invoke")) request->mode = MODE_INVOKE;
			else if (!strcmp(value, "restore")) request->mode = MODE_RESTORE_ONLY;
			else if (!strcmp(value, "cold")) request->mode = MODE_COLD;
			else return -1;
		} else if (!strcmp(key, "key")) {
			if (parse_u64(value, &request->key)) return -1;
		} else if (!strcmp(key, "generation")) {
			if (parse_u64(value, &request->generation)) return -1;
		} else if (!strcmp(key, "bytes")) {
			if (parse_u64(value, &request->bytes)) return -1;
		} else if (!strcmp(key, "page_checksum")) {
			if (parse_u64(value, &request->page_checksum)) return -1;
		} else if (!strcmp(key, "workload_checksum")) {
			if (parse_u64(value, &request->workload_checksum)) return -1;
		} else {
			return -1;
		}
	}
	if (!request->id[0])
		return -1;
	if (request->op != OP_PING && !request->function[0])
		return -1;
	return 0;
}

static struct registry_entry *find_registry(struct registry_entry *entries,
					     const char *function)
{
	int i;

	for (i = 0; i < MAX_REGISTRY; i++)
		if (entries[i].used && !strcmp(entries[i].function, function))
			return &entries[i];
	return NULL;
}

static int register_entry(struct registry_entry *entries,
			  const struct control_request *request)
{
	struct registry_entry *entry = find_registry(entries, request->function);
	int i;

	if (!request->key || !request->generation || !request->bytes)
		return EINVAL;
	if (!entry) {
		for (i = 0; i < MAX_REGISTRY; i++) {
			if (!entries[i].used) {
				entry = &entries[i];
				break;
			}
		}
	}
	if (!entry)
		return ENOSPC;
	memset(entry, 0, sizeof(*entry));
	entry->used = true;
	strcpy(entry->function, request->function);
	entry->key = request->key;
	entry->generation = request->generation;
	entry->bytes = round_len(request->bytes);
	entry->page_checksum = request->page_checksum;
	entry->workload_checksum = request->workload_checksum;
	return 0;
}

static int make_job(const struct control_request *request,
		    struct registry_entry *entries, struct worker_job *job)
{
	struct registry_entry *entry = find_registry(entries, request->function);
	uint64_t flags;
	enum function_kind fn;

	if (!entry)
		return ENOENT;
	fn = parse_function(request->function);
	if ((int)fn < 0)
		return EINVAL;
	flags = policy_flags(request->policy);
	if (flags == UINT64_MAX)
		return EINVAL;
	if (!strcmp(request->validation, "full"))
		flags |= CXLFORK_F_VALIDATE_FULL;
	else if (strcmp(request->validation, "fast"))
		return EINVAL;

	memset(job, 0, sizeof(*job));
	strcpy(job->id, request->id);
	strcpy(job->function, request->function);
	job->fn = (uint32_t)fn;
	job->mode = (uint32_t)request->mode;
	job->key = entry->key;
	job->generation = entry->generation;
	job->bytes = entry->bytes;
	job->page_checksum = entry->page_checksum;
	job->workload_checksum = entry->workload_checksum;
	job->restore_flags = flags;
	return 0;
}

static int queue_push(struct pending_queue *queue,
		      const struct pending_request *pending)
{
	size_t index;

	if (queue->count == MAX_PENDING)
		return -1;
	index = (queue->head + queue->count) % MAX_PENDING;
	queue->entries[index] = *pending;
	queue->count++;
	return 0;
}

static int queue_pop(struct pending_queue *queue,
		     struct pending_request *pending)
{
	if (!queue->count)
		return -1;
	*pending = queue->entries[queue->head];
	queue->head = (queue->head + 1) % MAX_PENDING;
	queue->count--;
	return 0;
}

static void send_error(int fd, const char *id, int status, const char *stage)
{
	dprintf(fd, "CXLP/1 RESULT id=%s status=%d verify=0 error_stage=%s\n",
		id && *id ? id : "unknown", status, stage);
}

static void send_result(int fd, const struct worker *worker,
			const struct worker_result *result)
{
	uint64_t daemon_total = nsec_now() - worker->enqueue_ns;
	uint64_t queue_wait = worker->dispatch_ns - worker->enqueue_ns;

	dprintf(fd,
		"CXLP/1 RESULT id=%s status=%d verify=%u function=%s key=%" PRIu64
		" generation=%" PRIu64 " bytes=%" PRIu64
		" daemon_total_ns=%" PRIu64 " queue_wait_ns=%" PRIu64
		" worker_wakeup_ns=%" PRIu64 " worker_total_ns=%" PRIu64
		" restore_ioctl_ns=%" PRIu64 " workload_ns=%" PRIu64
		" result_checksum=%" PRIu64 " page_checksum=%" PRIu64
		" restore_id=%" PRIu64 " pages=%" PRIu64
		" supported_mask=%" PRIu64 " valid_mask=%" PRIu64
		" path_mask=%" PRIu64 " kernel_total_ns=%" PRIu64
		" validation_ns=%" PRIu64 " vma_create_ns=%" PRIu64
		" upper_pt_ns=%" PRIu64 " shared_leaf_ns=%" PRIu64
		" fallback_ns=%" PRIu64 " dirty_prefault_ns=%" PRIu64
		" kernel_other_ns=%" PRIu64 " faults=%" PRIu64
		" fault_dispatch_ns=%" PRIu64 " leaf_materialize_ns=%" PRIu64
		" data_cow_ns=%" PRIu64 " cxl_copy_ns=%" PRIu64
		" shared_leaf_pages=%" PRIu64 " fallback_pages=%" PRIu64
		" local_copy_pages=%" PRIu64 " leaf_copy_pages=%" PRIu64 "\n",
		worker->job.id, result->status, result->verify,
		worker->job.function, worker->job.key, worker->job.generation,
		worker->job.bytes, daemon_total, queue_wait,
		result->worker_wakeup_ns, result->worker_total_ns,
		result->restore_ioctl_ns, result->workload_ns,
		result->result_checksum, result->page_checksum,
		result->restore_id, result->total_pages,
		result->stats.supported_mask, result->stats.valid_mask,
		result->stats.path_mask, result->stats.total_ns,
		result->stats.validation_ns, result->stats.vma_create_ns,
		result->stats.upper_pt_alloc_ns,
		result->stats.shared_leaf_attach_ns,
		result->stats.fallback_reconstruct_ns,
		result->stats.dirty_prefault_ns, result->stats.other_ns,
		result->stats.faults, result->stats.fault_dispatch_ns,
		result->stats.leaf_materialize_ns, result->stats.data_cow_ns,
		result->stats.cxl_to_local_copy_ns,
		result->stats.shared_leaf_pages, result->stats.fallback_pages,
		result->stats.local_copy_pages, result->stats.leaf_copy_pages);
}

static int dispatch_to_worker(struct worker *worker,
			      const struct pending_request *pending)
{
	worker->job = pending->job;
	worker->client_fd = pending->client_fd;
	worker->enqueue_ns = pending->enqueue_ns;
	worker->dispatch_ns = nsec_now();
	worker->job.dispatch_ns = worker->dispatch_ns;
	worker->busy = true;
	if (write_full(worker->fd, &worker->job, sizeof(worker->job))) {
		worker->busy = false;
		return -1;
	}
	return 0;
}

static int open_listener(const char *port)
{
	struct addrinfo hints, *addresses = NULL, *address;
	int listener = -1;
	int one = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(NULL, port, &hints, &addresses))
		return -1;
	for (address = addresses; address; address = address->ai_next) {
		listener = socket(address->ai_family, address->ai_socktype, 0);
		if (listener < 0)
			continue;
		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (!bind(listener, address->ai_addr, address->ai_addrlen) &&
		    !listen(listener, 1024))
			break;
		close(listener);
		listener = -1;
	}
	freeaddrinfo(addresses);
	return listener;
}

static int serve_cmd(int argc, char **argv)
{
	struct registry_entry registry[MAX_REGISTRY] = {0};
	struct pending_queue queue = {0};
	struct worker workers[MAX_WORKERS];
	struct cxlfork_query_caps_v2 caps;
	const char *port;
	int pool_size;
	int listener;
	int device;
	int i;

	if (argc != 4) {
		fprintf(stderr, "usage: %s serve PORT POOL\n", argv[0]);
		return 2;
	}
	port = argv[2];
	pool_size = atoi(argv[3]);
	if (pool_size < 1 || pool_size > MAX_WORKERS)
		return 2;
	device = open(CXLFORK_DEVICE, O_RDWR);
	if (device < 0) {
		perror(CXLFORK_DEVICE);
		return 1;
	}
	i = query_caps_fd(device, &caps);
	close(device);
	if (i) {
		errno = -i;
		perror("CXLFORK_QUERY_CAPS_V2");
		return 1;
	}
	listener = open_listener(port);
	if (listener < 0) {
		perror("listen");
		return 1;
	}
	for (i = 0; i < pool_size; i++) {
		if (spawn_worker(&workers[i])) {
			perror("spawn worker");
			return 1;
		}
	}
	signal(SIGTERM, stop_handler);
	signal(SIGINT, stop_handler);
	printf("cxlporter_daemon_ready protocol=CXLP/1 port=%s pool=%d abi=%u caps=%" PRIu64 "\n",
	       port, pool_size, caps.abi_version, caps.capabilities);
	fflush(stdout);

	while (!stop_requested) {
		struct pollfd pollfds[MAX_WORKERS + 1];
		int pollmap[MAX_WORKERS + 1];
		int nfds = 1;
		int ready;

		pollfds[0].fd = listener;
		pollfds[0].events = POLLIN;
		pollmap[0] = -1;
		for (i = 0; i < pool_size; i++) {
			if (workers[i].busy) {
				pollfds[nfds].fd = workers[i].fd;
				pollfds[nfds].events = POLLIN | POLLHUP | POLLERR;
				pollmap[nfds] = i;
				nfds++;
			}
		}
		ready = poll(pollfds, (nfds_t)nfds, 1000);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			perror("poll");
			break;
		}

		if (pollfds[0].revents & POLLIN) {
			struct timeval timeout = { .tv_sec = 2 };
			struct control_request request;
			struct pending_request pending;
			char line[MAX_LINE];
			int client = accept(listener, NULL, NULL);
			int status;
			int target = -1;

			if (client >= 0) {
				setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
					   &timeout, sizeof(timeout));
				if (read_line(client, line, sizeof(line)) ||
				    parse_request(line, &request)) {
					send_error(client, "unknown", EPROTO, "parse");
					close(client);
				} else if (request.op == OP_PING) {
					dprintf(client, "CXLP/1 RESULT id=%s status=0 verify=1 pong=1\n",
						request.id);
					close(client);
				} else if (request.op == OP_REGISTER) {
					status = register_entry(registry, &request);
					if (status)
						send_error(client, request.id, status, "register");
					else
						dprintf(client,
							"CXLP/1 RESULT id=%s status=0 verify=1 registered=%s key=%" PRIu64 " generation=%" PRIu64 "\n",
							request.id, request.function,
							request.key, request.generation);
					close(client);
				} else {
					memset(&pending, 0, sizeof(pending));
					pending.client_fd = client;
					pending.enqueue_ns = nsec_now();
					status = make_job(&request, registry, &pending.job);
					if (status) {
						send_error(client, request.id, status, "lookup");
						close(client);
					} else {
						for (i = 0; i < pool_size; i++)
							if (!workers[i].busy) { target = i; break; }
						if (target >= 0) {
							if (dispatch_to_worker(&workers[target], &pending)) {
								send_error(client, request.id, EPIPE, "dispatch");
								close(client);
							}
						} else if (queue_push(&queue, &pending)) {
							send_error(client, request.id, EBUSY, "queue");
							close(client);
						}
					}
				}
			}
		}

		for (int p = 1; p < nfds; p++) {
			struct worker_result result;
			struct pending_request pending;
			int index = pollmap[p];
			int status;

			if (!(pollfds[p].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			memset(&result, 0, sizeof(result));
			if (read_full(workers[index].fd, &result, sizeof(result))) {
				result.status = ECHILD;
				result.verify = 0;
			}
			send_result(workers[index].client_fd, &workers[index], &result);
			close(workers[index].client_fd);
			close(workers[index].fd);
			(void)waitpid(workers[index].pid, &status, 0);
			if (spawn_worker(&workers[index])) {
				perror("respawn worker");
				stop_requested = 1;
				break;
			}
			if (!queue_pop(&queue, &pending)) {
				if (dispatch_to_worker(&workers[index], &pending)) {
					send_error(pending.client_fd, pending.job.id,
						   EPIPE, "dispatch");
					close(pending.client_fd);
				}
			}
		}
	}

	close(listener);
	for (i = 0; i < pool_size; i++) {
		kill(workers[i].pid, SIGTERM);
		close(workers[i].fd);
		waitpid(workers[i].pid, NULL, 0);
	}
	while (queue.count) {
		struct pending_request pending;
		queue_pop(&queue, &pending);
		send_error(pending.client_fd, pending.job.id, ECANCELED, "shutdown");
		close(pending.client_fd);
	}
	return 0;
}

static int checkpoint_cmd(int argc, char **argv)
{
	struct cxlfork_query_caps_v2 caps;
	struct cxlfork_region regions[CXLFORK_MAX_REGIONS];
	struct cxlfork_prepare_v2 op;
	enum function_kind fn;
	uint64_t key;
	uint64_t checksum = 0;
	size_t len;
	int prime = 16;
	int fd;
	int i;

	if (argc < 5 || argc > 6) {
		fprintf(stderr, "usage: %s checkpoint KEY FUNCTION BYTES [PRIME]\n", argv[0]);
		return 2;
	}
	key = strtoull(argv[2], NULL, 0);
	fn = parse_function(argv[3]);
	len = round_len(strtoull(argv[4], NULL, 0));
	if (argc == 6)
		prime = atoi(argv[5]);
	if (!key || (int)fn < 0 || !len || prime < 0)
		return 2;
	if (map_and_fill(len, fn) == MAP_FAILED) {
		perror("mmap function state");
		return 1;
	}
	for (i = 0; i < prime; i++)
		checksum = run_function(len, fn);

	fd = open(CXLFORK_DEVICE, O_RDWR);
	if (fd < 0) {
		perror(CXLFORK_DEVICE);
		return 1;
	}
	i = query_caps_fd(fd, &caps);
	if (i) {
		errno = -i;
		perror("CXLFORK_QUERY_CAPS_V2");
		close(fd);
		return 1;
	}
	init_v2(&op, sizeof(op));
	op.key = key;
	op.scan_start = (uintptr_t)FUNCTION_BASE;
	op.scan_end = (uintptr_t)FUNCTION_BASE + len;
	op.regions_ptr = (uintptr_t)regions;
	op.region_capacity = CXLFORK_MAX_REGIONS;
	if (ioctl(fd, CXLFORK_PREPARE_V2, &op)) {
		perror("CXLFORK_PREPARE_V2");
		close(fd);
		return 1;
	}
	close(fd);
	printf("cxlporter_checkpoint_v2 function=%s key=%" PRIu64
	       " generation=%" PRIu64 " slot=%" PRIu64 " bytes=%zu"
	       " pages=%" PRIu64 " page_checksum=%" PRIu64
	       " workload_checksum=%" PRIu64 " prime=%d"
	       " supported_mask=%" PRIu64 " valid_mask=%" PRIu64
	       " path_mask=%" PRIu64 " total_ns=%" PRIu64
	       " lock_wait_ns=%" PRIu64 " lifecycle_ns=%" PRIu64
	       " vma_preflight_ns=%" PRIu64 " pte_walk_ns=%" PRIu64
	       " gup_ns=%" PRIu64 " page_copy_ns=%" PRIu64
	       " pte_recheck_ns=%" PRIu64 " metadata_ns=%" PRIu64
	       " checksum_ns=%" PRIu64 " publish_ns=%" PRIu64
	       " other_ns=%" PRIu64 " verify=ok\n",
	       argv[3], key, op.generation, op.slot, len, op.total_pages,
	       op.page_checksum, checksum, prime, op.stats.supported_mask,
	       op.stats.valid_mask, op.stats.path_mask, op.stats.total_ns,
	       op.stats.lock_wait_ns, op.stats.lifecycle_ns,
	       op.stats.vma_preflight_ns, op.stats.pte_walk_ns,
	       op.stats.gup_ns, op.stats.page_copy_ns,
	       op.stats.pte_recheck_ns, op.stats.metadata_build_ns,
	       op.stats.checksum_seal_ns, op.stats.publish_ns,
	       op.stats.other_ns);
	return 0;
}

static int caps_cmd(void)
{
	struct cxlfork_query_caps_v2 caps;
	int fd = open(CXLFORK_DEVICE, O_RDWR);
	int status;

	if (fd < 0) {
		perror(CXLFORK_DEVICE);
		return 1;
	}
	status = query_caps_fd(fd, &caps);
	close(fd);
	if (status) {
		errno = -status;
		perror("CXLFORK_QUERY_CAPS_V2");
		return 1;
	}
	printf("cxlporter_caps abi=%u capabilities=%" PRIu64
	       " cxl_size=%" PRIu64 " slot_bytes=%" PRIu64
	       " max_slots=%u max_regions=%u shared_leaf_alignment=%" PRIu64
	       " policy_mask=%" PRIu64 " verify=ok\n",
	       caps.abi_version, caps.capabilities, caps.cxl_size,
	       caps.slot_bytes, caps.max_slots, caps.max_regions,
	       caps.shared_leaf_alignment, caps.restore_policy_mask);
	return 0;
}

static int release_cmd(int argc, char **argv)
{
	struct cxlfork_release_v2 release;
	int fd;

	if (argc != 4)
		return 2;
	init_v2(&release, sizeof(release));
	release.key = strtoull(argv[2], NULL, 0);
	release.generation = strtoull(argv[3], NULL, 0);
	fd = open(CXLFORK_DEVICE, O_RDWR);
	if (fd < 0 || ioctl(fd, CXLFORK_RELEASE_V2, &release)) {
		perror("CXLFORK_RELEASE_V2");
		if (fd >= 0) close(fd);
		return 1;
	}
	close(fd);
	printf("cxlporter_release_v2 key=%" PRIu64 " generation=%" PRIu64 " verify=ok\n",
	       release.key, release.generation);
	return 0;
}

static int connect_tcp(const char *host, const char *port)
{
	struct addrinfo hints, *addresses = NULL, *address;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &addresses))
		return -1;
	for (address = addresses; address; address = address->ai_next) {
		fd = socket(address->ai_family, address->ai_socktype, 0);
		if (fd < 0)
			continue;
		if (!connect(fd, address->ai_addr, address->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(addresses);
	return fd;
}

static int send_cmd(int argc, char **argv)
{
	char line[MAX_LINE] = {0};
	size_t used = 0;
	int fd;
	int i;

	if (argc < 5) {
		fprintf(stderr, "usage: %s send HOST PORT CXLP/1 ...\n", argv[0]);
		return 2;
	}
	for (i = 4; i < argc; i++) {
		int n = snprintf(line + used, sizeof(line) - used, "%s%s",
				 i == 4 ? "" : " ", argv[i]);
		if (n < 0 || (size_t)n >= sizeof(line) - used)
			return 2;
		used += (size_t)n;
	}
	line[used++] = '\n';
	fd = connect_tcp(argv[2], argv[3]);
	if (fd < 0) {
		perror("connect");
		return 1;
	}
	if (write_full(fd, line, used) || read_line(fd, line, sizeof(line))) {
		perror("CXLP/1 exchange");
		close(fd);
		return 1;
	}
	puts(line);
	close(fd);
	return strstr(line, " status=0 ") ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s caps|checkpoint|serve|send|release ...\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "caps")) return caps_cmd();
	if (!strcmp(argv[1], "checkpoint")) return checkpoint_cmd(argc, argv);
	if (!strcmp(argv[1], "serve")) return serve_cmd(argc, argv);
	if (!strcmp(argv[1], "send")) return send_cmd(argc, argv);
	if (!strcmp(argv[1], "release")) return release_cmd(argc, argv);
	fprintf(stderr, "unknown command: %s\n", argv[1]);
	return 2;
}
