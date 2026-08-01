// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI task: boot splash, Home and quick settings, key handling (docs/ux/information-architecture.md 3, 5),
// backlight time-out. Reads the radio view and posts requests; never touches the radio hardware.
//
// Choices not fixed by docs/ux yet:
// - SELECT on Home would open the Hub, which isn't built: it shows a "coming soon" note in the hint row instead
//   of opening something else, so the key keeps its one meaning (P3). Same for CONTEXT, digits on a channel (Find),
//   long QUICK (key lock) and long SELECT (save).
// - Quick settings: Power, Squelch and Brightness are value tiles (SELECT or the digit opens an editor, Up/Dn or the
//   encoder change the value, SELECT / QUICK / BACK close it). Power is an editor rather than the doc's cycle tile
//   because radios have up to 9 levels. VFO on/off sits on tile 7 (Scan's place) until the Hub's Channels list,
//   its real home, exists. Other tiles show their value grayed and say "not available yet".
// - VFO entry: digits on Home in VFO type the frequency, decimal point implied after 3 MHz digits; digits that
//   can't lead to a frequency in any band are refused with the bands in the hint. SELECT tunes, BACK deletes a
//   digit (and leaves when empty), 10 s without a key cancels.
// - Backlight: full on any key and while receiving or transmitting; dims to a fifth after 30 s without either,
//   off after 2 min. When it is off, the first key only wakes the screen (PTT and MONITOR still work: the input
//   layer sends them to the radio directly).
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ui.h"
#include "ui_screens.h"
#include "input/input.h"
#include "radio/radio.h"

#define UI_TASK_PRIORITY	(tskIDLE_PRIORITY + 1)	// below the radio and input tasks
#define UI_TASK_STACK_WORDS	384	// measured worst path about 700 bytes plus the display driver

#define SPLASH_MS		1000
#define FRAME_MIN_MS		50	// at most 20 redraws a second
#define REFRESH_MS		1000	// clocks and battery
#define REFRESH_TX_MS		250	// TX timer
#define TOAST_MS		1800
#define ENTRY_TIMEOUT_MS	10000
#define BACKLIGHT_DIM_MS	30000
#define BACKLIGHT_OFF_MS	120000

typedef enum { SCR_HOME, SCR_QUICK, SCR_DIAG } screen_t;

static ui_state_t st;
static radio_view_t view;	// static: too large for a modest task stack
static screen_t screen = SCR_DIAG;	// bring-up: diagnostics first; BACK goes Home, long BACK on Home returns
static uint32_t toast_until, entry_last;
static uint32_t radio_active_ms;
static uint8_t backlight_now = 0xFF;
static bool dirty = true;

static void toast(const char *text)
{
	st.toast = text;
	toast_until = hal_millis() + TOAST_MS;
	dirty = true;
}

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
	if (idle < BACKLIGHT_DIM_MS)
		return st.brightness;
	if (idle < BACKLIGHT_OFF_MS) {
		uint8_t dim = st.brightness / 5;
		return dim < 5 ? 5 : dim;
	}
	return 0;
}

// --- VFO frequency entry ---

static bool entry_value(const char *digits, int len, uint32_t *lo, uint32_t *hi)
{
	// Digits are positional from 100 MHz down to 10 Hz: the prefix spans [lo, hi]
	uint32_t v = 0, unit = 10;	// Hz of the last of UI_ENTRY_MAX digits
	for (int i = 0; i < UI_ENTRY_MAX; i++) {
		v = v * 10u + (uint32_t)(i < len ? digits[i] - '0' : 0);
	}
	uint32_t span = unit;
	for (int i = len; i < UI_ENTRY_MAX; i++)
		span *= 10u;
	*lo = v * unit;
	*hi = *lo + span - unit;
	return true;
}

static bool entry_possible(const char *digits, int len)
{
	uint32_t lo, hi;
	entry_value(digits, len, &lo, &hi);
	for (int i = 0; i < board.band_count; i++)
		if (lo <= board.bands[i].hi_hz && hi >= board.bands[i].lo_hz)
			return true;
	return false;
}

