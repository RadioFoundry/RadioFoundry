// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 display: 320x240 ILI9341-class TFT on the SoC's i8080 LCD controller, backlight PWM0 on PTA15
//. Full resolution in 16 colors, as in hardware-dm32.md: the core's frame buffer
// holds a palette index per pixel (37.5 KB) and each row goes out as RGB565.
//
// Which controller is fitted comes from the panel record in program flash at 0x031FD000: "lcd" and a type. Type 1 is
// a register-style panel; the others are command-style init sets.
#include "gd88.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "display/ili9341/ili9341.h"

// IO_DIPLEX2 bits cleared: the bus pins to the LCD controller
#define LCDC_PINS		0x7FFF8u

#define PANEL_W			320u
#define PANEL_H			240u
#define PANEL_RECORD		0x031FD000u
// The record is four bytes, "lcd" and the type 0..3 (an ASCII digit is taken too); without one
// variant 3. A panel that answers ID 0xC505 to command 0xD3 is variant 1, but RadioFoundry
// doesn't read the panel's ID, so such a panel needs its record
#define PANEL_DEFAULT		ILI9341_VARIANT_3

static bool ready;
static ili9341_t panel;

static void bus_begin(void)
{
}

static void bus_reset(bool level)
{
	hrc_pin_write(PIN_LCD_RESET, level);
}

static const ili9341_bus_t bus = {
	.begin = bus_begin,
	.end = hrc_lcdc_drain,
	.cmd = hrc_lcdc_cmd,
	.data = hrc_lcdc_data,
	.pixels = hrc_lcdc_pixels,
	.pixels4 = hrc_lcdc_pixels4,
	.reset = bus_reset,
	.delay_ms = hal_delay_ms,
};

static bool from_record;

static uint8_t panel_variant(void)
{
	const volatile uint8_t *r = (const volatile uint8_t *)PANEL_RECORD;
	if (r[0] != 'l' || r[1] != 'c' || r[2] != 'd') {
		return PANEL_DEFAULT;
	}
	uint8_t t = r[3];
	if (t >= '0' && t <= '9') {
		t = (uint8_t)(t - '0');
	}
	from_record = t < ILI9341_VARIANTS;
	return from_record ? t : PANEL_DEFAULT;
}

uint8_t gd88_panel_variant(void)
{
	return panel.variant;
}

bool gd88_panel_from_record(void)
{
	return from_record;
}

static void lcdc_init(void)
{
	uint32_t s = hrc_irq_save();
	IO_DIPLEX2 &= ~LCDC_PINS;
	hrc_irq_restore(s);
	hrc_pin_out(PIN_LCD_RESET, true);
	hrc_lcdc_start();
}

static void init(void)
{
	lcdc_init();
	panel.bus = &bus;
	panel.width = PANEL_W;
	panel.height = PANEL_H;
	panel.variant = panel_variant();
	hrc_pin_out(PIN_KEY_LIGHT, false);
	ili9341_init(&panel);
	ready = true;
}

static void flush(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	if (!ready || fb == NULL) {
		return;
	}
	ili9341_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
}

static void flush_rect(const void *fb, const uint16_t *palette, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	if (!ready || fb == NULL) {
		return;
	}
	ili9341_write_rect_indexed(&panel, (const uint8_t *)fb, palette, x0, y0, x1, y1);
}

static void set_backlight(uint8_t percent)
{
	hrc_backlight(percent);
}

static void set_key_light(bool on)
{
	hrc_pin_write(PIN_KEY_LIGHT, on);
}

const hal_display_t gd88_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = PANEL_W,
	.height = PANEL_H,
	.bpp = 4,
	.init = init,
	.flush_indexed = flush,
	.flush_rect = flush_rect,
	.set_backlight = set_backlight,
	.set_key_light = set_key_light,
};
