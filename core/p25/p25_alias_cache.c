// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "config.h"
#if CONFIG_P25_ALIAS_CACHE
#include <string.h>
#include "p25_alias_cache.h"

static bool valid(uint32_t wacn, uint16_t sysid, uint32_t src)
{
	return wacn <= 0xfffffu && sysid <= 0xfffu && src && src <= 0xffffffu;
}

void p25_alias_cache_clear(p25_alias_cache_t *c)
{
	memset(c, 0, sizeof *c);
}

static unsigned find(const p25_alias_cache_t *c, uint32_t network, uint32_t src)
{
	unsigned i;
	for (i = 0; i < c->count; i++)
		if (c->entries[i].network == network && c->entries[i].src == src)
			break;
	return i;
}

void p25_alias_cache_learn(p25_alias_cache_t *c, uint32_t wacn, uint16_t sysid,
			 uint32_t src, const char *name)
{
	if (!valid(wacn, sysid, src) || !name || !name[0])
		return;
	unsigned len = 0;
	while (len < 15 && name[len])
		len++;
	if (len == 15)
		return;
	uint32_t network = (wacn << 12) | sysid;
	unsigned i = find(c, network, src);
	if (i == c->count && c->count < P25_ALIAS_CACHE_MAX)
		c->count++;
	if (i >= P25_ALIAS_CACHE_MAX)
		i = P25_ALIAS_CACHE_MAX - 1;
	p25_alias_cache_entry_t e = { .network = network, .src = src };
	strcpy(e.name, name);
	memmove(c->entries + 1, c->entries, i * sizeof e);
	c->entries[0] = e;
}

bool p25_alias_cache_get(p25_alias_cache_t *c, uint32_t wacn, uint16_t sysid,
			uint32_t src, char name[15])
{
	if (!valid(wacn, sysid, src))
		return false;
	unsigned i = find(c, (wacn << 12) | sysid, src);
	if (i == c->count)
		return false;
	p25_alias_cache_entry_t e = c->entries[i];
	memmove(c->entries + 1, c->entries, i * sizeof e);
	c->entries[0] = e;
	memcpy(name, e.name, sizeof e.name);
	return true;
}
#endif
