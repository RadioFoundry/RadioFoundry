// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 display: 128x64 monochrome ST7567 on SPI2, shared with the data flash (chip select PD14, A0 PD13, reset PD12;
// docs/spec/hardware-md9600.md 7), and the backlight PWM on TIM8 CH1 (PC6).
#include "md9600.h"
#include "config.h"
#include "pins.h"
#include "hal_time.h"
#include "display/st7567/st7567.h"

// TIM8 on APB2 (72 MHz): / 10 / 256 = 28 kHz PWM
#define BL_PSC		9u
#define BL_ARR		255u
// The LEDs light between about 16 % and 19.3 % duty and are fully on above (hardware-md9600.md 7.3, unverified):
// 1-99 % maps into that window
#define BL_DIM_MIN	41u
#define BL_DIM_MAX	49u

static bool ready;

static void bus_begin(void)
{
	md9600_spi2_lock();
	PIN_WRITE(PIN_LCD_CS, false);
}

static void bus_end(void)
{
	PIN_WRITE(PIN_LCD_CS, true);
	md9600_spi2_unlock();
}

static void bus_cmd(uint8_t c)
{
	PIN_WRITE(PIN_LCD_A0, false);
	(void)stm32_spi_xfer(SPI2, &c, NULL, 1);
}

static void bus_data(const uint8_t *d, uint32_t n)
{
	PIN_WRITE(PIN_LCD_A0, true);
	(void)stm32_spi_xfer(SPI2, d, NULL, n);
}

static void bus_reset(bool level)
{
	PIN_WRITE(PIN_LCD_RST, level);
}

static const st7567_bus_t bus = {
	.begin = bus_begin,
	.end = bus_end,
	.cmd = bus_cmd,
	.data = bus_data,
	.reset = bus_reset,
	.delay_ms = hal_delay_ms,
};

// Segments normal, COM reversed, bias 1/9, regulator ratio 4, electronic volume 0x28 (hardware-md9600.md 7.2). The
// orientation and the column offset are unverified: the first boot shows whether the picture is mirrored or shifted
static const st7567_t panel = {
	.bus = &bus,
	.col_offset = 0,
	.seg_reverse = false,
	.com_reverse = true,
	.regulator = 4,
	.contrast = 0x28,
	.bias = 0,
};

void md9600_backlight(uint8_t percent)
{
	uint32_t duty;
	if (percent == 0) {
		duty = 0;
	} else if (percent >= 100 || md9600_opt_backlight_switched()) {
		// 100 %, or a radio whose option block says its backlight can't be dimmed (hardware-md9600.md 7.3): a duty
		// in the dimming window would leave it dark or flickering there
		duty = BL_ARR + 1u;	// always high
	} else {
		duty = BL_DIM_MIN + (uint32_t)(percent - 1u) * (BL_DIM_MAX - BL_DIM_MIN) / 98u;
	}
	TIM8->CCR1 = duty;
}

static void backlight_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_TIM8EN;
	(void)RCC->APB2ENR;
	stm32_pin_af(PIN_PORT(PIN_BACKLIGHT), PIN_MASK(PIN_BACKLIGHT), GPIO_AF3_TIM8, false, GPIO_NOPULL);
	TIM8->CR1 = 0;
	TIM8->PSC = BL_PSC;
	TIM8->ARR = BL_ARR;
	TIM8->CCR1 = 0;
	TIM8->CCMR1 = (TIM8->CCMR1 & ~(TIM_CCMR1_OC1M | TIM_CCMR1_CC1S)) | (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
	TIM8->CCER |= TIM_CCER_CC1E;
	TIM8->BDTR |= TIM_BDTR_MOE;	// an advanced timer: outputs need the main output enable
	TIM8->EGR = TIM_EGR_UG;
	TIM8->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

static void init(void)
{
	md9600_spi2_init();
	PIN_OUT(PIN_LCD_A0, false);
	PIN_OUT(PIN_LCD_RST, false);
	st7567_init(&panel);
	backlight_init();
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
	if (ready) {
		md9600_backlight(percent);
	}
}

const hal_display_t md9600_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = ST7567_WIDTH,
	.height = ST7567_HEIGHT,
	.bpp = 1,
	.init = init,
	.flush = flush,
	.set_backlight = set_backlight,
};
