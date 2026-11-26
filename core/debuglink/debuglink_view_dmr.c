// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR parts of the debug link: the GET_VIEW sections dmr, dmr_call and dmr_stats, the voice path diagnostics
// lines and their counter reset. Kept apart from debuglink_view.c so DMR work can extend it on its own: add keys
// freely (new keys don't bump DL_VIEW_VERSION), keep the existing ones.
#include "config.h"
#include "board.h"
#include <stddef.h>
#include <string.h>
#include "debuglink/debuglink.h"
#if CONFIG_DMR
#include "dmr/call/dmr_call.h"
#include "dmr/ambe/ambeVoice.h"
#include "dsp/miclimit.h"
#include "radio/mic_gain.h"
#endif

#if CONFIG_DMR

// Unsigned snapshot fields share readonly keys; each END_GROUP is one original all-or-drop dl_text call.
// Getters, live call-view fields and custom formatting stay at their existing call sites.
#define END_GROUP 0x80u
#define OFF(type, member) (offsetof(type, member) + 0u * sizeof(char[offsetof(type, member) <= UINT8_MAX ? 1 : -1]))
#define WIDTH(type, member) (sizeof(((type *)0)->member) + 0u * sizeof(char[ \
 (sizeof(((type *)0)->member) == 1 || sizeof(((type *)0)->member) == 2 || sizeof(((type *)0)->member) == 4) ? 1 : -1]))
#define CALL_FIELDS(X) \
	X("src", 3, OFF(radio_dmr_call_t, src), WIDTH(radio_dmr_call_t, src)) \
	X("dst", 3, OFF(radio_dmr_call_t, dst), WIDTH(radio_dmr_call_t, dst) | END_GROUP) \
	X("group", 5, OFF(radio_dmr_call_t, group), WIDTH(radio_dmr_call_t, group)) \
	X("to_us", 5, OFF(radio_dmr_call_t, to_us), WIDTH(radio_dmr_call_t, to_us)) \
	X("encrypted", 9, OFF(radio_dmr_call_t, encrypted), WIDTH(radio_dmr_call_t, encrypted) | END_GROUP) \
	X("emergency", 9, OFF(radio_dmr_call_t, emergency), WIDTH(radio_dmr_call_t, emergency)) \
	X("pi_header", 9, OFF(radio_dmr_call_t, pi_header), WIDTH(radio_dmr_call_t, pi_header) | END_GROUP) \
	X("gps_valid", 9, OFF(radio_dmr_call_t, gps_valid), WIDTH(radio_dmr_call_t, gps_valid) | END_GROUP) \
	X("gps_dist_m", 10, OFF(radio_dmr_call_t, gps_dist_m), WIDTH(radio_dmr_call_t, gps_dist_m)) \
	X("gps_bearing", 11, OFF(radio_dmr_call_t, gps_bearing), WIDTH(radio_dmr_call_t, gps_bearing) | END_GROUP)
