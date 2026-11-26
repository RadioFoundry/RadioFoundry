// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The p25 section: P25 conventional on the channel (docs/spec/p25.md 7, core/radio/radio_p25.h): the call heard, the
// settings in force, the engine's and the voice path's counters. "p25.built=0" without CONFIG_P25
#include "config.h"
#include <stddef.h>
#include <string.h>
#include "debuglink/debuglink.h"
#if CONFIG_P25
#include "board.h"
#include "radio/radio_p25.h"
#if CONFIG_IMBE
#include "p25/p25Voice.h"
#include "dsp/miclimit.h"
#endif
#endif

#if CONFIG_P25
// One readonly field/key list replaces repeated format strings. Each END_GROUP is one original dl_text call;
// rollback stays in the formatter owner, preserving all-or-drop groups even with a byte-sized reply buffer.
typedef struct { uint32_t work, span; } p25_load_t;
#if CONFIG_P25_ZONE_SCAN
typedef struct { uint8_t active, count; } p25_zone_scan_t;
#define SCAN_CHANNEL_FIELDS(X) \
	X("scan_channel", 12, OFF(radio_p25_view_t, scan_channel), WIDTH(radio_p25_view_t, scan_channel) | END_GROUP)
#define ZONE_FIELDS(X) \
	X("zone_scan", 9, OFF(p25_zone_scan_t, active), WIDTH(p25_zone_scan_t, active)) \
	X("scan_groups", 11, OFF(p25_zone_scan_t, count), WIDTH(p25_zone_scan_t, count) | END_GROUP)
#else
#define SCAN_CHANNEL_FIELDS(X)
#define ZONE_FIELDS(X)
#endif
#define END_GROUP 0x80u
#define MASK(mask) ((__builtin_ctz(mask) + 1u) << 3)
#define OFF(type, member) offsetof(type, member)
#define WIDTH(type, member) (sizeof(((type *)0)->member) + 0u * sizeof(char[ \
	(sizeof(((type *)0)->member) == 1 || sizeof(((type *)0)->member) == 2 || \
	 sizeof(((type *)0)->member) == 4) ? 1 : -1]))
_Static_assert(sizeof(radio_p25_view_t) <= 256 && sizeof(radio_p25_settings_t) <= 256 &&
	       sizeof(radio_p25_stats_t) <= 256, "P25 view field offsets need one byte");
