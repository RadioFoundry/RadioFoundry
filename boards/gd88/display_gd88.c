// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 display: 320x240 ILI9341-class TFT on the SoC's i8080 LCD controller, backlight PWM0 on PTA15
// (docs/spec/hardware-gd88.md 9.4). Full resolution in 16 colors, as in hardware-dm32.md: the core's frame buffer
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

#define LCDC_CMD		REG32(LCDC_BASE + 0x00)	// write: command byte (RS low)
#define LCDC_DATA		REG32(LCDC_BASE + 0x04)	// write: data byte (RS high)
#define LCDC_CFG		REG32(LCDC_BASE + 0x10)
#define LCDC_EN			REG32(LCDC_BASE + 0x28)	// b0 enable, b1 ready
// TODO(gd88): unknown, needs a radio: the controller's timing word. hardware-dm32.md's is used
#define LCDC_CFG_VALUE		0x303u
#define LCDC_PINS		0x7FFF8u		// IO_DIPLEX2 bits cleared: the bus pins to the controller
#define LCDC_READY_SPINS	100000u

#define PANEL_W			320u
#define PANEL_H			240u
#define PANEL_RECORD		0x031FD000u
// TODO(gd88): unknown, needs a radio: the record's layout past "lcd" (the type is taken as the next byte, a digit or
// a number) and the variant to use without one
#define PANEL_DEFAULT		ILI9341_VARIANT_3

static bool ready;
static ili9341_t panel;

static inline void byte_gap(void)
{
	for (volatile uint32_t i = 0; i < 4; i++) {
	}
}

static void bus_begin(void)
{
}

static void bus_end(void)
{
	byte_gap();
}

static void bus_cmd(uint8_t c)
{
	LCDC_CMD = c;
	byte_gap();
}

static void bus_data(const uint8_t *d, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		LCDC_DATA = d[i];
		byte_gap();
	}
}

static void bus_pixels(const uint16_t *px, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		LCDC_DATA = px[i] >> 8;
		byte_gap();
		LCDC_DATA = px[i] & 0xFFu;
		byte_gap();
	}
}

static void bus_reset(bool level)
{
	hrc_pin_write(PIN_LCD_RESET, level);
}

static const ili9341_bus_t bus = {
	.begin = bus_begin,
	.end = bus_end,
	.cmd = bus_cmd,
	.data = bus_data,
	.pixels = bus_pixels,
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
	LCDC_CFG = LCDC_CFG_VALUE;
	LCDC_EN |= 1u;
	for (uint32_t n = LCDC_READY_SPINS; n != 0 && (LCDC_EN & 2u) == 0; n--) {
	}
}

static void init(void)
{
	lcdc_init();
	panel.bus = &bus;
	panel.width = PANEL_W;
	panel.height = PANEL_H;
	panel.variant = panel_variant();
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

static void set_backlight(uint8_t percent)
{
	hrc_backlight(percent);
}

const hal_display_t gd88_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = PANEL_W,
	.height = PANEL_H,
	.bpp = 4,
	.init = init,
	.flush_indexed = flush,
	.set_backlight = set_backlight,
};
