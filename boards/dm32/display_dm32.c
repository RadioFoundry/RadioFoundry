// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-32UV display: 240x320 ST7789-class TFT on the SoC's i8080 LCD controller, backlight on GPIO 0x0F / PWM0
// (docs/spec/hardware-dm32.md 3.6, 3.10, 7). Full resolution in 16 colors: the core's frame buffer holds a palette
// index per pixel (37.5 KB; RGB565 would take 150 KB), and each row goes out as RGB565 through the palette.
//
// The LCD data lines DB0-DB4 are also the keypad rows. Every LCD burst holds dm32_lcdbus_lock() and hands the bus
// pins to the controller itself, so the key scan may leave them as GPIOs when it unlocks.
#include "dm32.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "display/st7789/st7789.h"

#define LCDC_PINS_MASK		0xFFF80107u		// IO_DIPLEX2 &= this: CS, RS, WR, DB0-DB7 to the controller

#define PANEL_W			240u
#define PANEL_H			320u

static bool ready;

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
	hrc_lcdc_drain();
	dm32_lcdbus_unlock();
}

static void bus_reset(bool level)
{
	hrc_pin_write(PIN_LCD_RESET, level);
}

static const st7789_bus_t bus = {
	.begin = bus_begin,
	.end = bus_end,
	.cmd = hrc_lcdc_cmd,
	.data = hrc_lcdc_data,
	.pixels = hrc_lcdc_pixels,
	.pixels4 = hrc_lcdc_pixels4,
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
	.scale = 1,
};

static void lcdc_init(void)
{
	hrc_pin_out(PIN_LCD_RD, true);
	hrc_pin_out(PIN_LCD_RESET, true);
	hrc_lcdc_start();
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

static void flush(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	if (!ready || fb == NULL) {
		return;
	}
	st7789_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
}

const hal_display_t dm32_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = PANEL_W,
	.height = PANEL_H,
	.bpp = 4,
	.init = init,
	.flush_indexed = flush,
	.set_backlight = set_backlight,
};
