// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's commands (docs/spec/debuglink.md). Everything runs in the host link task, between the bytes of the
// other protocols: each command is short (a reply of at most DL_REPLY_MAX bytes, no waiting on other tasks; radio
// requests are queued, injected keys are timed by the input task).
#include <string.h>
#include "config.h"
#include "radio/beep.h"
#include "board.h"
#include "hal_time.h"
#include "debuglink/debuglink.h"
#include "debuglink/dl_mem.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_chip.h"
#include "debuglink/dlog.h"
#include "hostlink/hostlink.h"
#include "stubentry/stub_entry.h"
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#endif
#if CONFIG_CPS_FAST
#include "cps/cps_fast.h"
#endif
#include "input/input.h"
#include "radio/radio.h"
#include "storage/storage.h"
#include "settings/settings.h"
#include "ui/gfx.h"

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "unknown"
#endif
#ifndef RFOUNDRY_RELEASE
#define RFOUNDRY_RELEASE ""
#endif

#define SHOT_HOLD_MS		1000	// a screen capture freezes UI redraws at most this long
#define SHOT_TAIL_MS		250	// after the last chunk: long enough for the host's check pass to start
#define SHOT_CHUNK_MAX		1000u	// screenshot bytes per reply
#define RADIO_OP_GAP_MS		20u	// radio requests at most this often (the radio core's queue holds 16)
#define REBOOT_DELAY_MS		250u	// the REBOOT reply goes out over USB before the reset

// Radio operations (DL_CMD_RADIO, first payload byte)
enum {
	OP_CHANNEL_STEP = 1, OP_ZONE_POS, OP_ZONE, OP_VFO, OP_VFO_SET, OP_VFO_STEP, OP_POWER, OP_SQUELCH, OP_BANDWIDTH,
	OP_TONE, OP_MONITOR, OP_CLEAR_TEMP, OP_REPLY_CANCEL, OP_HOTSPOT_EXIT,
	OP_DMR_TARGET, OP_DMR_CONTACT, OP_DMR_CALL_BACK, OP_DMR_TARGET_CLEAR, OP_DMR_PRIVACY,
	// 20-24: the radio features' (DL_OP_SCAN .. DL_OP_SETTING, debuglink_view_radio.c)
	OP_USB_NETWORK = 25,
	// 26: DMR roaming (DL_OP_ROAM, debuglink_view_radio.c)
	OP_USB_NET_SETTINGS = 27,	// flags u8, link IP u32 (stored; docs/spec/usb-network.md 5)
	OP_PPP = 28,			// on u8: PPP on the serial host port (docs/spec/ppp.md; CONFIG_NET_PPP builds)
	// 29: a vibration pattern (DL_OP_VIBRATE, debuglink_view_radio.c)
	// 30: the speaker volume (DL_OP_VOLUME, debuglink_view_radio.c)
	// 31, 32: DMR fields, Copy to VFO (DL_OP_DMR_FIELDS, DL_OP_VFO_COPY, debuglink_view_radio.c)
	// 33, 34: the DMR voice test signal, a DTMF key down or up (DL_OP_VOICE_TEST, DL_OP_DTMF, debuglink_view_radio.c)
};

static dl_parser_t parser;
static uint8_t reply[DL_HDR + DL_REPLY_MAX + 2u];
static radio_view_t view;	// static: too large for the host link task's stack
static uint32_t last_radio_op_ms;
static bool radio_op_done;
static bool reboot_pending;
static uint8_t reboot_mode;
static uint32_t reboot_token;

static const char features[] = "features="
#if CONFIG_FM
	"FM,"
#endif
#if CONFIG_SUBTONE
	"SUBTONE,"
#endif
#if CONFIG_DMR
	"DMR,"
#endif
#if CONFIG_DMR_PRIVACY
	"DMR_PRIVACY,"
#endif
#if CONFIG_DMR_TALKER_ALIAS
	"DMR_TALKER_ALIAS,"
#endif
#if CONFIG_DMR_GPS
	"DMR_GPS,"
#endif
#if CONFIG_APRS
	"APRS,"
#endif
#if CONFIG_APRS_RX
	"APRS_RX,"
#endif
#if CONFIG_APRS_IGATE
	"APRS_IGATE,"
#endif
#if CONFIG_DMR_DATA_CORE
	"DMR_DATA_CORE,"
#endif
#if CONFIG_DMR_SMS
	"DMR_SMS,"
