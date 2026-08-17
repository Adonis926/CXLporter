// SPDX-License-Identifier: MIT
/*
 * Cooperative CXLfork capsule for the OpenWhisk state-action experiment.
 * This is intentionally a restricted function continuation, not arbitrary
 * Linux process restoration.  The 4 MiB state image is initialized once on
 * VM0, checkpointed with its continuation, scanned on VM1, and never rebuilt
 * by the target runtime.
 */
#define _GNU_SOURCE

#include "state_action_core.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/cxlfork.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define CAPSULE_BASE ((void *)0x480000000000ULL)
#define CAPSULE_BYTES (8UL * 1024 * 1024)
#define CAPSULE_STACK_BYTES (1024UL * 1024)
#define ACTIVATION_BASE ((void *)0x500000000000ULL)
#define ACTIVATION_BYTES (64UL * 1024)
#define CAPSULE_MAGIC 0x43584c5354415445ULL
#define CAPSULE_VERSION 1U
#define CAPSULE_QUIESCENT 1U
#define CAPSULE_RESUMED 2U
#define MARKER_R12 0xc012c012c012c012ULL
#define MARKER_R13 0xc013c013c013c013ULL
#define MARKER_R14 0xc014c014c014c014ULL
#define MARKER_R15 0xc015c015c015c015ULL
#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

struct capsule_ctx {
	uint64_t r15;
	uint64_t r14;
	uint64_t r13;
	uint64_t r12;
	uint64_t rbx;
	uint64_t rbp;
	uint64_t rsp;
	uint64_t rip;
};

struct capsule_state {
	uint64_t magic;
	uint32_t version;
	uint32_t stage;
	uint64_t key;
	uint64_t source_pid;
	uint64_t source_initializations;
	uint64_t invocation;
	uint64_t state_checksum;
	uint64_t continuation_cookie;
	uint64_t source_stack_pointer;
	uint64_t target_stack_pointer;
	uint64_t target_pid;
	uint64_t workload_ns;
	int32_t target_cpu;
	uint32_t reserved;
	uint64_t marker_r12;
	uint64_t marker_r13;
	uint64_t marker_r14;
	uint64_t marker_r15;
	struct capsule_ctx continuation;
	uint8_t state[STATE_ACTION_BYTES] __attribute__((aligned(4096)));
};

_Static_assert(sizeof(struct capsule_state) <
	       CAPSULE_BYTES - CAPSULE_STACK_BYTES,
	       "capsule state overlaps continuation stack");

static struct capsule_ctx control_ctx;
static struct capsule_ctx activation_ctx;
static struct cxlfork_resume_capsule_v4 resume_result;
static struct cxlfork_region resume_regions[CXLFORK_MAX_REGIONS];
static int restore_device_fd = -1;

/* Identity is deliberately fixed and checked on both prepare and restore. */
static const uint8_t runtime_sha256[CXLFORK_CAPSULE_DIGEST_BYTES] = {
	0x53, 0x54, 0x41, 0x54, 0x45, 0x41, 0x43, 0x54,
	0x49, 0x4f, 0x4e, 0x2d, 0x52, 0x55, 0x4e, 0x31,
	0x53, 0x54, 0x41, 0x54, 0x45, 0x41, 0x43, 0x54,
	0x49, 0x4f, 0x4e, 0x2d, 0x52, 0x55, 0x4e, 0x31,
};
static const uint8_t image_sha256[CXLFORK_CAPSULE_DIGEST_BYTES] = {
	0x53, 0x54, 0x41, 0x54, 0x45, 0x2d, 0x34, 0x4d,
	0x49, 0x42, 0x2d, 0x56, 0x45, 0x52, 0x53, 0x31,
	0x53, 0x54, 0x41, 0x54, 0x45, 0x2d, 0x34, 0x4d,
	0x49, 0x42, 0x2d, 0x56, 0x45, 0x52, 0x53, 0x31,
};

static struct capsule_state *capsule(void)
{
	return CAPSULE_BASE;
}

static uint64_t nsec_now(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC_RAW, &time);
	return (uint64_t)time.tv_sec * 1000000000ULL + time.tv_nsec;
}

static uint64_t fnv_u64(uint64_t hash, uint64_t value)
{
	unsigned int byte;

	for (byte = 0; byte < sizeof(value); byte++) {
		hash ^= value & 0xff;
		hash *= FNV_PRIME;
		value >>= 8;
	}
	return hash;
}

