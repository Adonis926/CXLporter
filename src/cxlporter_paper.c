// SPDX-License-Identifier: MIT
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "paper_protocol.h"

#define DEFAULT_LISTEN_ADDRESS "0.0.0.0"
#define DEFAULT_LISTEN_PORT "7100"
#define DEFAULT_HOST_GHOST_SOCKET "/run/cxlfork-paper/ghost.sock"
#define MAX_LINE 32768

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int write_full(int fd, const void *buffer, size_t length)
{
	const unsigned char *cursor = buffer;

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

static int read_line(int fd, char *line, size_t capacity)
{
	size_t used = 0;

	while (used + 1 < capacity) {
		char byte;
		ssize_t bytes = read(fd, &byte, 1);

		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes <= 0)
			return -1;
		if (byte == '\n') {
			line[used] = '\0';
			return 0;
		}
		if (byte != '\r')
			line[used++] = byte;
	}
	errno = EMSGSIZE;
	return -1;
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno || !*text || *end)
		return -1;
	*value = parsed;
	return 0;
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
	size_t length = strlen(source);

	if (length >= capacity)
		length = capacity - 1;
	memset(destination, 0, capacity);
	memcpy(destination, source, length);
}

static int parse_request(char *line, struct cxlporter_ghost_request *request)
{
	char *words[8];
	char *save = NULL;
	size_t count = 0;
	char *word;

	for (word = strtok_r(line, " \t", &save);
	     word && count < sizeof(words) / sizeof(words[0]);
	     word = strtok_r(NULL, " \t", &save))
		words[count++] = word;
	if (count < 3 || strcmp(words[0], CXLPORTER_TEXT_PROTOCOL) ||
	    parse_u64(words[2], &request->request_id))
		return -1;
	request->magic = CXLPORTER_GHOST_MAGIC;
	request->version = CXLPORTER_GHOST_VERSION;
	if (!strcmp(words[1], "PING") && count == 3) {
		request->op = CXLPORTER_GHOST_PING;
		return 0;
	}
	if (!strcmp(words[1], "POOL_STATUS") && count == 3) {
		request->op = CXLPORTER_GHOST_POOL_STATUS;
		return 0;
	}
	if (!strcmp(words[1], "ACTIVATE") && count == 6) {
		request->op = CXLPORTER_GHOST_ACTIVATE;
		copy_text(request->owner, sizeof(request->owner), words[3]);
		copy_text(request->function, sizeof(request->function), words[4]);
		copy_text(request->capsule_version,
			  sizeof(request->capsule_version), words[5]);
		return 0;
	}
	return -1;
}

static int ghost_roundtrip(const char *socket_path,
			   const struct cxlporter_ghost_request *request,
			   struct cxlporter_ghost_response *response,
			   uint64_t *connect_ns, uint64_t *roundtrip_ns)
{
	struct sockaddr_un address = {
		.sun_family = AF_UNIX,
	};
	uint64_t start;
	int fd;
	ssize_t bytes;

	if (strlen(socket_path) >= sizeof(address.sun_path))
		return -ENAMETOOLONG;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	start = nsec_now();
	if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
		int saved_errno = errno;

		close(fd);
		return -saved_errno;
	}
	*connect_ns = nsec_now() - start;
	start = nsec_now();
	if (send(fd, request, sizeof(*request), MSG_NOSIGNAL) !=
	    (ssize_t)sizeof(*request)) {
		int saved_errno = errno ? errno : EIO;

		close(fd);
		return -saved_errno;
	}
	bytes = recv(fd, response, sizeof(*response), MSG_WAITALL);
	*roundtrip_ns = nsec_now() - start;
	close(fd);
	if (bytes != (ssize_t)sizeof(*response))
		return -EPROTO;
	if (response->magic != CXLPORTER_GHOST_MAGIC ||
	    response->version != CXLPORTER_GHOST_VERSION ||
	    response->request_id != request->request_id ||
	    response->op != request->op ||
	    response->output_len >= sizeof(response->output))
		return -EPROTO;
	return 0;
}

