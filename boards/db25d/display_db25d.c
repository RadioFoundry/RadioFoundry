// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D display: 1.77" 160x128 ST7735S-class TFT on a 4-wire serial bus, bit-banged on bank C (docs/spec/hardware-db25d.md
// 3.1, 5). Bit-banging works on both board variants: the ADC 2 strap only says whether the stock firmware uses the SPI2
// controller on the same pins. The backlight pin isn't known yet; the loader leaves the backlight on.
#include "db25d.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "display/st7735/st7735.h"

#define LCD_W		160u
#define LCD_H		128u
#define DR_C		REG32(GPIOC_BASE + GPIO_DR)
#define SCL_BIT		hrc_gpio_bit(PIN_LCD_SCL)
#define SDA_BIT		hrc_gpio_bit(PIN_LCD_SDA)

static bool ready;

// One byte, MSB first: data set while SCL is low, taken by the panel on the rising edge. Bank C's other outputs (the
// RX front end, the LO chip's enable) are written with interrupts off too, so the read-modify-write can't lose them
static void send_byte(uint8_t v)
{
	uint32_t s = hrc_irq_save();
	uint32_t dr = DR_C & ~(SCL_BIT | SDA_BIT);
	for (uint8_t m = 0x80; m != 0; m >>= 1) {
		uint32_t low = dr | ((v & m) ? SDA_BIT : 0u);
		DR_C = low;
		DR_C = low | SCL_BIT;
	}
	DR_C = dr;
	hrc_irq_restore(s);
}

static void bus_begin(void)
{
	hrc_pin_write(PIN_LCD_CS, false);
}

static void bus_end(void)
{
	hrc_pin_write(PIN_LCD_CS, true);
}

static void bus_cmd(uint8_t c)
{
	hrc_pin_write(PIN_LCD_DC, false);
	send_byte(c);
	hrc_pin_write(PIN_LCD_DC, true);
}

static void bus_data(const uint8_t *d, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		send_byte(d[i]);
	}
}

static void bus_pixels(const uint16_t *px, uint32_t n, bool swap_rb)
{
	for (uint32_t i = 0; i < n; i++) {
		uint16_t v = px[i];
		if (swap_rb) {
			v = (uint16_t)((v & 0x07E0) | (v >> 11) | (v << 11));
		}
		send_byte((uint8_t)(v >> 8));
		send_byte((uint8_t)v);
	}
}

static void bus_reset(bool level)
{
	hrc_pin_write(PIN_LCD_RESET, level);
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

// Type 1: the ST7735 table with MADCTL 0x60, the landscape value the stock firmware writes (5). Offsets to check on the
// radio
static const st7735_t panel = {
	.bus = &bus,
	.type = 1,
	.width = LCD_W,
	.height = LCD_H,
};

static void init(void)
{
	hrc_pin_out(PIN_LCD_CS, true);
	hrc_pin_out(PIN_LCD_DC, true);
	hrc_pin_out(PIN_LCD_SCL, false);
	hrc_pin_out(PIN_LCD_SDA, false);
	hrc_pin_out(PIN_LCD_RESET, true);
	st7735_init(&panel);
	ready = true;
}

static void flush(const void *fb, uint16_t y0, uint16_t y1)
{
	if (ready) {
		st7735_write_rows(&panel, (const uint16_t *)fb, y0, y1);
	}
}

static void set_backlight(uint8_t percent)
{
	(void)percent;
}

const hal_display_t db25d_display = {
	.width = LCD_W,
	.height = LCD_H,
	.bpp = 16,
	.inset_top = CONFIG_DISPLAY_INSET_TOP,
	.inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT,
	.inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.init = init,
	.flush = flush,
	.set_backlight = set_backlight,
};
