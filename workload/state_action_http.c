// SPDX-License-Identifier: MIT
#define _GNU_SOURCE

#include "state_action_core.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HTTP_LIMIT (1024UL * 1024)

static void *state;
static uint64_t expected_checksum;
static uint64_t init_ns;
static uint64_t invocation;
static unsigned int init_count;
static char container_id[128];

static uint64_t nsec_now(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC_RAW, &time);
	return (uint64_t)time.tv_sec * 1000000000ULL + time.tv_nsec;
}

static int write_full(int fd, const void *buffer, size_t length)
{
	const char *cursor = buffer;

	while (length) {
		ssize_t bytes = write(fd, cursor, length);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return -1;
		cursor += bytes;
		length -= (size_t)bytes;
	}
	return 0;
}

static int send_json(int fd, int status, const char *json)
{
	char header[256];
	const char *reason = status == 200 ? "OK" : "Bad Request";
	int length;

	length = snprintf(header, sizeof(header),
			  "HTTP/1.1 %d %s\r\n"
			  "Content-Type: application/json\r\n"
			  "Content-Length: %zu\r\n"
			  "Connection: close\r\n\r\n",
			  status, reason, strlen(json));
	if (length < 0 || (size_t)length >= sizeof(header))
		return -1;
	return write_full(fd, header, (size_t)length) ||
		write_full(fd, json, strlen(json));
}

static int receive_request(int fd, char *method, size_t method_len,
			   char *path, size_t path_len)
{
	char *buffer;
	size_t used = 0;
	char *end;
	int result = -1;

	buffer = malloc(HTTP_LIMIT + 1);
	if (!buffer)
		return -1;
	while (used < HTTP_LIMIT) {
		ssize_t bytes = read(fd, buffer + used, HTTP_LIMIT - used);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			break;
		used += (size_t)bytes;
		buffer[used] = '\0';
		end = strstr(buffer, "\r\n\r\n");
		if (end) {
			result = sscanf(buffer, "%15s %127s", method, path) == 2 ? 0 : -1;
			break;
		}
	}
	free(buffer);
	(void)method_len;
	(void)path_len;
	return result;
}

static void handle_client(int fd)
{
	char method[16] = { 0 };
	char path[128] = { 0 };
	char response[1024];
	uint64_t start;
	uint64_t checksum;
	uint64_t workload_ns;
	int cpu;

	if (receive_request(fd, method, sizeof(method), path, sizeof(path)) ||
	    strcmp(method, "POST")) {
		send_json(fd, 400, "{\"error\":\"invalid request\"}");
		return;
	}
	if (!strcmp(path, "/init")) {
		snprintf(response, sizeof(response),
			 "{\"ok\":true,\"init_count\":%u,\"state_bytes\":%lu,"
			 "\"state_checksum\":\"%016" PRIx64 "\",\"init_ns\":%" PRIu64 "}",
			 init_count, STATE_ACTION_BYTES, expected_checksum, init_ns);
		send_json(fd, 200, response);
		return;
	}
	if (strcmp(path, "/run")) {
		send_json(fd, 400, "{\"error\":\"unknown path\"}");
		return;
	}
	start = nsec_now();
	checksum = state_action_scan(state, STATE_ACTION_BYTES);
	workload_ns = nsec_now() - start;
	cpu = sched_getcpu();
	invocation++;
	snprintf(response, sizeof(response),
		 "{\"verify\":%s,\"state_bytes\":%lu,\"state_pages\":%lu,"
		 "\"state_checksum\":\"%016" PRIx64 "\","
		 "\"expected_checksum\":\"%016" PRIx64 "\","
		 "\"workload_ns\":%" PRIu64 ",\"init_ns\":%" PRIu64 ","
		 "\"init_count\":%u,\"invocation\":%" PRIu64 ","
		 "\"worker_pid\":%ld,\"worker_cpu\":%d,"
		 "\"container_id\":\"%s\",\"workload\":\"state-scan-4m-v1\"}",
		 checksum == expected_checksum ? "true" : "false",
		 STATE_ACTION_BYTES, STATE_ACTION_PAGES, checksum,
		 expected_checksum, workload_ns, init_ns, init_count,
		 invocation, (long)getpid(), cpu, container_id);
	send_json(fd, checksum == expected_checksum ? 200 : 400, response);
}

int main(void)
{
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(8080),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	int server;
	int enabled = 1;
	uint64_t start;

	signal(SIGPIPE, SIG_IGN);
	if (posix_memalign(&state, STATE_ACTION_PAGE_BYTES, STATE_ACTION_BYTES))
		return 2;
	madvise(state, STATE_ACTION_BYTES, MADV_NOHUGEPAGE);
	start = nsec_now();
	state_action_initialize(state, STATE_ACTION_BYTES, STATE_ACTION_SEED);
	expected_checksum = state_action_scan(state, STATE_ACTION_BYTES);
	init_ns = nsec_now() - start;
	init_count = 1;
	if (gethostname(container_id, sizeof(container_id) - 1))
		snprintf(container_id, sizeof(container_id), "unknown");
	server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (server < 0)
		return 3;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
	if (bind(server, (struct sockaddr *)&address, sizeof(address)) ||
	    listen(server, 128))
		return 4;
	for (;;) {
		int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);

		if (client < 0) {
			if (errno == EINTR)
				continue;
			return 5;
		}
		handle_client(client);
		close(client);
	}
}