static void hex_encode(const char *input, size_t length,
		       char *output, size_t capacity)
{
	static const char digits[] = "0123456789abcdef";
	size_t index;

	if (length > (capacity - 1) / 2)
		length = (capacity - 1) / 2;
	for (index = 0; index < length; index++) {
		unsigned char byte = (unsigned char)input[index];

		output[index * 2] = digits[byte >> 4];
		output[index * 2 + 1] = digits[byte & 0xf];
	}
	output[length * 2] = '\0';
}

static void handle_client(int client, const char *socket_path)
{
	struct cxlporter_ghost_request request;
	struct cxlporter_ghost_response response;
	char line[1024];
	char output_hex[CXLPORTER_OUTPUT_LEN * 2 + 1];
	uint64_t daemon_receive_ns;
	uint64_t connect_ns = 0;
	uint64_t roundtrip_ns = 0;
	uint64_t daemon_total_ns;
	uint64_t daemon_to_agent_ns = 0;
	int status;

	daemon_receive_ns = nsec_now();
	memset(&request, 0, sizeof(request));
	if (read_line(client, line, sizeof(line)) ||
	    parse_request(line, &request)) {
		dprintf(client,
			"CXLP/2 ERROR status=%d reason=bad-request\n",
			-EPROTO);
		return;
	}
	memset(&response, 0, sizeof(response));
	status = ghost_roundtrip(socket_path, &request, &response,
				 &connect_ns, &roundtrip_ns);
	daemon_total_ns = nsec_now() - daemon_receive_ns;
	if (status) {
		dprintf(client,
			"CXLP/2 ERROR request_id=%" PRIu64
			" status=%d daemon_total_ns=%" PRIu64 "\n",
			request.request_id, status, daemon_total_ns);
		return;
	}
	if (response.agent_receive_ns >= daemon_receive_ns)
		daemon_to_agent_ns =
			response.agent_receive_ns - daemon_receive_ns;
	hex_encode(response.output, response.output_len, output_hex,
		   sizeof(output_hex));
	dprintf(client,
		"CXLP/2 %s request_id=%" PRIu64
		" status=%d child_status=%d verify=%u"
		" key=%" PRIu64 " slot_generation=%" PRIu64
		" catalog_generation=%" PRIu64
		" restore_id=%" PRIu64
		" daemon_receive_ns=%" PRIu64
		" unix_connect_ns=%" PRIu64
		" unix_roundtrip_ns=%" PRIu64
		" daemon_to_agent_ns=%" PRIu64
		" catalog_lookup_ns=%" PRIu64
		" fork_to_exit_ns=%" PRIu64
		" kernel_restore_ns=%" PRIu64
		" workload_ns=%" PRIu64
		" state_bytes=%" PRIu64
		" state_checksum=%016" PRIx64
		" worker_pid=%" PRIu64
		" worker_cpu=%" PRIu64
		" source_initializations=%" PRIu64
		" invocation=%" PRIu64
		" agent_total_ns=%" PRIu64
		" daemon_total_ns=%" PRIu64
		" output_hex=%s\n",
		response.status ? "ERROR" : "OK",
		response.request_id, response.status,
		response.child_status, response.verify,
		response.key, response.slot_generation,
		response.catalog_generation, response.restore_id,
		daemon_receive_ns, connect_ns, roundtrip_ns,
		daemon_to_agent_ns, response.catalog_lookup_ns,
		response.fork_to_exit_ns, response.kernel_restore_ns,
		response.workload_ns, response.state_bytes,
		response.state_checksum, response.worker_pid,
		response.worker_cpu, response.source_initializations,
		response.invocation,
		response.agent_total_ns, daemon_total_ns, output_hex);
}

