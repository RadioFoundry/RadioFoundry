// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A second SoC's debug link server (rx2_dl.h, docs/spec/rx2-link.md 2.3). The replies are laid out as the main SoC's
// (core/debuglink/debuglink.c, docs/spec/debuglink.md 3), so the host reads them with the same code. The second SoC
// never transmits, so nothing here waits for the transmitter.
#include <string.h>
#include "config.h"
#include "board.h"
#include "hal_time.h"
#include "rx2/rx2_dl.h"
#include "debuglink/debuglink.h"
#include "debuglink/dl_frame.h"
#include "debuglink/dl_chip.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dlog.h"
#include "debuglink/dl_mem.h"
#if CONFIG_CRASH_LOG
#include "crash/crash.h"
#endif

#if CONFIG_RX2_DL_SERVER

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "unknown"
#endif
#ifndef RFOUNDRY_RELEASE
#define RFOUNDRY_RELEASE ""
#endif

#define AFTER_REPLY_MS		50u	// the reply's last bytes leave the UART's FIFO (the link's writes wait for room)

enum { VIEW_OWN, VIEW_CRASH, VIEW_RESET, VIEW_COUNT };

static const rx2_recv_t *link;
static void (*own_view)(dl_text_t *t);
static uint8_t pending;		// what runs after the reply: 0 nothing, else PEND_*
enum { PEND_REBOOT = 1, PEND_TRAP, PEND_ASSERT, PEND_HANG };

