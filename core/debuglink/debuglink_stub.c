// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DL_CMD_STUB (CONFIG_STUB_ENTRY, docs/spec/debuglink.md 3.18): load the host's RAM-monitor image into a free RAM span
// and, once it verifies, hand the SoC to it at run time with no power cycle. The portable state machine is dl_stub.c;
// this file is the firmware glue: it finds the span through board.system, and jumps after the reply has gone out (as
// cmd_reboot does), so the host sees the ENTER ok and can switch to talking to the monitor.
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

#if CONFIG_STUB_ENTRY
#include "board.h"
#include "hal_time.h"
#include "debuglink/dl_stub.h"
#include "debuglink/dlog.h"
#include "ui/gfx.h"
#include "ui/ui_screens.h"
#include "radio/radio.h"

#ifndef ODR_GIT
#define ODR_GIT "unknown"
#endif

#define STUB_ENTER_DELAY_MS	100u	// let the ENTER reply leave the UART before interrupts are masked

static dl_stub_t stub;
static bool ready;		// the RAM span has been fetched from the platform
static bool enter_pending;
static uint32_t enter_addr;

// Replace the screen so it does not sit on a stale frame while the monitor runs: the boot splash with a "connected"
// message. Uses the UI's own ui_draw_splash, then flushes with the board's display (as the boot path does) — the host
// link task outranks the UI task and the flush is synchronous (dm1801pro bit-bangs its ST7567, dm32 writes the LCDC
// polled), so the frame is on the panel before the caller masks interrupts. ui_draw_splash writes only the visible rows,
// which sit clear of the loaded image on both boards (dm32 codec window; dm1801pro the oversized frame buffer's head)
static void show_banner(void)
{
	if (board.display == NULL || gfx_framebuffer() == NULL || gfx_width() <= 0) {
		return;
	}
	ui_draw_splash(board.name, ODR_GIT, "PC Loader Connected", "Power-cycle to exit");
	gfx_flush();
	// Full backlight: the UI's idle timeout may have dimmed or switched it off, and the UI never runs again while the
	// monitor has the radio, so the splash would otherwise sit on a dark panel
	if (board.display->set_backlight != NULL) {
		board.display->set_backlight(100);
	}
}

// Transmitting in any mode the radio core knows about (FM, DMR voice or data)
static bool on_air(void)
{
	radio_view_t v;
	radio_get_view(&v);
	return v.state == RS_TX;
}

static const hal_system_t *sys(void)
{
	return (board.system != NULL && board.system->stub_region != NULL && board.system->enter_stub != NULL)
		       ? board.system
		       : NULL;
}

size_t debuglink_cmd_stub(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	const hal_system_t *s = sys();

	if (s == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	if (!ready) {
		uint32_t size = 0;
		uint8_t *region = s->stub_region(&size);
		if (region == NULL || size == 0) {
			out[0] = DL_ST_UNAVAILABLE;
			return 1;
		}
		uint32_t run = s->stub_run_addr != 0 ? s->stub_run_addr : (uint32_t)(uintptr_t)region;
		dl_stub_init(&stub, region, size, run);
		ready = true;
	}
	bool enter = false;
	uint32_t entry = 0;
	stub.transmitting = on_air();
	size_t n = dl_stub_request(&stub, req, len, out, cap, &enter, &entry);
	if (enter) {
		enter_pending = true;
		enter_addr = entry;
		dlog("debuglink: stub entry armed at 0x%x in %u ms", (unsigned)entry, (unsigned)STUB_ENTER_DELAY_MS);
	}
	return n;
}

void debuglink_stub_service(void)
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
		dlog("debuglink: stub entry dropped, the radio is transmitting");
		return;
	}
	// The monitor never touches the RF chips, so whatever the transmitter is doing now lasts the whole session. Put
	// it in receive even when the core thinks it's idle (a hotspot or data burst the core doesn't track as RS_TX)
	if (board.radio != NULL && board.radio->tx_off != NULL) {
		board.radio->tx_off();
	}
	show_banner();				// tell the user, flushed to the panel before the jump
	s->enter_stub(enter_addr);		// does not return on success
	dlog("debuglink: stub entry failed");	// only reached if the jump was refused
}

#else

size_t debuglink_cmd_stub(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

void debuglink_stub_service(void)
{
}

#endif
