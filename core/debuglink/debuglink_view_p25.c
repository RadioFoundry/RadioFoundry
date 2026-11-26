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
typedef struct { uint8_t offset, type; } p25_field_t;
typedef struct { const p25_field_t *field; const char *key; } p25_fields_t;
typedef struct { uint32_t work, span; } p25_load_t;
#define END_GROUP 0x80u
#define MASK(mask) ((__builtin_ctz(mask) + 1u) << 3)
#define OFF(type, member) offsetof(type, member)
#define WIDTH(type, member) (sizeof(((type *)0)->member) + 0u * sizeof(char[ \
	(sizeof(((type *)0)->member) == 1 || sizeof(((type *)0)->member) == 2 || \
	 sizeof(((type *)0)->member) == 4) ? 1 : -1]))
_Static_assert(sizeof(radio_p25_view_t) <= 256 && sizeof(radio_p25_settings_t) <= 256 &&
	       sizeof(radio_p25_stats_t) <= 256, "P25 view field offsets need one byte");
#define CONVENTIONAL_FIELDS(X) \
	X("built", 1, 0) \
	X("on", OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_ON)) \
	X("rx", OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_RX)) \
	X("played", OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_PLAYED)) \
	X("enc", OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_ENC)) \
	X("unit", OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_UNIT)) \
	X("late", OFF(radio_p25_view_t, flags), WIDTH(radio_p25_view_t, flags) | MASK(RADIO_P25_LATE) | END_GROUP) \
	X("nac", OFF(radio_p25_view_t, nac), WIDTH(radio_p25_view_t, nac)) \
	X("src", OFF(radio_p25_view_t, src), WIDTH(radio_p25_view_t, src)) \
	X("dst", OFF(radio_p25_view_t, dst), WIDTH(radio_p25_view_t, dst)) \
	X("tx", OFF(radio_p25_view_t, tx), WIDTH(radio_p25_view_t, tx) | END_GROUP) \
	X("rx_work_us", OFF(p25_load_t, work), WIDTH(p25_load_t, work)) \
	X("rx_span_us", OFF(p25_load_t, span), WIDTH(p25_load_t, span) | END_GROUP) \
	X("rx_nac", OFF(radio_p25_settings_t, rx_nac), WIDTH(radio_p25_settings_t, rx_nac)) \
	X("tx_nac", OFF(radio_p25_settings_t, tx_nac), WIDTH(radio_p25_settings_t, tx_nac)) \
	X("rx_tgid", OFF(radio_p25_settings_t, rx_tgid), WIDTH(radio_p25_settings_t, rx_tgid)) \
	X("talkaround", OFF(radio_p25_settings_t, talkaround), WIDTH(radio_p25_settings_t, talkaround)) \
	X("unit_call", OFF(radio_p25_settings_t, unit_call), WIDTH(radio_p25_settings_t, unit_call) | END_GROUP) \
	X("tx_dst", OFF(radio_p25_settings_t, tx_dst), WIDTH(radio_p25_settings_t, tx_dst)) \
	X("unit_id", OFF(radio_p25_settings_t, unit_id), WIDTH(radio_p25_settings_t, unit_id) | END_GROUP) \
	X("rx_nac", OFF(radio_p25_settings_t, rx_nac), WIDTH(radio_p25_settings_t, rx_nac)) \
	X("tx_nac", OFF(radio_p25_settings_t, tx_nac), WIDTH(radio_p25_settings_t, tx_nac)) \
	X("rx_tgid", OFF(radio_p25_settings_t, rx_tgid), WIDTH(radio_p25_settings_t, rx_tgid)) \
	X("talkaround", OFF(radio_p25_settings_t, talkaround), WIDTH(radio_p25_settings_t, talkaround) | END_GROUP) \
	X("unit_call", OFF(radio_p25_settings_t, unit_call), WIDTH(radio_p25_settings_t, unit_call)) \
	X("tx_dst", OFF(radio_p25_settings_t, tx_dst), WIDTH(radio_p25_settings_t, tx_dst)) \
	X("unit_id", OFF(radio_p25_settings_t, unit_id), WIDTH(radio_p25_settings_t, unit_id) | END_GROUP) \
	X("begins", OFF(radio_p25_stats_t, begins), WIDTH(radio_p25_stats_t, begins)) \
	X("begin_fails", OFF(radio_p25_stats_t, begin_fails), WIDTH(radio_p25_stats_t, begin_fails)) \
	X("mem_lent", OFF(radio_p25_stats_t, mem_lent), WIDTH(radio_p25_stats_t, mem_lent)) \
	X("rx_calls", OFF(radio_p25_stats_t, rx_calls), WIDTH(radio_p25_stats_t, rx_calls)) \
	X("tx_calls", OFF(radio_p25_stats_t, tx_calls), WIDTH(radio_p25_stats_t, tx_calls) | END_GROUP) \
	X("tx_filler", OFF(radio_p25_stats_t, tx_filler), WIDTH(radio_p25_stats_t, tx_filler)) \
	X("units", OFF(radio_p25_stats_t, units), WIDTH(radio_p25_stats_t, units)) \
	X("voice", OFF(radio_p25_stats_t, voice), WIDTH(radio_p25_stats_t, voice)) \
	X("voice_bad", OFF(radio_p25_stats_t, voice_bad), WIDTH(radio_p25_stats_t, voice_bad)) \
	X("voice_bits", OFF(radio_p25_stats_t, voice_bits), WIDTH(radio_p25_stats_t, voice_bits) | END_GROUP) \
	X("lc_fail", OFF(radio_p25_stats_t, lc_fail), WIDTH(radio_p25_stats_t, lc_fail)) \
	X("es_fail", OFF(radio_p25_stats_t, es_fail), WIDTH(radio_p25_stats_t, es_fail)) \
	X("rx_chunks", OFF(radio_p25_stats_t, rx_chunks), WIDTH(radio_p25_stats_t, rx_chunks)) \
	X("tx_underruns", OFF(radio_p25_stats_t, tx_underruns), WIDTH(radio_p25_stats_t, tx_underruns)) \
	X("tx_syms", OFF(radio_p25_stats_t, tx_syms), WIDTH(radio_p25_stats_t, tx_syms) | END_GROUP)
