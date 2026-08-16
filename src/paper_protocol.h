/* SPDX-License-Identifier: MIT */
#ifndef CXLPORTER_PAPER_PROTOCOL_H
#define CXLPORTER_PAPER_PROTOCOL_H

#include <stdint.h>

#define CXLPORTER_GHOST_MAGIC 0x43584732U /* "CXG2" */
#define CXLPORTER_GHOST_VERSION 1U
#define CXLPORTER_GHOST_SOCKET "/run/cxlfork/ghost.sock"
#define CXLPORTER_TEXT_PROTOCOL "CXLP/2"

#define CXLPORTER_OWNER_LEN 32
#define CXLPORTER_FUNCTION_LEN 32
#define CXLPORTER_VERSION_LEN 16
#define CXLPORTER_OUTPUT_LEN 8192

enum cxlporter_ghost_op {
	CXLPORTER_GHOST_PING = 1,
	CXLPORTER_GHOST_POOL_STATUS = 2,
	CXLPORTER_GHOST_ACTIVATE = 3,
};

struct cxlporter_ghost_request {
	uint32_t magic;
	uint16_t version;
	uint16_t op;
	uint32_t flags;
	uint32_t reserved;
	uint64_t request_id;
	char owner[CXLPORTER_OWNER_LEN];
	char function[CXLPORTER_FUNCTION_LEN];
	char capsule_version[CXLPORTER_VERSION_LEN];
};

struct cxlporter_ghost_response {
	uint32_t magic;
	uint16_t version;
	uint16_t op;
	int32_t status;
	int32_t child_status;
	uint32_t verify;
	uint32_t output_len;
	uint64_t request_id;
	uint64_t key;
	uint64_t slot_generation;
	uint64_t catalog_generation;
	uint64_t restore_id;
	uint64_t agent_receive_ns;
	uint64_t catalog_lookup_ns;
	uint64_t fork_to_exit_ns;
	uint64_t kernel_restore_ns;
	uint64_t workload_ns;
	uint64_t state_bytes;
	uint64_t state_checksum;
	uint64_t worker_pid;
	uint64_t worker_cpu;
	uint64_t source_initializations;
	uint64_t invocation;
	uint64_t agent_total_ns;
	char output[CXLPORTER_OUTPUT_LEN];
};

#endif