static int listen_tcp(const char *address_text, const char *port)
{
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE,
	};
	struct addrinfo *addresses;
	struct addrinfo *address;
	int listen_socket = -1;
	int enabled = 1;
	int error;

	error = getaddrinfo(address_text, port, &hints, &addresses);
	if (error) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
		return -1;
	}
	for (address = addresses; address; address = address->ai_next) {
		listen_socket = socket(address->ai_family,
				       address->ai_socktype | SOCK_CLOEXEC,
				       address->ai_protocol);
		if (listen_socket < 0)
			continue;
		setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
			   &enabled, sizeof(enabled));
		if (!bind(listen_socket, address->ai_addr,
			  address->ai_addrlen) &&
		    !listen(listen_socket, 128))
			break;
		close(listen_socket);
		listen_socket = -1;
	}
	freeaddrinfo(addresses);
	return listen_socket;
}

static int run_daemon(const char *address, const char *port,
		      const char *socket_path)
{
	int server;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	server = listen_tcp(address, port);
	if (server < 0) {
		perror("listen TCP");
		return 1;
	}
	printf("cxlporter-paper event=ready protocol=CXLP/2 listen=%s:%s"
	       " ghost_socket=%s device_access=none verify=ok\n",
	       address, port, socket_path);
	fflush(stdout);
	for (;;) {
		int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
		pid_t child;

		if (client < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}
		child = fork();
		if (child < 0) {
			close(client);
			continue;
		}
		if (!child) {
			close(server);
			handle_client(client, socket_path);
			close(client);
			_exit(0);
		}
		close(client);
	}
	close(server);
	return 1;
}

static int connect_tcp(const char *host, const char *port)
{
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *addresses;
	struct addrinfo *address;
	int fd = -1;
	int error;

	error = getaddrinfo(host, port, &hints, &addresses);
	if (error) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
		return -1;
	}
	for (address = addresses; address; address = address->ai_next) {
		fd = socket(address->ai_family,
			    address->ai_socktype | SOCK_CLOEXEC,
			    address->ai_protocol);
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

static int run_client(int argc, char **argv)
{
	char request[512];
	char response[MAX_LINE];
	const char *host;
	const char *port;
	const char *operation;
	uint64_t request_id;
	int fd;

	if (argc < 6 || parse_u64(argv[5], &request_id))
		return 2;
	host = argv[2];
	port = argv[3];
	operation = argv[4];
	if (!strcmp(operation, "ACTIVATE") && argc == 9) {
		snprintf(request, sizeof(request),
			 "CXLP/2 ACTIVATE %" PRIu64 " %s %s %s\n",
			 request_id, argv[6], argv[7], argv[8]);
	} else if ((!strcmp(operation, "PING") ||
		    !strcmp(operation, "POOL_STATUS")) && argc == 6) {
		snprintf(request, sizeof(request), "CXLP/2 %s %" PRIu64 "\n",
			 operation, request_id);
	} else {
		return 2;
	}
	fd = connect_tcp(host, port);
	if (fd < 0) {
		perror("connect");
		return 1;
	}
	if (write_full(fd, request, strlen(request)) ||
	    read_line(fd, response, sizeof(response))) {
		perror("client I/O");
		close(fd);
		return 1;
	}
	close(fd);
	puts(response);
	return strstr(response, "CXLP/2 OK") == response ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "daemon")) {
		const char *address = argc > 2 ? argv[2] : DEFAULT_LISTEN_ADDRESS;
		const char *port = argc > 3 ? argv[3] : DEFAULT_LISTEN_PORT;
		const char *socket_path =
			argc > 4 ? argv[4] : DEFAULT_HOST_GHOST_SOCKET;

		if (argc > 5)
			goto usage;
		return run_daemon(address, port, socket_path);
	}
	if (argc >= 2 && !strcmp(argv[1], "client")) {
		int result = run_client(argc, argv);

		if (result != 2)
			return result;
	}
usage:
	fprintf(stderr,
		"usage:\n"
		"  %s daemon [ADDRESS [PORT [GHOST_SOCKET]]]\n"
		"  %s client HOST PORT ACTIVATE REQUEST_ID OWNER FUNCTION VERSION\n"
		"  %s client HOST PORT PING|POOL_STATUS REQUEST_ID\n",
		argv[0], argv[0], argv[0]);
	return 2;
}