#define CONVENTIONAL_FIELDS(X) \
	X("built", 5, 1, 0) \
	X("on", 2, OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_ON)) \
	X("rx", 2, OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_RX)) \
	X("played", 6, OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_PLAYED)) \
	X("enc", 3, OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_ENC)) \
	X("unit", 4, OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_UNIT)) \
	X("late", 4, OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_LATE) | END_GROUP) \
	X("nac", 3, OFF(radio_p25_view_t, nac), WIDTH(radio_p25_view_t, nac)) \
	X("src", 3, OFF(radio_p25_view_t, src), WIDTH(radio_p25_view_t, src)) \
	X("dst", 3, OFF(radio_p25_view_t, dst), WIDTH(radio_p25_view_t, dst)) \
	X("tx", 2, OFF(radio_p25_view_t, tx), WIDTH(radio_p25_view_t, tx) | END_GROUP) \
	X("rx_work_us", 10, OFF(p25_load_t, work), WIDTH(p25_load_t, work)) \
	X("rx_span_us", 10, OFF(p25_load_t, span), WIDTH(p25_load_t, span) | END_GROUP) \
	X("rx_nac", 6, OFF(radio_p25_settings_t, rx_nac), WIDTH(radio_p25_settings_t, rx_nac)) \
	X("tx_nac", 6, OFF(radio_p25_settings_t, tx_nac), WIDTH(radio_p25_settings_t, tx_nac)) \
	X("rx_tgid", 7, OFF(radio_p25_settings_t, rx_tgid), WIDTH(radio_p25_settings_t, rx_tgid)) \
	X("talkaround", 10, OFF(radio_p25_settings_t, talkaround), WIDTH(radio_p25_settings_t, talkaround)) \
	X("unit_call", 9, OFF(radio_p25_settings_t, unit_call), WIDTH(radio_p25_settings_t, unit_call) | END_GROUP) \
	X("tx_dst", 6, OFF(radio_p25_settings_t, tx_dst), WIDTH(radio_p25_settings_t, tx_dst)) \
	X("unit_id", 7, OFF(radio_p25_settings_t, unit_id), WIDTH(radio_p25_settings_t, unit_id) | END_GROUP) \
	X("rx_nac", 6, OFF(radio_p25_settings_t, rx_nac), WIDTH(radio_p25_settings_t, rx_nac)) \
	X("tx_nac", 6, OFF(radio_p25_settings_t, tx_nac), WIDTH(radio_p25_settings_t, tx_nac)) \
	X("rx_tgid", 7, OFF(radio_p25_settings_t, rx_tgid), WIDTH(radio_p25_settings_t, rx_tgid)) \
	X("talkaround", 10, OFF(radio_p25_settings_t, talkaround), WIDTH(radio_p25_settings_t, talkaround) | END_GROUP) \
	X("unit_call", 9, OFF(radio_p25_settings_t, unit_call), WIDTH(radio_p25_settings_t, unit_call)) \
	X("tx_dst", 6, OFF(radio_p25_settings_t, tx_dst), WIDTH(radio_p25_settings_t, tx_dst)) \
	X("unit_id", 7, OFF(radio_p25_settings_t, unit_id), WIDTH(radio_p25_settings_t, unit_id) | END_GROUP) \
	X("air_scans", 9, OFF(radio_air_scan_t, scans), WIDTH(radio_air_scan_t, scans)) \
	X("air_scan_last_us", 16, OFF(radio_air_scan_t, last_us), WIDTH(radio_air_scan_t, last_us)) \
	X("air_scan_max_us", 15, OFF(radio_air_scan_t, max_us), WIDTH(radio_air_scan_t, max_us) | END_GROUP) \
	X("begins", 6, OFF(radio_p25_stats_t, begins), WIDTH(radio_p25_stats_t, begins)) \
	X("begin_fails", 11, OFF(radio_p25_stats_t, begin_fails), WIDTH(radio_p25_stats_t, begin_fails)) \
	X("mem_lent", 8, OFF(radio_p25_stats_t, mem_lent), WIDTH(radio_p25_stats_t, mem_lent)) \
	X("rx_calls", 8, OFF(radio_p25_stats_t, rx_calls), WIDTH(radio_p25_stats_t, rx_calls)) \
	X("tx_calls", 8, OFF(radio_p25_stats_t, tx_calls), WIDTH(radio_p25_stats_t, tx_calls) | END_GROUP) \
	X("tx_filler", 9, OFF(radio_p25_stats_t, tx_filler), WIDTH(radio_p25_stats_t, tx_filler)) \
	X("units", 5, OFF(radio_p25_stats_t, units), WIDTH(radio_p25_stats_t, units)) \
	X("voice", 5, OFF(radio_p25_stats_t, voice), WIDTH(radio_p25_stats_t, voice)) \
	X("voice_bad", 9, OFF(radio_p25_stats_t, voice_bad), WIDTH(radio_p25_stats_t, voice_bad)) \
	X("voice_bits", 10, OFF(radio_p25_stats_t, voice_bits), WIDTH(radio_p25_stats_t, voice_bits) | END_GROUP) \
	X("lc_fail", 7, OFF(radio_p25_stats_t, lc_fail), WIDTH(radio_p25_stats_t, lc_fail)) \
	X("es_fail", 7, OFF(radio_p25_stats_t, es_fail), WIDTH(radio_p25_stats_t, es_fail)) \
	X("rx_chunks", 9, OFF(radio_p25_stats_t, rx_chunks), WIDTH(radio_p25_stats_t, rx_chunks)) \
	X("tx_underruns", 12, OFF(radio_p25_stats_t, tx_underruns), WIDTH(radio_p25_stats_t, tx_underruns)) \
	X("tx_syms", 7, OFF(radio_p25_stats_t, tx_syms), WIDTH(radio_p25_stats_t, tx_syms) | END_GROUP)