void rx2_dl_init(const rx2_recv_t *l, void (*view)(dl_text_t *t))
{
	link = l;
	own_view = view;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t status_only(uint8_t *out, uint8_t st)
{
	out[0] = st;
	return 1;
}

static size_t cmd_version(uint8_t *out, size_t cap)
{
	dl_text_t t;
	out[0] = DL_ST_OK;
	dl_text_init(&t, (char *)out + 1, cap - 1);
	dl_text(&t, "proto=%u\nview=%u\n", DL_PROTO_VERSION, DL_VIEW_VERSION);
	dl_text_kv_str(&t, "git", RFOUNDRY_GIT);
	if (RFOUNDRY_RELEASE[0] != '\0') {
		dl_text_kv_str(&t, "release", RFOUNDRY_RELEASE);
	}
	dl_text_kv_str(&t, "board", board.name);
	dl_text(&t, "via=forward\nuptime_ms=%u\nreq_max=%u\nreply_max=%u\n", (unsigned)hal_millis(), DL_REQ_MAX - 2u,
		RX2_DL_REPLY_MAX);
	return 1 + t.len;
}

// GET_DIAG: the board's lines only (no radio core here), from the line asked for
static size_t cmd_get_diag(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	uint8_t i = len >= 1 ? req[0] : 0;
	dl_text_t t;
	char line[64];
	out[0] = DL_ST_OK;
	put32(out + 1, hal_millis());
	dl_text_init(&t, (char *)out + 6, cap - 6);
	for (;; i++) {
		if (board.diag_line == NULL || !board.diag_line(i, line, sizeof(line))) {
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
	out[5] = i;
	return 6 + t.len;
}

static size_t cmd_get_log(uint8_t *out, size_t cap)
{
#if CONFIG_DLOG
	uint8_t more;
	uint32_t dropped;
	size_t n = dlog_read((char *)out + 6, cap - 6, &more, &dropped);
	out[0] = DL_ST_OK;
	put32(out + 1, dropped);
	out[5] = more;
	return 6 + n;
#else
	(void)cap;
	return status_only(out, DL_ST_UNAVAILABLE);
#endif
}

static void view_link(dl_text_t *t)
{
	if (link == NULL) {
		return;
	}
	dl_text(t, "rx2.frames=%u\nrx2.bad=%u\nrx2.dl_requests=%u\n", (unsigned)link->frames, (unsigned)link->parser.bad,
		(unsigned)link->dl_requests);
}

static size_t cmd_get_view(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	uint8_t section = len >= 1 ? req[0] : 0;
	dl_text_t t;
	if (section >= VIEW_COUNT) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	out[0] = DL_ST_OK;
	put32(out + 1, hal_millis());
	out[5] = (uint8_t)(section + 1u < VIEW_COUNT ? section + 1u : 0xFFu);
	dl_text_init(&t, (char *)out + 6, cap - 6 - 16);	// room kept for the truncation mark
	switch (section) {
	case VIEW_OWN:
		dl_text_kv_str(&t, "board", board.name);
		dl_text(&t, "uptime_ms=%u\n", (unsigned)hal_millis());
		view_link(&t);
		if (own_view != NULL) {
			own_view(&t);
		}
		break;
	case VIEW_CRASH:
		debuglink_view_crash(&t);
		break;
	default:
		debuglink_view_reset(&t);
		break;
	}
	if (t.full) {
		t.cap += 16;
		t.full = false;
		dl_text(&t, "truncated=1\n");
	}
	return 6 + t.len;
}

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
	out[5] = (uint8_t)c.flash_kb;
	out[6] = (uint8_t)(c.flash_kb >> 8);
	put32(out + 7, c.uid[0]);
	put32(out + 11, c.uid[1]);
	put32(out + 15, c.uid[2]);
	put32(out + 19, c.optcr);
	memset(out + 23, 0xFF, 6);	// no option bytes to decode
	out[29] = c.family;
	return 30;
}

static size_t cmd_read_mem(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (len != 6) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	uint32_t addr = get32(req);
	uint32_t n = (uint32_t)req[4] | (uint32_t)req[5] << 8;
	if (n == 0 || n > DL_MEM_READ_MAX || 5u + n > cap) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (board.system == NULL || board.system->read == NULL || board.system->mem_window == NULL) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	if (!dl_mem_allowed(addr, n, board.system->mem_window)) {
		return status_only(out, DL_ST_REFUSED);
	}
	out[0] = DL_ST_OK;
	put32(out + 1, addr);
	board.system->read(addr, out + 5, n);
	return 5u + n;
}

// REBOOT, normal only. The token is the chip UID's low word as on the main SoC; a SoC whose UID can't be read (no data
// flash of its own) takes DL_RX2_TOKEN instead
static size_t cmd_reboot(const uint8_t *req, uint16_t len, uint8_t *out)
{
	hal_chip_t c;
	if (len != 5 || req[0] > HAL_REBOOT_ROM_DFU) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (board.system == NULL || board.system->reboot == NULL) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	uint32_t token = get32(req + 1);
	uint8_t why;
	if (req[0] != HAL_REBOOT_NORMAL) {
		why = DL_REBOOT_RDP;
	} else if (read_chip(&c)) {
		why = dl_reboot_check(req[0], token, &c, false);
	} else {
		why = token == DL_RX2_TOKEN ? DL_REBOOT_OK : DL_REBOOT_BAD_TOKEN;
	}
	out[0] = why == DL_REBOOT_OK ? DL_ST_OK : DL_ST_REFUSED;
	out[1] = why;
	if (why == DL_REBOOT_OK) {
		dlog("debuglink: reboot in %u ms", (unsigned)AFTER_REPLY_MS);
		pending = PEND_REBOOT;
	}
	return 2;
}

static size_t cmd_crash(const uint8_t *req, uint16_t len, uint8_t *out)
{
#if CONFIG_CRASH_LOG
	if (len == 1 && req[0] == DL_CRASH_OP_CLEAR) {
		crash_clear();
		return status_only(out, DL_ST_OK);
	}
	if (len == 6 && req[0] == DL_CRASH_OP_TEST) {
		if (req[1] < DL_CRASH_TEST_TRAP || req[1] > DL_CRASH_TEST_HANG) {
			return status_only(out, DL_ST_BAD_ARG);
		}
		if (req[1] == DL_CRASH_TEST_HANG && !CONFIG_WATCHDOG) {
			return status_only(out, DL_ST_UNAVAILABLE);	// nothing would end the spin but a power cycle
		}
		if (get32(req + 2) != DL_CRASH_TOKEN) {
			return status_only(out, DL_ST_REFUSED);
		}
		dlog("debuglink: crash test %u in %u ms", req[1], (unsigned)AFTER_REPLY_MS);
		pending = req[1] == DL_CRASH_TEST_TRAP ? PEND_TRAP : req[1] == DL_CRASH_TEST_HANG ? PEND_HANG : PEND_ASSERT;
		return status_only(out, DL_ST_OK);
	}
	return status_only(out, DL_ST_BAD_ARG);
#else
	(void)req;
	(void)len;
	return status_only(out, DL_ST_UNAVAILABLE);
#endif
}

size_t rx2_dl_handle(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	switch (cmd) {
	case DL_CMD_PING:
		if (1u + len > cap) {
			return status_only(out, DL_ST_BAD_ARG);
		}
		out[0] = DL_ST_OK;
		memcpy(out + 1, req, len);
		return 1u + len;
	case DL_CMD_VERSION:
		return cmd_version(out, cap);
	case DL_CMD_GET_DIAG:
		return cmd_get_diag(req, len, out, cap);
	case DL_CMD_RESET_COUNTERS:
		if (board.diag_reset != NULL) {
			board.diag_reset();
		}
		return status_only(out, DL_ST_OK);
	case DL_CMD_GET_LOG:
		return cmd_get_log(out, cap);
	case DL_CMD_GET_VIEW:
		return cmd_get_view(req, len, out, cap);
	case DL_CMD_INFO_CHIP:
		return cmd_info_chip(out);
	case DL_CMD_READ_MEM:
		return cmd_read_mem(req, len, out, cap);
	case DL_CMD_REBOOT:
		return cmd_reboot(req, len, out);
	case DL_CMD_CRASH:
		return cmd_crash(req, len, out);
	case DL_CMD_MEM:
		return debuglink_cmd_mem(req, len, out, cap);
	default:
		return status_only(out, DL_ST_UNKNOWN_CMD);
	}
}

void rx2_dl_service(void)
{
	if (pending == 0) {
		return;
	}
	uint8_t what = pending;
	pending = 0;
	hal_delay_ms(AFTER_REPLY_MS);
	if (what == PEND_REBOOT) {
		board.system->reboot(HAL_REBOOT_NORMAL);
		return;
	}
#if CONFIG_CRASH_LOG
	if (what == PEND_TRAP) {
		__builtin_trap();	// a breakpoint (CK803) / an undefined instruction (CM4): a CPU exception
	}
	if (what == PEND_HANG) {
		for (;;) {	// the idle task stops running: the watchdog ends it
		}
	}
	crash_assert(__LINE__);
#endif
}

#endif
