// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The PA key run (pa_keyrun.h, docs/spec/pa-keyup.md 7)
#include "radio/pa_keyrun.h"
#include <stddef.h>

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

bool pa_keyrun_parse(const uint8_t *a, uint16_t len, pa_keyrun_req_t *r)
{
	if (len < PA_KEYRUN_HEAD) {
		return false;
	}
	r->key_ms = get16(a);
	r->gap_ms = get16(a + 2);
	r->has_cfg = (a[4] & PA_KEYRUN_FLAG_CFG) != 0u;
	r->reattach = (a[4] & PA_KEYRUN_FLAG_REATTACH) != 0u;
	r->restart = (a[4] & PA_KEYRUN_FLAG_RESTART) != 0u;
	r->n = a[5];
	uint16_t at = PA_KEYRUN_HEAD;
	if (r->has_cfg) {
		if (len < at + PA_KEYRUN_CFG) {
			return false;
		}
		const uint8_t *c = a + at;
		r->cfg = (pa_keyup_cfg_t){
			.dead_pm = get16(c), .knee_us = get16(c + 2), .slope_us = get16(c + 4), .window_us = get16(c + 6),
			.settle_us = get16(c + 8), .pa_us = get16(c + 10), .ease_pm = get16(c + 12),
		};
		at = (uint16_t)(at + PA_KEYRUN_CFG);
	}
	if ((a[4] & (uint8_t)~(PA_KEYRUN_FLAG_CFG | PA_KEYRUN_FLAG_REATTACH | PA_KEYRUN_FLAG_RESTART)) != 0u || r->n == 0u || len != at + 2u * r->n || r->key_ms == 0u ||
	    r->key_ms > PA_KEYRUN_KEY_MS_MAX || r->gap_ms < PA_KEYRUN_GAP_MS_MIN || r->gap_ms > PA_KEYRUN_GAP_MS_MAX) {
		return false;
	}
	r->drives = a + at;
	return true;
}

uint8_t pa_keyrun_run(const pa_keyrun_req_t *r, const pa_keyrun_io_t *io)
{
	if (r->has_cfg && !io->keyup(&r->cfg)) {
		return 0u;
	}
	uint8_t n = 0u;
	while (n < r->n) {
		uint16_t d = get16(r->drives + 2u * n);
		if (d == 0u || !io->drive(d) || !io->ptt(true, r->key_ms + 300u)) {
			break;
		}
		bool on = false;	// a key counts only when the radio transmitted: PTT is refused without an error
		for (uint32_t t = 0u; t < r->key_ms; t += 10u) {
			uint32_t step = r->key_ms - t < 10u ? r->key_ms - t : 10u;
			io->delay_ms(step);
			on = on || io->keyed();
		}
		io->ptt(false, 0u);
		if (!on) {
			break;
		}
		uint32_t waited = 0u;
		while (io->keyed() && waited < PA_KEYRUN_END_MS) {	// the next key's drive only after this key-down
			io->delay_ms(10u);
			waited += 10u;
		}
		n++;
		if (waited >= PA_KEYRUN_END_MS) {
			break;
		}
		io->delay_ms(r->gap_ms);
	}
	io->drive(0u);
	return n;
}
