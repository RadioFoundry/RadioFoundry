// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// APRS on the debug link (docs/spec/debuglink.md 3.14, docs/spec/aprs.md 13): the "aprs" view section and the APRS
// command (send now, status, message, a RAM-only test callsign, configuration fields, NMEA injection). Built without
// CONFIG_APRS: "aprs.built=0" and the command answers unavailable.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

#if CONFIG_APRS
#include "hal_time.h"
#include "aprs/aprs.h"
#include "aprs/aprs_beacon.h"

static const char *word(uint8_t v, const char *const *names, uint8_t n)
{
	return v < n ? names[v] : "?";
}

static unsigned deg_int(int32_t e7)
{
	return (unsigned)((e7 < 0 ? -(int64_t)e7 : e7) / 10000000);
}

static unsigned deg_frac(int32_t e7)
{
	return (unsigned)((e7 < 0 ? -(int64_t)e7 : e7) % 10000000);
}

static void deg(dl_text_t *t, const char *key, int32_t e7)
{
	dl_text(t, "%s=%s%u.%07u\n", key, e7 < 0 ? "-" : "", deg_int(e7), deg_frac(e7));
}

static void view_settings(dl_text_t *t)
{
	static const char *const vias[] = { "rf", "dmr", "both" };
	aprs_cfg_t c;

	aprs_get_cfg(&c);
	dl_text(t, "aprs.built=1\n");
	dl_text(t, "set.aprs_tx=%u\nset.aprs_region=%u\nset.aprs_ssid=%u\nset.aprs_symbol=%c%c\nset.aprs_path=%s\n",
		c.tx_on, c.region, c.ssid, c.sym_table, c.sym_code, aprs_path_text(c.path));
	dl_text(t, "set.aprs_interval_s=%u\nset.aprs_smart=%u\nset.aprs_distance_m=%u\nset.aprs_after_ptt=%u\n",
		c.interval_s, c.smart, c.distance_m, c.after_ptt);
	dl_text(t, "set.aprs_boot=%u\nset.aprs_level=%u\nset.aprs_txdelay=%u\nset.aprs_via=%s\nset.aprs_dmr_dst=%u\n",
		c.boot, c.level, c.txdelay, word(c.via, vias, 3), (unsigned)c.dmr_dst);
	dl_text(t, "set.aprs_dmr_udt=%u\nset.aprs_pos_src=%u\nset.aprs_kiss=%u\nset.aprs_rx=%u\nset.aprs_igate=%u\n",
		c.dmr_udt, c.pos_src, c.kiss, c.rx_on, c.igate);
	dl_text(t, "set.aprs_igate_tx=%u\nset.aprs_digi=%u\nset.aprs_dmr_gps=%u\n", c.igate_tx, c.digi, c.dmr_gps);
}