static uint64_t continuation_cookie(const struct capsule_state *state)
{
	uint64_t hash = FNV_OFFSET;

	hash = fnv_u64(hash, state->magic);
	hash = fnv_u64(hash, state->version);
	hash = fnv_u64(hash, state->key);
	hash = fnv_u64(hash, state->source_pid);
	hash = fnv_u64(hash, state->source_initializations);
	hash = fnv_u64(hash, state->state_checksum);
	hash = fnv_u64(hash, state->continuation.rsp);
	hash = fnv_u64(hash, state->continuation.rip);
	return hash;
}

__attribute__((naked, noinline))
static void capsule_swap(struct capsule_ctx *out __attribute__((unused)),
			 const struct capsule_ctx *in __attribute__((unused)))
{
	__asm__ volatile(
		"movq %r15, 0(%rdi)\n\t"
		"movq %r14, 8(%rdi)\n\t"
		"movq %r13, 16(%rdi)\n\t"
		"movq %r12, 24(%rdi)\n\t"
		"movq %rbx, 32(%rdi)\n\t"
		"movq %rbp, 40(%rdi)\n\t"
		"movq %rsp, 48(%rdi)\n\t"
		"leaq 1f(%rip), %rax\n\t"
		"movq %rax, 56(%rdi)\n\t"
		"movq 56(%rsi), %rax\n\t"
		"movq 0(%rsi), %r15\n\t"
		"movq 8(%rsi), %r14\n\t"
		"movq 16(%rsi), %r13\n\t"
		"movq 24(%rsi), %r12\n\t"
		"movq 32(%rsi), %rbx\n\t"
		"movq 40(%rsi), %rbp\n\t"
		"movq 48(%rsi), %rsp\n\t"
		"jmp *%rax\n\t"
		"1:\n\t"
		"ret\n\t");
}

__attribute__((naked, noinline))
static void capsule_yield_with_markers(
	struct capsule_ctx *out __attribute__((unused)),
	const struct capsule_ctx *control __attribute__((unused)))
{
	__asm__ volatile(
		"movabsq $0xc012c012c012c012, %r12\n\t"
		"movabsq $0xc013c013c013c013, %r13\n\t"
		"movabsq $0xc014c014c014c014, %r14\n\t"
		"movabsq $0xc015c015c015c015, %r15\n\t"
		"jmp capsule_swap\n\t");
}

static void read_markers(struct capsule_state *state)
{
	__asm__ volatile(
		"movq %%r12, %0\n\t"
		"movq %%r13, %1\n\t"
		"movq %%r14, %2\n\t"
		"movq %%r15, %3\n\t"
		: "=m"(state->marker_r12), "=m"(state->marker_r13),
		  "=m"(state->marker_r14), "=m"(state->marker_r15)
		:
		: "memory");
}

__attribute__((noreturn))
static void capsule_unexpected_return(void)
{
	dprintf(STDOUT_FILENO,
		"state-action event=unexpected-return verify=fail\n");
	_exit(91);
}

