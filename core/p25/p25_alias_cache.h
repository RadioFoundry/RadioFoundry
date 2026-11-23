// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef P25_ALIAS_CACHE_H
#define P25_ALIAS_CACHE_H
#include <stdbool.h>
#include <stdint.h>

#define P25_ALIAS_CACHE_MAX 64u
typedef struct {
	uint32_t network, src;
	char name[15];
} p25_alias_cache_entry_t;
typedef struct {
	p25_alias_cache_entry_t entries[P25_ALIAS_CACHE_MAX];
	uint8_t count;
} p25_alias_cache_t;

// Radio-task owned, RAM only. Learn only fully validated alias events. Unknown
// system identity is never a wildcard; talkgroup and site are not identity keys.
void p25_alias_cache_clear(p25_alias_cache_t *c);
void p25_alias_cache_learn(p25_alias_cache_t *c, uint32_t wacn, uint16_t sysid,
			 uint32_t src, const char *name);
bool p25_alias_cache_get(p25_alias_cache_t *c, uint32_t wacn, uint16_t sysid,
			uint32_t src, char name[15]);
#endif
