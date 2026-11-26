// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CRC admission and frame boundaries with actual P25 and air-framer owners linked.
#include <stdio.h>
#include <string.h>
#include "p25/p25.h"
#include "p25/p25_tsbk.h"
#include "air/air_syms.h"

static unsigned checks, failures, frames, good, bad;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void unitReceived(const p25_rx_unit_t *u, void *ctx)
{
	(void)ctx;
	frames++;
	CHECK(u->duid == P25_DUID_TSDU);
	for (unsigned k = 0; k < 3 && p25_block_end_sym(k) <= u->nsyms; k++) {
		uint8_t block[12];
		if (p25_block_get(u->air, k, block) >= 0) good++; else bad++;
	}
}

static void feed(air_framer_t *fr, const uint8_t *unit, unsigned nsyms, unsigned step)
{
	for (unsigned at = 0; at < nsyms;) {
		uint8_t hard[AIR_CHUNK_SYMS_MAX / 4] = { 0 };
		unsigned n = nsyms - at < step ? nsyms - at : step;
		for (unsigned i = 0; i < n; i++) air_sym_put(hard, i, air_sym_get(unit, at + i));
		air_chunk_t chunk = { .hard = hard, .nsyms = n, .sync_at = -1 };
		air_framer_rx(fr, &chunk);
		at += n;
	}
}

int main(void)
{
	static const unsigned chunks[] = { 1, 7, 73, 144 };
	for (unsigned bad_at = 0; bad_at < 3; bad_at++) {
		for (unsigned size = 0; size < sizeof chunks / sizeof chunks[0]; size++) {
			unsigned step = chunks[size];
			uint8_t air[P25_UNIT_BYTES_MAX], buffer[P25_UNIT_BYTES_MAX], block[12];
			p25_rx_t rx;
			air_framer_t fr;
			frames = good = bad = 0;
			p25_unit_begin(air, p25_blocks_syms(3), 0x293, P25_DUID_TSDU, P25_SS_UNKNOWN);
			for (unsigned k = 0; k < 3; k++) {
				memset(block, 0, sizeof block);
				block[0] = k == 2 ? 0x80 : 0;
				block[2] = k;
				p25_tsbk_seal(block);
				if (k == bad_at) block[0] ^= 0x80; // valid trellis but corrupt CRC and LB
				p25_block_put(air, k, block);
			}
			p25_rx_init(&rx, 0x293, unitReceived, NULL, NULL); rx.trunk = true;
			air_framer_init(&fr, &p25_air_mode, &rx, buffer, NULL, P25_UNIT_SYMS_MAX);
			feed(&fr, air, p25_blocks_syms(3), step);
			CHECK(frames == 1 && good == 2 && bad == 1);
			// The following independently framed unit must also arrive.
			p25_unit_begin(air, p25_blocks_syms(1), 0x293, P25_DUID_TSDU, P25_SS_UNKNOWN);
			memset(block, 0, sizeof block); block[0] = 0x80; p25_tsbk_seal(block); p25_block_put(air, 0, block);
			feed(&fr, air, p25_blocks_syms(1), step);
			CHECK(frames == 2 && good == 3 && bad == 1);
		}
	}
	printf("p25RxRegressionTest: %u checks, %u failed\n", checks, failures);
	return failures ? 1 : 0;
}
