// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The original view is the oracle, including partial bytes and every snapshot/hook call.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define board board_as_declared
#include "board.h"
#undef board
#include "debuglink/debuglink.h"
#if CONFIG_DMR
#include "dmr/call/dmr_call.h"
#include "dmr/ambe/ambeVoice.h"
#endif

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { if (failures++ < 12) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if CONFIG_DMR
board_t board;
static dmr_call_view_t call_view;
static dmr_call_stats_t call_stats;
static ambe_voice_stats_t voice_stats;
static unsigned voice_calls, variant;
static char trace[64];
static unsigned traced;
static void note(char c) { if (traced < sizeof trace) trace[traced++] = c; }
const dmr_call_view_t *dmr_call_view(void) { note('V'); return &call_view; }
void dmr_call_get_stats(dmr_call_stats_t *out) { note('C'); *out = call_stats; }
void ambeVoiceGetStats(ambe_voice_stats_t *out)
{
	note('A'); *out = voice_stats;
	// A second snapshot must remain a second getter, even when the reply filled earlier.
	out->tx_mic_peak += voice_calls; out->tx_mic_level_db += voice_calls++;
}
bool ambeVoiceTestSignal(void) { note('T'); return (variant & 1) != 0; }
int radio_mic_pcm_db(void) { note('M'); return -17; }
uint32_t ambeVoiceEncodeOptions(void) { note('O'); return UINT32_MAX; }
uint32_t ambeVoiceEncodeOptionsSupported(void) { note('S'); return 0xABCDEF98; }
void radio_get_sfr_settings(uint8_t *rx, uint8_t *cwid) { note('R'); *rx = 2; *cwid = 4; }
void dmr_call_tx_alias(char out[DMR_TA_TEXT_MAX + 1]) { note('L'); strcpy(out, "Transmit alias"); }
static uint32_t encode_flags(void) { note('F'); return 0xABCDEF98; }
static void keying(hal_dmr_keying_t *out)
{
	note('K'); *out = (hal_dmr_keying_t){ .last_direct = (variant & 1) != 0, .direct = UINT32_MAX, .idle = 0xFEDCBA98 };
}
static void health(dmr_modem_health_t *out)
{
	note('H'); *out = (dmr_modem_health_t){ .stalls = UINT32_MAX, .stuck = 0xFEDCBA98,
		.first_ms = (variant & 1) ? 0x12345678 : 0, .first_regs = { 0, UINT32_MAX, 0x12345678, 0xFEDCBA98 } };
}
static const hal_radio_t radio_hooks = { .dmr_keying = keying }, radio_no_hooks = { 0 };
static const hal_vocoder_t codec_hooks = { .encode_flags = encode_flags }, codec_no_hooks = { 0 };
static const hal_dmr_modem_t modem_hooks = { .health = health }, modem_no_hooks = { 0 };
void dmr_call_reset_stats(void) { note('c'); }
void ambeVoiceResetStats(void) { note('a'); }
#endif

#include "dmrDebugViewGolden.h"

#define CAP 4096u
#define GUARD 16u
static uint8_t actual[CAP + 2 * GUARD], expected[CAP + 2 * GUARD];

static void compare(const radio_view_t *view, uint8_t part, size_t cap, bool prefix, bool full)
{
	memset(actual, 0xA5, sizeof actual); memset(expected, 0xA5, sizeof expected);
	dl_text_t a, e;
	dl_text_init(&a, (char *)actual + GUARD, cap); dl_text_init(&e, (char *)expected + GUARD, cap);
	if (prefix) { dl_text(&a, "prefix=17\n"); dl_text(&e, "prefix=17\n"); }
	a.full |= full; e.full |= full;
#if CONFIG_DMR
	traced = voice_calls = 0; memset(trace, 0, sizeof trace);
#endif
	debuglink_view_dmr(part, view, &a);
#if CONFIG_DMR
	unsigned before_n = traced, before_voice = voice_calls;
	char before[sizeof trace]; memcpy(before, trace, sizeof trace);
	traced = voice_calls = 0; memset(trace, 0, sizeof trace);
#endif
	golden_dmr(part, view, &e);
	CHECK(a.len == e.len && a.full == e.full);
	CHECK(cap != CAP || full || (!a.full && !e.full));
	CHECK(memcmp(actual, expected, sizeof actual) == 0);
#if CONFIG_DMR
	CHECK(before_n == traced && before_voice == voice_calls && !memcmp(before, trace, sizeof trace));
#endif
	for (unsigned i = 0; i < GUARD; i++) CHECK(actual[i] == 0xA5 && actual[GUARD + cap + i] == 0xA5);
}

static void fixture(radio_view_t *v, unsigned n)
{
	memset(v, 0, sizeof *v);
#if CONFIG_DMR
	variant = n;
	radio_dmr_view_t *d = &v->dmr;
	d->active = (n & 1) != 0; d->codec = (n & 2) != 0; d->killed = (n & 4) != 0;
	d->state = (uint8_t)(n == 15 ? 255 : n); d->own_id = UINT32_MAX;
	d->cc = 15; d->ts = 2; d->rx_cc = 14; d->rx_ts = 1; d->rmo = (n & 8) != 0; d->repeater_up = (n & 1) != 0;
	d->target_id = 0xABCDEF98; d->target_private = (n & 2) != 0;
	strcpy(d->target_name, "Target\nName"); d->reply_active = (n & 4) != 0; d->reply_s = 255;
	d->hear_all = (n & 8) != 0; d->hear_all_latch = (n & 1) != 0;
	d->focus_id = 0xFEDCBA98; d->focus_private = (n & 2) != 0; d->one_shot = (n & 4) != 0;
	d->target_manual = (n & 8) != 0; d->privacy = (n & 1) != 0; d->alias_tx = (n & 2) != 0; d->gps_tx_channel = (n & 4) != 0;
	d->sfr_role = 255; d->sfr_rpt = (n & 8) != 0; d->sfr_cw = (n & 1) != 0;
	d->sfr_cwid_sent = 65535; d->sfr_cwid_cut = 54321; d->underruns = UINT32_MAX;
	d->late_entry = (n & 2) != 0; strcpy(d->alias, "Alias\nwith%content");
	d->rssi_dbm = -123; d->call_ms = 0xFEDCBA98; d->last_ms = UINT32_MAX;
	d->call = (radio_dmr_call_t){ .src = UINT32_MAX, .dst = 0xFEDCBA98, .group = (n & 1) != 0,
		.to_us = (n & 2) != 0, .encrypted = (n & 4) != 0, .emergency = (n & 8) != 0,
		.pi_header = (n & 1) != 0, .gps_valid = (n & 2) != 0, .gps_range = (n & 4) != 0,
		.gps_lat = INT32_MIN, .gps_lon = 1799999999, .gps_err = 255, .gps_dist_m = UINT32_MAX, .gps_bearing = 65535 };
	strcpy(d->call.src_name, "Source\rName"); strcpy(d->call.dst_name, "Destination");
	d->last = d->call; d->last.src = 0xABCDEF98; d->last.dst = 0x12345678;
	d->last.gps_lat = 899999999; d->last.gps_lon = INT32_MIN;
	d->last.gps_valid = (n & 4) != 0; d->last.gps_range = (n & 8) != 0;
	memset(&call_view, 0, sizeof call_view);
	call_view.phase = (uint8_t)(n == 15 ? 255 : n); call_view.tx_error = (uint8_t)n;
	call_view.caller.alias_complete = (n & 1) != 0; call_view.caller.fid = 255;
	call_view.caller.svc = 254; call_view.caller.pi_alg = 253; call_view.caller.pi_key = 252;
	uint32_t count = 0xF1234500;
#define COUNT(f) call_stats.f = ++count
	COUNT(rx_calls); COUNT(rx_calls_resumed); COUNT(rx_calls_played); COUNT(rx_late_entries);
	COUNT(rx_voice_bursts); COUNT(rx_lost_frames); COUNT(rx_dup_dropped); COUNT(rx_enc_dropped);
	COUNT(tx_calls); COUNT(tx_denied); COUNT(tx_underruns); COUNT(rx_in_tx); COUNT(rx_bs_in_tx);
	COUNT(tx_rpt_drops); COUNT(tx_wake_lost); COUNT(evq_overflows); COUNT(rx_cc_dropped); COUNT(rx_lc_taken);
	COUNT(rx_ts_dropped); COUNT(rx_term_dropped); COUNT(rx_private_calls); COUNT(rx_emergency_calls);
	COUNT(rx_encrypted_calls); COUNT(rx_pi_headers); COUNT(tx_pi_headers); COUNT(rx_alias_complete);
	COUNT(tx_alias_lcs); COUNT(rx_gps); COUNT(tx_gps_lcs); COUNT(rx_sfr_dup); COUNT(rx_sfr_direct);
	COUNT(rx_sfr_held); COUNT(rx_sfr_fixed); COUNT(rx_end_term); COUNT(rx_end_sync); COUNT(rx_end_fade);
	COUNT(rc_good); COUNT(rc_bad); COUNT(rc_ceased);
#undef COUNT
	call_stats.rc_last = 255;
#define COUNT(f) voice_stats.f = ++count
	COUNT(rx_in); COUNT(rx_frames); COUNT(rx_concealed); COUNT(rx_silence); COUNT(rx_calls); COUNT(rx_queue_max);
	COUNT(rx_bad); COUNT(rx_lost); COUNT(rx_underruns); COUNT(rx_underrun_at); COUNT(rx_overflows);
	COUNT(rx_bit_errors); COUNT(rx_peak); COUNT(rx_repeats); COUNT(tx_repeats); COUNT(tx_mic_peak);
	COUNT(tx_mic_low_q8); COUNT(tx_enc_peak); COUNT(tx_frames); COUNT(tx_overflows);
	COUNT(tx_mic_level_db); COUNT(tx_mic_spread_db); COUNT(tx_mic_zc); COUNT(tx_mic_blocks); COUNT(tx_mic_muted);
#undef COUNT
	voice_stats.rx_bad_at[0] = UINT32_MAX; voice_stats.rx_bad_at[1] = 0xFEDCBA98; voice_stats.rx_bad_at[2] = 0x12345678;
	voice_stats.tx_mic_low_q8 = n % 4 == 0 ? 0 : n % 4 == 1 ? 64 : n % 4 == 2 ? 256 : 0x10000 + 4057;
	if (n == 0) { memset(&call_stats, 0, sizeof call_stats); memset(&voice_stats, 0, sizeof voice_stats); }
	board.radio = n % 3 == 0 ? &radio_no_hooks : &radio_hooks;
	board.vocoder = n % 3 == 0 ? NULL : n % 3 == 1 ? &codec_no_hooks : &codec_hooks;
	board.dmr = n % 3 == 0 ? NULL : n % 3 == 1 ? &modem_no_hooks : &modem_hooks;
#else
	(void)n;
#endif
}

int main(void)
{
	radio_view_t view;
	for (unsigned n = 0; n < 16; n++) {
		fixture(&view, n);
		for (size_t cap = 0; cap <= CAP; cap++)
			for (uint8_t part = 0; part < 3; part++) {
				compare(&view, part, cap, (n & 1) != 0, false);
				if (n >= 14) compare(&view, part, cap, (n & 1) != 0, true);
			}
		compare(&view, 3, CAP, false, false); compare(&view, 255, CAP, true, false);
	}
#if CONFIG_DMR
	for (unsigned n = 0; n < 256; n++) {
		fixture(&view, n); view.dmr.state = (uint8_t)n; call_view.phase = (uint8_t)n; call_view.tx_error = (uint8_t)n;
		compare(&view, 0, CAP, false, false);
	}
	for (unsigned i = 0; i < 6; i++) for (unsigned cap = 0; cap <= 256; cap++) {
		char a[260], e[260]; memset(a, 0xA5, sizeof a); memset(e, 0xA5, sizeof e);
		traced = voice_calls = 0; bool aa = debuglink_dmr_diag_line((uint8_t)i, a + 2, (uint8_t)(cap > 255 ? 255 : cap));
		char before[sizeof trace]; memcpy(before, trace, sizeof trace); unsigned before_n = traced;
		traced = voice_calls = 0; bool ee = golden_dmr_diag_line((uint8_t)i, e + 2, (uint8_t)(cap > 255 ? 255 : cap));
		CHECK(aa == ee && !memcmp(a, e, sizeof a)); CHECK(before_n == traced && !memcmp(before, trace, before_n));
	}
	traced = 0; debuglink_dmr_reset(); CHECK(traced == 2 && trace[0] == 'c' && trace[1] == 'a');
	traced = 0; golden_dmr_reset(); CHECK(traced == 2 && trace[0] == 'c' && trace[1] == 'a');
#endif
	printf("dmrDebugViewTest: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