#define CASE0_FIELDS(X) \
	X("active", 6, OFF(radio_dmr_view_t, active), WIDTH(radio_dmr_view_t, active)) \
	X("codec", 5, OFF(radio_dmr_view_t, codec), WIDTH(radio_dmr_view_t, codec)) \
	X("killed", 6, OFF(radio_dmr_view_t, killed), WIDTH(radio_dmr_view_t, killed) | END_GROUP) \
	X("tx_mic_muted", 12, OFF(ambe_voice_stats_t, tx_mic_muted), WIDTH(ambe_voice_stats_t, tx_mic_muted) | END_GROUP) \
	X("own_id", 6, OFF(radio_dmr_view_t, own_id), WIDTH(radio_dmr_view_t, own_id)) \
	X("cc", 2, OFF(radio_dmr_view_t, cc), WIDTH(radio_dmr_view_t, cc)) \
	X("ts", 2, OFF(radio_dmr_view_t, ts), WIDTH(radio_dmr_view_t, ts) | END_GROUP) \
	X("rx_cc", 5, OFF(radio_dmr_view_t, rx_cc), WIDTH(radio_dmr_view_t, rx_cc)) \
	X("rx_ts", 5, OFF(radio_dmr_view_t, rx_ts), WIDTH(radio_dmr_view_t, rx_ts) | END_GROUP) \
	X("rmo", 3, OFF(radio_dmr_view_t, rmo), WIDTH(radio_dmr_view_t, rmo)) \
	X("repeater_up", 11, OFF(radio_dmr_view_t, repeater_up), WIDTH(radio_dmr_view_t, repeater_up) | END_GROUP) \
	X("target_id", 9, OFF(radio_dmr_view_t, target_id), WIDTH(radio_dmr_view_t, target_id)) \
	X("target_private", 14, OFF(radio_dmr_view_t, target_private), WIDTH(radio_dmr_view_t, target_private) | END_GROUP) \
	X("reply_active", 12, OFF(radio_dmr_view_t, reply_active), WIDTH(radio_dmr_view_t, reply_active)) \
	X("reply_s", 7, OFF(radio_dmr_view_t, reply_s), WIDTH(radio_dmr_view_t, reply_s) | END_GROUP) \
	X("hear_all", 8, OFF(radio_dmr_view_t, hear_all), WIDTH(radio_dmr_view_t, hear_all)) \
	X("hear_all_latch", 14, OFF(radio_dmr_view_t, hear_all_latch), WIDTH(radio_dmr_view_t, hear_all_latch) | END_GROUP) \
	X("focus_id", 8, OFF(radio_dmr_view_t, focus_id), WIDTH(radio_dmr_view_t, focus_id)) \
	X("focus_private", 13, OFF(radio_dmr_view_t, focus_private), WIDTH(radio_dmr_view_t, focus_private)) \
	X("one_shot", 8, OFF(radio_dmr_view_t, one_shot), WIDTH(radio_dmr_view_t, one_shot) | END_GROUP) \
	X("target_manual", 13, OFF(radio_dmr_view_t, target_manual), WIDTH(radio_dmr_view_t, target_manual)) \
	X("privacy", 7, OFF(radio_dmr_view_t, privacy), WIDTH(radio_dmr_view_t, privacy)) \
	X("alias_tx", 8, OFF(radio_dmr_view_t, alias_tx), WIDTH(radio_dmr_view_t, alias_tx)) \
	X("gps_tx_channel", 14, OFF(radio_dmr_view_t, gps_tx_channel), WIDTH(radio_dmr_view_t, gps_tx_channel) | END_GROUP) \
	X("modem_stalls", 12, OFF(dmr_modem_health_t, stalls), WIDTH(dmr_modem_health_t, stalls)) \
	X("modem_stuck", 11, OFF(dmr_modem_health_t, stuck), WIDTH(dmr_modem_health_t, stuck) | END_GROUP) \
	X("tx_mic_level_db", 15, OFF(ambe_voice_stats_t, tx_mic_level_db), WIDTH(ambe_voice_stats_t, tx_mic_level_db)) \
	X("tx_mic_spread_db", 16, OFF(ambe_voice_stats_t, tx_mic_spread_db), WIDTH(ambe_voice_stats_t, tx_mic_spread_db)) \
	X("tx_mic_zc", 9, OFF(ambe_voice_stats_t, tx_mic_zc), WIDTH(ambe_voice_stats_t, tx_mic_zc)) \
	X("tx_mic_blocks", 13, OFF(ambe_voice_stats_t, tx_mic_blocks), WIDTH(ambe_voice_stats_t, tx_mic_blocks) | END_GROUP)
#define CASE1_FIELDS(X) \
	X("late_entry", 10, OFF(radio_dmr_view_t, late_entry), WIDTH(radio_dmr_view_t, late_entry) | END_GROUP) \
	X("rx_end_term", 11, OFF(dmr_call_stats_t, rx_end_term), WIDTH(dmr_call_stats_t, rx_end_term)) \
	X("rx_end_sync", 11, OFF(dmr_call_stats_t, rx_end_sync), WIDTH(dmr_call_stats_t, rx_end_sync)) \
	X("rx_end_fade", 11, OFF(dmr_call_stats_t, rx_end_fade), WIDTH(dmr_call_stats_t, rx_end_fade) | END_GROUP) \
	X("rx_ts_dropped", 13, OFF(dmr_call_stats_t, rx_ts_dropped), WIDTH(dmr_call_stats_t, rx_ts_dropped)) \
	X("rx_cc_dropped", 13, OFF(dmr_call_stats_t, rx_cc_dropped), WIDTH(dmr_call_stats_t, rx_cc_dropped)) \
	X("rx_lc_taken", 11, OFF(dmr_call_stats_t, rx_lc_taken), WIDTH(dmr_call_stats_t, rx_lc_taken) | END_GROUP) \
	X("rc_good", 7, OFF(dmr_call_stats_t, rc_good), WIDTH(dmr_call_stats_t, rc_good)) \
	X("rc_bad", 6, OFF(dmr_call_stats_t, rc_bad), WIDTH(dmr_call_stats_t, rc_bad)) \
	X("rc_ceased", 9, OFF(dmr_call_stats_t, rc_ceased), WIDTH(dmr_call_stats_t, rc_ceased)) \
	X("rc_last", 7, OFF(dmr_call_stats_t, rc_last), WIDTH(dmr_call_stats_t, rc_last) | END_GROUP)
