// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV display: 240x320 ST7789-class TFT on the SoC's i8080 LCD controller, backlight on GPIO 0x0F / PWM0
// (docs/spec/hardware-dm32.md 3.6, 3.10, 7). Shown at half resolution: the core draws 120x160 and every pixel goes
// out as a 2x2 block (the core's frame buffer is 40 KB, core/ui/gfx.h).
//
// The LCD data lines DB0-DB4 are also the keypad rows. Every LCD burst holds dm32_lcdbus_lock() and hands the bus
// pins to the controller itself, so the key scan may leave them as GPIOs when it unlocks.
#include "dm32.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "display/st7789/st7789.h"

#define LCDC_CMD		REG32(LCDC_BASE + 0x00)	// write: command byte (RS low)
#define LCDC_DATA		REG32(LCDC_BASE + 0x04)	// write: data byte (RS high)
#define LCDC_CFG		REG32(LCDC_BASE + 0x10)
#define LCDC_EN			REG32(LCDC_BASE + 0x28)	// b0 enable, b1 ready
#define LCDC_CFG_VALUE		0x303u
#define LCDC_PINS_MASK		0xFFF80107u		// IO_DIPLEX2 &= this: CS, RS, WR, DB0-DB7 to the controller

#define SCALE			2u
#define PANEL_W			240u
#define PANEL_H			320u

static bool ready;

// The port needed a few cycles after each byte (3.10)
static inline void byte_gap(void)
{
	for (volatile uint32_t i = 0; i < 4; i++) {
	}
}

static void lcd_pins(void)
{
	uint32_t s = hrc_irq_save();
	IO_DIPLEX2 &= LCDC_PINS_MASK;
	hrc_irq_restore(s);
}

static void bus_begin(void)
{
	dm32_lcdbus_lock();
	lcd_pins();
}

static void bus_end(void)
{
	byte_gap();
	dm32_lcdbus_unlock();
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

static const st7789_bus_t bus = {
	.begin = bus_begin,
	.end = bus_end,
	.cmd = bus_cmd,
	.data = bus_data,
	.pixels = bus_pixels,
	.reset = bus_reset,
	.delay_ms = hal_delay_ms,
};

// Portrait, turned 180 degrees as the stock firmware sets it (MADCTL 0xC0); swap for 0x00 if the picture is upside down
static const st7789_t panel = {
	.bus = &bus,
	.width = PANEL_W,
	.height = PANEL_H,
	.madctl = 0xC0,
	.invert = true,
	.scale = SCALE,
};

static void lcdc_init(void)
{
	hrc_pin_out(PIN_LCD_RD, true);
	hrc_pin_out(PIN_LCD_RESET, true);
	LCDC_CFG = LCDC_CFG_VALUE;
	LCDC_EN |= 1u;
	for (uint32_t n = 100000; n != 0 && (LCDC_EN & 2u) == 0; n--) {
	}
}

static void set_backlight(uint8_t percent)
{
	hrc_backlight(percent);
}

static void init(void)
{
	lcdc_init();
	st7789_init(&panel);
	ready = true;
}

static void flush(const void *fb, uint16_t y0, uint16_t y1)
{
	if (!ready || fb == NULL) {
		return;
	}
	st7789_write_rows(&panel, (const uint16_t *)fb, y0, y1);
}

const hal_display_t dm32_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = PANEL_W / SCALE,
	.height = PANEL_H / SCALE,
	.bpp = 16,
	.init = init,
	.flush = flush,
	.set_backlight = set_backlight,
};