#if CONFIG_IMBE
_Static_assert(sizeof(p25_voice_stats_t) <= 256, "P25 voice field offsets need one byte");
#define VOICE_FIELDS(X) \
	X("built", 5, 1, 0) \
	X("in", 2, OFF(p25_voice_stats_t, rx_in), WIDTH(p25_voice_stats_t, rx_in)) \
	X("frames", 6, OFF(p25_voice_stats_t, rx_frames), WIDTH(p25_voice_stats_t, rx_frames)) \
	X("bad", 3, OFF(p25_voice_stats_t, rx_bad), WIDTH(p25_voice_stats_t, rx_bad)) \
	X("concealed", 9, OFF(p25_voice_stats_t, rx_concealed), WIDTH(p25_voice_stats_t, rx_concealed)) \
	X("calls", 5, OFF(p25_voice_stats_t, rx_calls), WIDTH(p25_voice_stats_t, rx_calls) | END_GROUP) \
	X("underruns", 9, OFF(p25_voice_stats_t, rx_underruns), WIDTH(p25_voice_stats_t, rx_underruns)) \
	X("underrun_at", 11, OFF(p25_voice_stats_t, rx_underrun_at), WIDTH(p25_voice_stats_t, rx_underrun_at)) \
	X("overflows", 9, OFF(p25_voice_stats_t, rx_overflows), WIDTH(p25_voice_stats_t, rx_overflows)) \
	X("bit_errors", 10, OFF(p25_voice_stats_t, rx_bit_errors), WIDTH(p25_voice_stats_t, rx_bit_errors) | END_GROUP) \
	X("queue_max", 9, OFF(p25_voice_stats_t, rx_queue_max), WIDTH(p25_voice_stats_t, rx_queue_max)) \
	X("peak", 4, OFF(p25_voice_stats_t, rx_peak), WIDTH(p25_voice_stats_t, rx_peak)) \
	X("tx_frames", 9, OFF(p25_voice_stats_t, tx_frames), WIDTH(p25_voice_stats_t, tx_frames) | END_GROUP) \
	X("tx_encoded", 10, OFF(p25_voice_stats_t, tx_encoded), WIDTH(p25_voice_stats_t, tx_encoded)) \
	X("tx_dropped", 10, OFF(p25_voice_stats_t, tx_dropped), WIDTH(p25_voice_stats_t, tx_dropped)) \
	X("tx_silent", 9, OFF(p25_voice_stats_t, tx_silent), WIDTH(p25_voice_stats_t, tx_silent)) \
	X("tx_mic_peak", 11, OFF(p25_voice_stats_t, tx_mic_peak), WIDTH(p25_voice_stats_t, tx_mic_peak)) \
	X("tx_mic_muted", 12, OFF(p25_voice_stats_t, tx_mic_muted), WIDTH(p25_voice_stats_t, tx_mic_muted) | END_GROUP)
