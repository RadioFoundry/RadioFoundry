// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The randomness pool (support/entropy.h, docs/spec/key-fill.md 6)
#include "config.h"
#if CONFIG_KEY_FILL	// the whole file
#include <string.h>
#include "support/entropy.h"
#include "support/sha256.h"
#include "support/hkdf.h"
#include "hal_time.h"

static uint8_t pool[SHA256_LEN];
static uint32_t count;		// stirs and takes so far: no two hashes of the pool alike
static uint16_t credit;

static void hash_pool(uint8_t label, const void *data, size_t n, uint8_t out[SHA256_LEN])
{
	uint8_t head[9];
	uint32_t us = hal_micros();
	head[0] = label;
	head[1] = (uint8_t)(count >> 24);
	head[2] = (uint8_t)(count >> 16);
	head[3] = (uint8_t)(count >> 8);
	head[4] = (uint8_t)count;
	head[5] = (uint8_t)(us >> 24);
	head[6] = (uint8_t)(us >> 16);
	head[7] = (uint8_t)(us >> 8);
	head[8] = (uint8_t)us;
	count++;
	sha256_t h;
	sha256_init(&h);
	sha256_update(&h, pool, sizeof pool);
	sha256_update(&h, head, sizeof head);
	if (n != 0)
		sha256_update(&h, data, n);
	sha256_final(&h, out);
	crypto_wipe(&h, sizeof h);
}

void entropy_stir(const void *data, size_t n, unsigned bits)
{
	hash_pool('S', data, n, pool);
	credit = (uint16_t)(credit + bits > 256u ? 256u : credit + bits);
}

unsigned entropy_bits(void)
{
	return credit;
}

void entropy_take(uint8_t *out, size_t n)
{
	uint8_t block[SHA256_LEN];
	size_t done = 0;
	while (done < n) {
		hash_pool('O', NULL, 0, block);	// output under its own label: never the pool itself
		size_t take = n - done < sizeof block ? n - done : sizeof block;
		memcpy(out + done, block, take);
		done += take;
	}
	hash_pool('N', NULL, 0, pool);		// the pool moves on: the output can't be found from it later
	crypto_wipe(block, sizeof block);
	credit = 0;
}

#endif	// CONFIG_KEY_FILL