#endif
#if CONFIG_DMR_ARS
	"DMR_ARS,"
#endif
#if CONFIG_DMR_LRRP
	"DMR_LRRP,"
#endif
#if CONFIG_DMR_SIGNALING
	"DMR_SIGNALING,"
#endif
#if CONFIG_DMR_ROAMING
	"DMR_ROAMING,"
#endif
#if CONFIG_SFR
	"SFR,"
#endif
#if CONFIG_HOTSPOT
	"HOTSPOT,"
#endif
#if CONFIG_HOTSPOT_SPLIT
	"HOTSPOT_SPLIT,"
#endif
#if CONFIG_USB_NETWORK
	"USB_NETWORK,"
#endif
#if CONFIG_DMR_IP_GATEWAY
	"DMR_IP_GATEWAY,"
#endif
#if CONFIG_DMR_UDP_COMPRESS && CONFIG_DMR_DATA_CORE
	"DMR_UDP_COMPRESS,"
#endif
#if CONFIG_DMR_CONFIRMED && CONFIG_DMR_DATA_CORE
	"DMR_CONFIRMED,"
#endif
#if CONFIG_NET_TCP
	"NET_TCP,"
#endif
#if CONFIG_NET_PPP
	"NET_PPP,"
#endif
#if CONFIG_NET_KISS
	"NET_KISS,"
#endif
#if CONFIG_NET_HTTP
	"NET_HTTP,"
#endif
#if CONFIG_NET_HTTP_GZIP
	"NET_HTTP_GZIP,"
#endif
#if CONFIG_NET_RIGCTL
	"NET_RIGCTL,"
#endif
#if CONFIG_NET_APRSIS
	"NET_APRSIS,"
#endif
#if CONFIG_NET_USRP
	"NET_USRP,"
#endif
#if CONFIG_MDNS
	"MDNS,"
#endif
#if CONFIG_TFTP
	"TFTP,"
#endif
#if CONFIG_TFTP_IDDB
	"TFTP_IDDB,"
#endif
#if CONFIG_SNMP
	"SNMP,"
#endif
#if CONFIG_VFO
	"VFO,"
#endif
#if CONFIG_VFO_FM
	"VFO_FM,"
#endif
#if CONFIG_VFO_DMR
	"VFO_DMR,"
#endif
#if CONFIG_AM_RX
	"AM_RX,"
#endif
#if CONFIG_BCAST_FM
	"BCAST_FM,"
#endif
#if CONFIG_DIAG
	"DIAG,"
#endif
#if CONFIG_CRASH_LOG
	"CRASH_LOG,"
#endif
#if CONFIG_SCAN
	"SCAN,"
#endif
#if CONFIG_BEEP
	"BEEP,"
#endif
#if CONFIG_FM_TX_TONES
	"FM_TX_TONES,"
#endif
#if CONFIG_DTMF
	"DTMF,"
#endif
#if CONFIG_VOX
	"VOX,"
#endif
#if CONFIG_BATTERY_SAVE
	"BATTERY_SAVE,"
#endif
#if CONFIG_CPS_FAST
	"CPS_FAST,"
#endif
#if CONFIG_USB
	"USB,"
#endif
	"HOSTLINK,DEBUGLINK\n";	// the debug link always runs on the host link (USB CDC or a UART)

// --- little endian helpers ---

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static size_t status_only(uint8_t *out, uint8_t st)
{
	out[0] = st;
	return 1;
}

// --- diagnostics lines: the radio summary, the DMR voice path, then the board's ---

#define SUMMARY_LINES	2

static const char *state_name(uint8_t s)
{
	switch (s) {
	case RS_IDLE:
		return "IDLE";
	case RS_RX:
		return "RX";
	case RS_TX:
		return "TX";
	case RS_TX_DENIED:
		return "TXDENY";
	default:
		return "?";
	}
}

static bool summary_line(uint8_t i, char *buf, uint8_t len)
{
	const char *pw = (view.power < board.power_count && board.power_names != NULL) ? board.power_names[view.power]
										       : "?";
	switch (i) {
	case 0:
		dl_fmt(buf, len, "radio %s %s ch %u '%s' %u.%05u %s%s", view.ch.mode == RADIO_MODE_DMR ? "DMR" : "FM",
		       view.vfo ? "VFO" : "MEM", view.channel_number, view.ch.name, (unsigned)(view.ch.rx_hz / 1000000u),
		       (unsigned)((view.ch.rx_hz % 1000000u) / 10u), state_name((uint8_t)view.state),
		       view.hotspot ? " HOTSPOT" : "");
		return true;
	case 1:
		dl_fmt(buf, len, "radio rssi %d nz %u sq %u%s pw %s tx %ums", view.rssi_dbm, view.noise, view.squelch,
		       view.monitor ? " mon" : "", pw, (unsigned)view.tx_elapsed_ms);
		return true;
	default:
		return false;
	}
}