#define STATS_CALL_FIELDS(X) \
	X("rx_calls", 8, OFF(dmr_call_stats_t, rx_calls), WIDTH(dmr_call_stats_t, rx_calls)) \
	X("rx_calls_played", 15, OFF(dmr_call_stats_t, rx_calls_played), WIDTH(dmr_call_stats_t, rx_calls_played)) \
	X("rx_late_entries", 15, OFF(dmr_call_stats_t, rx_late_entries), WIDTH(dmr_call_stats_t, rx_late_entries) | END_GROUP) \
	X("rx_calls_resumed", 16, OFF(dmr_call_stats_t, rx_calls_resumed), WIDTH(dmr_call_stats_t, rx_calls_resumed) | END_GROUP) \
	X("rx_voice_bursts", 15, OFF(dmr_call_stats_t, rx_voice_bursts), WIDTH(dmr_call_stats_t, rx_voice_bursts)) \
	X("rx_lost_frames", 14, OFF(dmr_call_stats_t, rx_lost_frames), WIDTH(dmr_call_stats_t, rx_lost_frames) | END_GROUP) \
	X("rx_dup_dropped", 14, OFF(dmr_call_stats_t, rx_dup_dropped), WIDTH(dmr_call_stats_t, rx_dup_dropped)) \
	X("rx_enc_dropped", 14, OFF(dmr_call_stats_t, rx_enc_dropped), WIDTH(dmr_call_stats_t, rx_enc_dropped)) \
	X("rx_term_dropped", 15, OFF(dmr_call_stats_t, rx_term_dropped), WIDTH(dmr_call_stats_t, rx_term_dropped) | END_GROUP) \
	X("rx_sfr_dup", 10, OFF(dmr_call_stats_t, rx_sfr_dup), WIDTH(dmr_call_stats_t, rx_sfr_dup)) \
	X("rx_sfr_direct", 13, OFF(dmr_call_stats_t, rx_sfr_direct), WIDTH(dmr_call_stats_t, rx_sfr_direct)) \
	X("rx_sfr_held", 11, OFF(dmr_call_stats_t, rx_sfr_held), WIDTH(dmr_call_stats_t, rx_sfr_held)) \
	X("rx_sfr_fixed", 12, OFF(dmr_call_stats_t, rx_sfr_fixed), WIDTH(dmr_call_stats_t, rx_sfr_fixed) | END_GROUP) \
	X("tx_calls", 8, OFF(dmr_call_stats_t, tx_calls), WIDTH(dmr_call_stats_t, tx_calls)) \
	X("tx_denied", 9, OFF(dmr_call_stats_t, tx_denied), WIDTH(dmr_call_stats_t, tx_denied)) \
	X("tx_underruns", 12, OFF(dmr_call_stats_t, tx_underruns), WIDTH(dmr_call_stats_t, tx_underruns)) \
	X("evq_overflows", 13, OFF(dmr_call_stats_t, evq_overflows), WIDTH(dmr_call_stats_t, evq_overflows) | END_GROUP) \
	X("rx_private_calls", 16, OFF(dmr_call_stats_t, rx_private_calls), WIDTH(dmr_call_stats_t, rx_private_calls)) \
	X("rx_emergency_calls", 18, OFF(dmr_call_stats_t, rx_emergency_calls), WIDTH(dmr_call_stats_t, rx_emergency_calls)) \
	X("rx_encrypted_calls", 18, OFF(dmr_call_stats_t, rx_encrypted_calls), WIDTH(dmr_call_stats_t, rx_encrypted_calls) | END_GROUP) \
	X("rx_pi_headers", 13, OFF(dmr_call_stats_t, rx_pi_headers), WIDTH(dmr_call_stats_t, rx_pi_headers)) \
	X("rx_alias_complete", 17, OFF(dmr_call_stats_t, rx_alias_complete), WIDTH(dmr_call_stats_t, rx_alias_complete)) \
	X("tx_alias_lcs", 12, OFF(dmr_call_stats_t, tx_alias_lcs), WIDTH(dmr_call_stats_t, tx_alias_lcs) | END_GROUP) \
	X("rx_gps", 6, OFF(dmr_call_stats_t, rx_gps), WIDTH(dmr_call_stats_t, rx_gps)) \
	X("tx_gps_lcs", 10, OFF(dmr_call_stats_t, tx_gps_lcs), WIDTH(dmr_call_stats_t, tx_gps_lcs) | END_GROUP) \
	X("rx_in_tx", 8, OFF(dmr_call_stats_t, rx_in_tx), WIDTH(dmr_call_stats_t, rx_in_tx)) \
	X("rx_bs_in_tx", 11, OFF(dmr_call_stats_t, rx_bs_in_tx), WIDTH(dmr_call_stats_t, rx_bs_in_tx)) \
	X("tx_rpt_drops", 12, OFF(dmr_call_stats_t, tx_rpt_drops), WIDTH(dmr_call_stats_t, tx_rpt_drops)) \
	X("tx_wake_lost", 12, OFF(dmr_call_stats_t, tx_wake_lost), WIDTH(dmr_call_stats_t, tx_wake_lost) | END_GROUP)
