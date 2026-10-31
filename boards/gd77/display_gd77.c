// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 display: a UC1701 / ST7567-class 128x64 monochrome LCD on a write-only serial bus on port C (chip select PTC8,
// reset PTC9, A0 PTC10, clock PTC11, data PTC12; docs/spec/hardware-gd77.md 7), reached through gd77_lcd_bus. The
// controller driver is the shared one (drivers/display/st7567). The backlight on PTC4 is on or off; a build with
// GD77_BACKLIGHT_PWM (bus_cfg.h) dims it with an FTM channel on the same pin instead (unverified pin mux).
#include "gd77.h"
#include "bus_cfg.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "display/st7567/st7567.h"

static bool ready;
static bool pwm;	// the backlight runs on the FTM channel

static void bus_begin(void)
{
	gd77_lcd_bus.select(true);
}

static void bus_end(void)
{
	gd77_lcd_bus.select(false);
}

static void bus_cmd(uint8_t c)
{
	k22f_pin_write(PIN_LCD_A0, false);
	(void)gd77_lcd_bus.xfer(&c, NULL, 1);
}

static void bus_data(const uint8_t *d, uint32_t n)
{
	k22f_pin_write(PIN_LCD_A0, true);
	(void)gd77_lcd_bus.xfer(d, NULL, n);
}

static void bus_reset(bool level)
{
	k22f_pin_write(PIN_LCD_RESET, level);
}

static const st7567_bus_t bus = {
	.begin = bus_begin,
	.end = bus_end,
	.cmd = bus_cmd,
	.data = bus_data,
	.reset = bus_reset,
	.delay_ms = hal_delay_ms,
};

// Column offset 4 (132-column RAM, 128 visible) with the segment order reversed and the common order normal; bias 1/9,
// electronic volume 0x11. Unverified on a radio: the orientation and contrast are the first things to check
// (hardware-gd77.md 7). The regulator ratio isn't known; 4 is a starting value
static const st7567_t panel = {
	.bus = &bus,
	.col_offset = 4,
	.seg_reverse = true,
	.com_reverse = false,
	.regulator = 4,
	.contrast = 0x11,
	.bias = 0,
};

static void backlight_init(void)
{
	const k22f_mux_t *m = k22f_mux_find(PIN_BACKLIGHT, GD77_BACKLIGHT_FN);
	if (!GD77_BACKLIGHT_PWM || gd77_backlight_cfg_check(true) != 0 || m == 0) {
		return;
	}
	// Off on the GPIO until the FTM runs, then the pin moves to the channel (which starts at 0 %)
	if (k22f_pwm_start(GD77_BACKLIGHT_FTM, GD77_BACKLIGHT_CH, GD77_BACKLIGHT_HZ)) {
		k22f_pin_config(PIN_BACKLIGHT, K22F_MUX(m->alt));
		pwm = true;
	}
}

bool gd77_backlight_pwm(void)
{
	return pwm;
}

static void init(void)
{
	backlight_init();
	st7567_init(&panel);
	ready = true;
}

static void flush(const void *fb, uint16_t y0, uint16_t y1)
{
	if (!ready || fb == NULL) {
		return;
	}
	st7567_write_rows(&panel, (const uint8_t *)fb, y0, y1);
}

static void set_backlight(uint8_t percent)
{
	if (pwm) {
		k22f_pwm_set(GD77_BACKLIGHT_FTM, GD77_BACKLIGHT_CH, percent > 100u ? 100u : percent);
	} else {
		k22f_pin_write(PIN_BACKLIGHT, percent > 0);
	}
}

const hal_display_t gd77_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = ST7567_WIDTH,
	.height = ST7567_HEIGHT,
	.bpp = 1,
	.init = init,
	.flush = flush,
	.set_backlight = set_backlight,
};