#if CONFIG_DMR
#define DMR_LINES	DL_DMR_DIAG_LINES
#else
#define DMR_LINES	0
#endif

static bool diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (i < SUMMARY_LINES) {
		return summary_line(i, buf, len);
	}
	i -= SUMMARY_LINES;
#if CONFIG_DMR
	if (i < DMR_LINES) {
		if (debuglink_dmr_diag_line(i, buf, len)) {
			return true;
		}
		buf[0] = '\0';
		return true;	// a line the DMR side has nothing for: keep the numbering
	}
#endif
	i -= DMR_LINES;
	return (board.diag_line != NULL) && board.diag_line(i, buf, len);
}

static size_t cmd_get_diag(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	uint8_t first = len >= 1 ? req[0] : 0;
	dl_text_t t;
	char line[64];
	uint8_t i = first;

	radio_get_view(&view);
	out[0] = DL_ST_OK;
	put32(out + 1, hal_millis());
	dl_text_init(&t, (char *)out + 6, cap - 6);
	for (;; i++) {
		if (!diag_line(i, line, sizeof(line))) {
			i = 0xFF;
			break;
		}
		dl_text(&t, "%s\n", line);
		if (t.full || i == 0xFE) {
			if (!t.full) {
				i++;
			}
			break;
		}
	}
	out[5] = i;	// next line to ask for, 0xFF: done
	return 6 + t.len;
}

static size_t cmd_reset(uint8_t *out)
{
	if (board.diag_reset != NULL) {
		board.diag_reset();
	}
#if CONFIG_DMR
	debuglink_dmr_reset();
#endif
#if CONFIG_BEEP
	beep_reset_stats();
#endif
	debuglink_dmr_data_reset();
	uint32_t now = hal_millis();
	dlog("debuglink: counters reset");
	out[0] = DL_ST_OK;
	put32(out + 1, now);
	return 5;
}

// --- version ---

static size_t cmd_version(uint8_t *out, size_t cap)
{
	dl_text_t t;
	const hal_display_t *d = board.display;

	out[0] = DL_ST_OK;
	dl_text_init(&t, (char *)out + 1, cap - 1);
	dl_text(&t, "proto=%u\nview=%u\n", DL_PROTO_VERSION, DL_VIEW_VERSION);
	dl_text_kv_str(&t, "git", RFOUNDRY_GIT);
	if (RFOUNDRY_RELEASE[0] != '\0') {
		dl_text_kv_str(&t, "release", RFOUNDRY_RELEASE);
	}
	dl_text_kv_str(&t, "board", board.name);
	dl_text(&t, "%s", features);
	dl_text_kv_str(&t, "codec", debuglink_codec_id(board.vocoder));
	if (d != NULL) {
		dl_text(&t, "display=%ux%ux%u\ninset=%u,%u,%u,%u\n", d->width, d->height, d->bpp, d->inset_top,
			d->inset_bottom, d->inset_left, d->inset_right);
	}
	dl_text(&t, "uptime_ms=%u\nreq_max=%u\nreply_max=%u\ninject_max_ms=%u\n", (unsigned)hal_millis(), DL_REQ_MAX,
		DL_REPLY_MAX, INPUT_INJECT_MAX_MS);
	return 1 + t.len;
}

// --- screenshot: the UI frame buffer, read live (see the doc on tearing) ---