#if CONFIG_IMBE
_Static_assert(sizeof(p25_voice_stats_t) <= 256, "P25 voice field offsets need one byte");
#define VOICE_FIELDS(X) \
	X("built", 1, 0) \
	X("in", OFF(p25_voice_stats_t, rx_in), WIDTH(p25_voice_stats_t, rx_in)) \
	X("frames", OFF(p25_voice_stats_t, rx_frames), WIDTH(p25_voice_stats_t, rx_frames)) \
	X("bad", OFF(p25_voice_stats_t, rx_bad), WIDTH(p25_voice_stats_t, rx_bad)) \
	X("concealed", OFF(p25_voice_stats_t, rx_concealed), WIDTH(p25_voice_stats_t, rx_concealed)) \
	X("calls", OFF(p25_voice_stats_t, rx_calls), WIDTH(p25_voice_stats_t, rx_calls) | END_GROUP) \
	X("underruns", OFF(p25_voice_stats_t, rx_underruns), WIDTH(p25_voice_stats_t, rx_underruns)) \
	X("underrun_at", OFF(p25_voice_stats_t, rx_underrun_at), WIDTH(p25_voice_stats_t, rx_underrun_at)) \
	X("overflows", OFF(p25_voice_stats_t, rx_overflows), WIDTH(p25_voice_stats_t, rx_overflows)) \
	X("bit_errors", OFF(p25_voice_stats_t, rx_bit_errors), WIDTH(p25_voice_stats_t, rx_bit_errors) | END_GROUP) \
	X("queue_max", OFF(p25_voice_stats_t, rx_queue_max), WIDTH(p25_voice_stats_t, rx_queue_max)) \
	X("peak", OFF(p25_voice_stats_t, rx_peak), WIDTH(p25_voice_stats_t, rx_peak)) \
	X("tx_frames", OFF(p25_voice_stats_t, tx_frames), WIDTH(p25_voice_stats_t, tx_frames) | END_GROUP) \
	X("tx_encoded", OFF(p25_voice_stats_t, tx_encoded), WIDTH(p25_voice_stats_t, tx_encoded)) \
	X("tx_dropped", OFF(p25_voice_stats_t, tx_dropped), WIDTH(p25_voice_stats_t, tx_dropped)) \
	X("tx_silent", OFF(p25_voice_stats_t, tx_silent), WIDTH(p25_voice_stats_t, tx_silent)) \
	X("tx_mic_peak", OFF(p25_voice_stats_t, tx_mic_peak), WIDTH(p25_voice_stats_t, tx_mic_peak)) \
	X("tx_mic_muted", OFF(p25_voice_stats_t, tx_mic_muted), WIDTH(p25_voice_stats_t, tx_mic_muted) | END_GROUP)
