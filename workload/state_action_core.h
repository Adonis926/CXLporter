// SPDX-License-Identifier: MIT
#ifndef CXLPORTER_STATE_ACTION_CORE_H
#define CXLPORTER_STATE_ACTION_CORE_H

#include <stddef.h>
#include <stdint.h>

#define STATE_ACTION_BYTES (4UL * 1024 * 1024)
#define STATE_ACTION_PAGE_BYTES 4096UL
#define STATE_ACTION_PAGES (STATE_ACTION_BYTES / STATE_ACTION_PAGE_BYTES)
#define STATE_ACTION_SEED 0x43584c464f524b31ULL

void state_action_initialize(void *state, size_t bytes, uint64_t seed);
uint64_t state_action_scan(const void *state, size_t bytes);

#endif
