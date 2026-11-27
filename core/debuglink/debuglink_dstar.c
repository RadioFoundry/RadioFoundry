// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR on the debug link (docs/spec/debuglink.md 3.31, docs/testing/dstar-bench.md): the "dstar" view section (what
// was heard, what PTT sends, the voice path's counters, the flat paths' calibration in force) and the D-STAR command,
// which the bench uses to tune the calibration, pick a test signal for the next transmissions, set UR and the station,
// and reach the board's own flat path knobs. Without CONFIG_DSTAR: "dstar.built=0" and the command answers unavailable.
#include <string.h>
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "config.h"

#if CONFIG_DSTAR
#include "board.h"
#include "hal_time.h"
#include "radio/radio.h"
#include "radio/radio_dstar.h"
#include "settings/settings.h"
#include "dstar/dstar_codeplug.h"

static const char *const test_names[RDSTAR_TEST_COUNT] = { "off", "tone", "sweep", "square", "dots", "silent" };
static const char *const fault_names[] = { "ok", "no_path", "rx_refused", "no_cal" };
static const char *const voice_names[] = { "ok", "no_ram", "no_stream" };
static const char *const ur_names[] = { "channel", "cq", "reply", "link", "unlink", "echo", "info" };

static const char *word(uint8_t i, const char *const *w, uint8_t n)
{
	return i < n ? w[i] : "?";
}