#else
#define VOICE_FIELDS(X)
#endif
#if CONFIG_P25_TRUNK
_Static_assert(sizeof(radio_p25_trunk_view_t) <= 256, "P25 trunk field offsets need one byte");
#define TRUNK_FIELDS(X) \
	X("built", 1, 0) \
	X("on", OFF(radio_p25_trunk_view_t, on), WIDTH(radio_p25_trunk_view_t, on)) \
	X("state", OFF(radio_p25_trunk_view_t, v.state), WIDTH(radio_p25_trunk_view_t, v.state)) \
	X("cc", OFF(radio_p25_trunk_view_t, v.cc), WIDTH(radio_p25_trunk_view_t, v.cc)) \
	X("cc_hz", OFF(radio_p25_trunk_view_t, v.cc_hz), WIDTH(radio_p25_trunk_view_t, v.cc_hz)) \
	X("voice_hz", OFF(radio_p25_trunk_view_t, v.voice_hz), WIDTH(radio_p25_trunk_view_t, v.voice_hz) | END_GROUP) \
	X("nac", OFF(radio_p25_trunk_view_t, v.nac), WIDTH(radio_p25_trunk_view_t, v.nac)) \
	X("wacn", OFF(radio_p25_trunk_view_t, v.wacn), WIDTH(radio_p25_trunk_view_t, v.wacn)) \
	X("sysid", OFF(radio_p25_trunk_view_t, v.sysid), WIDTH(radio_p25_trunk_view_t, v.sysid)) \
	X("rfss", OFF(radio_p25_trunk_view_t, v.rfss), WIDTH(radio_p25_trunk_view_t, v.rfss)) \
	X("site", OFF(radio_p25_trunk_view_t, v.site), WIDTH(radio_p25_trunk_view_t, v.site) | END_GROUP) \
	X("expected_nac", OFF(radio_p25_trunk_view_t, v.expected_nac), WIDTH(radio_p25_trunk_view_t, v.expected_nac)) \
	X("sys_ok", OFF(radio_p25_trunk_view_t, v.sys_ok), WIDTH(radio_p25_trunk_view_t, v.sys_ok)) \
	X("tuning", OFF(radio_p25_trunk_view_t, v.tuning), WIDTH(radio_p25_trunk_view_t, v.tuning) | END_GROUP) \
	X("tg", OFF(radio_p25_trunk_view_t, v.tg), WIDTH(radio_p25_trunk_view_t, v.tg)) \
	X("src", OFF(radio_p25_trunk_view_t, v.src), WIDTH(radio_p25_trunk_view_t, v.src)) \
	X("dst", OFF(radio_p25_trunk_view_t, v.dst), WIDTH(radio_p25_trunk_view_t, v.dst)) \
	X("unit_call", OFF(radio_p25_trunk_view_t, v.unit_call), WIDTH(radio_p25_trunk_view_t, v.unit_call)) \
	X("enc", OFF(radio_p25_trunk_view_t, v.enc), WIDTH(radio_p25_trunk_view_t, v.enc) | END_GROUP) \
	X("idens", OFF(radio_p25_trunk_view_t, v.idens), WIDTH(radio_p25_trunk_view_t, v.idens)) \
	X("learned", OFF(radio_p25_trunk_view_t, v.learned), WIDTH(radio_p25_trunk_view_t, v.learned)) \
	X("refused", OFF(radio_p25_trunk_view_t, v.refused), WIDTH(radio_p25_trunk_view_t, v.refused)) \
	X("refused_tg", OFF(radio_p25_trunk_view_t, v.refused_tg), WIDTH(radio_p25_trunk_view_t, v.refused_tg)) \
	X("fallback", OFF(radio_p25_trunk_view_t, v.fallback), WIDTH(radio_p25_trunk_view_t, v.fallback) | END_GROUP) \
	X("blocks", OFF(radio_p25_trunk_view_t, v.blocks), WIDTH(radio_p25_trunk_view_t, v.blocks)) \
	X("crc_fail", OFF(radio_p25_trunk_view_t, v.crc_fail), WIDTH(radio_p25_trunk_view_t, v.crc_fail)) \
	X("crc_taken", OFF(radio_p25_trunk_view_t, v.crc_taken), WIDTH(radio_p25_trunk_view_t, v.crc_taken)) \
	X("unverified", OFF(radio_p25_trunk_view_t, v.unverified), WIDTH(radio_p25_trunk_view_t, v.unverified)) \
	X("mfr", OFF(radio_p25_trunk_view_t, v.mfr), WIDTH(radio_p25_trunk_view_t, v.mfr)) \
	X("mbt", OFF(radio_p25_trunk_view_t, v.mbt), WIDTH(radio_p25_trunk_view_t, v.mbt)) \
	X("mbt_op", OFF(radio_p25_trunk_view_t, v.mbt_op), WIDTH(radio_p25_trunk_view_t, v.mbt_op)) \
	X("pdu_other", OFF(radio_p25_trunk_view_t, v.pdu_other), WIDTH(radio_p25_trunk_view_t, v.pdu_other) | END_GROUP) \
	X("grants", OFF(radio_p25_trunk_view_t, v.grants), WIDTH(radio_p25_trunk_view_t, v.grants)) \
	X("follows", OFF(radio_p25_trunk_view_t, v.follows), WIDTH(radio_p25_trunk_view_t, v.follows)) \
	X("enc_skip", OFF(radio_p25_trunk_view_t, v.enc_skip), WIDTH(radio_p25_trunk_view_t, v.enc_skip)) \
	X("tdma", OFF(radio_p25_trunk_view_t, v.tdma), WIDTH(radio_p25_trunk_view_t, v.tdma)) \
	X("no_iden", OFF(radio_p25_trunk_view_t, v.no_iden), WIDTH(radio_p25_trunk_view_t, v.no_iden) | END_GROUP) \
	X("wrong_sys", OFF(radio_p25_trunk_view_t, v.wrong_sys), WIDTH(radio_p25_trunk_view_t, v.wrong_sys)) \
	X("hunts", OFF(radio_p25_trunk_view_t, v.hunts), WIDTH(radio_p25_trunk_view_t, v.hunts)) \
	X("cc_lost", OFF(radio_p25_trunk_view_t, v.cc_lost), WIDTH(radio_p25_trunk_view_t, v.cc_lost)) \
	X("voice_lost", OFF(radio_p25_trunk_view_t, v.voice_lost), WIDTH(radio_p25_trunk_view_t, v.voice_lost)) \
	X("wrong_call", OFF(radio_p25_trunk_view_t, v.wrong_call), WIDTH(radio_p25_trunk_view_t, v.wrong_call) | END_GROUP) \
	X("retunes", OFF(radio_p25_trunk_view_t, retunes), WIDTH(radio_p25_trunk_view_t, retunes)) \
	X("untuned", OFF(radio_p25_trunk_view_t, untuned), WIDTH(radio_p25_trunk_view_t, untuned)) \
	X("skips", OFF(radio_p25_trunk_view_t, skips), WIDTH(radio_p25_trunk_view_t, skips) | END_GROUP) \
	X("tx", OFF(radio_p25_trunk_view_t, tx), WIDTH(radio_p25_trunk_view_t, tx)) \
	X("reg", OFF(radio_p25_trunk_view_t, v.reg), WIDTH(radio_p25_trunk_view_t, v.reg)) \
	X("req", OFF(radio_p25_trunk_view_t, v.req), WIDTH(radio_p25_trunk_view_t, v.req)) \
	X("aff_tg", OFF(radio_p25_trunk_view_t, v.aff_tg), WIDTH(radio_p25_trunk_view_t, v.aff_tg)) \
	X("talk_hz", OFF(radio_p25_trunk_view_t, v.talk_hz), WIDTH(radio_p25_trunk_view_t, v.talk_hz)) \
	X("deny_svc", OFF(radio_p25_trunk_view_t, v.deny_svc), WIDTH(radio_p25_trunk_view_t, v.deny_svc)) \
	X("deny_reason", OFF(radio_p25_trunk_view_t, v.deny_reason), WIDTH(radio_p25_trunk_view_t, v.deny_reason) | END_GROUP) \
	X("reg_tx", OFF(radio_p25_trunk_view_t, v.reg_tx), WIDTH(radio_p25_trunk_view_t, v.reg_tx)) \
	X("reg_ok", OFF(radio_p25_trunk_view_t, v.reg_ok), WIDTH(radio_p25_trunk_view_t, v.reg_ok)) \
	X("aff_tx", OFF(radio_p25_trunk_view_t, v.aff_tx), WIDTH(radio_p25_trunk_view_t, v.aff_tx)) \
	X("aff_ok", OFF(radio_p25_trunk_view_t, v.aff_ok), WIDTH(radio_p25_trunk_view_t, v.aff_ok)) \
	X("req_tx", OFF(radio_p25_trunk_view_t, v.req_tx), WIDTH(radio_p25_trunk_view_t, v.req_tx)) \
	X("req_grant", OFF(radio_p25_trunk_view_t, v.req_grant), WIDTH(radio_p25_trunk_view_t, v.req_grant) | END_GROUP) \
	X("denials", OFF(radio_p25_trunk_view_t, v.denials), WIDTH(radio_p25_trunk_view_t, v.denials)) \
	X("queued", OFF(radio_p25_trunk_view_t, v.queued), WIDTH(radio_p25_trunk_view_t, v.queued)) \
	X("timeouts", OFF(radio_p25_trunk_view_t, v.timeouts), WIDTH(radio_p25_trunk_view_t, v.timeouts)) \
	X("send_fail", OFF(radio_p25_trunk_view_t, v.send_fail), WIDTH(radio_p25_trunk_view_t, v.send_fail)) \
	X("inb_busy", OFF(radio_p25_trunk_view_t, v.inb_busy), WIDTH(radio_p25_trunk_view_t, v.inb_busy) | END_GROUP) \
	X("bursts", OFF(radio_p25_trunk_view_t, bursts), WIDTH(radio_p25_trunk_view_t, bursts)) \
	X("burst_refused", OFF(radio_p25_trunk_view_t, burst_refused), WIDTH(radio_p25_trunk_view_t, burst_refused) | END_GROUP)
