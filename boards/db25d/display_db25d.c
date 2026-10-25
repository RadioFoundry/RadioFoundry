// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D display: 1.77" 160x128 ST7735S-class TFT on a 4-wire serial bus (docs/spec/hardware-db25d.md 3.1, 5). ADC 2
// is a board strap: 50 or more, the bus is bit-banged with data on PTC7; below 50, it's the SPI2 controller with data on
// PTC8. Clock (PTC4), CS, D/C and reset are the same on both. The backlight is PWM1 on PTA16 and needs PTA15 high.
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
#define ADC2_BITBANG	50u		// ADC 2 at or above: bit-banged bus
// IO_DIPLEX2 fields for SPI2 (the stock SPI2 set-up): bits 6:5 = 01 puts SCLK on PTC4, bits 12:11 = 01 MOSI on PTC8
#define DIPLEX2_SCLK_MASK	(3u << 5)
#define DIPLEX2_SCLK_SPI2	(1u << 5)
#define DIPLEX2_MOSI_MASK	(3u << 11)
#define DIPLEX2_MOSI_SPI2	(1u << 11)
// SPI2 clock divider: the stock value is 2 (18 MHz at the loader's 36 MHz APB). 6 keeps the clock below the ST7735S's
// 15 MHz write limit at any APB clock up to 90 MHz: 14 MHz at RadioFoundry's 84 MHz, 6 MHz at 36 MHz
#define LCD_SPI_BAUDR	6u
_Static_assert(HRC_APB_HZ / LCD_SPI_BAUDR <= 15000000u, "SPI2 is too fast for the ST7735S");

static bool ready;
static bool spi2;

// One byte, MSB first: data set while SCL is low, taken by the panel on the rising edge. Bank C's other outputs (the
// RX front end, the LO chip's enable) are written with interrupts off too, so the read-modify-write can't lose them.
// Each phase is written BB_REPEAT times: a store takes about one APB cycle, so this keeps the phases over 30 ns at any
// APB clock (the panel's write cycle is 66 ns)
#define BB_REPEAT	hrc_bitbang_repeat(HRC_APB_HZ)
static void bitbang_byte(uint8_t v)
{
	uint32_t s = hrc_irq_save();
	uint32_t dr = DR_C & ~(SCL_BIT | SDA_BIT);
	for (uint8_t m = 0x80; m != 0; m >>= 1) {
		uint32_t low = dr | ((v & m) ? SDA_BIT : 0u);
		for (uint32_t r = 0; r < BB_REPEAT; r++) {
			DR_C = low;
		}
		for (uint32_t r = 0; r < BB_REPEAT; r++) {
			DR_C = low | SCL_BIT;
		}
	}
	DR_C = dr;
	hrc_irq_restore(s);
}

// SPI2 waits for the byte to leave the shifter, so D/C can change right after
static void send_byte(uint8_t v)
{
	if (spi2) {
		hrc_ssi_xfer(SPI2_BASE, &v, NULL, 1);
	} else {
		bitbang_byte(v);
	}
}

static void bus_begin(void)
{
	hrc_pin_write(PIN_LCD_CS, false);
	if (spi2) {
		hrc_ssi_select(SPI2_BASE, true);
	}
}

static void bus_end(void)
{
	if (spi2) {
		hrc_ssi_select(SPI2_BASE, false);
	}
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
	spi2 = hrc_adc_read(2) < ADC2_BITBANG;
	if (spi2) {
		uint32_t s = hrc_irq_save();
		IO_DIPLEX2 = (IO_DIPLEX2 & ~(DIPLEX2_SCLK_MASK | DIPLEX2_MOSI_MASK)) | DIPLEX2_SCLK_SPI2 | DIPLEX2_MOSI_SPI2;
		hrc_irq_restore(s);
		hrc_ssi_init(SPI2_BASE, LCD_SPI_BAUDR);
	}
	st7735_init(&panel);
	ready = true;
}

static void flush(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	if (ready) {
		st7735_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
	}
}

// The stock recipe (hardware-db25d.md 5): PWM1 at 1 kHz, CFG bits 5, 8 and 9, repeat 5
static const hrc_backlight_t backlight = { .ch = 1, .cfg = 0x320u, .rpt = 5u, .period = 24000u };

static void set_backlight(uint8_t percent)
{
	hrc_backlight_pwm(&backlight, percent);
}

const hal_display_t db25d_display = {
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