__attribute__((noreturn, noinline))
static void capsule_entry(void)
{
	struct capsule_state *state = capsule();
	uint64_t local_stack_probe;
	uint64_t checksum;
	uint64_t start;

	state->source_initializations++;
	state->source_pid = getpid();
	state_action_initialize(state->state, STATE_ACTION_BYTES,
				STATE_ACTION_SEED);
	state->state_checksum =
		state_action_scan(state->state, STATE_ACTION_BYTES);
	state->stage = CAPSULE_QUIESCENT;
	state->source_stack_pointer = (uintptr_t)&local_stack_probe;
	capsule_yield_with_markers(&state->continuation, &control_ctx);

	read_markers(state);
	if (state->magic != CAPSULE_MAGIC ||
	    state->version != CAPSULE_VERSION ||
	    state->stage != CAPSULE_QUIESCENT ||
	    state->source_initializations != 1 ||
	    state->continuation_cookie != continuation_cookie(state) ||
	    state->marker_r12 != MARKER_R12 ||
	    state->marker_r13 != MARKER_R13 ||
	    state->marker_r14 != MARKER_R14 ||
	    state->marker_r15 != MARKER_R15) {
		dprintf(STDOUT_FILENO,
			"state-action event=continuation-validate verify=fail\n");
		_exit(92);
	}
	state->target_stack_pointer = (uintptr_t)&local_stack_probe;
	state->target_pid = getpid();
	state->target_cpu = sched_getcpu();
	start = nsec_now();
	checksum = state_action_scan(state->state, STATE_ACTION_BYTES);
	state->workload_ns = nsec_now() - start;
	state->invocation = 1;
	state->stage = CAPSULE_RESUMED;
	dprintf(STDOUT_FILENO,
		"state-action event=target-continued source_pid=%" PRIu64
		" target_pid=%" PRIu64 " source_inits=%" PRIu64
		" invocation=%" PRIu64 " source_sp=%016" PRIx64
		" target_sp=%016" PRIx64 " state_bytes=%lu state_pages=%lu"
		" state_checksum=%016" PRIx64 " expected_checksum=%016" PRIx64
		" workload_ns=%" PRIu64 " worker_cpu=%d"
		" workload=state-scan-4m-v1 markers=ok verify=%s\n",
		state->source_pid, state->target_pid,
		state->source_initializations, state->invocation,
		state->source_stack_pointer, state->target_stack_pointer,
		STATE_ACTION_BYTES, STATE_ACTION_PAGES, checksum,
		state->state_checksum, state->workload_ns, state->target_cpu,
		checksum == state->state_checksum ? "ok" : "fail");
	_exit(checksum == state->state_checksum ? 0 : 98);
}

static int open_device(void)
{
	return open(CXLFORK_DEVICE, O_RDWR | O_CLOEXEC);
}

static int map_fixed(void *address, size_t bytes)
{
	void *mapping = mmap(address, bytes, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			     -1, 0);

	if (mapping == MAP_FAILED)
		return -1;
	if (madvise(mapping, bytes, MADV_NOHUGEPAGE)) {
		munmap(mapping, bytes);
		return -1;
	}
	/* The restricted checkpoint requires private anonymous pages.  Merely
	 * mapping the arena leaves untouched holes backed by the shared zero page;
	 * populate every page before the cooperative continuation is sealed. */
	memset(mapping, 0, bytes);
	return 0;
}

static int prove_shared_address(int fd, uint64_t key, uintptr_t address,
				const char *kind)
{
	struct cxlfork_inspect_v2 inspect = {
		.size = sizeof(inspect),
		.abi_version = CXLFORK_ABI_VERSION_V2,
		.key = key,
		.address = address,
	};

	if (ioctl(fd, CXLFORK_INSPECT_V2, &inspect) ||
	    !(inspect.flags & CXLFORK_INSPECT_F_SHARED_LEAF_VALID) ||
	    !(inspect.flags & CXLFORK_INSPECT_F_CURRENT_PMD_VALID) ||
	    !(inspect.flags & CXLFORK_INSPECT_F_CURRENT_PTE_VALID) ||
	    inspect.current_pmd_pfn != inspect.shared_leaf_pfn ||
	    inspect.current_pfn != inspect.shared_pfn)
		return -1;
	dprintf(STDOUT_FILENO,
		"state-action event=shared-leaf-proof kind=%s key=%" PRIu64
		" generation=%" PRIu64 " address=%016" PRIxPTR
		" shared_leaf_pfn=%" PRIu64 " current_pmd_pfn=%" PRIu64
		" shared_pfn=%" PRIu64 " current_pfn=%" PRIu64
		" verify=ok\n",
		kind, key, (uint64_t)inspect.generation, address,
		(uint64_t)inspect.shared_leaf_pfn,
		(uint64_t)inspect.current_pmd_pfn,
		(uint64_t)inspect.shared_pfn, (uint64_t)inspect.current_pfn);
	return 0;
}