#if CONFIG_P25_ROAM
#define ROAM_FIELDS(X) \
	X("wuid", OFF(radio_p25_trunk_view_t, v.wuid), WIDTH(radio_p25_trunk_view_t, v.wuid)) \
	X("agid", OFF(radio_p25_trunk_view_t, v.agid), WIDTH(radio_p25_trunk_view_t, v.agid)) \
	X("lra", OFF(radio_p25_trunk_view_t, v.lra), WIDTH(radio_p25_trunk_view_t, v.lra)) \
	X("isolated", OFF(radio_p25_trunk_view_t, v.isolated), WIDTH(radio_p25_trunk_view_t, v.isolated)) \
	X("location_ok", OFF(radio_p25_trunk_view_t, v.location_ok), WIDTH(radio_p25_trunk_view_t, v.location_ok) | END_GROUP) \
	X("mobility", OFF(radio_p25_trunk_view_t, v.mobility), WIDTH(radio_p25_trunk_view_t, v.mobility)) \
	X("loc_tx", OFF(radio_p25_trunk_view_t, v.loc_tx), WIDTH(radio_p25_trunk_view_t, v.loc_tx)) \
	X("loc_ok", OFF(radio_p25_trunk_view_t, v.loc_ok), WIDTH(radio_p25_trunk_view_t, v.loc_ok)) \
	X("ack_wait", OFF(radio_p25_trunk_view_t, v.ack_wait), WIDTH(radio_p25_trunk_view_t, v.ack_wait) | END_GROUP)