#else
#define VOICE_FIELDS(X)
#endif
#if CONFIG_P25_TRUNK
_Static_assert(sizeof(radio_p25_trunk_view_t) <= 256, "P25 trunk field offsets need one byte");
#define TRUNK_FIELDS(X) \
	X("built", 5, 1, 0) \
	X("on", 2, OFF(radio_p25_trunk_view_t, on), WIDTH(radio_p25_trunk_view_t, on)) \
	X("state", 5, OFF(radio_p25_trunk_view_t, v.state), WIDTH(radio_p25_trunk_view_t, v.state)) \
	X("cc", 2, OFF(radio_p25_trunk_view_t, v.cc), WIDTH(radio_p25_trunk_view_t, v.cc)) \
	X("cc_hz", 5, OFF(radio_p25_trunk_view_t, v.cc_hz), WIDTH(radio_p25_trunk_view_t, v.cc_hz)) \
	X("voice_hz", 8, OFF(radio_p25_trunk_view_t, v.voice_hz), WIDTH(radio_p25_trunk_view_t, v.voice_hz) | END_GROUP) \
	X("nac", 3, OFF(radio_p25_trunk_view_t, v.nac), WIDTH(radio_p25_trunk_view_t, v.nac)) \
	X("wacn", 4, OFF(radio_p25_trunk_view_t, v.wacn), WIDTH(radio_p25_trunk_view_t, v.wacn)) \
	X("sysid", 5, OFF(radio_p25_trunk_view_t, v.sysid), WIDTH(radio_p25_trunk_view_t, v.sysid)) \
	X("rfss", 4, OFF(radio_p25_trunk_view_t, v.rfss), WIDTH(radio_p25_trunk_view_t, v.rfss)) \
	X("site", 4, OFF(radio_p25_trunk_view_t, v.site), WIDTH(radio_p25_trunk_view_t, v.site) | END_GROUP) \
	X("expected_nac", 12, OFF(radio_p25_trunk_view_t, v.expected_nac), WIDTH(radio_p25_trunk_view_t, v.expected_nac)) \
	X("sys_ok", 6, OFF(radio_p25_trunk_view_t, v.sys_ok), WIDTH(radio_p25_trunk_view_t, v.sys_ok)) \
	X("tuning", 6, OFF(radio_p25_trunk_view_t, v.tuning), WIDTH(radio_p25_trunk_view_t, v.tuning) | END_GROUP) \
	X("tg", 2, OFF(radio_p25_trunk_view_t, v.tg), WIDTH(radio_p25_trunk_view_t, v.tg)) \
	X("src", 3, OFF(radio_p25_trunk_view_t, v.src), WIDTH(radio_p25_trunk_view_t, v.src)) \
	X("dst", 3, OFF(radio_p25_trunk_view_t, v.dst), WIDTH(radio_p25_trunk_view_t, v.dst)) \
	X("unit_call", 9, OFF(radio_p25_trunk_view_t, v.unit_call), WIDTH(radio_p25_trunk_view_t, v.unit_call)) \
	X("enc", 3, OFF(radio_p25_trunk_view_t, v.enc), WIDTH(radio_p25_trunk_view_t, v.enc) | END_GROUP) \
	X("idens", 5, OFF(radio_p25_trunk_view_t, v.idens), WIDTH(radio_p25_trunk_view_t, v.idens)) \
	X("learned", 7, OFF(radio_p25_trunk_view_t, v.learned), WIDTH(radio_p25_trunk_view_t, v.learned)) \
	X("refused", 7, OFF(radio_p25_trunk_view_t, v.refused), WIDTH(radio_p25_trunk_view_t, v.refused)) \
	X("refused_tg", 10, OFF(radio_p25_trunk_view_t, v.refused_tg), WIDTH(radio_p25_trunk_view_t, v.refused_tg)) \
	X("fallback", 8, OFF(radio_p25_trunk_view_t, v.fallback), WIDTH(radio_p25_trunk_view_t, v.fallback) | END_GROUP) \
	X("blocks", 6, OFF(radio_p25_trunk_view_t, v.blocks), WIDTH(radio_p25_trunk_view_t, v.blocks)) \
	X("crc_fail", 8, OFF(radio_p25_trunk_view_t, v.crc_fail), WIDTH(radio_p25_trunk_view_t, v.crc_fail)) \
	X("crc_taken", 9, OFF(radio_p25_trunk_view_t, v.crc_taken), WIDTH(radio_p25_trunk_view_t, v.crc_taken)) \
	X("unverified", 10, OFF(radio_p25_trunk_view_t, v.unverified), WIDTH(radio_p25_trunk_view_t, v.unverified)) \
	X("mfr", 3, OFF(radio_p25_trunk_view_t, v.mfr), WIDTH(radio_p25_trunk_view_t, v.mfr)) \
	X("mbt", 3, OFF(radio_p25_trunk_view_t, v.mbt), WIDTH(radio_p25_trunk_view_t, v.mbt)) \
	X("mbt_op", 6, OFF(radio_p25_trunk_view_t, v.mbt_op), WIDTH(radio_p25_trunk_view_t, v.mbt_op)) \
	X("pdu_other", 9, OFF(radio_p25_trunk_view_t, v.pdu_other), WIDTH(radio_p25_trunk_view_t, v.pdu_other) | END_GROUP) \
	X("grants", 6, OFF(radio_p25_trunk_view_t, v.grants), WIDTH(radio_p25_trunk_view_t, v.grants)) \
	X("follows", 7, OFF(radio_p25_trunk_view_t, v.follows), WIDTH(radio_p25_trunk_view_t, v.follows)) \
	X("enc_skip", 8, OFF(radio_p25_trunk_view_t, v.enc_skip), WIDTH(radio_p25_trunk_view_t, v.enc_skip)) \
	X("tdma", 4, OFF(radio_p25_trunk_view_t, v.tdma), WIDTH(radio_p25_trunk_view_t, v.tdma)) \
	X("no_iden", 7, OFF(radio_p25_trunk_view_t, v.no_iden), WIDTH(radio_p25_trunk_view_t, v.no_iden) | END_GROUP) \
	X("wrong_sys", 9, OFF(radio_p25_trunk_view_t, v.wrong_sys), WIDTH(radio_p25_trunk_view_t, v.wrong_sys)) \
	X("hunts", 5, OFF(radio_p25_trunk_view_t, v.hunts), WIDTH(radio_p25_trunk_view_t, v.hunts)) \
	X("cc_lost", 7, OFF(radio_p25_trunk_view_t, v.cc_lost), WIDTH(radio_p25_trunk_view_t, v.cc_lost)) \
	X("voice_lost", 10, OFF(radio_p25_trunk_view_t, v.voice_lost), WIDTH(radio_p25_trunk_view_t, v.voice_lost)) \
	X("wrong_call", 10, OFF(radio_p25_trunk_view_t, v.wrong_call), WIDTH(radio_p25_trunk_view_t, v.wrong_call) | END_GROUP) \
	X("retunes", 7, OFF(radio_p25_trunk_view_t, retunes), WIDTH(radio_p25_trunk_view_t, retunes)) \
	X("untuned", 7, OFF(radio_p25_trunk_view_t, untuned), WIDTH(radio_p25_trunk_view_t, untuned)) \
	X("skips", 5, OFF(radio_p25_trunk_view_t, skips), WIDTH(radio_p25_trunk_view_t, skips) | END_GROUP) \
	X("tx", 2, OFF(radio_p25_trunk_view_t, tx), WIDTH(radio_p25_trunk_view_t, tx)) \
	X("reg", 3, OFF(radio_p25_trunk_view_t, v.reg), WIDTH(radio_p25_trunk_view_t, v.reg)) \
	X("req", 3, OFF(radio_p25_trunk_view_t, v.req), WIDTH(radio_p25_trunk_view_t, v.req)) \
	X("aff_tg", 6, OFF(radio_p25_trunk_view_t, v.aff_tg), WIDTH(radio_p25_trunk_view_t, v.aff_tg)) \
	X("talk_hz", 7, OFF(radio_p25_trunk_view_t, v.talk_hz), WIDTH(radio_p25_trunk_view_t, v.talk_hz)) \
	X("deny_svc", 8, OFF(radio_p25_trunk_view_t, v.deny_svc), WIDTH(radio_p25_trunk_view_t, v.deny_svc)) \
	X("deny_reason", 11, OFF(radio_p25_trunk_view_t, v.deny_reason), WIDTH(radio_p25_trunk_view_t, v.deny_reason) | END_GROUP) \
	X("reg_tx", 6, OFF(radio_p25_trunk_view_t, v.reg_tx), WIDTH(radio_p25_trunk_view_t, v.reg_tx)) \
	X("reg_ok", 6, OFF(radio_p25_trunk_view_t, v.reg_ok), WIDTH(radio_p25_trunk_view_t, v.reg_ok)) \
	X("aff_tx", 6, OFF(radio_p25_trunk_view_t, v.aff_tx), WIDTH(radio_p25_trunk_view_t, v.aff_tx)) \
	X("aff_ok", 6, OFF(radio_p25_trunk_view_t, v.aff_ok), WIDTH(radio_p25_trunk_view_t, v.aff_ok)) \
	X("req_tx", 6, OFF(radio_p25_trunk_view_t, v.req_tx), WIDTH(radio_p25_trunk_view_t, v.req_tx)) \
	X("req_grant", 9, OFF(radio_p25_trunk_view_t, v.req_grant), WIDTH(radio_p25_trunk_view_t, v.req_grant) | END_GROUP) \
	X("denials", 7, OFF(radio_p25_trunk_view_t, v.denials), WIDTH(radio_p25_trunk_view_t, v.denials)) \
	X("queued", 6, OFF(radio_p25_trunk_view_t, v.queued), WIDTH(radio_p25_trunk_view_t, v.queued)) \
	X("timeouts", 8, OFF(radio_p25_trunk_view_t, v.timeouts), WIDTH(radio_p25_trunk_view_t, v.timeouts)) \
	X("send_fail", 9, OFF(radio_p25_trunk_view_t, v.send_fail), WIDTH(radio_p25_trunk_view_t, v.send_fail)) \
	X("inb_busy", 8, OFF(radio_p25_trunk_view_t, v.inb_busy), WIDTH(radio_p25_trunk_view_t, v.inb_busy) | END_GROUP) \
	X("bursts", 6, OFF(radio_p25_trunk_view_t, bursts), WIDTH(radio_p25_trunk_view_t, bursts)) \
	X("burst_refused", 13, OFF(radio_p25_trunk_view_t, burst_refused), WIDTH(radio_p25_trunk_view_t, burst_refused) | END_GROUP)