static void entry_cancel(void)
{
	st.entry = false;
	st.entry_len = 0;
	st.entry_digits[0] = 0;
	dirty = true;
}

static void entry_digit(char d)
{
	static char hint[40];
	if (st.entry_len >= UI_ENTRY_MAX) {
		toast("That's all the digits: OK tunes");
		return;
	}
	st.entry_digits[st.entry_len] = d;
	if (!entry_possible(st.entry_digits, st.entry_len + 1)) {
		st.entry_digits[st.entry_len] = 0;
		char bands[32];
		ui_bands_text(bands, sizeof bands);
		strcpy(hint, "Bands ");
		strncat(hint, bands, sizeof hint - strlen(hint) - 1);
		toast(hint);
		return;
	}
	st.entry_len++;
	st.entry_digits[st.entry_len] = 0;
	st.entry = true;
	dirty = true;
}

static void entry_commit(void)
{
	if (st.entry_len < UI_ENTRY_MHZ) {
		toast("Type at least the MHz digits");
		return;
	}
	uint32_t lo, hi;
	entry_value(st.entry_digits, st.entry_len, &lo, &hi);
	for (int i = 0; i < board.band_count; i++) {
		if (lo >= board.bands[i].lo_hz && lo <= board.bands[i].hi_hz) {
			radio_req_vfo_set(lo);
			entry_cancel();
			return;
		}
	}
	toast("Not in a band: type more digits");
}

// --- quick settings ---

static void quick_change(int tile, int8_t dir)
{
	switch (tile) {
	case QT_POWER:
		radio_req_power(dir);
		break;
	case QT_SQUELCH:
		if ((dir > 0 && view.squelch < 9) || (dir < 0 && view.squelch > 0))
			radio_req_squelch(dir);
		break;
	case QT_BRIGHTNESS: {
		int b = st.brightness + dir * 10;
		st.brightness = (uint8_t)(b < 10 ? 10 : b > 100 ? 100 : b);
		break;
	}
	default:
		break;
	}
	dirty = true;
}

static void quick_activate(int tile)
{
	char label[12], value[12];
	st.quick_focus = (uint8_t)tile;
	if (!ui_quick_tile(tile, &view, &st, label, sizeof label, value, sizeof value)) {
		toast("Not available yet");
		return;
	}
	if (tile == QT_VFO) {
		radio_req_vfo(!view.vfo);
		toast(view.vfo ? "Back to channels" : "VFO on");
	} else {
		st.quick_edit = true;
	}
	dirty = true;
}

static void quick_open(void)
{
	screen = SCR_QUICK;
	st.quick_focus = QT_POWER;
	st.quick_edit = false;
	st.toast = NULL;
	entry_cancel();
}

static void go_home(void)
{
	screen = SCR_HOME;
	st.quick_edit = false;
	st.toast = NULL;
	entry_cancel();
}

static int8_t nav_dir(const input_event_t *ev)
{
	switch (ev->action) {
	case ACT_NAV_UP: return 1;
	case ACT_NAV_DOWN: return -1;
	case ACT_ENC_STEP: return ev->value > 0 ? 1 : ev->value < 0 ? -1 : 0;
	default: return 0;
	}
}

static int steps(const input_event_t *ev)
{
	if (ev->action != ACT_ENC_STEP)
		return 1;
	int n = ev->value < 0 ? -ev->value : ev->value;
	return n > 8 ? 8 : n;
}

