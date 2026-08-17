// SPDX-License-Identifier: MIT
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "paper_protocol.h"

#define DEFAULT_LISTEN_ADDRESS "0.0.0.0"
#define DEFAULT_LISTEN_PORT "7100"
#define DEFAULT_HOST_GHOST_SOCKET "/run/cxlfork-paper/ghost.sock"
#define DEFAULT_COLD_ROOT "/run/cxlfork-cold"
#define DEFAULT_COLD_IMAGE "cxlfork-ghost:state-action-v1"
#define DEFAULT_COLD_CPUSET "0"
#define MAX_LINE 32768

struct daemon_config {
	const char *socket_path;
	bool cold_ghost;
	const char *cold_root;
	const char *cold_image;
	const char *cold_cpuset;
};

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

static bool environment_enabled(const char *name)
{
	const char *value = getenv(name);

	return value && (!strcmp(value, "1") || !strcasecmp(value, "true") ||
		       !strcasecmp(value, "yes"));
}

static int run_quiet_command(char *const arguments[])
{
	int status;
	pid_t child = fork();

	if (child < 0)
		return -errno;
	if (!child) {
		int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null_fd >= 0) {
			dup2(null_fd, STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			close(null_fd);
		}
		execvp(arguments[0], arguments);
		_exit(127);
	}
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return -errno;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		return -EIO;
	return 0;
}

static void remove_cold_directory(const char *directory,
				  const char *socket_path)
{
	unlink(socket_path);
	rmdir(directory);
}

static int create_cold_ghost(const struct daemon_config *config,
			     uint64_t request_id, char *instance,
			     size_t instance_capacity, char *directory,
			     size_t directory_capacity, char *socket_path,
			     size_t socket_capacity, uint64_t *create_ns)
{
	char mount_spec[PATH_MAX + 32];
	char workers[] = "CXLPORTER_GHOST_WORKERS=1";
	char *arguments[] = {
		"docker", "run", "-d", "--name", instance,
		"--network", "none", "--cpuset-cpus",
		(char *)config->cold_cpuset,
		"--device", "/dev/cxlfork:/dev/cxlfork",
		"-e", workers, "-v", mount_spec,
		(char *)config->cold_image, NULL,
	};
	char *cleanup_arguments[] = {
		"docker", "rm", "-f", instance, NULL,
	};
	struct stat metadata;
	struct timespec pause = {
		.tv_nsec = 1000000,
	};
	uint64_t start = nsec_now();
	int index;
	int status;

	if (snprintf(instance, instance_capacity, "cxlfork-cold-%ld-%016" PRIx64,
		     (long)getpid(), request_id) >= (int)instance_capacity ||
	    snprintf(directory, directory_capacity, "%s/%ld-%016" PRIx64,
		     config->cold_root, (long)getpid(), request_id) >=
		     (int)directory_capacity ||
	    snprintf(socket_path, socket_capacity, "%s/ghost.sock", directory) >=
		     (int)socket_capacity ||
	    snprintf(mount_spec, sizeof(mount_spec), "%s:/run/cxlfork",
		     directory) >= (int)sizeof(mount_spec))
		return -ENAMETOOLONG;
	if (mkdir(config->cold_root, 0777) && errno != EEXIST)
		return -errno;
	chmod(config->cold_root, 0777);
	if (mkdir(directory, 0777))
		return -errno;
	chmod(directory, 0777);
	status = run_quiet_command(arguments);
	if (status) {
		run_quiet_command(cleanup_arguments);
		remove_cold_directory(directory, socket_path);
		return status;
	}
	for (index = 0; index < 60000; index++) {
		if (!stat(socket_path, &metadata) && S_ISSOCK(metadata.st_mode)) {
			*create_ns = nsec_now() - start;
			return 0;
		}
		nanosleep(&pause, NULL);
	}
	run_quiet_command(cleanup_arguments);
	remove_cold_directory(directory, socket_path);
	return -ETIMEDOUT;
}

