// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR parts of the debug link: the GET_VIEW sections dmr, dmr_call and dmr_stats, the voice path diagnostics
// lines and their counter reset. Kept apart from debuglink_view.c so DMR work can extend it on its own: add keys
// freely (new keys don't bump DL_VIEW_VERSION), keep the existing ones.
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"
#if CONFIG_DMR
#include "dmr/call/dmr_call.h"
#include "dmr/ambe/ambeVoice.h"
#endif

#if CONFIG_DMR

static const char *dmr_state_key(uint8_t s)
{
	static const char *const names[] = { "idle", "rx", "rx_other", "tx_wait", "tx", "tx_ending",
					     "tx_data" };
	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static const char *phase_key(uint8_t p)
{
	static const char *const names[] = { "idle", "rx_voice", "rx_busy", "tx_access", "tx_wakeup", "tx_voice",
					     "tx_ending", "tx_denied", "tx_data" };
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

	dl_text(t, "%s.src=%u\n%s.dst=%u\n", p, (unsigned)c->src, p, (unsigned)c->dst);
	dl_text(t, "%s.group=%u\n%s.to_us=%u\n%s.encrypted=%u\n", p, c->group, p, c->to_us, p, c->encrypted);
	dl_text(t, "%s.emergency=%u\n%s.pi_header=%u\n", p, c->emergency, p, c->pi_header);
	dl_fmt(k, sizeof(k), "%s.src_name", p);
	dl_text_kv_str(t, k, c->src_name);
	dl_fmt(k, sizeof(k), "%s.dst_name", p);
	dl_text_kv_str(t, k, c->dst_name);
	// ETSI GPS Info (CONFIG_DMR_GPS): degrees with 7 decimals, the error code, range and bearing from us
	dl_text(t, "%s.gps_valid=%u\n", p, c->gps_valid);
	if (c->gps_valid) {
		uint32_t la = (uint32_t)(c->gps_lat < 0 ? -(int64_t)c->gps_lat : c->gps_lat);
		uint32_t lo = (uint32_t)(c->gps_lon < 0 ? -(int64_t)c->gps_lon : c->gps_lon);

		dl_text(t, "%s.gps_lat=%s%u.%07u\n%s.gps_lon=%s%u.%07u\n%s.gps_err=%u\n", p, c->gps_lat < 0 ? "-" : "",
			(unsigned)(la / 10000000u), (unsigned)(la % 10000000u), p, c->gps_lon < 0 ? "-" : "",
			(unsigned)(lo / 10000000u), (unsigned)(lo % 10000000u), p, c->gps_err);
		if (c->gps_range)
			dl_text(t, "%s.gps_dist_m=%u\n%s.gps_bearing=%u\n", p, (unsigned)c->gps_dist_m, p, c->gps_bearing);
	}
}

void debuglink_view_dmr(uint8_t part, const radio_view_t *v, dl_text_t *t)
{
	const radio_dmr_view_t *d = &v->dmr;
	const dmr_call_view_t *cv = dmr_call_view();	// single bytes only (the stack's task writes it)

	switch (part) {
	case 0:
		dl_text(t, "dmr.built=1\n");
		dl_text(t, "dmr.active=%u\ndmr.codec=%u\n", d->active, d->codec);
		dl_text(t, "dmr.state=%s\n", dmr_state_key(d->state));
		dl_text(t, "dmr.phase=%s\n", phase_key(cv->phase));
		dl_text(t, "dmr.tx_error=%s\n", tx_error_key(cv->tx_error));
		dl_text(t, "dmr.own_id=%u\ndmr.cc=%u\ndmr.ts=%u\n", (unsigned)d->own_id, d->cc, d->ts);
		char alias[DMR_TA_TEXT_MAX + 1];
		dmr_call_tx_alias(alias);
		dl_text(t, "dmr.tx_alias=%s\n", alias);
		dl_text(t, "dmr.rmo=%u\ndmr.repeater_up=%u\n", d->rmo, d->repeater_up);
		dl_text(t, "dmr.target_id=%u\ndmr.target_private=%u\n", (unsigned)d->target_id, d->target_private);
		dl_text_kv_str(t, "dmr.target_name", d->target_name);
		dl_text(t, "dmr.reply_active=%u\ndmr.reply_s=%u\n", d->reply_active, d->reply_s);
		dl_text(t, "dmr.hear_all=%u\ndmr.hear_all_latch=%u\n", d->hear_all, d->hear_all_latch);
		dl_text(t, "dmr.focus_id=%u\ndmr.focus_private=%u\ndmr.one_shot=%u\n", (unsigned)d->focus_id,
			d->focus_private, d->one_shot);
		dl_text(t, "dmr.target_manual=%u\ndmr.privacy=%u\ndmr.alias_tx=%u\n", d->target_manual, d->privacy,
			d->alias_tx);
		dl_text(t, "dmr.underruns=%u\n", (unsigned)d->underruns);
		break;
	case 1:
		call(t, "call", &d->call);
		dl_text(t, "call.late_entry=%u\n", d->late_entry);
		dl_text(t, "call.alias_complete=%u\n", cv->caller.alias_complete);
		dl_text(t, "call.fid=%u\ncall.svc=%u\ncall.pi_alg=%u\ncall.pi_key=%u\n", cv->caller.fid, cv->caller.svc,
			cv->caller.pi_alg, cv->caller.pi_key);
		dl_text_kv_str(t, "call.alias", d->alias);
		dl_text(t, "call.rssi_dbm=%d\ncall.ms=%u\n", d->rssi_dbm, (unsigned)d->call_ms);
		call(t, "last", &d->last);
		dl_text(t, "last.ms=%u\n", (unsigned)d->last_ms);
		break;
	case 2: {
		dmr_call_stats_t c;
		ambe_voice_stats_t a;

		dmr_call_get_stats(&c);
		ambeVoiceGetStats(&a);
		dl_text(t, "call.rx_calls=%u\ncall.rx_calls_played=%u\ncall.rx_late_entries=%u\n", (unsigned)c.rx_calls,
			(unsigned)c.rx_calls_played, (unsigned)c.rx_late_entries);
		dl_text(t, "call.rx_voice_bursts=%u\ncall.rx_lost_frames=%u\n", (unsigned)c.rx_voice_bursts,
			(unsigned)c.rx_lost_frames);
		dl_text(t, "call.rx_dup_dropped=%u\ncall.rx_enc_dropped=%u\ncall.rx_term_dropped=%u\n",
			(unsigned)c.rx_dup_dropped, (unsigned)c.rx_enc_dropped, (unsigned)c.rx_term_dropped);
		dl_text(t, "call.tx_calls=%u\ncall.tx_denied=%u\ncall.tx_underruns=%u\ncall.evq_overflows=%u\n",
			(unsigned)c.tx_calls, (unsigned)c.tx_denied, (unsigned)c.tx_underruns, (unsigned)c.evq_overflows);
		dl_text(t, "call.rx_private_calls=%u\ncall.rx_emergency_calls=%u\ncall.rx_encrypted_calls=%u\n",
			(unsigned)c.rx_private_calls, (unsigned)c.rx_emergency_calls, (unsigned)c.rx_encrypted_calls);
		dl_text(t, "call.rx_pi_headers=%u\ncall.rx_alias_complete=%u\ncall.tx_alias_lcs=%u\n",
			(unsigned)c.rx_pi_headers, (unsigned)c.rx_alias_complete, (unsigned)c.tx_alias_lcs);
		dl_text(t, "call.rx_gps=%u\ncall.tx_gps_lcs=%u\n", (unsigned)c.rx_gps, (unsigned)c.tx_gps_lcs);
		dl_text(t, "voice.rx_in=%u\nvoice.rx_frames=%u\nvoice.rx_concealed=%u\nvoice.rx_silence=%u\n",
			(unsigned)a.rx_in, (unsigned)a.rx_frames, (unsigned)a.rx_concealed, (unsigned)a.rx_silence);
		dl_text(t, "voice.rx_calls=%u\nvoice.rx_queue_max=%u\nvoice.rx_bad=%u\nvoice.rx_lost=%u\n",
			(unsigned)a.rx_calls, (unsigned)a.rx_queue_max, (unsigned)a.rx_bad, (unsigned)a.rx_lost);
		dl_text(t, "voice.rx_underruns=%u\nvoice.rx_overflows=%u\nvoice.rx_bit_errors=%u\n",
			(unsigned)a.rx_underruns, (unsigned)a.rx_overflows, (unsigned)a.rx_bit_errors);
		dl_text(t, "voice.tx_frames=%u\nvoice.tx_overflows=%u\n", (unsigned)a.tx_frames, (unsigned)a.tx_overflows);
		// Encode options (VOCODER_OPT_*): in force and supported; the codec's own word when the board reports it
		dl_text(t, "voice.enc_options=0x%X\nvoice.enc_supported=0x%X\n", (unsigned)ambeVoiceEncodeOptions(),
			(unsigned)ambeVoiceEncodeOptionsSupported());
		if (board.vocoder != NULL && board.vocoder->encode_flags != NULL) {
			dl_text(t, "voice.enc_flags=0x%X\n", (unsigned)board.vocoder->encode_flags());
		}
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