static size_t cmd_screenshot(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	const hal_display_t *d = board.display;

	if (len != 0 && len != 6) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (d == NULL || gfx_width() <= 0 || gfx_framebuffer() == NULL) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	// A 16-color frame (bpp 4) goes out as RGB565, read through its palette
	bool indexed = GFX_INDEXED && d->bpp == 4;
	uint32_t stride = d->bpp == 1 ? (d->width + 7u) / 8u : d->width * 2u;
	uint32_t total = stride * d->height;
	if (!indexed && total > GFX_FB_BYTES) {
		total = GFX_FB_BYTES;
	}
	uint32_t offset = len == 6 ? get32(req) : 0;
	uint32_t n = len == 6 ? get16(req + 4) : 0;
	if (offset > total) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (n > SHOT_CHUNK_MAX) {
		n = SHOT_CHUNK_MAX;
	}
	if (n > total - offset) {
		n = total - offset;
	}
	if (19u + n > cap) {
		n = (uint32_t)cap - 19u;
	}
	out[0] = DL_ST_OK;
	put16(out + 1, d->width);
	put16(out + 3, d->height);
	out[5] = indexed ? 16 : d->bpp;
	out[6] = d->bpp == 1 ? 2 : 1;	// 1: RGB565 little endian, 2: 1 bpp rows, MSB left
	out[7] = d->inset_top;
	out[8] = d->inset_bottom;
	out[9] = d->inset_left;
	out[10] = d->inset_right;
	put32(out + 11, total);
	put32(out + 15, offset);
	// Hold the UI's redraws while the host reads the chunks, so the capture is one frame. Every chunk extends the
	// hold, so the host's check pass sees the same frame; redraws resume SHOT_TAIL_MS after the last chunk (not the
	// full hold: a key pressed right after a capture must show up in the next one)
	gfx_hold(hal_millis() + (len == 6 && offset + n >= total ? SHOT_TAIL_MS : SHOT_HOLD_MS));
	if (indexed) {
		gfx_read_rgb565(offset, out + 19, n);
	} else {
		memcpy(out + 19, (const uint8_t *)gfx_framebuffer() + offset, n);
	}
	return 19u + n;
}

// --- input ---

static uint8_t inject_status(input_inject_t r)
{
	switch (r) {
	case INPUT_INJ_OK:
		return DL_ST_OK;
	case INPUT_INJ_BUSY:
		return DL_ST_BUSY;
	case INPUT_INJ_NO_KEY:
		return DL_ST_UNAVAILABLE;
	default:
		return DL_ST_BAD_ARG;
	}
}

static size_t cmd_key(const uint8_t *req, uint16_t len, uint8_t *out)
{
	if (len >= 4 && req[0] == 0) {
		return status_only(out, inject_status(input_inject_action(req[1], req[2], (int8_t)req[3])));
	}
	if (len >= 7 && req[0] == 1) {
		return status_only(out, inject_status(input_inject_keys(get32(req + 1), get16(req + 5))));
	}
	if (len >= 1 && req[0] == 2) {
		input_inject_wake();
		return status_only(out, DL_ST_OK);
	}
	if (len >= 4 && req[0] == 3) {
		return status_only(out, inject_status(input_inject_action_hold(req[1], get16(req + 2))));
	}
	return status_only(out, DL_ST_BAD_ARG);
}

static size_t cmd_ptt(const uint8_t *req, uint16_t len, uint8_t *out)
{
	if (len != 1 && len != 3) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	bool down = req[0] != 0;
	uint32_t max_ms = len == 3 ? get16(req + 1) : 0;
	if (max_ms == 0 || max_ms > INPUT_INJECT_MAX_MS) {
		max_ms = INPUT_INJECT_MAX_MS;
	}
	uint8_t st = inject_status(input_inject_ptt(down, max_ms));
	if (st == DL_ST_OK) {
		dlog("debuglink: PTT %s (max %u ms)", down ? "down" : "up", (unsigned)max_ms);
	}
	out[0] = st;
	put16(out + 1, (uint16_t)max_ms);
	return 3;
}

// --- log ---

static size_t cmd_get_log(uint8_t *out, size_t cap)
{
	uint8_t more;
	uint32_t dropped;
	size_t n = dlog_read((char *)out + 6, cap - 6, &more, &dropped);

	out[0] = DL_ST_OK;
	put32(out + 1, dropped);
	out[5] = more;
	return 6 + n;
}

// --- view ---

static size_t cmd_get_view(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	uint8_t section = len >= 1 ? req[0] : 0;
	dl_text_t t;

	if (section >= DL_VIEW_COUNT) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	radio_get_view(&view);
	out[0] = DL_ST_OK;
	put32(out + 1, hal_millis());
	out[5] = (uint8_t)(section + 1u < DL_VIEW_COUNT ? section + 1u : 0xFFu);
	dl_text_init(&t, (char *)out + 6, cap - 6 - 16);	// room kept for the truncation mark
	debuglink_view_section(section, &view, &t);
	if (t.full) {
		t.cap += 16;
		t.full = false;
		dl_text(&t, "truncated=1\n");
	}
	return 6 + t.len;
}

