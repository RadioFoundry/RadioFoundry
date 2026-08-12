// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The debug link's commands (docs/spec/debuglink.md). Everything runs in the host link task, between the bytes of the
// other protocols: each command is short (a reply of at most DL_REPLY_MAX bytes, no waiting on other tasks; radio
// requests are queued, injected keys are timed by the input task).
#include <string.h>
#include "config.h"
#include "board.h"
#include "hal_time.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_chip.h"
#include "debuglink/dlog.h"
#include "hostlink/hostlink.h"
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#endif
#include "input/input.h"
#include "radio/radio.h"
#include "ui/gfx.h"

#ifndef ODR_GIT
#define ODR_GIT "unknown"
#endif

#define SHOT_HOLD_MS		1000	// a screen capture freezes UI redraws at most this long
#define SHOT_CHUNK_MAX		1000u	// screenshot bytes per reply
#define RADIO_OP_GAP_MS		20u	// radio requests at most this often (the radio core's queue holds 16)
#define REBOOT_DELAY_MS		250u	// the REBOOT reply goes out over USB before the reset

// Radio operations (DL_CMD_RADIO, first payload byte)
enum {
	OP_CHANNEL_STEP = 1, OP_ZONE_POS, OP_ZONE, OP_VFO, OP_VFO_SET, OP_VFO_STEP, OP_POWER, OP_SQUELCH, OP_BANDWIDTH,
	OP_TONE, OP_MONITOR, OP_CLEAR_TEMP, OP_REPLY_CANCEL, OP_HOTSPOT_EXIT,
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
#if CONFIG_SFR
	"SFR,"
#endif
#if CONFIG_HOTSPOT
	"HOTSPOT,"
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
#if CONFIG_DIAG
	"DIAG,"
#endif
	"USB,DEBUGLINK\n";

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
	dl_text_kv_str(&t, "git", ODR_GIT);
	dl_text_kv_str(&t, "board", board.name);
	dl_text(&t, "%s", features);
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
	uint32_t stride = d->bpp == 1 ? (d->width + 7u) / 8u : d->width * 2u;
	uint32_t total = stride * d->height;
	if (total > GFX_FB_BYTES) {
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
	out[5] = d->bpp;
	out[6] = d->bpp == 1 ? 2 : 1;	// 1: RGB565 little endian, 2: 1 bpp rows, MSB left
	out[7] = d->inset_top;
	out[8] = d->inset_bottom;
	out[9] = d->inset_left;
	out[10] = d->inset_right;
	put32(out + 11, total);
	put32(out + 15, offset);
	// Hold the UI's redraws while the host reads the chunks, so the capture is one frame. Every chunk extends the
	// hold, so the host's check pass sees the same frame; redraws resume SHOT_HOLD_MS after the last chunk
	gfx_hold(hal_millis() + SHOT_HOLD_MS);
	memcpy(out + 19, (const uint8_t *)gfx_framebuffer() + offset, n);
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
	};
	uint32_t now = hal_millis();

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
	out[23] = dl_rdp_level(c.optcr);
	out[24] = dl_rdp_byte(c.optcr);
	put16(out + 25, dl_wrp_mask(c.optcr));
	out[27] = dl_bor_level(c.optcr);
	out[28] = dl_user_bits(c.optcr);
	return 29;
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
	if (board.system == NULL || board.system->read == NULL || !read_chip(&c)) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	if (!dl_mem_allowed(addr, n, c.flash_kb)) {
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
	dlog("debuglink: up, %s %s", board.name, ODR_GIT);
}
