// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000's CACH short LC, read at task level (hrc7000_dmr.h). A file of its own so the board's linker script,
// which puts the slot engine (hrc7000_dmr.c) in SRAM whole, leaves it in flash
#include <string.h>
#include "hrc7000_dmr.h"

#define RX_SLC	0x2Bu	// RX RAM: the last short LC, 28 bits and the CRC-8, the low 4 bits of 0x2F unused

bool hrc7000_dmr_short_lc(hrc7000_dmr_t *e, dmr_event_t *ev)
{
	uint8_t seq = e->slc_seq;
	if (seq == e->slc_seen) {
		return false;
	}
	uint8_t b[5];
	for (uint8_t i = 0; i < 5; i++) {
		b[i] = e->dev->buf[C7_RX_RAM + RX_SLC + i];
	}
	// A new one assembled while reading: torn, the next call takes it
	if (e->slc_seq != seq) {
		return false;
	}
	e->slc_seen = seq;
	if (memcmp(b, e->slc_last, sizeof b) == 0) {
		return false;
	}
	memcpy(e->slc_last, b, sizeof b);
	e->st.slc_reads++;
	memset(ev, 0, sizeof *ev);
	ev->kind = DMR_EV_SHORT_LC;
	ev->flags = (uint8_t)((b[3] << 4) | (b[4] >> 4));
	ev->arg = ((uint32_t)b[0] << 20) | ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | ((uint32_t)b[3] >> 4);
	return true;
}