#else
#define ROAM_FIELDS(X)
#endif
#else
#define TRUNK_FIELDS(X)
#define ROAM_FIELDS(X)
#endif
#define FIELD(key, offset, type) { (uint8_t)(offset), (uint8_t)(type) },
#define KEY(key, offset, type) key "\0"
static const p25_field_t p25_fields[] = {
	CONVENTIONAL_FIELDS(FIELD) VOICE_FIELDS(FIELD) TRUNK_FIELDS(FIELD) ROAM_FIELDS(FIELD)
};
static const char p25_keys[] = CONVENTIONAL_FIELDS(KEY) VOICE_FIELDS(KEY) TRUNK_FIELDS(KEY) ROAM_FIELDS(KEY);
#if CONFIG_P25_TRUNK
enum { CONVENTIONAL_COUNT = sizeof((p25_field_t[]){ CONVENTIONAL_FIELDS(FIELD) VOICE_FIELDS(FIELD) }) /
			   sizeof(p25_field_t),
       CONVENTIONAL_KEY_BYTES = sizeof(CONVENTIONAL_FIELDS(KEY) VOICE_FIELDS(KEY)) - 1 };
#endif
#undef KEY
#undef FIELD
#undef CONVENTIONAL_FIELDS
#undef VOICE_FIELDS
#undef TRUNK_FIELDS
#undef ROAM_FIELDS
#undef WIDTH
#undef OFF
#undef MASK

