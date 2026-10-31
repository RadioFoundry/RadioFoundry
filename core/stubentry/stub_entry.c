// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stub entry's firmware glue (see stub_entry.h, docs/spec/stub-entry.md): load the host's RAM-monitor image into a
// free RAM span and, once it verifies, hand the SoC to it at run time with no power cycle. The portable state machine
// is stub_load.c, the listener of a build without the debug link stub_link.c; this file finds the span through
// board.system, and jumps after the reply has gone out, so the host sees the ENTER ok and can switch to talking to the
// monitor.
#include "config.h"
#include "board.h"
#include "hal_time.h"
#include "hal_vocoder.h"
#include "stubentry/stub_entry.h"
#include "stubentry/stub_load.h"
#include "stubentry/stub_link.h"
#include "debuglink/dlog.h"
#include "hostlink/hostlink.h"
#include "ui/gfx.h"
#include "ui/ui_screens.h"
#include "radio/radio.h"
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#endif

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "unknown"
#endif
#ifndef RFOUNDRY_RELEASE
#define RFOUNDRY_RELEASE ""
#endif

#define STUB_ENTER_DELAY_MS	100u	// let the ENTER reply leave the UART before interrupts are masked
#define STUB_HOLD_MS		5000u	// frame buffer staging: the UI stays held this long after each STUB request
#define STUB_FRAME_WAIT_MS	200u	// and a frame it is drawing gets this long to finish first

static stub_load_t stub;
static bool ready;		// the RAM span has been fetched from the platform
static bool enter_pending;
static uint32_t enter_addr;

// Replace the screen so it does not sit on a stale frame while the monitor runs: the boot splash with a "connected"
// message. Uses the UI's own ui_draw_splash, then flushes with the board's display (as the boot path does). The host
// link task outranks the UI task and the flush is synchronous (one board bit-bangs its ST7567, another writes the LCDC
// polled), so the frame is on the panel before the caller masks interrupts. ui_draw_splash writes only the visible rows,
// which sit clear of the loaded image on every board (a codec window, or the oversized frame buffer's head)
static void show_banner(void)
{
	if (board.display == NULL || gfx_framebuffer() == NULL || gfx_width() <= 0) {
		return;
	}
	ui_draw_splash(board.name, RFOUNDRY_RELEASE[0] != '\0' ? RFOUNDRY_RELEASE : RFOUNDRY_GIT, STUB_ENTRY_LINE1, STUB_ENTRY_LINE2);
	gfx_flush();
	// Full backlight: the UI's idle timeout may have dimmed or switched it off, and the UI never runs again while the
	// monitor has the radio, so the splash would otherwise sit on a dark panel
	if (board.display->set_backlight != NULL) {
		board.display->set_backlight(100);
	}
}

