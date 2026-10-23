// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI task: boot splash, the frame pacing, the backlight, and the glue between the RTOS and the UI's behavior in
// ui_app.c (which the host simulator runs without an RTOS). Reads the radio view and posts requests; never touches
// the radio hardware.
//
// Backlight: full on any key and while receiving or transmitting; dims to a fifth after the time-out
// (Settings > Display, default 30 s), off after four times that. When it is off, the first key only wakes the screen
// (PTT and MONITOR still work: the input layer sends them to the radio directly).
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
#include "config.h"

#define UI_TASK_PRIORITY	(tskIDLE_PRIORITY + 1)	// below the radio and input tasks
// List screens and Find: about 1.2 KB measured on the host, plus the driver. The static stack check's deepest chain
// (ui_task > ui_draw_hotspot > ui_draw_hint > gfx_text, down to the 4 bpp span) needs 1736 bytes on the STM32 radios'
// default builds with the CI's Arm toolchain (8 to 16 bytes more than the macOS build of the same compiler);
// 519 words leave the 128 byte margin plus 4 bytes. Every word here is RAM on all radios: raise it only by what the
// check shows
#define UI_TASK_STACK_WORDS	519

#define SPLASH_MS		1000
#define FRAME_MIN_MS		50	// at most 20 redraws a second

static radio_view_t view;	// static: too large for a modest task stack
static uint8_t backlight_now = 0xFF;
static uint32_t radio_active_ms;
static ui_app_status_t status;

void ui_crit_enter(void) { taskENTER_CRITICAL(); }
void ui_crit_exit(void) { taskEXIT_CRITICAL(); }

static void set_backlight(uint8_t pct)
{
	if (pct != backlight_now) {
		backlight_now = pct;
		board.display->set_backlight(pct);
	}
}

static uint8_t backlight_target(uint32_t now)
{
	uint32_t idle = input_idle_ms();
	uint32_t since_radio = now - radio_active_ms;
	if (since_radio < idle)
		idle = since_radio;
	// Dim and off times from the radio's settings (Display > Backlight time), dimmer on a low battery
	return radio_backlight_pct(ui_app_brightness(), idle);
}

const char *ui_screen_name(void)
{
	return status.screen != NULL ? status.screen : "home";
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
		ui_draw_splash(board.name, ODR_GIT, line1, line2);
		gfx_flush();
	}
	ui_app_init();
	set_backlight(ui_app_brightness());
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
		if (input_next(&ev, wait)) {
			bool was_off = backlight_now == 0;
			set_backlight(ui_app_brightness());
			if (!was_off || ev.action == ACT_PTT || ev.action == ACT_MONITOR)
				ui_app_key(&ev);
			dirty = true;
		}
		now = hal_millis();
		uint32_t s = radio_view_serial();
		if (s != serial) {
			serial = s;
			radio_get_view(&view);
		}
		if (view.state != RS_IDLE)
			radio_active_ms = now;
		if (ui_app_tick(&view, now))
			dirty = true;
		set_backlight(backlight_target(now));

		if (dirty && have_fb && now - last_draw >= FRAME_MIN_MS && !gfx_held(now)) {
			dirty = false;
			last_draw = now;
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
