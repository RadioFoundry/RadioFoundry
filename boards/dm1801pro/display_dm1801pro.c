// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1801 Pro display: 128x64 monochrome ST7567-class LCD on a write-only serial bus bit-banged on bank C (chip select
// PTC3, clock PTC4, data/command PTC6, data PTC8, reset PTC2; docs/spec/hardware-dm1801pro.md 3.2, 7). The backlight is
// on PTA15, which is also the SoC's PWM0 output: set_backlight() dims it.
#include "dm1801pro.h"
#include "config.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "display/st7567/st7567.h"

#define DR_C		REG32(GPIOC_BASE + GPIO_DR)
#define CLK_BIT		hrc_gpio_bit(PIN_LCD_CLK)
#define DATA_BIT	hrc_gpio_bit(PIN_LCD_DATA)
#define LCD_PINS_GPIO	(0xFFu << 3)	// IO_DIPLEX2: PTC2-PTC8 (and a neighbor either side of the mapping) as GPIOs

static bool ready;

// One byte, MSB first: data set while the clock is low, taken by the panel on the rising edge. Bank C's other pins are
// written with interrupts off, so the read-modify-write can't lose them
static void send_byte(uint8_t v)
{
	uint32_t s = hrc_irq_save();
	uint32_t dr = DR_C & ~(CLK_BIT | DATA_BIT);
	for (uint8_t m = 0x80; m != 0; m >>= 1) {
		uint32_t low = dr | ((v & m) ? DATA_BIT : 0u);
		DR_C = low;
		DR_C = low | CLK_BIT;
	}
	DR_C = dr | CLK_BIT;	// the clock idles high
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
}

static void bus_data(const uint8_t *d, uint32_t n)
{
	hrc_pin_write(PIN_LCD_DC, true);
	for (uint32_t i = 0; i < n; i++) {
		send_byte(d[i]);
	}
}

static void bus_reset(bool level)
{
	hrc_pin_write(PIN_LCD_RESET, level);
}

static const st7567_bus_t bus = {
	.begin = bus_begin,
	.end = bus_end,
	.cmd = bus_cmd,
	.data = bus_data,
	.reset = bus_reset,
	.delay_ms = hal_delay_ms,
};

// Column offset 4 and the page order are the ones the radio uses; the orientation (columns reversed, rows not) was
// checked on the radio. Bias, regulator and contrast are the controller's common values (hardware-dm1801pro.md 7)
static const st7567_t panel = {
	.bus = &bus,
	.col_offset = 4,
	.seg_reverse = true,
	.com_reverse = false,
	.regulator = 4,
	.contrast = 0x20,
	.bias = 0,
};

static void init(void)
{
	uint32_t s = hrc_irq_save();
	IO_DIPLEX2 |= LCD_PINS_GPIO;
	hrc_irq_restore(s);
	hrc_pin_out(PIN_LCD_CS, true);
	hrc_pin_out(PIN_LCD_CLK, true);
	hrc_pin_out(PIN_LCD_DC, false);
	hrc_pin_out(PIN_LCD_DATA, false);
	hrc_pin_out(PIN_LCD_RESET, true);
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
	hrc_backlight(percent);	// PTA15 is PWM0, as on the DM-32UV (stock only switches it on and off)
}

const hal_display_t dm1801pro_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = ST7567_WIDTH,
	.height = ST7567_HEIGHT,
	.bpp = 1,
	.init = init,
	.flush = flush,
	.set_backlight = set_backlight,
};