static int destroy_cold_ghost(const char *instance, const char *directory,
			      const char *socket_path, uint64_t *teardown_ns)
{
	char *arguments[] = {
		"docker", "rm", "-f", (char *)instance, NULL,
	};
	uint64_t start = nsec_now();
	int status = run_quiet_command(arguments);

	remove_cold_directory(directory, socket_path);
	*teardown_ns = nsec_now() - start;
	return status;
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

static void handle_client(int client, const struct daemon_config *config)
{
	struct cxlporter_ghost_request request;
	struct cxlporter_ghost_response response;
	char line[1024];
	char output_hex[CXLPORTER_OUTPUT_LEN * 2 + 1];
	char cold_instance[128] = "persistent";
	char cold_directory[PATH_MAX] = "";
	char cold_socket[PATH_MAX] = "";
	const char *socket_path = config->socket_path;
	uint64_t daemon_receive_ns;
	uint64_t connect_ns = 0;
	uint64_t roundtrip_ns = 0;
	uint64_t daemon_total_ns;
	uint64_t daemon_to_agent_ns = 0;
	uint64_t ghost_create_ns = 0;
	uint64_t ghost_teardown_ns = 0;
	bool cold_created = false;
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
	if (config->cold_ghost && request.op == CXLPORTER_GHOST_ACTIVATE) {
		status = create_cold_ghost(config, request.request_id,
					   cold_instance, sizeof(cold_instance),
					   cold_directory, sizeof(cold_directory),
					   cold_socket, sizeof(cold_socket),
					   &ghost_create_ns);
		if (!status) {
			cold_created = true;
			socket_path = cold_socket;
		}
	} else {
		status = 0;
	}
	if (!status)
		status = ghost_roundtrip(socket_path, &request, &response,
					 &connect_ns, &roundtrip_ns);
	if (cold_created) {
		int cleanup_status = destroy_cold_ghost(cold_instance,
							cold_directory, cold_socket,
							&ghost_teardown_ns);

		if (!status && cleanup_status)
			status = cleanup_status;
	}
	daemon_total_ns = nsec_now() - daemon_receive_ns;
	if (status) {
		dprintf(client,
			"CXLP/2 ERROR request_id=%" PRIu64
			" status=%d daemon_total_ns=%" PRIu64
			" ghost_create_ns=%" PRIu64
			" ghost_teardown_ns=%" PRIu64
			" ghost_instance_id=%s\n",
			request.request_id, status, daemon_total_ns,
			ghost_create_ns, ghost_teardown_ns, cold_instance);
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
		" ghost_create_ns=%" PRIu64
		" ghost_teardown_ns=%" PRIu64
		" ghost_instance_id=%s"
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
		response.agent_total_ns, daemon_total_ns,
		ghost_create_ns, ghost_teardown_ns, cold_instance, output_hex);
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
		      const struct daemon_config *config)
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
	       " ghost_socket=%s cold_ghost=%u cold_image=%s"
	       " device_access=none verify=ok\n",
	       address, port, config->socket_path, config->cold_ghost,
	       config->cold_ghost ? config->cold_image : "NA");
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
			signal(SIGCHLD, SIG_DFL);
			handle_client(client, config);
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
		struct daemon_config config = {
			.cold_ghost = environment_enabled("CXLPORTER_COLD_GHOST"),
			.cold_root = getenv("CXLPORTER_COLD_ROOT"),
			.cold_image = getenv("CXLPORTER_COLD_IMAGE"),
			.cold_cpuset = getenv("CXLPORTER_COLD_CPUSET"),
		};
		const char *address = argc > 2 ? argv[2] : DEFAULT_LISTEN_ADDRESS;
		const char *port = argc > 3 ? argv[3] : DEFAULT_LISTEN_PORT;

		config.socket_path =
			argc > 4 ? argv[4] : DEFAULT_HOST_GHOST_SOCKET;
		if (!config.cold_root)
			config.cold_root = DEFAULT_COLD_ROOT;
		if (!config.cold_image)
			config.cold_image = DEFAULT_COLD_IMAGE;
		if (!config.cold_cpuset)
			config.cold_cpuset = DEFAULT_COLD_CPUSET;

		if (argc > 5)
			goto usage;
		return run_daemon(address, port, &config);
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