void debuglink_link_counters(uint32_t out[DL_LINK_COUNTERS])
{
	out[0] = parser.frames;
	out[1] = parser.bad_crc;
	out[2] = parser.too_long;
	out[3] = parser.timeouts;
}

// --- radio requests ---

static size_t cmd_radio(const uint8_t *req, uint16_t len, uint8_t *out)
{
	static const uint8_t arg_len[] = {
		[OP_CHANNEL_STEP] = 1, [OP_ZONE_POS] = 2, [OP_ZONE] = 2, [OP_VFO] = 1, [OP_VFO_SET] = 4, [OP_VFO_STEP] = 1,
		[OP_POWER] = 1, [OP_SQUELCH] = 1, [OP_BANDWIDTH] = 1, [OP_TONE] = 4, [OP_MONITOR] = 1, [OP_CLEAR_TEMP] = 0,
		[OP_REPLY_CANCEL] = 0, [OP_HOTSPOT_EXIT] = 0,
		[OP_DMR_TARGET] = 4, [OP_DMR_CONTACT] = 2, [OP_DMR_CALL_BACK] = 0, [OP_DMR_TARGET_CLEAR] = 0,
		[OP_DMR_PRIVACY] = 3, [OP_USB_NETWORK] = 1, [OP_USB_NET_SETTINGS] = 5,
#if CONFIG_NET_PPP
		[OP_PPP] = 1,
#endif
	};
	uint32_t now = hal_millis();

	if (len >= 1 && debuglink_is_feature_op(req[0])) {
		// The radio features' operations (debuglink_view_radio.c), under the same rate limit
		if (radio_op_done && (now - last_radio_op_ms) < RADIO_OP_GAP_MS) {
			return status_only(out, DL_ST_BUSY);
		}
		uint8_t st = debuglink_radio_feature_op(req[0], req + 1, (uint16_t)(len - 1u));
		if (st == DL_ST_OK) {
			last_radio_op_ms = now;
			radio_op_done = true;
		}
		return status_only(out, st);
	}
	if (len < 1 || req[0] == 0 || req[0] >= sizeof(arg_len) || len < 1u + arg_len[req[0]]) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (radio_op_done && (now - last_radio_op_ms) < RADIO_OP_GAP_MS) {
		return status_only(out, DL_ST_BUSY);
	}
	const uint8_t *a = req + 1;
	switch (req[0]) {
	case OP_CHANNEL_STEP:
		radio_req_channel_step((int8_t)a[0]);
		break;
	case OP_ZONE_POS:
		radio_req_zone_pos(get16(a));
		break;
	case OP_ZONE:
		radio_req_zone(get16(a));
		break;
	case OP_VFO:
		radio_req_vfo(a[0] != 0);
		break;
	case OP_VFO_SET:
		radio_req_vfo_set(get32(a));
		break;
	case OP_VFO_STEP:
		radio_req_vfo_step((int8_t)a[0]);
		break;
	case OP_POWER:
		radio_req_power((int8_t)a[0]);
		break;
	case OP_SQUELCH:
		radio_req_squelch((int8_t)a[0]);
		break;
	case OP_BANDWIDTH:
		if (a[0] > RADIO_BW_25K) {
			return status_only(out, DL_ST_BAD_ARG);
		}
		radio_req_bandwidth((radio_bw_t)a[0]);
		break;
	case OP_TONE: {
		subtone_t tone = { .type = a[1], .value = get16(a + 2) };
		if ((a[0] & RADIO_TONE_BOTH) == 0 || a[0] > RADIO_TONE_BOTH || tone.type > TONE_DCS_INV) {
			return status_only(out, DL_ST_BAD_ARG);
		}
		radio_req_tone(a[0], tone);
		break;
	}
	case OP_MONITOR:
		radio_req_monitor(a[0] != 0);
		break;
	case OP_CLEAR_TEMP:
		radio_req_clear_temporary();
		break;
	case OP_REPLY_CANCEL:
#if CONFIG_DMR
		radio_req_reply_cancel();
		break;
#else
		return status_only(out, DL_ST_UNAVAILABLE);
#endif
	case OP_HOTSPOT_EXIT:
		hostlink_hotspot_exit();
		break;
#if CONFIG_DMR
	case OP_DMR_TARGET:	// u32 LE: ID in bits 23:0, bit 24 private
		radio_req_dmr_target(get32(a) & 0xFFFFFFu, (get32(a) >> 24) & 1u);
		break;
	case OP_DMR_CONTACT:
		radio_req_dmr_contact(get16(a));
		break;
	case OP_DMR_CALL_BACK:
		radio_req_dmr_call_back();
		break;
	case OP_DMR_TARGET_CLEAR:
		radio_req_dmr_target_clear();
		break;
#else
	case OP_DMR_TARGET:
	case OP_DMR_CONTACT:
	case OP_DMR_CALL_BACK:
	case OP_DMR_TARGET_CLEAR:
		return status_only(out, DL_ST_UNAVAILABLE);
#endif
	case OP_DMR_PRIVACY:	// type u8, key u16 LE (RAM only, never stored)
#if CONFIG_DMR_PRIVACY
		radio_req_dmr_privacy(a[0], get16(a + 1));
		break;
#else
		return status_only(out, DL_ST_UNAVAILABLE);
#endif
	case OP_USB_NETWORK:
		if (!hostlink_request_usb_network(a[0] != 0)) {
			return status_only(out, DL_ST_UNAVAILABLE);
		}
		break;
#if CONFIG_NET_PPP
	case OP_PPP:
		hostlink_request_ppp(a[0] != 0);
		break;
#endif
	case OP_USB_NET_SETTINGS: {	// bit 0 network at power-on, 1 passthrough, 2 fixed link; the link IP (0 = default)
#if CONFIG_USB_NETWORK
		uint32_t ip = (uint32_t)a[1] | ((uint32_t)a[2] << 8) | ((uint32_t)a[3] << 16) | ((uint32_t)a[4] << 24);
		settings_t *st = settings_begin();

		st->usb_mode = (a[0] & 1u) ? 1 : 0;
		st->net_flags = (uint8_t)((st->net_flags & SETTINGS_NET_TFTP_NO_WRITE) |	// not this op's
					  (CONFIG_DMR_CONTROL_STATION && (a[0] & 2u) ? SETTINGS_NET_PASSTHROUGH : 0) |
					  (CONFIG_DMR_CONTROL_STATION && (a[0] & 4u) ? SETTINGS_NET_FIXED_LINK : 0));
		st->net_link_ip = ip;
		settings_end(now);
		hostlink_settings_changed();
		break;
#else
		return status_only(out, DL_ST_UNAVAILABLE);
#endif
	}
	default:
		return status_only(out, DL_ST_BAD_ARG);
	}
	last_radio_op_ms = now;
	radio_op_done = true;
	return status_only(out, DL_ST_OK);
}