#define STATS_VOICE_A_FIELDS(X) \
	X("rx_in", 5, OFF(ambe_voice_stats_t, rx_in), WIDTH(ambe_voice_stats_t, rx_in)) \
	X("rx_frames", 9, OFF(ambe_voice_stats_t, rx_frames), WIDTH(ambe_voice_stats_t, rx_frames)) \
	X("rx_concealed", 12, OFF(ambe_voice_stats_t, rx_concealed), WIDTH(ambe_voice_stats_t, rx_concealed)) \
	X("rx_silence", 10, OFF(ambe_voice_stats_t, rx_silence), WIDTH(ambe_voice_stats_t, rx_silence) | END_GROUP) \
	X("rx_calls", 8, OFF(ambe_voice_stats_t, rx_calls), WIDTH(ambe_voice_stats_t, rx_calls)) \
	X("rx_queue_max", 12, OFF(ambe_voice_stats_t, rx_queue_max), WIDTH(ambe_voice_stats_t, rx_queue_max)) \
	X("rx_bad", 6, OFF(ambe_voice_stats_t, rx_bad), WIDTH(ambe_voice_stats_t, rx_bad)) \
	X("rx_lost", 7, OFF(ambe_voice_stats_t, rx_lost), WIDTH(ambe_voice_stats_t, rx_lost) | END_GROUP) \
	X("rx_underruns", 12, OFF(ambe_voice_stats_t, rx_underruns), WIDTH(ambe_voice_stats_t, rx_underruns)) \
	X("rx_underrun_at", 14, OFF(ambe_voice_stats_t, rx_underrun_at), WIDTH(ambe_voice_stats_t, rx_underrun_at)) \
	X("rx_overflows", 12, OFF(ambe_voice_stats_t, rx_overflows), WIDTH(ambe_voice_stats_t, rx_overflows)) \
	X("rx_bit_errors", 13, OFF(ambe_voice_stats_t, rx_bit_errors), WIDTH(ambe_voice_stats_t, rx_bit_errors) | END_GROUP)
#define STATS_VOICE_B_FIELDS(X) \
	X("tx_frames", 9, OFF(ambe_voice_stats_t, tx_frames), WIDTH(ambe_voice_stats_t, tx_frames)) \
	X("tx_overflows", 12, OFF(ambe_voice_stats_t, tx_overflows), WIDTH(ambe_voice_stats_t, tx_overflows)) \
	X("rx_peak", 7, OFF(ambe_voice_stats_t, rx_peak), WIDTH(ambe_voice_stats_t, rx_peak) | END_GROUP) \
	X("rx_repeats", 10, OFF(ambe_voice_stats_t, rx_repeats), WIDTH(ambe_voice_stats_t, rx_repeats)) \
	X("tx_repeats", 10, OFF(ambe_voice_stats_t, tx_repeats), WIDTH(ambe_voice_stats_t, tx_repeats) | END_GROUP)
