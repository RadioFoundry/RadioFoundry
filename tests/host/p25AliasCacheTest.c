// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "p25/p25_alias_cache.h"

int main(void)
{
	p25_alias_cache_t c;
	char name[15] = "";
	p25_alias_cache_clear(&c);
	assert(sizeof c == 1540);
	assert(!p25_alias_cache_get(&c, 0xabc00, 0x123, 7654321, name));
	p25_alias_cache_learn(&c, 0xabc00, 0x123, 7654321, "UNIT 001");
	assert(p25_alias_cache_get(&c, 0xabc00, 0x123, 7654321, name));
	assert(!strcmp(name, "UNIT 001"));
	// Same numeric ID on another network is a different identity.
	assert(!p25_alias_cache_get(&c, 0xabc01, 0x123, 7654321, name));
	assert(!p25_alias_cache_get(&c, 0xabc00, 0x124, 7654321, name));
	p25_alias_cache_learn(&c, 0xabc01, 0x123, 7654321, "OTHER");
	assert(p25_alias_cache_get(&c, 0xabc00, 0x123, 7654321, name));
	assert(!strcmp(name, "UNIT 001"));
	p25_alias_cache_learn(&c, 0xabc00, 0x123, 7654321, "NEW NAME");
	assert(c.count == 2);
	assert(p25_alias_cache_get(&c, 0xabc00, 0x123, 7654321, name));
	assert(!strcmp(name, "NEW NAME"));
	// Unknown identity, invalid IDs, and incomplete/empty text cannot poison it.
	p25_alias_cache_learn(&c, UINT32_MAX, 0x123, 1, "BAD");
	p25_alias_cache_learn(&c, 0xabc00, UINT16_MAX, 1, "BAD");
	p25_alias_cache_learn(&c, 0xabc00, 0x123, 0, "BAD");
	p25_alias_cache_learn(&c, 0xabc00, 0x123, 0x1000000, "BAD");
	p25_alias_cache_learn(&c, 0xabc00, 0x123, 7654321, "");
	char bad[15]; memset(bad, 'X', sizeof bad);
	p25_alias_cache_learn(&c, 0xabc00, 0x123, 7654321, bad);
	assert(c.count == 2);
	assert(p25_alias_cache_get(&c, 0xabc00, 0x123, 7654321, name));
	assert(!strcmp(name, "NEW NAME"));
	p25_alias_cache_clear(&c);
	for (unsigned i = 1; i <= 64; i++)
		p25_alias_cache_learn(&c, 0xabc00, 0x123, i, "UNIT");
	assert(c.count == 64);
	assert(p25_alias_cache_get(&c, 0xabc00, 0x123, 1, name));
	p25_alias_cache_learn(&c, 0xabc00, 0x123, 65, "12345678901234");
	assert(!p25_alias_cache_get(&c, 0xabc00, 0x123, 2, name));
	assert(p25_alias_cache_get(&c, 0xabc00, 0x123, 1, name));
	assert(p25_alias_cache_get(&c, 0xabc00, 0x123, 65, name));
	assert(!strcmp(name, "12345678901234"));
	// Boot clears every learned identity. No serialization or flash API exists.
	p25_alias_cache_clear(&c);
	assert(!c.count && !p25_alias_cache_get(&c, 0xabc00, 0x123, 65, name));
	puts("p25AliasCacheTest: identity isolation, replacement, eviction, boot clear passed (1540 RAM bytes)");
	return 0;
}
