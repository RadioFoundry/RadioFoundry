// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The PA key run (core/radio/pa_keyrun.c, docs/spec/pa-keyup.md 7): the request's limits, and what a run writes and
// when: each key's drive before its PTT, the key-down awaited before the next drive, the override cleared at the end
#include <stdio.h>
#include <string.h>
#include "radio/pa_keyrun.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define LOG_MAX 256
static struct { char what; unsigned v, t; } logv[LOG_MAX];
static int nlog;
static unsigned now, keyed_until, keydown_ms = 30;
static int refuse_drive_after = -1, drives;
static bool stuck;	// the radio never keys down
static bool refuse_tx;	// PTT is taken, the radio never transmits (RX only, TX inhibited)
static pa_keyup_cfg_t got_cfg;

static void rec(char w, unsigned v)
{
	if (nlog < LOG_MAX) {
		logv[nlog].what = w;
		logv[nlog].v = v;
		logv[nlog].t = now;
		nlog++;
	}
}

static bool drive(uint16_t d)
{
	if (d != 0u && refuse_drive_after >= 0 && drives >= refuse_drive_after) {
		return false;
	}
	drives += d != 0u;
	rec('d', d);
	return true;
}

static bool keyup(const pa_keyup_cfg_t *c)
{
	got_cfg = *c;
	rec('c', c->ease_pm);
	return true;
}

static bool ptt(bool down, uint32_t max_ms)
{
	rec(down ? 'P' : 'p', max_ms);
	if (down) {
		keyed_until = refuse_tx ? 0u : 0xFFFFFFFFu;
	} else if (!refuse_tx) {
		keyed_until = stuck ? 0xFFFFFFFFu : now + keydown_ms;
	}
	return true;
}

static bool keyed(void)
{
	return now < keyed_until;
}

static void delay_ms(uint32_t ms)
{
	now += ms;
}

static const pa_keyrun_io_t io = { .drive = drive, .keyup = keyup, .ptt = ptt, .keyed = keyed, .delay_ms = delay_ms };

static void reset(void)
{
	nlog = 0;
	now = keyed_until = 0;
	drives = 0;
	refuse_drive_after = -1;
	stuck = false;
	refuse_tx = false;
}

static uint16_t req(uint8_t *b, uint16_t key, uint16_t gap, bool cfg, const uint16_t *d, uint8_t n)
{
	uint16_t at = 0;
	b[at++] = (uint8_t)key; b[at++] = (uint8_t)(key >> 8);
	b[at++] = (uint8_t)gap; b[at++] = (uint8_t)(gap >> 8);
	b[at++] = cfg ? PA_KEYRUN_FLAG_CFG : 0u;
	b[at++] = n;
	if (cfg) {
		static const uint16_t c[7] = { 563, 96, 1002, 1530, 506, 2000, 304 };
		for (int i = 0; i < 7; i++) {
			b[at++] = (uint8_t)c[i]; b[at++] = (uint8_t)(c[i] >> 8);
		}
	}
	for (int i = 0; i < n; i++) {
		b[at++] = (uint8_t)d[i]; b[at++] = (uint8_t)(d[i] >> 8);
	}
	return at;
}

int main(void)
{
	uint8_t b[64];
	pa_keyrun_req_t r;
	static const uint16_t d3[3] = { 517, 800, 1073 };

	// Limits: the length must match the keys, the times their ranges, no unknown flag
	uint16_t len = req(b, 300, 350, true, d3, 3);
	CHECK(len == 6 + 14 + 6 && pa_keyrun_parse(b, len, &r) && r.n == 3 && r.has_cfg && r.cfg.ease_pm == 304 &&
	      r.cfg.dead_pm == 563 && r.key_ms == 300 && r.gap_ms == 350);
	CHECK(!pa_keyrun_parse(b, (uint16_t)(len - 1), &r) && !pa_keyrun_parse(b, 5, &r));
	len = req(b, 300, 350, false, d3, 3);
	CHECK(len == 12 && pa_keyrun_parse(b, len, &r) && !r.has_cfg);
	b[4] = PA_KEYRUN_FLAG_REATTACH;
	CHECK(pa_keyrun_parse(b, len, &r) && r.reattach && !r.has_cfg);
	b[4] = PA_KEYRUN_FLAG_RESTART;
	CHECK(pa_keyrun_parse(b, len, &r) && r.restart && !r.reattach);
	b[4] = 8u;					// an unknown flag
	CHECK(!pa_keyrun_parse(b, len, &r));
	CHECK(!pa_keyrun_parse(b, req(b, 0, 350, false, d3, 3), &r));
	CHECK(!pa_keyrun_parse(b, req(b, PA_KEYRUN_KEY_MS_MAX + 1, 350, false, d3, 3), &r));
	CHECK(!pa_keyrun_parse(b, req(b, 300, PA_KEYRUN_GAP_MS_MIN - 1, false, d3, 3), &r));
	CHECK(!pa_keyrun_parse(b, req(b, 300, 350, false, d3, 0), &r));
	// The most keys that fit a request with the constants: 21
	uint16_t d21[21];
	for (int i = 0; i < 21; i++) {
		d21[i] = (uint16_t)(500 + i);
	}
	CHECK(req(b, 300, 350, true, d21, 21) == 62 && pa_keyrun_parse(b, 62, &r) && r.n == 21);

	// A run: the constants once, then per key its drive, PTT down with its time-out past the key, up after key_ms,
	// the key-down awaited, the gap; the override cleared at the end
	reset();
	len = req(b, 300, 350, true, d3, 3);
	CHECK(pa_keyrun_parse(b, len, &r) && pa_keyrun_run(&r, &io) == 3);
	CHECK(got_cfg.slope_us == 1002 && logv[0].what == 'c');
	for (int k = 0; k < 3; k++) {
		int i = 1 + 3 * k;
		unsigned t0 = (unsigned)k * (300u + 30u + 350u);
		CHECK(logv[i].what == 'd' && logv[i].v == d3[k] && logv[i].t == t0);
		CHECK(logv[i + 1].what == 'P' && logv[i + 1].v == 600u && logv[i + 1].t == t0);
		CHECK(logv[i + 2].what == 'p' && logv[i + 2].t == t0 + 300u);
	}
	CHECK(nlog == 11 && logv[10].what == 'd' && logv[10].v == 0u);

	// A refused drive stops it; the override is cleared all the same
	reset();
	refuse_drive_after = 1;
	CHECK(pa_keyrun_parse(b, len, &r) && pa_keyrun_run(&r, &io) == 1 && logv[nlog - 1].what == 'd' &&
	      logv[nlog - 1].v == 0u);

	// A radio that doesn't key down: the run stops after PA_KEYRUN_END_MS, the next key never keyed
	reset();
	stuck = true;
	CHECK(pa_keyrun_run(&r, &io) == 1);
	int ptts = 0;
	for (int i = 0; i < nlog; i++) {
		ptts += logv[i].what == 'P';
	}
	CHECK(ptts == 1 && now >= 300u + PA_KEYRUN_END_MS);

	// PTT taken but nothing transmitted: no key counts, the run stops after the first, the override cleared
	reset();
	refuse_tx = true;
	CHECK(pa_keyrun_run(&r, &io) == 0 && logv[nlog - 1].what == 'd' && logv[nlog - 1].v == 0u);

	if (failures == 0) {
		printf("paKeyrunTest: all passed\n");
	}
	return failures != 0;
}