// Transmitting in any mode the radio core knows about (FM, DMR voice or data), or the hotspot sending what its host sent
static bool on_air(void)
{
	if (radio_get_state() == RS_TX) {
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

// A platform that has a RAM span of its own, or one that takes the image staged in the frame buffer
static const hal_system_t *sys(void)
{
	const hal_system_t *s = board.system;
	if (s == NULL) {
		return NULL;
	}
	if (s->stub_region != NULL && s->enter_stub != NULL) {
		return s;
	}
	return s->enter_stub_staged != NULL && s->stub_run_addr != 0 ? s : NULL;
}

static bool staged(const hal_system_t *s)
{
	return s->stub_region == NULL;
}

// Frame buffer staging (hal_system_t.enter_stub_staged): the image goes where the UI draws, so the UI is held from the
// first STUB request to the jump and the banner goes up first: the panel keeps showing it from its own memory while
// the image overwrites the buffer. A host that stops part way lets the hold run out and the UI draws again; the image
// is then damaged, and ENTER's CRC check refuses it. The span is NULL when there is no display to borrow from
static uint8_t *frame_buffer_span(uint32_t *size)
{
	if (board.display == NULL || gfx_framebuffer() == NULL || gfx_width() <= 0) {
		return NULL;
	}
	*size = GFX_FB_BYTES;
	return (uint8_t *)(uintptr_t)gfx_framebuffer();	// gfx owns it; nothing draws while the hold lasts
}

static void hold_frame_buffer(void)
{
	uint32_t now = hal_millis();
	bool was_held = gfx_held(now);
	gfx_hold(now + STUB_HOLD_MS);
	if (was_held) {
		return;		// the banner is up already (or a screen capture holds the frame: the CRC check guards)
	}
	uint32_t frames;
	for (uint32_t waited = 0; !gfx_frame_idle(&frames) && waited < STUB_FRAME_WAIT_MS; waited += 10u) {
		hal_delay_ms(10);	// the UI task, below this one, finishes the frame it was drawing
	}
	show_banner();
}

size_t stub_entry_request(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	const hal_system_t *s = sys();

	if (s == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	// Never hand the SoC over with the transmitter keyed. Answered before the image is checked, as the debug link
	// always has
	bool tx = on_air();
	if (len >= 1 && req[0] == STUB_OP_ENTER && tx) {
		out[0] = DL_ST_BUSY;
		return 1;
	}
	if (staged(s)) {
		hold_frame_buffer();
	}
	if (!ready) {
		uint32_t size = 0;
		uint8_t *region = staged(s) ? frame_buffer_span(&size) : s->stub_region(&size);
		if (region == NULL || size == 0) {
			out[0] = DL_ST_UNAVAILABLE;
			return 1;
		}
		uint32_t run = s->stub_run_addr != 0 ? s->stub_run_addr : (uint32_t)(uintptr_t)region;
		stub_load_init(&stub, region, size, run);
		stub.image_ok = s->stub_image_ok;
		ready = true;
	}
	bool enter = false;
	uint32_t entry = 0;
	stub.transmitting = tx;
	size_t n = stub_load_request(&stub, req, len, out, cap, &enter, &entry);
	if (n == 2 && out[0] == DL_ST_REFUSED && out[1] == STUB_REFUSE_WATCHDOG) {
		dlog("stub: refused, it doesn't feed the watchdog (an older loader)");
	}
	if (enter) {
		enter_pending = true;
		enter_addr = entry;
		dlog("stub: entry armed at 0x%x in %u ms", (unsigned)entry, (unsigned)STUB_ENTER_DELAY_MS);
	}
	return n;
}

void stub_entry_service(void)
{
	if (!enter_pending) {
		return;
	}
	enter_pending = false;
	const hal_system_t *s = sys();
	if (s == NULL) {
		return;
	}
	hal_delay_ms(STUB_ENTER_DELAY_MS);	// the reply reaches the host before the radio goes quiet
	if (on_air()) {				// PTT went down after the ENTER was accepted
		dlog("stub: entry dropped, the radio is transmitting");
		return;
	}
	// The monitor never touches the RF chips, so whatever the transmitter is doing now lasts the whole session. Put
	// it in receive even when the core thinks it's idle (a hotspot burst the core doesn't track as RS_TX)
	if (board.radio != NULL && board.radio->tx_off != NULL) {
		board.radio->tx_off();
	}
	if (staged(s)) {
		// The banner is up since the load began, and drawing it now would land on the image
		s->enter_stub_staged(stub.region, stub.total_len, enter_addr);
	} else {
		show_banner();			// tell the user, flushed to the panel before the jump
		s->enter_stub(enter_addr);	// does not return on success
	}
	dlog("stub: entry failed");		// only reached if the jump was refused
}

#if !CONFIG_DEBUGLINK
// --- The listener of a build without the debug link (stub_link.c): the same framing as the debug link's ---

static dl_parser_t parser;
static uint8_t reply[DL_HDR + STUB_LINK_REPLY_MAX + 2u];
static stub_link_t ident;

// Asked at each VERSION, not at start: the board checks its codec on the first request (some 20 ms)
static const char *codec_id(void)
{
	return hal_vocoder_id(board.vocoder);
}

static size_t handle(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	return stub_link_handle(&ident, cmd, req, len, out, cap);
}

static void write_out(const uint8_t *buf, size_t len)
{
	hostlink_write(buf, len);
}

static size_t side_feed(const uint8_t *buf, size_t len, uint32_t now_ms)
{
	size_t took = dl_parser_feed(&parser, buf, len, now_ms, handle, write_out, reply);
	stub_entry_service();	// enters the RAM monitor after a verified ENTER (noreturn); else a no-op
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

void stub_entry_start(void)
{
	dl_parser_init(&parser);
	parser.reply_cap = STUB_LINK_REPLY_MAX;
	ident.board = board.name;
	ident.git = RFOUNDRY_GIT;
	ident.codec = codec_id;
	ident.stub = stub_entry_request;
	hostlink_set_side_protocol(&side);
}
#endif