// Keys contain only a-z, underscore and NUL: five bits per character, with each key byte-aligned.
// Lengths are checked against the readable source literal; packing is constant C initialization, not a second list.
#define KEY_BYTES(n) ((((n) + 1u) * 5u + 7u) / 8u)
#define AT(s, i) (s)[(i) < sizeof(s) ? (i) : sizeof(s) - 1]
#define LETTER(s, i) (AT(s, i) == '_' ? 27u : AT(s, i) ? (unsigned)(AT(s, i) - 'a' + 1) : 0u)
#define BYTE_0(s) ((uint8_t)((LETTER(s, 0) << 3) | (LETTER(s, 1) >> 2)))
#define BYTE_1(s) ((uint8_t)((LETTER(s, 1) << 6) | (LETTER(s, 2) << 1) | (LETTER(s, 3) >> 4)))
#define BYTE_2(s) ((uint8_t)((LETTER(s, 3) << 4) | (LETTER(s, 4) >> 1)))
#define BYTE_3(s) ((uint8_t)((LETTER(s, 4) << 7) | (LETTER(s, 5) << 2) | (LETTER(s, 6) >> 3)))
#define BYTE_4(s) ((uint8_t)((LETTER(s, 6) << 5) | LETTER(s, 7)))
#define BYTE_5(s) ((uint8_t)((LETTER(s, 8) << 3) | (LETTER(s, 9) >> 2)))
#define BYTE_6(s) ((uint8_t)((LETTER(s, 9) << 6) | (LETTER(s, 10) << 1) | (LETTER(s, 11) >> 4)))
#define BYTE_7(s) ((uint8_t)((LETTER(s, 11) << 4) | (LETTER(s, 12) >> 1)))
#define BYTE_8(s) ((uint8_t)((LETTER(s, 12) << 7) | (LETTER(s, 13) << 2) | (LETTER(s, 14) >> 3)))
#define BYTE_9(s) ((uint8_t)((LETTER(s, 14) << 5) | LETTER(s, 15)))
#define BYTE_10(s) ((uint8_t)((LETTER(s, 16) << 3) | (LETTER(s, 17) >> 2)))
#define BYTE_11(s) ((uint8_t)((LETTER(s, 17) << 6) | (LETTER(s, 18) << 1) | (LETTER(s, 19) >> 4)))
#define KEY_1(s) BYTE_0(s), BYTE_1(s)
#define KEY_2(s) BYTE_0(s), BYTE_1(s)
#define KEY_3(s) BYTE_0(s), BYTE_1(s), BYTE_2(s)
#define KEY_4(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s)
#define KEY_5(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s)
#define KEY_6(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s)
#define KEY_7(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s)
#define KEY_8(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s)
#define KEY_9(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s)
#define KEY_10(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s)
#define KEY_11(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s), BYTE_7(s)
#define KEY_12(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s), BYTE_7(s), BYTE_8(s)
#define KEY_13(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s), BYTE_7(s), BYTE_8(s)
#define KEY_14(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s), BYTE_7(s), BYTE_8(s), BYTE_9(s)
#define KEY_15(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s), BYTE_7(s), BYTE_8(s), BYTE_9(s)
#define KEY_16(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s), BYTE_7(s), BYTE_8(s), BYTE_9(s), BYTE_10(s)
#define KEY_17(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s), BYTE_7(s), BYTE_8(s), BYTE_9(s), BYTE_10(s), BYTE_11(s)
#define KEY_18(s) BYTE_0(s), BYTE_1(s), BYTE_2(s), BYTE_3(s), BYTE_4(s), BYTE_5(s), BYTE_6(s), BYTE_7(s), BYTE_8(s), BYTE_9(s), BYTE_10(s), BYTE_11(s)
#define JOIN_(a, b) a ## b
#define JOIN(a, b) JOIN_(a, b)
#define DECLARE(label, n, offset, format) \
	struct { uint8_t off, type, key[KEY_BYTES(n)]; } JOIN(field_, __COUNTER__);
#define VALUE(label, n, offset, format) \
	{ (uint8_t)((offset) + 0u * sizeof(char[sizeof(label) == (n) + 1u ? 1 : -1])), \
	  (uint8_t)(format), { JOIN(KEY_, n)(label) } },
#define BYTES(label, n, offset, format) + 2u + KEY_BYTES(n)
#define ALL_FIELDS(X) CALL_FIELDS(X) CASE0_FIELDS(X) CASE1_FIELDS(X) STATS_CALL_FIELDS(X) STATS_VOICE_A_FIELDS(X) STATS_VOICE_B_FIELDS(X)
// Clang accepts literal subscripts in assertions; the supported GNU compilers fold strspn instead.
#if defined(__clang__)
#define CHAR_OK(s, n, i) ((i) >= (n) || ((AT(s, i) >= 'a' && AT(s, i) <= 'z') || AT(s, i) == '_'))
#define ALPHABET_OK(s, n) (CHAR_OK(s, n, 0) && CHAR_OK(s, n, 1) && CHAR_OK(s, n, 2) && \
	CHAR_OK(s, n, 3) && CHAR_OK(s, n, 4) && CHAR_OK(s, n, 5) && CHAR_OK(s, n, 6) && \
	CHAR_OK(s, n, 7) && CHAR_OK(s, n, 8) && CHAR_OK(s, n, 9) && CHAR_OK(s, n, 10) && \
	CHAR_OK(s, n, 11) && CHAR_OK(s, n, 12) && CHAR_OK(s, n, 13) && CHAR_OK(s, n, 14) && CHAR_OK(s, n, 15) && CHAR_OK(s, n, 16) && CHAR_OK(s, n, 17))
#else
#define ALPHABET_OK(s, n) (__builtin_strspn((s), "abcdefghijklmnopqrstuvwxyz_") == (n))
#endif
#define CHECK_KEY(label, n, offset, format) \
	_Static_assert((n) >= 1 && (n) <= 18 && sizeof(label) == (n) + 1u && ALPHABET_OK(label, n), \
		       "DMR diagnostic key alphabet/length");