#if CONFIG_P25_ROAM
#define ROAM_FIELDS(X) \
	X("wuid", 4, OFF(radio_p25_trunk_view_t, v.wuid), WIDTH(radio_p25_trunk_view_t, v.wuid)) \
	X("agid", 4, OFF(radio_p25_trunk_view_t, v.agid), WIDTH(radio_p25_trunk_view_t, v.agid)) \
	X("lra", 3, OFF(radio_p25_trunk_view_t, v.lra), WIDTH(radio_p25_trunk_view_t, v.lra)) \
	X("isolated", 8, OFF(radio_p25_trunk_view_t, v.isolated), WIDTH(radio_p25_trunk_view_t, v.isolated)) \
	X("location_ok", 11, OFF(radio_p25_trunk_view_t, v.location_ok), WIDTH(radio_p25_trunk_view_t, v.location_ok) | END_GROUP) \
	X("mobility", 8, OFF(radio_p25_trunk_view_t, v.mobility), WIDTH(radio_p25_trunk_view_t, v.mobility)) \
	X("loc_tx", 6, OFF(radio_p25_trunk_view_t, v.loc_tx), WIDTH(radio_p25_trunk_view_t, v.loc_tx)) \
	X("loc_ok", 6, OFF(radio_p25_trunk_view_t, v.loc_ok), WIDTH(radio_p25_trunk_view_t, v.loc_ok)) \
	X("ack_wait", 8, OFF(radio_p25_trunk_view_t, v.ack_wait), WIDTH(radio_p25_trunk_view_t, v.ack_wait) | END_GROUP)
