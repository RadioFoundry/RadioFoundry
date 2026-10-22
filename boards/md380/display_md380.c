// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 display: 160x128 TFT on FSMC bank 1 (8-bit on D0-D7, RS on A17), wired as on the DM-1701; backlight on PC6
// with TIM8 CH1 PWM (docs/spec/hardware-md380.md 7).
//
// The controller is reported as an HX8302A (ST7735 compatible) or "maybe an ILI9481": unconfirmed. The ST7735 driver's
// type 0 init is used; MD380_PANEL_TYPE picks another of its types and MD380_PANEL_MADCTL_XOR mirrors the picture,
// both build-time until the panel is seen. All 128 rows are assumed visible.
//
// The LCD data lines D0-D7 are also the keypad columns: every LCD burst holds md380_lcdbus_lock(), starts by
// reconnecting them to the FSMC and ends with a 0x00 write so the lines rest low for the next key scan.
#include "md380.h"
#include "config.h"
#include "pins.h"
#include "hal_time.h"
#include "display/st7735/st7735.h"

#ifndef MD380_PANEL_TYPE
#define MD380_PANEL_TYPE	0
#endif
#ifndef MD380_PANEL_MADCTL_XOR
#define MD380_PANEL_MADCTL_XOR	0
#endif

#define LCD_AF		12	// GPIO_AF12_FSMC
#define LCD_CTRL_PORTD	(GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_12)	// NOE, NWE, A17 (RS)

#define VISIBLE_W	160
#define VISIBLE_H	128

// ---- FSMC bus (same timing as the DM-1701: HCLK 72 MHz, about 100-125 ns per byte) ----

static void fsmc_init(void)
{
	__HAL_RCC_FSMC_CLK_ENABLE();
	FSMC_Bank1->BTCR[0] &= ~FSMC_BCR1_MBKEN;
	FSMC_Bank1->BTCR[1] = (2u << FSMC_BTR1_ADDSET_Pos) | (4u << FSMC_BTR1_DATAST_Pos) | (1u << FSMC_BTR1_BUSTURN_Pos);
	FSMC_Bank1->BTCR[0] = FSMC_BCR1_MWID_0 | FSMC_BCR1_WREN | (1u << 7) | FSMC_BCR1_MBKEN;
}

static void set_af12(GPIO_TypeDef *port, uint16_t pins)
{
	for (uint32_t i = 0; i < 16; i++) {
		if ((pins & (1u << i)) == 0) {
			continue;
		}
		volatile uint32_t *afr = &port->AFR[i >> 3];
		uint32_t shift = (i & 7) * 4;
		*afr = (*afr & ~(0xFu << shift)) | ((uint32_t)LCD_AF << shift);
		port->OSPEEDR |= 3u << (i * 2);
		port->OTYPER &= ~(1u << i);
		port->PUPDR &= ~(3u << (i * 2));
		port->MODER = (port->MODER & ~(3u << (i * 2))) | (2u << (i * 2));
	}
}

void md380_lcd_pins_fsmc(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();	// the port registers are shared with other pins on PD and PE
	set_af12(GPIOD, KEY_COLS_PORTD);
	set_af12(GPIOE, KEY_COLS_PORTE);
	__set_PRIMASK(primask);
}

static void bus_settle(void)
{
	__DSB();
	for (volatile uint32_t i = 0; i < 8; i++) {
	}
}

static void bus_begin(void)
{
	md380_lcdbus_lock();
	md380_lcd_pins_fsmc();
	PIN_WRITE(PIN_LCD_CS, false);
}

static void bus_end(void)
{
	bus_settle();
	PIN_WRITE(PIN_LCD_CS, true);
	__DSB();
	*LCD_DATA_ADDR = 0x00;	// panel deselected: only parks the shared lines low
	bus_settle();
	md380_lcdbus_unlock();
}

static void bus_cmd(uint8_t c)
{
	*LCD_CMD_ADDR = c;
}

static void bus_data(const uint8_t *d, uint32_t n)
{
	volatile uint8_t *port = LCD_DATA_ADDR;
	for (uint32_t i = 0; i < n; i++) {
		*port = d[i];
	}
}

static void bus_pixels(const uint16_t *px, uint32_t n, bool swap_rb)
{
	volatile uint8_t *port = LCD_DATA_ADDR;
	for (uint32_t i = 0; i < n; i++) {
		uint16_t v = px[i];
		if (swap_rb) {
			v = (uint16_t)((v & 0x07E0) | (v >> 11) | (v << 11));
		}
		*port = (uint8_t)(v >> 8);
		*port = (uint8_t)v;
	}
}

static void bus_reset(bool level)
{
	PIN_WRITE(PIN_LCD_RST, level);
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

static st7735_t panel = {
	.bus = &bus,
	.type = MD380_PANEL_TYPE,
	.width = VISIBLE_W,
	.height = VISIBLE_H,
	.col_offset = 0,
	.row_offset = 0,
	.madctl_xor = MD380_PANEL_MADCTL_XOR,
};

static bool ready;

// ---- Backlight: TIM8 CH1 PWM on PC6 (AF3), 1 kHz, 100 steps ----

#define BL_STEPS	100u

static void bl_init(void)
{
	__HAL_RCC_TIM8_CLK_ENABLE();
	stm32_pin_af(PIN_PORT(PIN_LCD_BACKLIGHT), PIN_MASK(PIN_LCD_BACKLIGHT), GPIO_AF3_TIM8, false, GPIO_NOPULL);
	TIM8->CR1 = 0;
	TIM8->PSC = 720 - 1;		// APB2 timer clock 72 MHz -> 100 kHz
	TIM8->ARR = BL_STEPS - 1;	// 1 kHz
	TIM8->CCR1 = 0;
	TIM8->CCMR1 = (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;	// PWM mode 1, preload
	TIM8->CCER = TIM_CCER_CC1E;
	TIM8->BDTR = TIM_BDTR_MOE;	// advanced timer: main output enable
	TIM8->EGR = TIM_EGR_UG;
	TIM8->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

// 0 = off, 100 or more = fully on, otherwise PWM duty in percent
static void set_backlight(uint8_t percent)
{
	if (!ready) {
		return;
	}
	TIM8->CCR1 = percent >= BL_STEPS ? BL_STEPS : percent;
}

// ---- hal_display_t ----

static void init(void)
{
	stm32_pin_out(PIN_PORT(PIN_LCD_CS), PIN_MASK(PIN_LCD_CS), true, false);
	stm32_pin_out(PIN_PORT(PIN_LCD_RST), PIN_MASK(PIN_LCD_RST), true, false);
	stm32_pin_af(GPIOD, LCD_CTRL_PORTD, LCD_AF, false, GPIO_NOPULL);
	stm32_gpio_clock(GPIOE);
	fsmc_init();
	bl_init();
	st7735_init(&panel);
	ready = true;
}

static void flush(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	if (!ready || fb == NULL) {
		return;
	}
	st7735_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
}

const hal_display_t md380_display = {
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = VISIBLE_W,
	.height = VISIBLE_H,
	.bpp = 4,
	.init = init,
	.flush_indexed = flush,
	.set_backlight = set_backlight,
};