void debuglink_view_aprs(uint8_t part, dl_text_t *t)
{
	static const char *const blocks[] = { "ok", "no_call", "no_freq", "not_amateur", "no_position", "no_afsk",
					      "busy" };
	static const char *const srcs[] = { "none", "gps", "host", "fixed" };
	static const char *const whys[] = { "none", "interval", "smart", "turn", "distance", "ptt", "manual", "boot" };
	aprs_view_t v;

	if (part == 1) {
		view_settings(t);
		return;
	}
	aprs_get_view(&v);
	dl_text(t, "aprs.built=1\naprs.rx_built=%u\naprs.igate_built=%u\naprs.call=%s\naprs.block=%s\naprs.freq_hz=%u\n",
		CONFIG_APRS_RX, CONFIG_APRS_IGATE, v.call, word(v.block, blocks, 7), (unsigned)v.freq_hz);
	dl_text(t, "aprs.pos_src=%s\naprs.pos_valid=%u\n", word(v.pos_src, srcs, 4), v.pos_valid);
	if (v.pos_valid) {
		deg(t, "aprs.lat", v.lat);
		deg(t, "aprs.lon", v.lon);
		dl_text(t, "aprs.pos_age_s=%u\n", (unsigned)v.pos_age_s);
	}
	dl_text(t, "aprs.next_s=%d\naprs.last_why=%s\naprs.last_ms=%u\naprs.rf_active=%u\n",
		v.next_s == 0xFFFFu ? -1 : (int)v.next_s, word(v.last_why, whys, 8), (unsigned)v.last_ms, v.rf_active);
	dl_text_kv_str(t, "aprs.last_info", v.last_info);
	dl_text(t, "aprs.rf_sent=%u\naprs.rf_failed=%u\naprs.rf_busy=%u\naprs.dmr_sent=%u\naprs.dmr_failed=%u\n",
		(unsigned)v.rf_sent, (unsigned)v.rf_failed, (unsigned)v.rf_busy, (unsigned)v.dmr_sent,
		(unsigned)v.dmr_failed);
	dl_text(t, "aprs.rx_frames=%u\naprs.rx_bad=%u\naprs.host_in=%u\naprs.host_out=%u\naprs.host_dropped=%u\n",
		(unsigned)v.rx_frames, (unsigned)v.rx_bad, (unsigned)v.host_in, (unsigned)v.host_out,
		(unsigned)v.host_dropped);
	dl_text(t, "aprs.igated=%u\naprs.is_to_rf=%u\naprs.digi_sent=%u\naprs.stations=%u\naprs.messages=%u\n",
		(unsigned)v.igated, (unsigned)v.is_to_rf, (unsigned)v.digi_sent, v.stations, v.messages);
	dl_text(t, "aprs.msg_pending=%u\naprs.msg_tries=%u\naprs.msg_acked=%u\n", v.msg_pending, v.msg_tries,
		v.msg_acked);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The text after the op byte, NUL terminated (n <= DL_REQ_MAX - 1)
static void text_arg(char *dst, const uint8_t *a, uint16_t n)
{
	memcpy(dst, a, n);
	dst[n] = '\0';
}

// DL_APRS_OP_SET: field u8, value u32 (signed fields as two's complement)
static uint8_t set_field(uint8_t field, uint32_t val)
{
	aprs_cfg_t c;

	aprs_get_cfg(&c);
	switch (field) {
	case DL_APRS_F_TX: c.tx_on = val != 0; break;
	case DL_APRS_F_REGION:
		if (val >= APRS_REGION_COUNT) {
			return DL_ST_BAD_ARG;
		}
		c.region = (uint8_t)val;
		break;
	case DL_APRS_F_FREQ_HZ: c.freq_hz = val; break;
	case DL_APRS_F_SSID:
		if (val > 15) {
			return DL_ST_BAD_ARG;
		}
		c.ssid = (uint8_t)val;
		break;
	case DL_APRS_F_LAT: c.fix_lat = (int32_t)val; break;
	case DL_APRS_F_LON: c.fix_lon = (int32_t)val; break;
	case DL_APRS_F_POS_SRC:
		if (val >= APRS_POS_COUNT) {
			return DL_ST_BAD_ARG;
		}
		c.pos_src = (uint8_t)val;
		break;
	case DL_APRS_F_INTERVAL_S: c.interval_s = (uint16_t)val; break;
	case DL_APRS_F_SMART: c.smart = val != 0; break;
	case DL_APRS_F_VIA:
		if (val >= APRS_VIA_COUNT) {
			return DL_ST_BAD_ARG;
		}
		c.via = (uint8_t)val;
		break;
	case DL_APRS_F_DMR_DST: c.dmr_dst = val; break;
	case DL_APRS_F_LEVEL:
		if (val > 100) {
			return DL_ST_BAD_ARG;
		}
		c.level = (uint8_t)val;
		break;
	case DL_APRS_F_PATH:
		if (val >= APRS_PATH_COUNT) {
			return DL_ST_BAD_ARG;
		}
		c.path = (uint8_t)val;
		break;
	case DL_APRS_F_KISS: c.kiss = val != 0; break;
	case DL_APRS_F_DMR_UDT: c.dmr_udt = val != 0; break;
	case DL_APRS_F_AFTER_PTT: c.after_ptt = val != 0; break;
	case DL_APRS_F_DISTANCE_M: c.distance_m = (uint16_t)val; break;
	case DL_APRS_F_RX: c.rx_on = val != 0; break;
	case DL_APRS_F_IGATE: c.igate = val != 0; break;
	case DL_APRS_F_IGATE_TX: c.igate_tx = val != 0; break;
	case DL_APRS_F_DIGI: c.digi = (uint8_t)val; break;
	case DL_APRS_F_DMR_GPS: c.dmr_gps = val != 0; break;
	case DL_APRS_F_TXDELAY:
		if (val < 5 || val > 100) {
			return DL_ST_BAD_ARG;
		}
		c.txdelay = (uint8_t)val;
		break;
	default:
		return DL_ST_BAD_ARG;
	}
	aprs_set_cfg(&c, hal_millis());
	return DL_ST_OK;
}

size_t debuglink_cmd_aprs(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	char text[DL_REQ_MAX];
	uint8_t st = DL_ST_OK;

	(void)cap;
	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	const uint8_t *a = req + 1;
	uint16_t n = (uint16_t)(len - 1u);
	switch (req[0]) {
	case DL_APRS_OP_SEND_NOW:
		st = aprs_send_now() ? DL_ST_OK : DL_ST_REFUSED;
		break;
	case DL_APRS_OP_STATUS:
		text_arg(text, a, n);
		st = aprs_send_status(text) ? DL_ST_OK : DL_ST_REFUSED;
		break;
	case DL_APRS_OP_MESSAGE: {
		// "CALL text": the addressee, one space, the text
		text_arg(text, a, n);
		char *sp = strchr(text, ' ');
		if (sp == NULL) {
			st = DL_ST_BAD_ARG;
			break;
		}
		*sp = '\0';
		st = aprs_send_message(text, sp + 1) ? DL_ST_OK : DL_ST_BUSY;
		break;
	}
	case DL_APRS_OP_TEST_CALL:
		if (n > 9) {
			st = DL_ST_BAD_ARG;
			break;
		}
		text_arg(text, a, n);
		aprs_set_test_call(text);
		break;
	case DL_APRS_OP_SET:
		st = n == 5 ? set_field(a[0], get32(a + 1)) : DL_ST_BAD_ARG;
		break;
	case DL_APRS_OP_NMEA:
		aprs_nmea((const char *)a, n, false, hal_millis());
		break;
	default:
		st = DL_ST_BAD_ARG;
		break;
	}
	out[0] = st;
	return 1;
}

#else

void debuglink_view_aprs(uint8_t part, dl_text_t *t)
{
	(void)part;
	dl_text(t, "aprs.built=0\n");
}

size_t debuglink_cmd_aprs(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

#endif
