// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI task: boot splash, the frame pacing, the backlight, and the glue between the RTOS and the UI's behavior in
// ui_app.c (which the host simulator runs without an RTOS). Reads the radio view and posts requests; never touches
// the radio hardware.
//
// Backlight: full on any key, a banner or an emergency, and while the radio does something the user would want to see
// (radio_view_wakes: a call that plays, an open FM squelch, their own transmission; never ARS, ACKs or other data the
// radio sends or takes on its own); dims to a fifth after the time-out (Settings > Display, default 30 s), off after
// four times that. When it is off, the first key only wakes the screen (PTT and MONITOR still work: the input layer
// sends them to the radio directly).
#include "board.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ui.h"
#include "ui_app.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "input/input.h"
#include "radio/radio.h"
#include "radio/battery.h"
#include "config.h"
#if CONFIG_WATCHDOG
#include "app/watchdog.h"
#endif

#ifndef RFOUNDRY_RELEASE
#define RFOUNDRY_RELEASE ""
#endif

#define UI_TASK_PRIORITY	(tskIDLE_PRIORITY + 1)	// below the radio and input tasks
// List screens and Find: about 1.2 KB measured on the host, plus the driver. The static stack check's deepest chain
// needs up to 1720 bytes on the STM32
// radios, the same on macOS and in CI now that the link order is sorted (gotchas 13.10); the quick screen's tiles and
// the dual Home's layout table (dual_choose) live in frames of their own to stay below it, as a 4 bpp panel's palette
// lookup adds 48 bytes under every fill. 516 words (2064 bytes) leave the 208 byte port overhead, the 128 byte margin and 8 bytes.
// Every word here is RAM on all radios: raise it only by what the check shows
#define UI_TASK_STACK_WORDS	516

#define SPLASH_MS		1000
#define FRAME_MIN_MS		50	// at most 20 redraws a second
// Keys still waiting put a redraw off, so a burst of keys costs one: a changed list repaints most of a large panel
// (a full frame of a 240x320 panel on an 8-bit bus costs tens of ms). Not longer than this, so the screen keeps up
// with a held key
#define FRAME_DEFER_MS		300
// The idle task runs below this one and feeds the watchdog (watchdog.md 4): keys that come faster than the work
// they cause (Find on a full codeplug, a filtered list of 1024 contacts) never let this task wait. After this long
// busy it rests (ui_rest), so the idle task's gap stays far below the hang check whatever the work costs. A flush counts
// too: it rests between its bands (gfx_set_rest), as one frame of a large panel outlasts the hang check while DMR TX
// leaves this task a tenth of the CPU
#define BUSY_MAX_MS		250
// A rest ends once the idle task has run, or after this long: while the vocoder and the radio keep the CPU busy, a
// sleep of one tick often ends before the idle task gets in
#define REST_MAX_MS		100

static radio_view_t view;	// static: too large for a modest task stack
static uint8_t backlight_now = 0xFF;
static uint32_t radio_active_ms;
static ui_app_status_t status;
static uint32_t rested;	// the last time this task waited (hal_millis)

// After BUSY_MAX_MS without a wait: sleep until the idle task has fed the watchdog (watchdog.md 4)
static void ui_rest(void)
{
	if (hal_millis() - rested < BUSY_MAX_MS)
		return;
#if CONFIG_WATCHDOG
	uint32_t feeds = watchdog_state()->feeds;
	for (uint32_t t0 = hal_millis(); watchdog_state()->feeds == feeds && hal_millis() - t0 < REST_MAX_MS;)
		vTaskDelay(1);
#else
	vTaskDelay(1);
#endif
	rested = hal_millis();
}

void ui_crit_enter(void) { taskENTER_CRITICAL(); }
void ui_crit_exit(void) { taskEXIT_CRITICAL(); }

static void set_backlight(uint8_t pct)
{
#if CONFIG_BACKLIGHT_TEST
	int16_t hold = ui_app_backlight_hold();
	if (hold >= 0)
		pct = (uint8_t)hold;	// the backlight tests have it
	if (ui_app_take_backlight_reapply())
		backlight_now = 0xFF;
#endif
	if (pct != backlight_now) {
		backlight_now = pct;
		board.display->set_backlight(pct);
		if (board.display->set_key_light != NULL)
			board.display->set_key_light(batt_key_light(pct, ui_app_brightness()));
	}
}