// --- chip: identity and option bytes, memory reads, reboot ---

static bool read_chip(hal_chip_t *c)
{
	return board.system != NULL && board.system->chip != NULL && board.system->chip(c);
}

static size_t cmd_info_chip(uint8_t *out)
{
	hal_chip_t c;

	if (!read_chip(&c)) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	out[0] = DL_ST_OK;
	put32(out + 1, c.idcode);
	put16(out + 5, c.flash_kb);
	put32(out + 7, c.uid[0]);
	put32(out + 11, c.uid[1]);
	put32(out + 15, c.uid[2]);
	put32(out + 19, c.optcr);
	if (c.family == HAL_CHIP_STM32F4) {
		out[23] = dl_rdp_level(c.optcr);
		out[24] = dl_rdp_byte(c.optcr);
		put16(out + 25, dl_wrp_mask(c.optcr));
		out[27] = dl_bor_level(c.optcr);
		out[28] = dl_user_bits(c.optcr);
	} else {
		memset(out + 23, 0xFF, 6);	// no option bytes: nothing to decode
	}
	out[29] = c.family;
	return 30;
}

static size_t cmd_read_mem(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	hal_chip_t c;

	if (len != 6) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	uint32_t addr = get32(req);
	uint32_t n = get16(req + 4);
	if (n == 0 || n > DL_MEM_READ_MAX || 5u + n > cap) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	// The platform's windows, else the STM32F4's
	if (board.system == NULL || board.system->read == NULL) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	if (board.system->readable != NULL) {
		if (!board.system->readable(addr, n)) {
			return status_only(out, DL_ST_REFUSED);
		}
	} else if (!read_chip(&c) || c.family != HAL_CHIP_STM32F4) {
		return status_only(out, DL_ST_UNAVAILABLE);
	} else if (!dl_mem_allowed(addr, n, c.flash_kb)) {
		return status_only(out, DL_ST_REFUSED);
	}
	out[0] = DL_ST_OK;
	put32(out + 1, addr);
	board.system->read(addr, out + 5, n);
	return 5u + n;
}