void debuglink_view_dstar(const radio_view_t *v, dl_text_t *t)
{
	const radio_dstar_view_t *d = &v->dstar;
	radio_dstar_stats_t s;
	radio_dstar_stats(&s);
	dl_text(t, "dstar.built=1\ndstar.on=%u\ndstar.fault=%s\ndstar.voice=%s\ndstar.rx=%u\ndstar.playing=%u\n", d->on,
		word(d->fault, fault_names, 4), word(d->voice_off, voice_names, 3), d->rx, d->playing);
	dl_text(t, "dstar.heard=%u\ndstar.heard_slow=%u\ndstar.heard_ms=%u\n", d->heard, d->heard_slow,
		(unsigned)d->heard_ms);
	dl_text_kv_str(t, "dstar.my", d->my);
	dl_text_kv_str(t, "dstar.suffix", d->suffix);
	dl_text_kv_str(t, "dstar.ur", d->ur);
	dl_text_kv_str(t, "dstar.rpt1", d->rpt1);
	dl_text_kv_str(t, "dstar.rpt2", d->rpt2);
	dl_text_kv_str(t, "dstar.msg", d->msg);
	dl_text_kv_str(t, "dstar.tx_my", d->tx_my);
	dl_text_kv_str(t, "dstar.tx_suffix", d->tx_suffix);
	dl_text_kv_str(t, "dstar.tx_ur", d->tx_ur);
	dl_text_kv_str(t, "dstar.tx_rpt1", d->tx_rpt1);
	dl_text_kv_str(t, "dstar.tx_rpt2", d->tx_rpt2);
	dl_text(t, "dstar.ur_mode=%s\ndstar.tx_repeater=%u\ndstar.no_call=%u\n", word(d->ur_mode, ur_names, 7),
		d->tx_repeater, d->no_call);
	dl_text(t, "dstar.rx_samples=%u\ndstar.rx_overs=%u\ndstar.rx_frames=%u\ndstar.rx_played=%u\n"
		"dstar.rx_concealed=%u\ndstar.rx_underruns=%u\ndstar.rx_overflows=%u\ndstar.rx_bad=%u\n"
		"dstar.rx_bit_errors=%u\n", (unsigned)s.rx_samples, (unsigned)s.rx_overs, (unsigned)s.rx_frames,
		(unsigned)s.rx_played, (unsigned)s.rx_concealed, (unsigned)s.rx_underruns, (unsigned)s.rx_overflows,
		(unsigned)s.rx_bad, (unsigned)s.rx_bit_errors);
	dl_text(t, "dstar.polarity=%d\ndstar.hdr_corrected=%u\n", s.polarity, s.hdr_corrected);
	dl_text(t, "dstar.tx_overs=%u\ndstar.tx_frames=%u\ndstar.tx_underruns=%u\ndstar.tx_overflows=%u\n"
		"dstar.tx_samples=%u\ndstar.tx_peak=%u\ndstar.blocks=%u\n", (unsigned)s.tx_overs, (unsigned)s.tx_frames,
		(unsigned)s.tx_underruns, (unsigned)s.tx_overflows, (unsigned)s.tx_samples, (unsigned)s.tx_peak,
		(unsigned)s.blocks);
	dl_text(t, "dstar.test=%s\n", word(radio_dstar_test_kind(), test_names, RDSTAR_TEST_COUNT));
	// The calibration: the board's values and the settings over them (0 = the board's)
	const hal_audio_tap_t *tap = board.audio_tap;
	const audio_tap_flat_cal_t *c = tap != NULL && tap->flat_cal != NULL ? tap->flat_cal() : NULL;
	if (c != NULL) {
		dl_text(t, "dstar.board_tx_1khz=%d\ndstar.board_tx_invert=%u\ndstar.board_rx_invert=%u\n"
			"dstar.board_tx_hpf_hz=%u\ndstar.board_rx_hpf_hz=%u\ndstar.board_tx_latency_ms=%u\n", c->tx_1khz,
			c->tx_invert, c->rx_invert, c->tx_hpf_hz, c->rx_hpf_hz, c->tx_latency_ms);
	}
	settings_t *st = settings_begin();
	int16_t tx_1khz = st->flat_tx_1khz;
	uint8_t flags = st->flat_flags;
	uint16_t tx_hpf = st->flat_tx_hpf, rx_hpf = st->flat_rx_hpf;
	settings_end(0);
	dl_text(t, "dstar.set_tx_1khz=%d\ndstar.set_flags=%u\ndstar.set_tx_hpf=%d\ndstar.set_rx_hpf=%d\n", tx_1khz, flags,
		tx_hpf == 0 ? -1 : (int)tx_hpf - 1, rx_hpf == 0 ? -1 : (int)rx_hpf - 1);
	for (uint8_t i = 0; tap != NULL && tap->flat_knob != NULL && i < tap->flat_knob_count && !t->full; i++) {
		const char *name = NULL;
		int32_t val = 0;
		if (tap->flat_knob(i, &name, &val, NULL) && name != NULL) {
			dl_text(t, "dstar.knob.%s=%d\n", name, (int)val);
		}
	}
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// DL_DSTAR_OP_CAL: field u8, value i32. Stored in the settings, taken by the radio core at once (a new over)
static uint8_t set_cal(uint8_t field, int32_t val)
{
	uint8_t st = DL_ST_OK;
	settings_t *s = settings_begin();
	switch (field) {
	case DL_DSTAR_CAL_TX_1KHZ:
		if (val < -32000 || val > 32000) {
			st = DL_ST_BAD_ARG;
			break;
		}
		s->flat_tx_1khz = (int16_t)val;
		break;
	case DL_DSTAR_CAL_TX_INVERT:
	case DL_DSTAR_CAL_RX_INVERT: {
		bool tx = field == DL_DSTAR_CAL_TX_INVERT;
		uint8_t set = tx ? SETTINGS_FLAT_TX_INV_SET : SETTINGS_FLAT_RX_INV_SET;
		uint8_t on = tx ? SETTINGS_FLAT_TX_INV : SETTINGS_FLAT_RX_INV;
		s->flat_flags = (uint8_t)(s->flat_flags & ~(set | on));
		if (val >= 0) {	// -1: the board's
			s->flat_flags = (uint8_t)(s->flat_flags | set | (val != 0 ? on : 0u));
		}
		break;
	}
	case DL_DSTAR_CAL_TX_HPF:
	case DL_DSTAR_CAL_RX_HPF:
		if (val < -1 || val > 2000) {
			st = DL_ST_BAD_ARG;
			break;
		}
		if (field == DL_DSTAR_CAL_TX_HPF) {
			s->flat_tx_hpf = (uint16_t)(val + 1);	// -1: the board's (stored 0)
		} else {
			s->flat_rx_hpf = (uint16_t)(val + 1);
		}
		break;
	default:
		st = DL_ST_BAD_ARG;
		break;
	}
	settings_end(hal_millis());
	if (st == DL_ST_OK) {
		radio_req_dstar_reload();
	}
	return st;
}

// DL_DSTAR_OP_STATION: field u8, the text. Empty text: the codeplug's again
static uint8_t set_station(uint8_t field, const uint8_t *a, uint16_t n)
{
	char text[RADIO_DSTAR_MSG + 1];
	if (n > RADIO_DSTAR_MSG) {
		return DL_ST_BAD_ARG;
	}
	memcpy(text, a, n);
	text[n] = '\0';
	settings_t *s = settings_begin();
	switch (field) {
	case DL_DSTAR_ST_MY:
		dstar_field_set(s->dstar_my, sizeof s->dstar_my, text);
		break;
	case DL_DSTAR_ST_SUFFIX:
		dstar_field_set(s->dstar_suffix, sizeof s->dstar_suffix, text);
		break;
	case DL_DSTAR_ST_MSG:
		dstar_msg_set(s->dstar_msg, text);
		break;
	default:
		settings_end(0);
		return DL_ST_BAD_ARG;
	}
	settings_end(hal_millis());
	radio_req_dstar_reload();
	return DL_ST_OK;
}

size_t debuglink_cmd_dstar(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	uint8_t st = DL_ST_OK;
	(void)cap;
	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	const uint8_t *a = req + 1;
	uint16_t n = (uint16_t)(len - 1u);
	const hal_audio_tap_t *tap = board.audio_tap;
	switch (req[0]) {
	case DL_DSTAR_OP_CAL:
		st = n == 5 ? set_cal(a[0], (int32_t)get32(a + 1)) : DL_ST_BAD_ARG;
		break;
	case DL_DSTAR_OP_TEST:
		// kind u8, hz u16, level u8: refused mid-over (the next transmission takes it)
		if (n != 4 || a[0] >= RDSTAR_TEST_COUNT) {
			st = DL_ST_BAD_ARG;
			break;
		}
		if (radio_get_state() == RS_TX) {
			st = DL_ST_BUSY;
			break;
		}
		radio_dstar_test(a[0], (uint16_t)(a[1] | a[2] << 8), a[3]);
		break;
	case DL_DSTAR_OP_UR: {
		// mode u8, then up to seven characters (the reflector and module for a link)
		if (n < 1 || n > 8) {
			st = DL_ST_BAD_ARG;
			break;
		}
		char ref[8] = { 0 };
		memcpy(ref, a + 1, (size_t)(n - 1u));
		radio_req_dstar_ur(a[0], ref);
		break;
	}
	case DL_DSTAR_OP_STATION:
		st = n >= 1 ? set_station(a[0], a + 1, (uint16_t)(n - 1u)) : DL_ST_BAD_ARG;
		break;
	case DL_DSTAR_OP_KNOB: {
		// index u8, value i32: one of the board's flat path knobs (names in the view's dstar.knob.* keys)
		if (n != 5 || tap == NULL || tap->flat_knob == NULL || a[0] >= tap->flat_knob_count) {
			st = n != 5 ? DL_ST_BAD_ARG : DL_ST_UNAVAILABLE;
			break;
		}
		const char *name = NULL;
		int32_t val = (int32_t)get32(a + 1);
		st = tap->flat_knob(a[0], &name, &val, &val) ? DL_ST_OK : DL_ST_REFUSED;
		break;
	}
	case DL_DSTAR_OP_RESET:
		radio_dstar_reset_stats();
		break;
	default:
		st = DL_ST_BAD_ARG;
		break;
	}
	out[0] = st;
	return 1;
}

#else

void debuglink_view_dstar(const radio_view_t *v, dl_text_t *t)
{
	(void)v;
	dl_text(t, "dstar.built=0\n");
}

size_t debuglink_cmd_dstar(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

#endif
