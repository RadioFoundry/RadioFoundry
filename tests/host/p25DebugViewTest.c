// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Every exported byte and the old per-call truncation boundaries, independent of the new field descriptors.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define board board_as_declared
#include "board.h"
#undef board
#include "debuglink/debuglink.h"
#include "radio/radio_p25.h"
#if CONFIG_IMBE
#include "p25/p25Voice.h"
#endif

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if CONFIG_P25
board_t board;
static radio_p25_settings_t settings, running;
static radio_p25_stats_t stats;
static bool have_running, load_ok;
static uint32_t load_work, load_span;
static unsigned settings_calls, running_calls, stats_calls, load_calls, trunk_calls, voice_calls;
void radio_p25_settings(radio_p25_settings_t *out) { settings_calls++; *out = settings; }
bool radio_p25_in_force(radio_p25_settings_t *out) { running_calls++; if (have_running) *out = running; return have_running; }
void radio_p25_stats(radio_p25_stats_t *out) { stats_calls++; *out = stats; }
void radio_p25_reset_stats(void) { }
static bool rx_load(uint32_t *work, uint32_t *span) { load_calls++; *work = load_work; *span = load_span; return load_ok; }
static const hal_air_t air_load = { .rx_load = rx_load };
static const hal_air_t air_no_load = { 0 };
#if CONFIG_P25_TRUNK
static radio_p25_trunk_view_t trunk;
void radio_p25_trunk_view(radio_p25_trunk_view_t *out) { trunk_calls++; *out = trunk; }
#endif
#if CONFIG_IMBE
static p25_voice_stats_t voice;
void p25VoiceGetStats(p25_voice_stats_t *out) { voice_calls++; *out = voice; }
void p25VoiceResetStats(void) { }
#endif
#endif

#include "p25DebugViewGolden.h"

#define CAP 4096u
#define GUARD 16u
static uint8_t actual[CAP + 2 * GUARD], expected[CAP + 2 * GUARD];

static void compare(const radio_view_t *view, bool is_trunk, size_t cap, bool prefix)
{
	memset(actual, 0xA5, sizeof actual); memset(expected, 0xA5, sizeof expected);
	dl_text_t a, e;
	dl_text_init(&a, (char *)actual + GUARD, cap); dl_text_init(&e, (char *)expected + GUARD, cap);
	if (prefix) { dl_text(&a, "prefix=17\n"); dl_text(&e, "prefix=17\n"); }
#if CONFIG_P25
	settings_calls = running_calls = stats_calls = load_calls = trunk_calls = voice_calls = 0;
#endif
	if (is_trunk) debuglink_view_p25_trunk(&a); else debuglink_view_p25(view, &a);
#if CONFIG_P25
	unsigned calls[] = { settings_calls, running_calls, stats_calls, load_calls, trunk_calls, voice_calls };
	settings_calls = running_calls = stats_calls = load_calls = trunk_calls = voice_calls = 0;
#endif
	if (is_trunk) golden_p25_trunk(&e); else golden_p25(view, &e);
	CHECK(a.len == e.len && a.full == e.full);
	CHECK(cap != CAP || (!a.full && !e.full));
	// Include the unused tail: partial formatting of a dropped group must match the old formatter too.
	CHECK(memcmp(actual, expected, sizeof actual) == 0);
#if CONFIG_P25
	unsigned after[] = { settings_calls, running_calls, stats_calls, load_calls, trunk_calls, voice_calls };
	CHECK(memcmp(calls, after, sizeof calls) == 0);
#endif
	for (unsigned i = 0; i < GUARD; i++) {
		CHECK(actual[i] == 0xA5 && actual[GUARD + cap + i] == 0xA5);
	}
}