#else
#define ROAM_FIELDS(X)
#endif
#else
#define TRUNK_FIELDS(X)
#define ROAM_FIELDS(X)
#endif
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
#define JOIN_(a, b) a ## b
#define JOIN(a, b) JOIN_(a, b)
#define DECLARE(label, n, offset, format) \
	struct { uint8_t off, type, key[KEY_BYTES(n)]; } JOIN(field_, __COUNTER__);
#define VALUE(label, n, offset, format) \
	{ (uint8_t)((offset) + 0u * sizeof(char[sizeof(label) == (n) + 1u ? 1 : -1])), \
	  (uint8_t)(format), { JOIN(KEY_, n)(label) } },
#define BYTES(label, n, offset, format) + 2u + KEY_BYTES(n)
#define ALL_FIELDS(X) SCAN_CHANNEL_FIELDS(X) CONVENTIONAL_FIELDS(X) VOICE_FIELDS(X) \
	TRUNK_FIELDS(X) ZONE_FIELDS(X) ROAM_FIELDS(X)
// Clang accepts literal subscripts in assertions; the supported GNU compilers fold strspn instead.
#if defined(__clang__)
#define CHAR_OK(s, n, i) ((i) >= (n) || ((AT(s, i) >= 'a' && AT(s, i) <= 'z') || AT(s, i) == '_'))
#define ALPHABET_OK(s, n) (CHAR_OK(s, n, 0) && CHAR_OK(s, n, 1) && CHAR_OK(s, n, 2) && \
	CHAR_OK(s, n, 3) && CHAR_OK(s, n, 4) && CHAR_OK(s, n, 5) && CHAR_OK(s, n, 6) && \
	CHAR_OK(s, n, 7) && CHAR_OK(s, n, 8) && CHAR_OK(s, n, 9) && CHAR_OK(s, n, 10) && \
	CHAR_OK(s, n, 11) && CHAR_OK(s, n, 12) && CHAR_OK(s, n, 13) && CHAR_OK(s, n, 14) && CHAR_OK(s, n, 15))