static bool transmitting(void)
{
	radio_get_view(&view);
	if (view.state == RS_TX) {
		return true;
	}
#if CONFIG_HOTSPOT
	hotspot_view_t h;
	hotspot_get_view(&h);
	if (h.transmitting) {
		return true;
	}
#endif
	return false;
}

static uint8_t reboot_gate(uint8_t mode, uint32_t token)
{
	hal_chip_t c;
	bool have = read_chip(&c);
	return dl_reboot_check(mode, token, have ? &c : NULL, transmitting());
}

static size_t cmd_reboot(const uint8_t *req, uint16_t len, uint8_t *out)
{
	if (len != 5 || req[0] > HAL_REBOOT_ROM_DFU) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (board.system == NULL || board.system->reboot == NULL) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	uint8_t mode = req[0];
	uint32_t token = get32(req + 1);
	uint8_t why = reboot_gate(mode, token);
	if (why != DL_REBOOT_OK) {
		dlog("debuglink: reboot %u refused (%u)", mode, why);
		out[0] = DL_ST_REFUSED;
		out[1] = why;
		return 2;
	}
	reboot_pending = true;
	reboot_mode = mode;
	reboot_token = token;
	dlog("debuglink: reboot %s in %u ms", mode == HAL_REBOOT_ROM_DFU ? "ROM DFU" : "normal",
	     (unsigned)REBOOT_DELAY_MS);
	out[0] = DL_ST_OK;
	out[1] = DL_REBOOT_OK;
	return 2;
}

// After the reply went out: wait for USB to send it, check the gate again (a PTT may have come in), then reboot
static void reboot_now(void)
{
	reboot_pending = false;
	hal_delay_ms(REBOOT_DELAY_MS);
	uint8_t why = reboot_gate(reboot_mode, reboot_token);
	if (why != DL_REBOOT_OK) {
		dlog("debuglink: reboot %u aborted (%u)", reboot_mode, why);
		return;
	}
	if (!board.system->reboot(reboot_mode)) {
		dlog("debuglink: reboot %u failed (%u)", reboot_mode, DL_REBOOT_ARM_FAILED);
	}
}

// --- the settings / journal region (docs/spec/debuglink.md 3.13) ---

static size_t cmd_storage(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	const hal_flash_map_t *m = storage_map();

	if (len < 1) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (m == NULL || board.flash == NULL) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	switch (req[0]) {
	case DL_STORAGE_OP_INFO: {
		out[0] = DL_ST_OK;
		out[1] = (uint8_t)storage_region();
		out[2] = storage_claim_pending();
		put32(out + 3, m->settings_addr);
		put32(out + 7, m->settings_size);
		put32(out + 11, m->journal_addr);
		put32(out + 15, 2u * STORAGE_SECTOR);
		put32(out + 19, m->calibration_addr);
		put32(out + 23, m->calibration_size);
		return 27;
	}
	case DL_STORAGE_OP_READ: {
		if (len != 7) {
			return status_only(out, DL_ST_BAD_ARG);
		}
		uint32_t addr = get32(req + 1);
		uint32_t n = get16(req + 5);
		if (n == 0 || n > DL_MEM_READ_MAX || 5u + n > cap) {
			return status_only(out, DL_ST_BAD_ARG);
		}
		if (!storage_debug_read_allowed(addr, n)) {
			return status_only(out, DL_ST_REFUSED);
		}
		storage_lock();
		bool ok = board.flash->read(addr, out + 5, n);
		storage_unlock();
		if (!ok) {
			return status_only(out, DL_ST_UNAVAILABLE);
		}
		out[0] = DL_ST_OK;
		put32(out + 1, addr);
		return 5u + n;
	}
	case DL_STORAGE_OP_CLAIM: {
		hal_chip_t c;
		if (len != 5) {
			return status_only(out, DL_ST_BAD_ARG);
		}
		bool have = read_chip(&c);
		uint8_t why = dl_reboot_check(HAL_REBOOT_NORMAL, get32(req + 1), have ? &c : NULL, false);
		if (why != DL_REBOOT_OK) {
			out[0] = DL_ST_REFUSED;
			out[1] = why;
			return 2;
		}
		if (!storage_claim_request()) {
			return status_only(out, DL_ST_UNAVAILABLE);	// not foreign: nothing to claim
		}
		dlog("debuglink: storage region claimed, erasing");
		out[0] = DL_ST_OK;
		out[1] = DL_REBOOT_OK;
		return 2;
	}
	default:
		return status_only(out, DL_ST_BAD_ARG);
	}
}

