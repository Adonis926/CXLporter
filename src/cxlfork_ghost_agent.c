// SPDX-License-Identifier: MIT
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/cxlfork.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "paper_protocol.h"

#define DEFAULT_RUNTIME "/opt/cxlfork/cxlfork_capsule_k0"
#define DEFAULT_WORKERS 16U
#define MAX_WORKERS 128U

static volatile sig_atomic_t stop_requested;
static int listen_fd = -1;
static const char *runtime_path = DEFAULT_RUNTIME;
static unsigned int worker_count = DEFAULT_WORKERS;

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
	if (listen_fd >= 0)
		close(listen_fd);
}

static void copy_identity(uint8_t *dst, size_t dst_len,
			  const char *src, size_t src_len)
{
	size_t len = strnlen(src, src_len);

	if (len >= dst_len)
		len = dst_len - 1;
	memset(dst, 0, dst_len);
	memcpy(dst, src, len);
}

static int catalog_lookup(const struct cxlporter_ghost_request *request,
			  struct cxlfork_catalog_record_v4 *record)
{
	struct cxlfork_catalog_lookup_v4 op = {
		.size = sizeof(op),
		.abi_version = CXLFORK_ABI_VERSION_V4,
	};
	int fd;
	int saved_errno;

	copy_identity(op.record.owner, sizeof(op.record.owner),
		      request->owner, sizeof(request->owner));
	copy_identity(op.record.function, sizeof(op.record.function),
		      request->function, sizeof(request->function));
	copy_identity(op.record.version, sizeof(op.record.version),
		      request->capsule_version,
		      sizeof(request->capsule_version));
	fd = open("/dev/cxlfork", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (ioctl(fd, CXLFORK_CATALOG_LOOKUP_V4, &op)) {
		saved_errno = errno;
		close(fd);
		return -saved_errno;
	}
	close(fd);
	*record = op.record;
	return 0;
}

static uint64_t parse_v4_metric(const char *output, const char *name)
{
	const char *stats;
	const char *field;
	char *end;

	stats = strstr(output, "event=v4-stats");
	if (!stats)
		return 0;
	field = strstr(stats, name);
	if (!field)
		return 0;
	field += strlen(name);
	errno = 0;
	return strtoull(field, &end, 10);
}

static uint64_t parse_hex_metric(const char *output, const char *name)
{
	const char *field = strstr(output, name);
	char *end;

	if (!field)
		return 0;
	field += strlen(name);
	errno = 0;
	return strtoull(field, &end, 16);
}

static uint64_t parse_runtime_metric(const char *output, const char *name)
{
	const char *continued = strstr(output, "event=target-continued");
	const char *field;
	char *end;

	if (!continued)
		return 0;
	field = strstr(continued, name);
	if (!field)
		return 0;
	field += strlen(name);
	errno = 0;
	return strtoull(field, &end, 10);
}

static int run_capsule(const struct cxlfork_catalog_record_v4 *record,
		       struct cxlporter_ghost_response *response)
{
	char key[32];
	int pipefd[2];
	pid_t child;
	size_t used = 0;
	int wait_status;
	uint64_t start;

