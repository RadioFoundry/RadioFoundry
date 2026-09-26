// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 keypad, side keys, PTT and encoder (docs/spec/hardware-dm1701.md section 10).
// The keypad columns are the LCD data lines D0..D7: a scan borrows them from the FSMC under the LCD bus lock.
#include "dm1701.h"
#include "pins.h"
#include "hal_time.h"
#include "input/quadrature/quadrature.h"

#define MODER_INPUT	0u
#define MODER_OUTPUT	1u
#define MODER_AF	2u
#define PUPDR_NONE	0u
#define PUPDR_DOWN	2u
#define FSMC_AF		12u

#define ROW_COUNT	3
#define NO_KEY		0xFF

// Matrix positions, [row][column D0..D7] (spec 10.1, DM-1701 / RT84 table)
static const uint8_t matrix[ROW_COUNT][8] = {
	{ KEY_1, KEY_4, KEY_7, KEY_STAR, KEY_UP, KEY_FN_RIGHT, KEY_FN_LEFT, NO_KEY },
	{ KEY_2, KEY_5, KEY_8, KEY_0, KEY_DOWN, KEY_RED, KEY_GREEN, NO_KEY },
	{ KEY_3, KEY_6, KEY_9, KEY_HASH, NO_KEY, KEY_TOP, KEY_SK2, KEY_SK1 },
};

static GPIO_TypeDef *const row_port[ROW_COUNT] = { PIN_PORT(PIN_KEY_ROW0), PIN_PORT(PIN_KEY_ROW1), PIN_PORT(PIN_KEY_ROW2) };
static const uint16_t row_pin[ROW_COUNT] = { PIN_MASK(PIN_KEY_ROW0), PIN_MASK(PIN_KEY_ROW1), PIN_MASK(PIN_KEY_ROW2) };

static uint32_t last_raw;
static uint32_t stable;

// Encoder (spec 10.3): both lines on both edges, a table decoder (drivers/input/quadrature) with two steps per detent,
// resting where A == B. The interrupt is the only writer of enc; the input task takes detents with enc_taken
#define ENC_STEPS	2
static quad_dec_t enc;
static int32_t enc_taken;

// Two-bit fields (MODER, PUPDR) of every pin in mask set to value
static uint32_t field2(uint32_t reg, uint16_t mask, uint32_t value)
{
	for (uint32_t i = 0; i < 16; i++) {
		if (mask & (1u << i)) {
			reg = (reg & ~(3u << (i * 2))) | (value << (i * 2));
		}
	}
	return reg;
}

// Four-bit alternate function fields of every pin in mask
static void set_af(GPIO_TypeDef *port, uint16_t mask, uint32_t af)
{
	for (uint32_t i = 0; i < 16; i++) {
		if (mask & (1u << i)) {
			uint32_t idx = i >> 3, shift = (i & 7u) * 4;
			port->AFR[idx] = (port->AFR[idx] & ~(0xFu << shift)) | (af << shift);
		}
	}
}

// Mode and pull of a group of pins; interrupts off so a read-modify-write of the same port elsewhere can't race
static void set_mode(GPIO_TypeDef *port, uint16_t mask, uint32_t mode, uint32_t pull)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	port->PUPDR = field2(port->PUPDR, mask, pull);
	port->MODER = field2(port->MODER, mask, mode);
	__set_PRIMASK(primask);
}

static void columns_to_gpio(void)
{
	// Discharge first (spec 10.1 step 1): leaving FSMC mode can leave charge that reads as phantom keys
	GPIOD->BSRR = (uint32_t)KEY_COLS_PORTD << 16;
	GPIOE->BSRR = (uint32_t)KEY_COLS_PORTE << 16;
	set_mode(GPIOD, KEY_COLS_PORTD, MODER_OUTPUT, PUPDR_NONE);
	set_mode(GPIOE, KEY_COLS_PORTE, MODER_OUTPUT, PUPDR_NONE);
	hal_delay_us(1);
	set_mode(GPIOD, KEY_COLS_PORTD, MODER_INPUT, PUPDR_DOWN);
	set_mode(GPIOE, KEY_COLS_PORTE, MODER_INPUT, PUPDR_DOWN);
}

static void columns_to_fsmc(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	set_af(GPIOD, KEY_COLS_PORTD, FSMC_AF);
	set_af(GPIOE, KEY_COLS_PORTE, FSMC_AF);
	__set_PRIMASK(primask);
	set_mode(GPIOD, KEY_COLS_PORTD, MODER_AF, PUPDR_NONE);
	set_mode(GPIOE, KEY_COLS_PORTE, MODER_AF, PUPDR_NONE);
}