// --- dispatch ---

static size_t handle(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	switch (cmd) {
	case DL_CMD_PING:
		out[0] = DL_ST_OK;
		memcpy(out + 1, req, len);
		return 1u + len;
	case DL_CMD_VERSION:
		return cmd_version(out, cap);
	case DL_CMD_GET_DIAG:
		return cmd_get_diag(req, len, out, cap);
	case DL_CMD_RESET_COUNTERS:
		return cmd_reset(out);
	case DL_CMD_SCREENSHOT:
		return cmd_screenshot(req, len, out, cap);
	case DL_CMD_KEY:
		return cmd_key(req, len, out);
	case DL_CMD_PTT:
		return cmd_ptt(req, len, out);
	case DL_CMD_GET_LOG:
		return cmd_get_log(out, cap);
	case DL_CMD_GET_VIEW:
		return cmd_get_view(req, len, out, cap);
	case DL_CMD_RADIO:
		return cmd_radio(req, len, out);
	case DL_CMD_INFO_CHIP:
		return cmd_info_chip(out);
	case DL_CMD_READ_MEM:
		return cmd_read_mem(req, len, out, cap);
	case DL_CMD_REBOOT:
		return cmd_reboot(req, len, out);
	case DL_CMD_DMR_DATA:
		return debuglink_cmd_dmr_data(req, len, out, cap);
	case DL_CMD_STORAGE:
		return cmd_storage(req, len, out, cap);
	case DL_CMD_AUDIO_PROBE:
		return debuglink_cmd_audio_probe(req, len, out, cap);
#if CONFIG_HOTSPOT_SPLIT
	case DL_CMD_SPLIT:
		return debuglink_cmd_split(req, len, out, cap);
#endif
	case DL_CMD_APRS:
		return debuglink_cmd_aprs(req, len, out, cap);
	case DL_CMD_SNMP:
		return debuglink_cmd_snmp(req, len, out, cap);
	case DL_CMD_STUB:
		return stub_entry_request(req, len, out, cap);	// core/stubentry; unavailable without CONFIG_STUB_ENTRY
	case DL_CMD_CRASH:
		return debuglink_cmd_crash(req, len, out, cap);
	case DL_CMD_MEM:
		return debuglink_cmd_mem(req, len, out, cap);
	case DL_CMD_WATCHDOG:
		return debuglink_cmd_watchdog(req, len, out, cap);
	case DL_CMD_RX2:
		return debuglink_cmd_rx2(req, len, out, cap);
	case DL_CMD_CODEPLUG:
#if CONFIG_CPS_FAST
		return cps_fast_cmd(req, len, out, cap, hal_millis());
#else
		return status_only(out, DL_ST_UNAVAILABLE);
#endif
	default:
		return status_only(out, DL_ST_UNKNOWN_CMD);
	}
}

static void write_out(const uint8_t *buf, size_t len)
{
	hostlink_write(buf, len);
}

static size_t side_feed(const uint8_t *buf, size_t len, uint32_t now_ms)
{
	size_t took = dl_parser_feed(&parser, buf, len, now_ms, handle, write_out, reply);
	if (reboot_pending) {
		reboot_now();
	}
	stub_entry_service();		// enters the RAM monitor after a verified STUB ENTER (noreturn); else a no-op
	debuglink_crash_service();	// a crash test asked for (noreturn); else a no-op
	debuglink_watchdog_service();	// a watchdog test asked for (ends in a reset); else a no-op
	debuglink_rx2_service();	// the second SoC's update bridge asked for (noreturn); else a no-op
	return took;
}

static bool side_in_frame(uint32_t now_ms)
{
	return dl_parser_in_frame(&parser, now_ms);
}

static const hostlink_side_t side = {
	.start = DL_SOF1,
	.feed = side_feed,
	.in_frame = side_in_frame,
};

void debuglink_start(void)
{
	dl_parser_init(&parser);
	hostlink_set_side_protocol(&side);
	dlog("debuglink: up, %s %s", board.name, RFOUNDRY_GIT);
}