ALL_FIELDS(CHECK_KEY)
#undef CHECK_KEY
#undef ALPHABET_OK
#if defined(__clang__)
#undef CHAR_OK
#endif
static const struct { ALL_FIELDS(DECLARE) } dmr_field_data = { ALL_FIELDS(VALUE) };
_Static_assert(sizeof dmr_field_data == 0u ALL_FIELDS(BYTES), "DMR field stream must have no padding");
enum { CALL_BYTES = 0u CALL_FIELDS(BYTES), CASE0_BYTES = 0u CASE0_FIELDS(BYTES),
       CASE1_BYTES = 0u CASE1_FIELDS(BYTES) };
#undef ALL_FIELDS
#undef DECLARE
#undef VALUE
#undef BYTES
#undef CALL_FIELDS
#undef CASE0_FIELDS
#undef CASE1_FIELDS
#undef STATS_CALL_FIELDS
#undef STATS_VOICE_A_FIELDS
#undef STATS_VOICE_B_FIELDS
#undef WIDTH
#undef OFF
// A key has at most eighteen characters; its checked literal length selects the packed initializer above.
static const uint8_t *dmr_key(const uint8_t *p, char *text)
{
	unsigned bits = 0, value = 0, letter;
	do {
		if (bits < 5) {
			value = (value << 8) | *p++;
			bits += 8;
		}
		bits -= 5;
		letter = (value >> bits) & 31u;
		if (text)
			*text++ = letter ? (char)(letter == 27 ? '_' : 'a' + letter - 1) : '\0';
	} while (letter);
	return p;
}

// A NULL text skips an optional snapshot group without reading an uninitialized value.
static void dmr_fields(dl_text_t *t, const uint8_t **at, const void *base, const char *prefix, unsigned groups)
{
	while (groups--) {
		size_t mark = t ? t->len : 0;
		uint8_t type;
		char key[19];
		do {
			uint8_t offset = (*at)[0];
			type = (*at)[1];
			bool write = t && !t->full;
			*at = dmr_key(*at + 2, write ? key : NULL);
			if (write) {
				const uint8_t *value_at = (const uint8_t *)base + offset;
				uint32_t value = 0;
				switch (type & 7u) {
				case 1: value = *value_at; break;
				case 2: { uint16_t n; memcpy(&n, value_at, sizeof n); value = n; break; }
				case 4: memcpy(&value, value_at, sizeof value); break;
				}
				dl_text(t, "%s.%s=%u\n", prefix, key, (unsigned)value);
			}
		} while (!(type & END_GROUP));
		if (t && t->full) dl_text_rollback(t, mark);
	}
}
#undef END_GROUP