	if (pipe2(pipefd, O_CLOEXEC))
		return -errno;
	snprintf(key, sizeof(key), "%" PRIu64, (uint64_t)record->key);
	start = nsec_now();
	child = fork();
	if (child < 0) {
		int saved_errno = errno;

		close(pipefd[0]);
		close(pipefd[1]);
		return -saved_errno;
	}
	if (!child) {
		if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
		    dup2(pipefd[1], STDERR_FILENO) < 0)
			_exit(126);
		close(pipefd[0]);
		close(pipefd[1]);
		if (listen_fd >= 0)
			close(listen_fd);
		execl(runtime_path, runtime_path, "restore", key, NULL);
		_exit(errno == ENOENT ? 127 : 126);
	}
	close(pipefd[1]);
	for (;;) {
		char discard[1024];
		char *target;
		size_t room;
		ssize_t bytes;

		if (used < sizeof(response->output) - 1) {
			target = response->output + used;
			room = sizeof(response->output) - 1 - used;
		} else {
			target = discard;
			room = sizeof(discard);
		}
		bytes = read(pipefd[0], target, room);
		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			break;
		if (target != discard)
			used += (size_t)bytes;
	}
	close(pipefd[0]);
	while (waitpid(child, &wait_status, 0) < 0) {
		if (errno != EINTR)
			return -errno;
	}
	response->fork_to_exit_ns = nsec_now() - start;
	response->output[used] = '\0';
	response->output_len = (uint32_t)used;
	if (WIFEXITED(wait_status))
		response->child_status = WEXITSTATUS(wait_status);
	else if (WIFSIGNALED(wait_status))
		response->child_status = 128 + WTERMSIG(wait_status);
	else
		response->child_status = 255;
	response->kernel_restore_ns =
		parse_v4_metric(response->output, "total_ns=");
	response->restore_id =
		parse_v4_metric(response->output, "restore_id=");
	response->workload_ns =
		parse_runtime_metric(response->output, "workload_ns=");
	response->state_bytes =
		parse_runtime_metric(response->output, "state_bytes=");
	response->state_checksum =
		parse_hex_metric(response->output, "state_checksum=");
	response->worker_pid =
		parse_runtime_metric(response->output, "target_pid=");
	response->worker_cpu =
		parse_runtime_metric(response->output, "worker_cpu=");
	response->source_initializations =
		parse_runtime_metric(response->output, "source_inits=");
	response->invocation =
		parse_runtime_metric(response->output, "invocation=");
	response->verify = response->child_status == 0 &&
		strstr(response->output, "event=shared-leaf-proof") &&
		strstr(response->output, "event=target-continued") &&
		strstr(response->output, "markers=ok verify=ok");
	return response->verify ? 0 : -EIO;
}

static void text_response(struct cxlporter_ghost_response *response,
			  const char *text)
{
	int length;

	length = snprintf(response->output, sizeof(response->output), "%s", text);
	if (length < 0)
		length = 0;
	if ((size_t)length >= sizeof(response->output))
		length = sizeof(response->output) - 1;
	response->output_len = (uint32_t)length;
	response->verify = 1;
}

static void process_request(const struct cxlporter_ghost_request *request,
			    struct cxlporter_ghost_response *response)
{
	struct cxlfork_catalog_record_v4 record = { 0 };
	uint64_t lookup_start;

	memset(response, 0, sizeof(*response));
	response->magic = CXLPORTER_GHOST_MAGIC;
	response->version = CXLPORTER_GHOST_VERSION;
	response->op = request->op;
	response->request_id = request->request_id;
	response->agent_receive_ns = nsec_now();
	if (request->magic != CXLPORTER_GHOST_MAGIC ||
	    request->version != CXLPORTER_GHOST_VERSION ||
	    request->reserved) {
		response->status = -EPROTO;
		goto out;
	}
	if (request->op == CXLPORTER_GHOST_PING) {
		text_response(response, "ghost=pong verify=ok");
		goto out;
	}
	if (request->op == CXLPORTER_GHOST_POOL_STATUS) {
		char status[128];

		snprintf(status, sizeof(status),
			 "ghost=ready pid=%ld model=persistent-pid1-fork-per-activation"
			 " workers=%u verify=ok", (long)getpid(), worker_count);
		text_response(response, status);
		goto out;
	}
	if (request->op != CXLPORTER_GHOST_ACTIVATE) {
		response->status = -EOPNOTSUPP;
		goto out;
	}
	lookup_start = nsec_now();
	response->status = catalog_lookup(request, &record);
	response->catalog_lookup_ns = nsec_now() - lookup_start;
	if (response->status)
		goto out;
	response->key = record.key;
	response->slot_generation = record.slot_generation;
	response->catalog_generation = record.catalog_generation;
	response->status = run_capsule(&record, response);
out:
	response->agent_total_ns = nsec_now() - response->agent_receive_ns;
}