static void fixture(radio_view_t *v, unsigned variant)
{
	memset(v, 0, sizeof *v);
#if CONFIG_P25
	v->p25 = (radio_p25_view_t){ .flags = variant ? 0xFF : 0, .tx = 5, .nac = 0xFED,
		.src = UINT32_MAX, .dst = 0xFEDCBA98 };
	settings = (radio_p25_settings_t){ .rx_nac = 0x123, .tx_nac = 0x456, .rx_tgid = 0xABCD,
		.talkaround = 2, .unit_call = true, .tx_dst = 0xFEDCBA98, .unit_id = 0xFFFFFFFE };
	running = (radio_p25_settings_t){ .rx_nac = 0x789, .tx_nac = 0xABC, .rx_tgid = 0xFEDC,
		.talkaround = 1, .unit_call = false, .tx_dst = 0x12345678, .unit_id = UINT32_MAX };
	have_running = variant != 0;
	load_ok = variant != 3; load_work = 0xF1234567; load_span = UINT32_MAX;
	board.air = variant == 0 ? NULL : variant == 1 ? &air_no_load : &air_load;
	uint32_t n = 0xF1234500;
#define STAT(f) stats.f = ++n
	STAT(rx_calls); STAT(tx_calls); STAT(tx_filler); STAT(units); STAT(voice); STAT(voice_bad); STAT(voice_bits);
	STAT(lc_fail); STAT(es_fail); STAT(rx_chunks); STAT(tx_underruns); STAT(tx_syms); STAT(begins); STAT(begin_fails);
	STAT(mem_lent); STAT(trunk_retunes); STAT(trunk_untuned); STAT(trunk_skips); STAT(trunk_bursts); STAT(trunk_burst_refused);
#undef STAT
#if CONFIG_IMBE
#define VOICE(f) voice.f = ++n
	VOICE(rx_in); VOICE(rx_frames); VOICE(rx_bad); VOICE(rx_concealed); VOICE(rx_calls); VOICE(rx_underruns);
	VOICE(rx_underrun_at); VOICE(rx_overflows); VOICE(rx_bit_errors); VOICE(rx_queue_max); VOICE(rx_peak); VOICE(tx_frames);
	VOICE(tx_encoded); VOICE(tx_dropped); VOICE(tx_silent); VOICE(tx_mic_peak); VOICE(tx_mic_muted);
#undef VOICE
	voice.tx_mic_low_q8 = variant == 0 ? 64 : variant == 1 ? 256 : variant == 2 ? 4057 : 0x10000 + 256;
#endif
#if CONFIG_P25_TRUNK
	memset(&trunk, 0, sizeof trunk);
	trunk.on = (1u >> variant) & 1u; trunk.tx = (2u >> variant) & 1u;
	trunk.retunes = UINT32_MAX; trunk.untuned = 0xABCDEF12; trunk.skips = 0x12345678;
	trunk.bursts = 0xFEDCBA98; trunk.burst_refused = 0x87654321;
	p25_follow_view_t *f = &trunk.v;
	f->state = 3; f->cc = 255; f->fallback = (3u >> variant) & 1u; f->refused = 3; f->refused_tg = 65535; f->nac = 3966;
	f->wacn = UINT32_MAX; f->sysid = 65535; f->rfss = 254; f->site = 255; f->cc_hz = 123456789; f->voice_hz = 987654321;
	f->unit_call = (4u >> variant) & 1u; f->enc = (5u >> variant) & 1u; f->tg = 54321; f->src = 0xFEDCBA98; f->dst = UINT32_MAX;
	f->idens = 0xABCD; f->learned = 4; f->mbt_op = 0x3B; f->blocks = 0xFEDCBA98; f->crc_fail = UINT32_MAX;
	f->crc_taken = 0xA123; f->unverified = (6u >> variant) & 1u; f->sys_ok = (7u >> variant) & 1u;
	uint16_t m = 40000;
#define COUNT(x) f->x = ++m
	COUNT(mfr); COUNT(mbt); COUNT(pdu_other); COUNT(grants); COUNT(follows); COUNT(enc_skip); COUNT(tdma);
	COUNT(no_iden); COUNT(wrong_sys); COUNT(hunts); COUNT(cc_lost); COUNT(voice_lost); COUNT(wrong_call);
#undef COUNT
	f->reg = 11; f->req = 6; f->deny_svc = 255; f->deny_reason = 254; f->aff_tg = 65534; f->talk_hz = 400123456;
	uint8_t b = 200;
#define COUNT8(x) f->x = ++b
	COUNT8(reg_tx); COUNT8(reg_ok); COUNT8(aff_tx); COUNT8(aff_ok); COUNT8(req_tx); COUNT8(req_grant);
	COUNT8(denials); COUNT8(queued); COUNT8(timeouts); COUNT8(send_fail); COUNT8(inb_busy);
#undef COUNT8
	f->tuning = (8u >> variant) & 1u; f->expected_nac = 0;
#if CONFIG_P25_ROAM
	f->wuid = 0xABCDEF; f->agid = 0xDEFA; f->lra = 0; f->mobility = 4; f->loc_tx = 253; f->loc_ok = 252;
	f->ack_wait = (9u >> variant) & 1u; f->isolated = (10u >> variant) & 1u; f->location_ok = (11u >> variant) & 1u;
#endif
#endif
#else
	(void)variant;
#endif
}

static void rollback(void)
{
	uint8_t buf[32], before[32];
	memset(buf, 0xA5, sizeof buf);
	dl_text_t t;
	dl_text_init(&t, (char *)buf + 8, 8);
	dl_text(&t, "a=1\n");
	CHECK(t.len == 4 && !t.full);
	memcpy(before, buf, sizeof buf);
	dl_text_rollback(&t, 0);
	CHECK(t.len == 0 && !t.full && !memcmp(buf, before, sizeof buf));
	dl_text(&t, "a=1\n");
	dl_text(&t, "b=2\n"); // exact remaining room includes the NUL: the whole group is dropped
	CHECK(t.len == 4 && t.full);
	memcpy(before, buf, sizeof buf);
	dl_text_rollback(&t, 0);
	CHECK(t.len == 0 && t.full && !memcmp(buf, before, sizeof buf));
	dl_text_rollback(&t, 1); dl_text_rollback(&t, SIZE_MAX);
	CHECK(t.len == 0 && t.full && !memcmp(buf, before, sizeof buf));
	dl_text_init(&t, (char *)buf + 8, 0);
	dl_text_rollback(&t, 0); dl_text_rollback(&t, SIZE_MAX);
	CHECK(t.len == 0 && !t.full && !memcmp(buf, before, sizeof buf));
	dl_text(&t, "x"); dl_text_rollback(&t, 0);
	CHECK(t.len == 0 && t.full && !memcmp(buf, before, sizeof buf));
	// Malformed current cursors and marks must not change anything or write past a zero-byte buffer.
	t.len = 10; dl_text_rollback(&t, 0);
	CHECK(t.len == 10 && t.full && !memcmp(buf, before, sizeof buf));
}

int main(void)
{
	rollback();
	radio_view_t view;
	for (unsigned variant = 0; variant < 4; variant++) {
		fixture(&view, variant);
		for (size_t cap = 0; cap <= CAP; cap++) {
			compare(&view, false, cap, (variant & 1) != 0);
			compare(&view, true, cap, (variant & 1) != 0);
		}
	}
#if CONFIG_P25
	for (unsigned flags = 0; flags <= UINT8_MAX; flags++) {
		fixture(&view, 2); view.p25.flags = (uint8_t)flags;
		compare(&view, false, CAP, false);
	}
#endif
	printf("p25DebugViewTest: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