static const char *dmr_state_key(uint8_t s)
{
	static const char *const names[] = { "idle", "rx", "rx_other", "tx_wait", "tx", "tx_ending",
					     "tx_data", "tx_permit" };
	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static const char *phase_key(uint8_t p)
{
	static const char *const names[] = { "idle", "rx_voice", "rx_busy", "tx_access", "tx_wakeup", "tx_voice",
					     "tx_ending", "tx_denied", "tx_data", "tx_permit" };
	return p < sizeof(names) / sizeof(names[0]) ? names[p] : "?";
}

static const char *tx_error_key(uint8_t e)
{
	static const char *const names[] = { "ok", "no_modem", "busy", "no_repeater", "rejected", "timeout",
					     "stalled" };
	return e < sizeof(names) / sizeof(names[0]) ? names[e] : "?";
}

static void call(dl_text_t *t, const char *p, const radio_dmr_call_t *c)
{
	char k[24];
	const uint8_t *at = (const uint8_t *)&dmr_field_data;

	dmr_fields(t, &at, c, p, 3);
	dl_fmt(k, sizeof(k), "%s.src_name", p);
	dl_text_kv_str(t, k, c->src_name);
	dl_fmt(k, sizeof(k), "%s.dst_name", p);
	dl_text_kv_str(t, k, c->dst_name);
	// ETSI GPS Info (CONFIG_DMR_GPS): degrees with 7 decimals, the error code, range and bearing from us
	dmr_fields(t, &at, c, p, 1);
	if (c->gps_valid) {
		uint32_t la = (uint32_t)(c->gps_lat < 0 ? -(int64_t)c->gps_lat : c->gps_lat);
		uint32_t lo = (uint32_t)(c->gps_lon < 0 ? -(int64_t)c->gps_lon : c->gps_lon);

		dl_text(t, "%s.gps_lat=%s%u.%07u\n%s.gps_lon=%s%u.%07u\n%s.gps_err=%u\n", p, c->gps_lat < 0 ? "-" : "",
			(unsigned)(la / 10000000u), (unsigned)(la % 10000000u), p, c->gps_lon < 0 ? "-" : "",
			(unsigned)(lo / 10000000u), (unsigned)(lo % 10000000u), p, c->gps_err);
		if (c->gps_range)
			dmr_fields(t, &at, c, p, 1);
	}
}

void debuglink_view_dmr(uint8_t part, const radio_view_t *v, dl_text_t *t)
{
	const radio_dmr_view_t *d = &v->dmr;
	const dmr_call_view_t *cv = dmr_call_view();	// single bytes only (the stack's task writes it)
	const uint8_t *at = (const uint8_t *)&dmr_field_data + CALL_BYTES;

	switch (part) {
	case 0:
		dl_text(t, "dmr.built=1\n");
		dmr_fields(t, &at, d, "dmr", 1);
		// The vocoder's TX state: here, as dmr_stats (the counters) fills most of a reply
		{
			ambe_voice_stats_t a;
			ambeVoiceGetStats(&a);
			dl_text(t, "voice.tx_mic_peak=%u\nvoice.tx_test=%u\n", (unsigned)a.tx_mic_peak,
				ambeVoiceTestSignal() ? 1u : 0u);
			// The mic gain (DMR's) in force now; after it, the encoder's largest sample and the
			// limiter's lowest gain in the latest transmission (- before one). Here, not in features: that section
			// is at a reply's size
			dl_text(t, "voice.mic_pcm_db=%d\nvoice.tx_enc_peak=%u\n", radio_mic_pcm_db(), (unsigned)a.tx_enc_peak);
			// Blocks sent as silence while a beep played (the mic gate)
			dmr_fields(t, &at, &a, "voice", 1);
			if (a.tx_mic_low_q8 != 0) {
				dl_text(t, "voice.tx_mic_low_db=%d\n", miclimit_q8_db((uint16_t)a.tx_mic_low_q8));
			} else {
				dl_text(t, "voice.tx_mic_low_db=-\n");
			}
		}
		// Encode options (VOCODER_OPT_*): in force and supported; the codec's own word when the board reports it
		dl_text(t, "voice.enc_options=0x%X\nvoice.enc_supported=0x%X\n", (unsigned)ambeVoiceEncodeOptions(),
			(unsigned)ambeVoiceEncodeOptionsSupported());
		if (board.vocoder != NULL && board.vocoder->encode_flags != NULL) {
			dl_text(t, "voice.enc_flags=0x%X\n", (unsigned)board.vocoder->encode_flags());
		}
		dl_text(t, "dmr.state=%s\n", dmr_state_key(d->state));
		dl_text(t, "dmr.phase=%s\n", phase_key(cv->phase));
		dl_text(t, "dmr.tx_error=%s\n", tx_error_key(cv->tx_error));
		dmr_fields(t, &at, d, "dmr", 2);
		char alias[DMR_TA_TEXT_MAX + 1];
		dmr_call_tx_alias(alias);
		dl_text(t, "dmr.tx_alias=%s\n", alias);
		dmr_fields(t, &at, d, "dmr", 1);
		// The channel's SFR role (sfrRole_t; sfr.md 4) and whether its repeater role has the modem
		uint8_t sfr_rx, sfr_cwid;
		radio_get_sfr_settings(&sfr_rx, &sfr_cwid);
		dl_text(t, "dmr.sfr_role=%u\ndmr.sfr_rpt=%u\ndmr.sfr_rx=%u\n", d->sfr_role, d->sfr_rpt, sfr_rx);
		dl_text(t, "dmr.sfr_cwid=%u\ndmr.sfr_cw=%u\ndmr.sfr_cwid_sent=%u\ndmr.sfr_cwid_cut=%u\n", sfr_cwid, d->sfr_cw,
			d->sfr_cwid_sent, d->sfr_cwid_cut);
		if (board.radio->dmr_keying != NULL) {	// how the board keyed DMR bursts (hal_radio_t)
			hal_dmr_keying_t k;
			board.radio->dmr_keying(&k);
			dl_text(t, "dmr.keyed_direct=%u\ndmr.keyed_direct_n=%u\ndmr.keyed_idle_n=%u\n", k.last_direct ? 1u : 0u,
				(unsigned)k.direct, (unsigned)k.idle);
		}
		dmr_fields(t, &at, d, "dmr", 1);
		dl_text_kv_str(t, "dmr.target_name", d->target_name);
		dmr_fields(t, &at, d, "dmr", 4);
		dl_text(t, "dmr.underruns=%u\n", (unsigned)d->underruns);
		if (board.dmr != NULL && board.dmr->health != NULL) {
			// Modem interrupts the driver found lost and recovered from; the first one's controller words
			dmr_modem_health_t h;
			board.dmr->health(&h);
			dmr_fields(t, &at, &h, "dmr", 1);
			if (h.first_ms != 0) {
				dl_text(t, "dmr.modem_lost_ms=%u\ndmr.modem_lost_regs=%x,%x,%x,%x\n", (unsigned)h.first_ms,
					(unsigned)h.first_regs[0], (unsigned)h.first_regs[1], (unsigned)h.first_regs[2],
					(unsigned)h.first_regs[3]);
			}
		} else {
			dmr_fields(NULL, &at, NULL, "dmr", 1);
		}
		// The mic content of the latest transmission: here, not in dmr_stats, which is near the reply size
		{
			ambe_voice_stats_t a;
			ambeVoiceGetStats(&a);
			dmr_fields(t, &at, &a, "voice", 1);
		}
		break;
	case 1:
		at += CASE0_BYTES;
		call(t, "call", &d->call);
		dmr_fields(t, &at, d, "call", 1);
		dl_text(t, "call.alias_complete=%u\n", cv->caller.alias_complete);
		dl_text(t, "call.fid=%u\ncall.svc=%u\ncall.pi_alg=%u\ncall.pi_key=%u\n", cv->caller.fid, cv->caller.svc,
			cv->caller.pi_alg, cv->caller.pi_key);
		dl_text_kv_str(t, "call.alias", d->alias);
		dl_text(t, "call.rssi_dbm=%d\ncall.ms=%u\n", d->rssi_dbm, (unsigned)d->call_ms);
		call(t, "last", &d->last);
		{
			// Why received calls ended, and what the filters dropped: a call cut short shows here
			dmr_call_stats_t c;
			dmr_call_get_stats(&c);
			dmr_fields(t, &at, &c, "call", 2);
			// The reverse channel heard while sending: here, as dmr_stats fills a reply
			dmr_fields(t, &at, &c, "call", 1);
		}
		dl_text(t, "last.ms=%u\n", (unsigned)d->last_ms);
		break;
	case 2: {
		at += CASE0_BYTES + CASE1_BYTES;
		dmr_call_stats_t c;
		ambe_voice_stats_t a;

		dmr_call_get_stats(&c);
		ambeVoiceGetStats(&a);
		dmr_fields(t, &at, &c, "call", 4);
		// SFR user: the other copy of a burst played, the talker's copy dropped, dual-path RX's held bursts and fixed frames
		dmr_fields(t, &at, &c, "call", 6);
		dmr_fields(t, &at, &a, "voice", 3);
		dl_text(t, "voice.rx_bad_at=%u,%u,%u\n", (unsigned)a.rx_bad_at[0], (unsigned)a.rx_bad_at[1],
			(unsigned)a.rx_bad_at[2]);
		dmr_fields(t, &at, &a, "voice", 2);
		break;
	}
	default:
		break;
	}
}

bool debuglink_dmr_diag_line(uint8_t i, char *buf, uint8_t len)
{
	dmr_call_stats_t c;
	ambe_voice_stats_t a;

	dmr_call_get_stats(&c);
	ambeVoiceGetStats(&a);
	switch (i) {
	case 0:
		dl_fmt(buf, len, "call rx=%u play=%u late=%u tx=%u deny=%u", (unsigned)c.rx_calls,
		       (unsigned)c.rx_calls_played, (unsigned)c.rx_late_entries, (unsigned)c.tx_calls, (unsigned)c.tx_denied);
		return true;
	case 1:
		dl_fmt(buf, len, "call vb=%u lost=%u dup=%u enc=%u txu=%u evq=%u", (unsigned)c.rx_voice_bursts,
		       (unsigned)c.rx_lost_frames, (unsigned)c.rx_dup_dropped, (unsigned)c.rx_enc_dropped,
		       (unsigned)c.tx_underruns, (unsigned)c.evq_overflows);
		return true;
	case 2:
		dl_fmt(buf, len, "voice in=%u play=%u conc=%u sil=%u und=%u", (unsigned)a.rx_in, (unsigned)a.rx_frames,
		       (unsigned)a.rx_concealed, (unsigned)a.rx_silence, (unsigned)a.rx_underruns);
		return true;
	case 3:
		dl_fmt(buf, len, "voice ovf=%u bad=%u lost=%u bits=%u q=%u calls=%u", (unsigned)a.rx_overflows,
		       (unsigned)a.rx_bad, (unsigned)a.rx_lost, (unsigned)a.rx_bit_errors, (unsigned)a.rx_queue_max,
		       (unsigned)a.rx_calls);
		return true;
	default:
		return false;
	}
}

void debuglink_dmr_reset(void)
{
	dmr_call_reset_stats();
	ambeVoiceResetStats();
}

#else	// built without DMR

void debuglink_view_dmr(uint8_t part, const radio_view_t *v, dl_text_t *t)
{
	(void)part;
	(void)v;
	dl_text(t, "dmr.built=0\n");
}

#endif