static void *serve_worker(void *unused)
{
	(void)unused;
	while (!stop_requested) {
		struct cxlporter_ghost_request request;
		struct cxlporter_ghost_response response;
		int client;
		ssize_t bytes;

		client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
		if (client < 0) {
			if (errno == EINTR && !stop_requested)
				continue;
			if (!stop_requested)
				perror("accept");
			break;
		}
		bytes = recv(client, &request, sizeof(request), MSG_WAITALL);
		if (bytes != (ssize_t)sizeof(request)) {
			memset(&response, 0, sizeof(response));
			response.magic = CXLPORTER_GHOST_MAGIC;
			response.version = CXLPORTER_GHOST_VERSION;
			response.status = -EPROTO;
		} else {
			process_request(&request, &response);
		}
		if (send(client, &response, sizeof(response), MSG_NOSIGNAL) !=
		    (ssize_t)sizeof(response))
			perror("send ghost response");
		close(client);
	}
	return NULL;
}

static int serve(const char *socket_path)
{
	struct sockaddr_un address = {
		.sun_family = AF_UNIX,
	};
	struct sigaction action = {
		.sa_handler = stop_handler,
	};
	pthread_t workers[MAX_WORKERS];
	unsigned int started = 0;
	unsigned int index;
	int result = 0;

	if (strlen(socket_path) >= sizeof(address.sun_path)) {
		fprintf(stderr, "ghost socket path is too long: %s\n", socket_path);
		return 2;
	}
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		perror("socket");
		return 1;
	}
	unlink(socket_path);
	if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) ||
	    chmod(socket_path, 0666) || listen(listen_fd, 64)) {
		perror("bind/listen ghost socket");
		close(listen_fd);
		unlink(socket_path);
		return 1;
	}
	for (index = 0; index < worker_count; index++) {
		int error = pthread_create(&workers[index], NULL, serve_worker, NULL);

		if (error) {
			errno = error;
			perror("pthread_create");
			result = 1;
			stop_requested = 1;
			close(listen_fd);
			listen_fd = -1;
			break;
		}
		started++;
	}
	printf("ghost-agent event=ready pid=%ld socket=%s runtime=%s"
	       " workers=%u verify=%s\n",
	       (long)getpid(), socket_path, runtime_path, started,
	       started == worker_count ? "ok" : "failed");
	fflush(stdout);
	for (index = 0; index < started; index++) {
		int error = pthread_join(workers[index], NULL);

		if (error && !result) {
			errno = error;
			perror("pthread_join");
			result = 1;
		}
	}
	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}
	unlink(socket_path);
	return result;
}

static int parse_workers(const char *text, unsigned int *workers)
{
	char *end;
	unsigned long value;

	if (!text || !*text)
		return 0;
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || *end || value < 1 || value > MAX_WORKERS)
		return -1;
	*workers = (unsigned int)value;
	return 0;
}

int main(int argc, char **argv)
{
	const char *socket_path = CXLPORTER_GHOST_SOCKET;
	const char *workers = getenv("CXLPORTER_GHOST_WORKERS");

	if (parse_workers(workers, &worker_count)) {
		fprintf(stderr, "invalid CXLPORTER_GHOST_WORKERS: %s\n", workers);
		return 2;
	}

	if (argc == 3 && !strcmp(argv[1], "--runtime")) {
		runtime_path = argv[2];
	} else if (argc == 5 && !strcmp(argv[1], "--runtime") &&
		   !strcmp(argv[3], "--socket")) {
		runtime_path = argv[2];
		socket_path = argv[4];
	} else if (argc != 1) {
		fprintf(stderr,
			"usage: %s [--runtime PATH [--socket PATH]]"
			" (workers: CXLPORTER_GHOST_WORKERS=1..%u)\n",
			argv[0], MAX_WORKERS);
		return 2;
	}
	return serve(socket_path);
}
