// SPDX-License-Identifier: MIT
#include "state_action_core.h"

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t splitmix64(uint64_t *state)
{
	uint64_t value;

	*state += 0x9e3779b97f4a7c15ULL;
	value = *state;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
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

void state_action_initialize(void *state, size_t bytes, uint64_t seed)
{
	uint64_t *words = state;
	size_t index;

	for (index = 0; index < bytes / sizeof(*words); index++)
		words[index] = splitmix64(&seed);
}

uint64_t state_action_scan(const void *state, size_t bytes)
{
	const unsigned char *data = state;
	uint64_t hash = FNV_OFFSET;
	size_t page;

	for (page = 0; page < bytes / STATE_ACTION_PAGE_BYTES; page++) {
		const uint64_t *word =
			(const uint64_t *)(data + page * STATE_ACTION_PAGE_BYTES);

		hash = fnv_u64(hash, page);
		hash = fnv_u64(hash, *word);
	}
	return hash;
}