// Columns as a byte, bit n = Dn
static uint8_t read_columns(void)
{
	uint32_t d = GPIOD->IDR, e = GPIOE->IDR;
	uint8_t c = 0;
	c |= (d & GPIO_PIN_14) ? 0x01 : 0;
	c |= (d & GPIO_PIN_15) ? 0x02 : 0;
	c |= (d & GPIO_PIN_0) ? 0x04 : 0;
	c |= (d & GPIO_PIN_1) ? 0x08 : 0;
	c |= (uint8_t)(((e >> 7) & 0xFu) << 4);	// PE7..PE10 -> D4..D7
	return c;
}

// Every key found is reported (side keys may be held together); the core decides what a combination means
static uint32_t scan_matrix(void)
{
	uint32_t keys = 0;
	dm1701_lcdbus_lock();
	columns_to_gpio();
	for (int r = 0; r < ROW_COUNT; r++) {
		row_port[r]->BSRR = row_pin[r];
		set_mode(row_port[r], row_pin[r], MODER_OUTPUT, PUPDR_NONE);
		hal_delay_us(5);
		uint8_t cols = read_columns();
		// Drive the row low before letting go: a row released while high stays charged, and through a pressed key
		// the next row's scan saw it (every key also read as the one below it, seen on the bench 2026-10-09)
		row_port[r]->BSRR = (uint32_t)row_pin[r] << 16;
		hal_delay_us(2);
		set_mode(row_port[r], row_pin[r], MODER_INPUT, PUPDR_DOWN);
		for (int c = 0; c < 8; c++) {
			if ((cols & (1u << c)) && matrix[r][c] != NO_KEY) {
				keys |= 1u << matrix[r][c];
			}
		}
	}
	columns_to_fsmc();
	dm1701_lcdbus_unlock();
	return keys;
}

static void init(void)
{
	// Rows are inputs with a weak pull-down between scans (a definite low that can't load the LCD bus); one is driven
	// high only while it is read
	for (int r = 0; r < ROW_COUNT; r++) {
		stm32_pin_in(row_port[r], row_pin[r], GPIO_PULLDOWN);
	}
	stm32_pin_in(PIN_PORT(PIN_PTT), PIN_MASK(PIN_PTT) | PIN_MASK(PIN_PTT_EXT), GPIO_PULLUP);
	// Both encoder lines interrupt on both edges (A = PE14 on EXTI14, B = PB11 on EXTI11, both in EXTI15_10)
	GPIO_InitTypeDef enc_pin = {
		.Mode = GPIO_MODE_IT_RISING_FALLING,
		.Pull = GPIO_PULLUP,
		.Speed = GPIO_SPEED_FREQ_LOW,
	};
	stm32_gpio_clock(PIN_PORT(PIN_ENC_A));
	stm32_gpio_clock(PIN_PORT(PIN_ENC_B));
	enc_pin.Pin = PIN_MASK(PIN_ENC_A);
	HAL_GPIO_Init(PIN_PORT(PIN_ENC_A), &enc_pin);	// also routes EXTI14 to port E
	enc_pin.Pin = PIN_MASK(PIN_ENC_B);
	HAL_GPIO_Init(PIN_PORT(PIN_ENC_B), &enc_pin);	// also routes EXTI11 to port B
	hal_delay_us(10);				// let the pull-ups settle before the first sample
	quad_init(&enc, ENC_STEPS, QUAD_REST_EQUAL, QUAD_AB(PIN_READ(PIN_ENC_A), PIN_READ(PIN_ENC_B)));
	enc_taken = 0;
	HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6, 0);	// spec 3.1; must stay numerically >= 5 for FreeRTOS
	HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = scan_matrix();
	if (!PIN_READ(PIN_PTT) || !PIN_READ(PIN_PTT_EXT)) {
		raw |= 1u << KEY_PTT;
	}
	if (raw == last_raw) {
		stable = raw;
	}
	last_raw = raw;
	return stable;
}

// Detents since the last call (the running total is one 32-bit word the interrupt writes: no lock needed)
static int8_t encoder(void)
{
	return quad_take(&enc, &enc_taken);
}

static void encoder_stats(hal_encoder_stats_t *st)
{
	st->detents = enc.detents;
	st->transitions = enc.transitions;
	st->illegal = enc.illegal;
	st->reversals = enc.reversals;
	st->steps_per_detent = ENC_STEPS;
}

// An edge on either encoder line (spec 10.3): the flags are cleared before the lines are read, so an edge that comes
// while this runs raises the interrupt again and its level is sampled too. Bounce is harmless: each sample is the
// levels as they are, and the decoder only counts whole detents
void EXTI15_10_IRQHandler(void)
{
	uint32_t pending = EXTI->PR & (PIN_MASK(PIN_ENC_A) | PIN_MASK(PIN_ENC_B));
	if (pending) {
		EXTI->PR = pending;
		quad_sample(&enc, QUAD_AB(PIN_READ(PIN_ENC_A), PIN_READ(PIN_ENC_B)));
	}
}

const hal_input_t dm1701_input = { .init = init, .scan = scan, .encoder = encoder, .encoder_stats = encoder_stats };