// NULL text advances over an optional group without observing uninitialized snapshot fields.
static void p25_group(dl_text_t *t, p25_fields_t *at, const void *base, const char *prefix)
{
	size_t mark = t ? t->len : 0;
	const p25_field_t *field;
	do {
		field = at->field++;
		const char *key = at->key;
		at->key += strlen(key) + 1;
		if (t && !t->full) {
			const uint8_t *p = (const uint8_t *)base + field->offset;
			uint32_t value = field->offset;
			switch (field->type & 7u) {
			case 1: value = *p; break;
			case 2: { uint16_t n; memcpy(&n, p, sizeof n); value = n; break; }
			case 4: memcpy(&value, p, sizeof value); break;
			default: break; // a literal, such as built=1
			}
			unsigned mask = (field->type >> 3) & 7u;
			if (mask)
				value = (value & (1u << (mask - 1))) != 0;
			dl_text(t, "%s.%s=%u\n", prefix, key, (unsigned)value);
		}
	} while (!(field->type & END_GROUP));
	if (t && t->full)
		dl_text_rollback(t, mark);
}
#undef END_GROUP
#endif

void debuglink_view_p25(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_P25
	const radio_p25_view_t *p = &v->p25;
	p25_fields_t at = { p25_fields, p25_keys };
	radio_p25_settings_t s;
	radio_p25_stats_t c;
	p25_load_t load;
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
	p25_fields_t at = { p25_fields + CONVENTIONAL_COUNT, p25_keys + CONVENTIONAL_KEY_BYTES };
	radio_p25_trunk_view(&tv);
	while (at.field < p25_fields + sizeof p25_fields / sizeof p25_fields[0])
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
