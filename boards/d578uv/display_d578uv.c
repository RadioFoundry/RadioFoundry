// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV display: 1.77" 160x128 ST7735S-class TFT on an 8-bit 8080 bus driven by GPIO (docs/spec/hardware-d578uv.md
// 5): data on PE7-PE14, a write strobe, D/C, chip select and reset on PB10-PB13, RD held high.
#include "d578uv.h"
#include "config.h"
#include "pins.h"
#include "mcu_io.h"
#include "hal_time.h"
#include "display/st7735/st7735.h"

#define LCD_W		160u
#define LCD_H		128u

static bool ready;

// One byte: the data lines in one store (set and clear at once), then WR low and high. The panel takes the byte on
// WR's rising edge; WR low is held for two stores, above the 15 ns minimum at 120 MHz, and the whole cycle stays over
// the 66 ns write cycle
static inline void put_byte(uint8_t v)
{
	mcu_port_put(LCD_DATA_PORT, (uint16_t)(v << LCD_DATA_SHIFT), (uint16_t)((uint8_t)~v << LCD_DATA_SHIFT));
	mcu_pin_write(LCD_WR_PORT, LCD_WR_BIT, false);
	mcu_pin_write(LCD_WR_PORT, LCD_WR_BIT, false);
	mcu_pin_write(LCD_WR_PORT, LCD_WR_BIT, true);
	mcu_pin_write(LCD_WR_PORT, LCD_WR_BIT, true);
}

static void bus_begin(void)
{
	mcu_pin_write(PIN_LCD_CS, false);
}

static void bus_end(void)
{
	mcu_pin_write(PIN_LCD_CS, true);
}

static void bus_cmd(uint8_t c)
{
	mcu_pin_write(PIN_LCD_RS, false);
	put_byte(c);
	mcu_pin_write(PIN_LCD_RS, true);
}

static void bus_data(const uint8_t *d, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		put_byte(d[i]);
	}
}

static void bus_pixels(const uint16_t *px, uint32_t n, bool swap_rb)
{
	for (uint32_t i = 0; i < n; i++) {
		uint16_t v = px[i];
		if (swap_rb) {
			v = (uint16_t)((v & 0x07E0) | (v >> 11) | (v << 11));
		}
		put_byte((uint8_t)(v >> 8));
		put_byte((uint8_t)v);
	}
}

static void bus_reset(bool level)
{
	mcu_pin_write(PIN_LCD_RESET, level);
}

static const st7735_bus_t bus = {
	.begin = bus_begin,
	.end = bus_end,
	.cmd = bus_cmd,
	.data = bus_data,
	.pixels = bus_pixels,
	.reset = bus_reset,
	.delay_ms = hal_delay_ms,
};

// The ST7735 table (the stock init sends the same frame rate and power values). Landscape; the stock firmware writes
// MADCTL 0x48 for a portrait 128x160 frame, so the orientation and offsets are to be checked on the radio
static const st7735_t panel = {
	.bus = &bus,
	.type = 0,
	.width = LCD_W,
	.height = LCD_H,
};

static void init(void)
{
	mcu_pin_out(PIN_LCD_CS, true, false);
	mcu_pin_out(PIN_LCD_RS, true, false);
	mcu_pin_out(PIN_LCD_WR, true, false);
	mcu_pin_out(PIN_LCD_RESET, true, false);
	mcu_pin_out(PIN_LCD_RD, true, false);
	mcu_pin_out(LCD_DATA_PORT, LCD_DATA_MASK, false, false);
	st7735_init(&panel);
	ready = true;
}

static void flush(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	if (ready) {
		st7735_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
	}
}

static void set_backlight(uint8_t percent)
{
	d578uv_backlight(percent);
}

const hal_display_t d578uv_display = {
	.width = LCD_W,
	.height = LCD_H,
	.bpp = 4,
	.inset_top = CONFIG_DISPLAY_INSET_TOP,
	.inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT,
	.inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.init = init,
	.flush_indexed = flush,
	.set_backlight = set_backlight,
};