__attribute__((noreturn, noinline))
static void capsule_restore_trampoline(void)
{
	struct capsule_state *state = capsule();
	struct cxlfork_call_stats_v4 call_stats = {
		.size = sizeof(call_stats),
		.abi_version = CXLFORK_ABI_VERSION_V4,
	};
	int fd;

	if (state->magic != CAPSULE_MAGIC ||
	    state->version != CAPSULE_VERSION ||
	    state->stage != CAPSULE_QUIESCENT ||
	    state->continuation_cookie != continuation_cookie(state))
		_exit(93);
	if (restore_device_fd >= 0) {
		close(restore_device_fd);
		restore_device_fd = -1;
	}
	fd = open_device();
	if (fd < 0 ||
	    prove_shared_address(fd, state->key, (uintptr_t)CAPSULE_BASE,
				 "metadata") ||
	    prove_shared_address(fd, state->key, (uintptr_t)state->state,
				 "state"))
		_exit(95);
	call_stats.key = state->key;
	call_stats.generation = resume_result.generation;
	call_stats.restore_id = resume_result.restore_id;
	if (ioctl(fd, CXLFORK_GET_CALL_STATS_V4, &call_stats) ||
	    call_stats.stats.total_ns != resume_result.stats.total_ns)
		_exit(97);
	dprintf(STDOUT_FILENO,
		"state-action event=context-after-fd-close key=%" PRIu64
		" restore_id=%" PRIu64 " total_ns=%" PRIu64
		" verify=ok\n",
		state->key, (uint64_t)resume_result.restore_id,
		(uint64_t)call_stats.stats.total_ns);
	close(fd);
	if (resume_result.stats.total_ns !=
	    resume_result.stats.core.total_ns +
	    resume_result.stats.capsule_validate_ns +
	    resume_result.stats.activation_commit_ns)
		_exit(96);
	dprintf(STDOUT_FILENO,
		"state-action event=v4-stats restore_id=%" PRIu64
		" total_ns=%" PRIu64 " core_ns=%" PRIu64
		" capsule_validate_ns=%" PRIu64
		" activation_commit_ns=%" PRIu64 " verify=ok\n",
		(uint64_t)resume_result.restore_id,
		(uint64_t)resume_result.stats.total_ns,
		(uint64_t)resume_result.stats.core.total_ns,
		(uint64_t)resume_result.stats.capsule_validate_ns,
		(uint64_t)resume_result.stats.activation_commit_ns);
	capsule_swap(&activation_ctx, &state->continuation);
	_exit(94);
}

static int prepare_capsule(uint64_t key)
{
	struct cxlfork_region regions[CXLFORK_MAX_REGIONS];
	struct cxlfork_prepare_capsule_v4 op = {
		.size = sizeof(op),
		.abi_version = CXLFORK_ABI_VERSION_V4,
		.regions_ptr = (uintptr_t)regions,
		.region_capacity = CXLFORK_MAX_REGIONS,
	};
	struct capsule_state *state;
	uintptr_t stack_top;
	int fd;

	if (map_fixed(CAPSULE_BASE, CAPSULE_BYTES)) {
		perror("mmap capsule");
		return 1;
	}
	state = capsule();
	state->magic = CAPSULE_MAGIC;
	state->version = CAPSULE_VERSION;
	state->key = key;
	stack_top = (uintptr_t)CAPSULE_BASE + CAPSULE_BYTES - 16;
	stack_top &= ~0xfULL;
	stack_top -= 8;
	*(uintptr_t *)stack_top = (uintptr_t)capsule_unexpected_return;
	state->continuation.rsp = stack_top;
	state->continuation.rip = (uintptr_t)capsule_entry;
	capsule_swap(&control_ctx, &state->continuation);
	if (state->stage != CAPSULE_QUIESCENT ||
	    state->source_initializations != 1)
		return 1;
	state->continuation_cookie = continuation_cookie(state);
	fd = open_device();
	if (fd < 0)
		return 4;
	op.key = key;
	op.scan_start = (uintptr_t)CAPSULE_BASE;
	op.scan_end = (uintptr_t)CAPSULE_BASE + CAPSULE_BYTES;
	op.manifest.manifest_version = CXLFORK_CAPSULE_MANIFEST_VERSION;
	op.manifest.flags = CXLFORK_CAPSULE_F_FIXED_CODE |
		CXLFORK_CAPSULE_F_FIXED_ARENA |
		CXLFORK_CAPSULE_F_COOPERATIVE_YIELD;
	memcpy(op.manifest.runtime_sha256, runtime_sha256,
	       sizeof(runtime_sha256));
	memcpy(op.manifest.image_sha256, image_sha256,
	       sizeof(image_sha256));
	op.manifest.context_va = (uintptr_t)&state->continuation;
	op.manifest.trampoline_ip = (uintptr_t)capsule_restore_trampoline;
	op.manifest.activation_sp =
		(uintptr_t)ACTIVATION_BASE + ACTIVATION_BYTES - 8;
	op.manifest.continuation_cookie = state->continuation_cookie;
	op.manifest.state_checksum = state->state_checksum;
	if (ioctl(fd, CXLFORK_PREPARE_CAPSULE_V4, &op)) {
		perror("PREPARE_CAPSULE_V4");
		close(fd);
		return 1;
	}
	dprintf(STDOUT_FILENO,
		"state-action event=source-prepared key=%" PRIu64
		" source_pid=%" PRIu64 " source_inits=%" PRIu64
		" state_bytes=%lu state_pages=%lu state_checksum=%016" PRIx64
		" generation=%" PRIu64 " pages=%" PRIu64 " verify=ok\n",
		key, state->source_pid, state->source_initializations,
		STATE_ACTION_BYTES, STATE_ACTION_PAGES, state->state_checksum,
		(uint64_t)op.generation, (uint64_t)op.total_pages);
	close(fd);
	return 0;
}