#else
#define ALPHABET_OK(s, n) (__builtin_strspn((s), "abcdefghijklmnopqrstuvwxyz_") == (n))
#endif
#define CHECK_KEY(label, n, offset, format) \
	_Static_assert((n) >= 1 && (n) <= 16 && sizeof(label) == (n) + 1u && ALPHABET_OK(label, n), \
		       "P25 diagnostic key alphabet/length");
ALL_FIELDS(CHECK_KEY)
#undef CHECK_KEY
#undef ALPHABET_OK
#if defined(__clang__)
#undef CHAR_OK
#endif
static const struct { ALL_FIELDS(DECLARE) } p25_fields = { ALL_FIELDS(VALUE) };
_Static_assert(sizeof p25_fields == 0u ALL_FIELDS(BYTES), "P25 field stream must have no padding");
#if CONFIG_P25_TRUNK
enum { CONVENTIONAL_BYTES = 0u SCAN_CHANNEL_FIELDS(BYTES) CONVENTIONAL_FIELDS(BYTES) VOICE_FIELDS(BYTES),
       TRUNK_BYTES = 0u TRUNK_FIELDS(BYTES) };
#endif
#undef ALL_FIELDS
#undef DECLARE
#undef VALUE
#undef BYTES
#undef CONVENTIONAL_FIELDS
#undef VOICE_FIELDS
#undef TRUNK_FIELDS
#undef ROAM_FIELDS
#undef SCAN_CHANNEL_FIELDS
#undef ZONE_FIELDS
#undef WIDTH
#undef OFF
#undef MASK

// A key has at most sixteen characters; its checked literal length selects the packed initializer above.
static const uint8_t *p25_key(const uint8_t *p, char *text)
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