// What a key or start-up wakes the light to: the set brightness, or the low battery cap. Full first and the cap in
// the same pass flashed the screen and the key light (on only at the set brightness, batt_key_light)
static uint8_t backlight_wake(void)
{
	return radio_backlight_pct(ui_app_brightness(), 0);
}

static uint8_t backlight_target(uint32_t now)
{
	uint32_t idle = input_idle_ms();
	uint32_t since_radio = now - radio_active_ms;
	if (since_radio < idle)
		idle = since_radio;
	// Dim and off times from the radio's settings (Display > Backlight Dim/Off), dimmer on a low battery
	return radio_backlight_pct(ui_app_brightness(), idle);
}

const char *ui_screen_name(void)
{
	return status.screen != NULL ? status.screen : "home";
}

uint8_t ui_backlight(void)
{
	return (backlight_now > 100u) ? 100u : backlight_now;	// 0xFF: not set yet, full at start-up
}

void ui_get_status(ui_app_status_t *st)
{
	*st = status;
	// Before the first frame nothing is filled in
	if (st->screen == NULL)
		st->screen = "home";
	if (st->banner == NULL)
		st->banner = "";
	if (st->toast == NULL)
		st->toast = "";
	if (st->query == NULL)
		st->query = "";
}

static void ui_task(void *arg)
{
	(void)arg;
	bool have_fb = gfx_init(board.display);
	input_event_t ev;

	if (have_fb) {
		ui_layout_init();
		char line1[17], line2[17];
		ui_svc_boot_text(line1, line2);
		// the release name when this is one (v26.11), else the commit
		gfx_frame_begin();
		ui_draw_splash(board.name, RFOUNDRY_RELEASE[0] != '\0' ? RFOUNDRY_RELEASE : RFOUNDRY_GIT, line1, line2);
		gfx_flush();
	}
	ui_app_init();
	rested = hal_millis();
	gfx_set_rest(ui_rest);
	set_backlight(backlight_wake());
	vTaskDelay(pdMS_TO_TICKS(SPLASH_MS));
	while (input_next(&ev, 0))
		;	// keys pressed during the splash are dropped
	radio_get_view(&view);
	ui_app_status(&status);

	uint32_t serial = radio_view_serial() - 1u;
	uint32_t last_draw = 0;
	bool dirty = true;
	radio_active_ms = hal_millis();
	for (;;) {
		uint32_t now = hal_millis();
		uint32_t wait = gfx_pass_wait_ms(dirty, gfx_held(now), now - last_draw, FRAME_MIN_MS, 100);
		ui_rest();
		// Nothing queued and a wait: input_next blocks until a key or the time-out, and the idle task gets in
		bool rests = wait != 0 && !input_pending();
		bool got = input_next(&ev, wait);
		if (rests)
			rested = hal_millis();
		// The view before the key: the radio core may have changed while this task waited (the VFO turned off from
		// the host link, a scan stopping), and a key read against the old view sends the wrong request (a VFO step
		// the core drops on a channel)
		uint32_t s = radio_view_serial();
		if (s != serial) {
			serial = s;
			radio_get_view(&view);
		}
		if (got) {
			bool was_off = backlight_now == 0;
#if CONFIG_BACKLIGHT_TEST
			was_off = was_off && ui_app_backlight_hold() < 0;	// a key wakes the light, but not in the tests
#endif
			set_backlight(backlight_wake());
			if (!was_off || ui_app_key_while_dark(&ev))
				ui_app_key(&ev);
			dirty = true;
		}
		now = hal_millis();
		s = radio_view_serial();
		if (s != serial) {	// and after it: what the key changed shows in this pass
			serial = s;
			radio_get_view(&view);
		}
		if (radio_view_wakes(&view))
			radio_active_ms = now;
		if (ui_app_tick(&view, now))
			dirty = true;
		if (ui_app_take_wake())
			radio_active_ms = now;
		set_backlight(backlight_target(now));

		if (dirty && have_fb && now - last_draw >= FRAME_MIN_MS && !gfx_held(now) &&
		    (!input_pending() || now - last_draw >= FRAME_DEFER_MS)) {
			dirty = false;
			last_draw = now;
			gfx_frame_begin();
			ui_app_draw();
			gfx_flush();
		} else if (!have_fb) {
			dirty = false;
		}
		ui_app_status(&status);
	}
}

void ui_start(void)
{
	xTaskCreate(ui_task, "ui", UI_TASK_STACK_WORDS, NULL, UI_TASK_PRIORITY, NULL);
}