static int restore_capsule(uint64_t key)
{
	struct cxlfork_inspect_capsule_v4 inspect = {
		.size = sizeof(inspect),
		.abi_version = CXLFORK_ABI_VERSION_V4,
		.key = key,
	};
	int fd;

	if (map_fixed(ACTIVATION_BASE, ACTIVATION_BYTES))
		return 1;
	fd = open_device();
	if (fd < 0)
		return 4;
	if (ioctl(fd, CXLFORK_INSPECT_CAPSULE_V4, &inspect)) {
		perror("INSPECT_CAPSULE_V4");
		close(fd);
		return 1;
	}
	memset(&resume_result, 0, sizeof(resume_result));
	resume_result.size = sizeof(resume_result);
	resume_result.abi_version = CXLFORK_ABI_VERSION_V4;
	resume_result.key = key;
	resume_result.generation = inspect.generation;
	resume_result.flags = CXLFORK_F_SHARED_COW;
	resume_result.regions_ptr = (uintptr_t)resume_regions;
	resume_result.region_capacity = CXLFORK_MAX_REGIONS;
	memcpy(resume_result.runtime_sha256, runtime_sha256,
	       sizeof(runtime_sha256));
	memcpy(resume_result.image_sha256, image_sha256,
	       sizeof(image_sha256));
	restore_device_fd = fd;
	if (ioctl(fd, CXLFORK_RESUME_CAPSULE_V4, &resume_result)) {
		restore_device_fd = -1;
		perror("RESUME_CAPSULE_V4");
		close(fd);
		return 1;
	}
	return 1;
}

static void catalog_identity(struct cxlfork_catalog_record_v4 *record)
{
	memset(record, 0, sizeof(*record));
	snprintf((char *)record->owner, sizeof(record->owner), "paper");
	snprintf((char *)record->function, sizeof(record->function),
		 "state-action");
	snprintf((char *)record->version, sizeof(record->version), "v1");
}

static int catalog_register(uint64_t key)
{
	struct cxlfork_inspect_capsule_v4 inspect = {
		.size = sizeof(inspect),
		.abi_version = CXLFORK_ABI_VERSION_V4,
		.key = key,
	};
	struct cxlfork_catalog_register_v4 op = {
		.size = sizeof(op),
		.abi_version = CXLFORK_ABI_VERSION_V4,
	};
	int fd = open_device();

	if (fd < 0 || ioctl(fd, CXLFORK_INSPECT_CAPSULE_V4, &inspect))
		return 1;
	catalog_identity(&op.record);
	memcpy(op.record.runtime_sha256, inspect.manifest.runtime_sha256,
	       sizeof(op.record.runtime_sha256));
	memcpy(op.record.image_sha256, inspect.manifest.image_sha256,
	       sizeof(op.record.image_sha256));
	op.record.key = key;
	op.record.slot_generation = inspect.generation;
	op.record.state_checksum = inspect.manifest.state_checksum;
	op.record.policy = CXLFORK_F_SHARED_COW;
	if (ioctl(fd, CXLFORK_CATALOG_REGISTER_V4, &op)) {
		perror("CATALOG_REGISTER_V4");
		close(fd);
		return 1;
	}
	dprintf(STDOUT_FILENO,
		"state-action event=catalog-register key=%" PRIu64
		" slot_generation=%" PRIu64
		" catalog_generation=%" PRIu64 " verify=ok\n",
		(uint64_t)op.record.key,
		(uint64_t)op.record.slot_generation,
		(uint64_t)op.record.catalog_generation);
	close(fd);
	return 0;
}