// NULL text advances over an optional group without observing uninitialized snapshot fields.
static void p25_group(dl_text_t *t, const uint8_t **at, const void *base, const char *prefix)
{
	size_t mark = t ? t->len : 0;
	uint8_t type;
	char key[17];
	do {
		uint8_t offset = (*at)[0];
		type = (*at)[1];
		bool write = t && !t->full;
		*at = p25_key(*at + 2, write ? key : NULL);
		if (write) {
			const uint8_t *p = (const uint8_t *)base + offset;
			uint32_t value = offset;
			switch (type & 7u) {
			case 1: value = *p; break;
			case 2: { uint16_t n; memcpy(&n, p, sizeof n); value = n; break; }
			case 4: memcpy(&value, p, sizeof value); break;
			default: break; // a literal, such as built=1
			}
			unsigned mask = (type >> 3) & 7u;
			if (mask)
				value = (value & (1u << (mask - 1))) != 0;
			dl_text(t, "%s.%s=%u\n", prefix, key, (unsigned)value);
		}
	} while (!(type & END_GROUP));
	if (t && t->full)
		dl_text_rollback(t, mark);
}
#undef END_GROUP
#endif

void debuglink_view_p25(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_P25
	const radio_p25_view_t *p = &v->p25;
	const uint8_t *at = (const uint8_t *)&p25_fields;
	radio_p25_settings_t s;
	radio_p25_stats_t c;
	p25_load_t load;
#if CONFIG_P25_ZONE_SCAN
	p25_group(t, &at, p, "p25");
#endif
	p25_group(t, &at, p, "p25");
	p25_group(t, &at, p, "p25");
	bool measured = (p->flags & RADIO_P25_ON) != 0 && board.air != NULL && board.air->rx_load != NULL &&
		board.air->rx_load(&load.work, &load.span);
	p25_group(measured ? t : NULL, &at, &load, "p25");
	radio_p25_settings(&s);
	p25_group(t, &at, &s, "p25");
	p25_group(t, &at, &s, "p25");
	bool running = radio_p25_in_force(&s);
	p25_group(running ? t : NULL, &at, &s, "p25.run");
	p25_group(running ? t : NULL, &at, &s, "p25.run");
	p25_group(t, &at, &v->air_scan, "p25");
	radio_p25_stats(&c);
	p25_group(t, &at, &c, "p25");
	p25_group(t, &at, &c, "p25");
	p25_group(t, &at, &c, "p25");
#if CONFIG_IMBE
	p25_voice_stats_t vs;
	p25VoiceGetStats(&vs);
	p25_group(t, &at, &vs, "p25v");
	p25_group(t, &at, &vs, "p25v");
	p25_group(t, &at, &vs, "p25v");
	p25_group(t, &at, &vs, "p25v");
	dl_text(t, "p25v.tx_mic_low_db=%d\n", miclimit_q8_db((uint16_t)vs.tx_mic_low_q8));
#else
	dl_text(t, "p25v.built=0\n");
#endif
#else
	(void)v;
	dl_text(t, "p25.built=0\n");
#endif
}

// noinline: its snapshot must not sit in the section dispatcher's frame under every section's path.
__attribute__((noinline)) void debuglink_view_p25_trunk(dl_text_t *t)
{
#if CONFIG_P25_TRUNK
	radio_p25_trunk_view_t tv;
	const uint8_t *at = (const uint8_t *)&p25_fields + CONVENTIONAL_BYTES;
	radio_p25_trunk_view(&tv);
	while (at < (const uint8_t *)&p25_fields + CONVENTIONAL_BYTES + TRUNK_BYTES)
		p25_group(t, &at, &tv, "p25t");
#if CONFIG_P25_ZONE_SCAN
	p25_zone_scan_t zone = { radio_p25_zone_scanning(), radio_p25_zone_scan_count() };
	p25_group(t, &at, &zone, "p25t");
#endif
	while (at < (const uint8_t *)&p25_fields + sizeof p25_fields)
		p25_group(t, &at, &tv, "p25t");
#else
	dl_text(t, "p25t.built=0\n");
#endif
}

void debuglink_p25_reset(void)
{
#if CONFIG_P25
	radio_p25_reset_stats();
#if CONFIG_IMBE
	p25VoiceResetStats();
#endif
#endif
}