static void quick_key(const input_event_t *ev)
{
	int8_t dir = nav_dir(ev);
	bool press = ev->gesture == GESTURE_PRESS;
	if (dir != 0 && (press || ev->gesture == GESTURE_REPEAT || ev->gesture == GESTURE_LONG ||
			ev->action == ACT_ENC_STEP)) {
		for (int i = steps(ev); i > 0; i--) {
			if (st.quick_edit)
				quick_change(st.quick_focus, dir);
			else
				st.quick_focus = (uint8_t)((st.quick_focus + UI_QUICK_TILES - dir) % UI_QUICK_TILES);
		}
		dirty = true;
		return;
	}
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && (press || ev->gesture == GESTURE_LONG)) {
		int d = ev->action - ACT_DIGIT_0;
		if (st.quick_edit && st.quick_focus == QT_SQUELCH) {
			for (int n = d - view.squelch; n != 0; n += n > 0 ? -1 : 1)
				radio_req_squelch(n > 0 ? 1 : -1);
			st.quick_edit = false;
		} else if (d == 0) {
			toast("All settings: coming soon");
		} else {
			st.quick_edit = false;
			quick_activate(d - 1);
		}
		dirty = true;
		return;
	}
	switch (ev->action) {
	case ACT_SELECT:
		if (ev->gesture == GESTURE_LONG)
			toast("Saving to the channel: coming soon");
		else if (press && st.quick_edit)
			st.quick_edit = false;
		else if (press)
			quick_activate(st.quick_focus);
		break;
	case ACT_BACK:
		if (ev->gesture == GESTURE_LONG)
			go_home();
		else if (press && st.quick_edit)
			st.quick_edit = false;
		else if (press)
			go_home();
		break;
	case ACT_QUICK:
		if (press) {
			if (st.quick_edit)
				st.quick_edit = false;
			else
				go_home();
		}
		break;
	default:
		break;
	}
	dirty = true;
}

// Next / previous zone, wrapping; the new zone's name is shown as a toast
static void zone_step(int dir)
{
	static zone_t z;	// large (80 channels): not on the UI task's stack
	uint16_t n = codeplug_zone_count();
	if (n < 2) {
		toast("Only one zone");
		return;
	}
	uint16_t next = (uint16_t)((view.zone_index + n + dir) % n);
	radio_req_zone(next);
	if (codeplug_zone(next, &z)) {
		static char msg[CODEPLUG_NAME_LEN + 8];
		strcpy(msg, "Zone: ");
		strncat(msg, z.name, CODEPLUG_NAME_LEN);
		toast(msg);
	}
}

static void home_key(const input_event_t *ev)
{
	int8_t dir = nav_dir(ev);
	bool press = ev->gesture == GESTURE_PRESS;
	if (dir != 0 && (press || ev->gesture == GESTURE_REPEAT || ev->gesture == GESTURE_LONG ||
			ev->action == ACT_ENC_STEP)) {
		if (st.entry)
			entry_cancel();
		for (int i = steps(ev); i > 0; i--) {
			if (view.vfo)
				radio_req_vfo_step(dir);
			else
				radio_req_channel_step(dir);
		}
		return;
	}
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && (press || ev->gesture == GESTURE_LONG)) {
		if (view.vfo) {
			entry_last = hal_millis();
			entry_digit((char)('0' + (ev->action - ACT_DIGIT_0)));
		} else {
			toast("Find: coming soon");
		}
		return;
	}
	if (ev->gesture == GESTURE_LONG) {
		switch (ev->action) {
		case ACT_BACK: go_home(); break;
		case ACT_SELECT: toast(view.temporary ? "Saving to the channel: coming soon" : "Nothing to save"); break;
		case ACT_QUICK: toast("Key lock: coming soon"); break;
		case ACT_CONTEXT: toast("Recent: coming soon"); break;
		default: break;
		}
		return;
	}
	if (!press)
		return;
	switch (ev->action) {
	case ACT_SELECT:
		if (st.entry)
			entry_commit();
		else
			toast("Menu: coming soon");
		break;
	case ACT_BACK:
		if (st.entry) {
			entry_last = hal_millis();
			if (--st.entry_len == 0)
				entry_cancel();
			else
				st.entry_digits[st.entry_len] = 0;
			dirty = true;
		} else if (st.toast != NULL) {
			st.toast = NULL;
			dirty = true;
		} else if (view.temporary) {
			radio_req_clear_temporary();
			toast("Changes undone");
		}
		break;
	case ACT_QUICK:
		quick_open();
		break;
	case ACT_CONTEXT:
		toast("Caller actions: coming soon");
		break;
	case ACT_USER_3:
		if (ev->gesture == GESTURE_LONG) {
			screen = SCR_DIAG;	// long top key: diagnostics (bench checks), until the Hub has a place for it
			break;
		}
		toast("Not assigned yet");
		break;
	case ACT_USER_1:
	case ACT_USER_4:
		if (ev->gesture == GESTURE_PRESS)
			zone_step(ev->action == ACT_USER_4 ? 1 : -1);	// interim "Zone picker" until the Hub exists
		break;
	case ACT_TALK_TARGET:
	case ACT_USER_2:
		toast("Not assigned yet");
		break;
	default:
		break;
	}
}