static int catalog_lookup(uint64_t expected_key)
{
	struct cxlfork_catalog_lookup_v4 op = {
		.size = sizeof(op),
		.abi_version = CXLFORK_ABI_VERSION_V4,
	};
	int fd = open_device();

	catalog_identity(&op.record);
	if (fd < 0 || ioctl(fd, CXLFORK_CATALOG_LOOKUP_V4, &op) ||
	    op.record.key != expected_key ||
	    !(op.record.flags & CXLFORK_CATALOG_RECORD_F_READY))
		return 1;
	dprintf(STDOUT_FILENO,
		"state-action event=catalog-lookup key=%" PRIu64
		" slot_generation=%" PRIu64
		" catalog_generation=%" PRIu64 " verify=ok\n",
		(uint64_t)op.record.key,
		(uint64_t)op.record.slot_generation,
		(uint64_t)op.record.catalog_generation);
	close(fd);
	return 0;
}

static int catalog_evict(uint64_t expected_key)
{
	struct cxlfork_catalog_lookup_v4 lookup = {
		.size = sizeof(lookup),
		.abi_version = CXLFORK_ABI_VERSION_V4,
	};
	struct cxlfork_catalog_evict_v4 op = {
		.size = sizeof(op),
		.abi_version = CXLFORK_ABI_VERSION_V4,
	};
	int fd = open_device();

	catalog_identity(&lookup.record);
	if (fd < 0 || ioctl(fd, CXLFORK_CATALOG_LOOKUP_V4, &lookup) ||
	    lookup.record.key != expected_key ||
	    !(lookup.record.flags & CXLFORK_CATALOG_RECORD_F_READY)) {
		if (fd >= 0)
			close(fd);
		return 1;
	}
	memcpy(op.owner, lookup.record.owner, sizeof(op.owner));
	memcpy(op.function, lookup.record.function, sizeof(op.function));
	memcpy(op.version, lookup.record.version, sizeof(op.version));
	op.catalog_generation = lookup.record.catalog_generation;
	if (ioctl(fd, CXLFORK_CATALOG_EVICT_V4, &op)) {
		perror("CATALOG_EVICT_V4");
		close(fd);
		return 1;
	}
	dprintf(STDOUT_FILENO,
		"state-action event=catalog-evict key=%" PRIu64
		" catalog_generation=%" PRIu64 " verify=ok\n",
		expected_key, (uint64_t)lookup.record.catalog_generation);
	close(fd);
	return 0;
}

static int release_capsule(uint64_t key)
{
	struct cxlfork_inspect_capsule_v4 inspect = {
		.size = sizeof(inspect),
		.abi_version = CXLFORK_ABI_VERSION_V4,
		.key = key,
	};
	struct cxlfork_release_v2 op = {
		.size = sizeof(op),
		.abi_version = CXLFORK_ABI_VERSION_V2,
		.key = key,
	};
	int fd = open_device();

	if (fd < 0 || ioctl(fd, CXLFORK_INSPECT_CAPSULE_V4, &inspect))
		return 1;
	op.generation = inspect.generation;
	if (ioctl(fd, CXLFORK_RELEASE_V2, &op)) {
		perror("RELEASE_V2");
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

static int parse_key(const char *text, uint64_t *key)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno || !*text || *end || !value)
		return -1;
	*key = value;
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t key;

	if (argc != 3 || parse_key(argv[2], &key)) {
		fprintf(stderr,
			"usage: %s prepare|restore|release|catalog-register|catalog-lookup|catalog-evict KEY\n",
			argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "prepare"))
		return prepare_capsule(key);
	if (!strcmp(argv[1], "restore"))
		return restore_capsule(key);
	if (!strcmp(argv[1], "release"))
		return release_capsule(key);
	if (!strcmp(argv[1], "catalog-register"))
		return catalog_register(key);
	if (!strcmp(argv[1], "catalog-lookup"))
		return catalog_lookup(key);
	if (!strcmp(argv[1], "catalog-evict"))
		return catalog_evict(key);
	return 2;
}