static void handle_key(const input_event_t *ev)
{
	if (ev->action == ACT_PTT || ev->action == ACT_MONITOR)
		return;		// the input layer already sent these to the radio
	if (screen == SCR_DIAG) {
		if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS)
			screen = SCR_HOME;
		else if (ev->action != ACT_BACK && ev->action != ACT_SELECT && ev->action != ACT_QUICK)
			home_key(ev);	// channel, VFO and digits still work underneath
	} else if (screen == SCR_HOME && ev->action == ACT_BACK && ev->gesture == GESTURE_LONG) {
		screen = SCR_DIAG;
	} else if (screen == SCR_QUICK) {
		quick_key(ev);
	} else {
		home_key(ev);
	}
	dirty = true;
}

static void draw(void)
{
	st.now_ms = hal_millis();
	st.battery_pct = board.power->battery_pct();
	radio_get_view(&view);
	if (screen == SCR_DIAG)
		ui_draw_diag(&view, input_raw_keys(), input_encoder_total());
	else if (screen == SCR_QUICK)
		ui_draw_quick(&view, &st);
	else
		ui_draw_home(&view, &st);
	gfx_flush();
}

static void ui_task(void *arg)
{
	(void)arg;
	bool have_fb = gfx_init(board.display);
	st.brightness = 100;
	input_event_t ev;

	if (have_fb) {
		ui_layout_init();
		ui_draw_splash("OpenDigitalRadio", board.name, ODR_GIT);
		gfx_flush();
	}
	set_backlight(st.brightness);
	vTaskDelay(pdMS_TO_TICKS(SPLASH_MS));
	while (input_next(&ev, 0))
		;	// keys pressed during the splash are dropped
	radio_get_view(&view);

	uint32_t serial = radio_view_serial() - 1u;
	uint32_t last_draw = 0, last_refresh = 0;
	radio_active_ms = hal_millis();
	for (;;) {
		uint32_t now = hal_millis();
		uint32_t wait = 100;
		if (dirty) {
			uint32_t since = now - last_draw;
			wait = since >= FRAME_MIN_MS ? 0 : FRAME_MIN_MS - since;
		}
		if (input_next(&ev, wait)) {
			bool was_off = backlight_now == 0;
			set_backlight(st.brightness);
			if (!was_off)
				handle_key(&ev);
			dirty = true;
		}
		now = hal_millis();

		uint32_t s = radio_view_serial();
		if (s != serial) {
			serial = s;
			radio_get_view(&view);
			dirty = true;
		}
		if (view.state != RS_IDLE)
			radio_active_ms = now;
		if (st.toast != NULL && (int32_t)(now - toast_until) >= 0) {
			st.toast = NULL;
			dirty = true;
		}
		if (st.entry && now - entry_last >= ENTRY_TIMEOUT_MS)
			entry_cancel();
		if (st.entry && !view.vfo)
			entry_cancel();
		uint32_t refresh = (view.state == RS_TX || screen == SCR_DIAG) ? REFRESH_TX_MS : REFRESH_MS;
		if (now - last_refresh >= refresh) {
			last_refresh = now;
			dirty = true;
		}
		set_backlight(backlight_target(now));

		if (dirty && have_fb && now - last_draw >= FRAME_MIN_MS) {
			dirty = false;
			last_draw = now;
			draw();
		} else if (!have_fb) {
			dirty = false;
		}
	}
}

void ui_start(void)
{
	xTaskCreate(ui_task, "ui", UI_TASK_STACK_WORDS, NULL, UI_TASK_PRIORITY, NULL);
}
