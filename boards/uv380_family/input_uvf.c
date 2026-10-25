// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family keypad, side keys, PTT and encoder (docs/spec/hardware-dm1701.md section 10); the matrix is the board's
// (uvf_key_matrix)
// The keypad columns are the LCD data lines D0..D7: the shared scan (platform/stm32f4/fsmc_keypad) borrows them.
#include "uvf.h"
#include "pins.h"
#include "hal_time.h"
#include "fsmc_keypad/fsmc_keypad.h"
#include "input/quadrature/quadrature.h"

#define ROW_COUNT	3
#define NO_KEY		STM32_KEYPAD_NO_KEY

static const stm32_fsmc_keypad_cfg_t keypad = {
	.row_port = { PIN_PORT(PIN_KEY_ROW0), PIN_PORT(PIN_KEY_ROW1), PIN_PORT(PIN_KEY_ROW2) },
	.row_pin = { PIN_MASK(PIN_KEY_ROW0), PIN_MASK(PIN_KEY_ROW1), PIN_MASK(PIN_KEY_ROW2) },
	.rows = ROW_COUNT,
	.matrix = uvf_key_matrix,
	.lock = uvf_lcdbus_lock,
	.unlock = uvf_lcdbus_unlock,
};

static uint32_t last_raw;
static uint32_t stable;

// Encoder (spec 10.3): both lines on both edges, a table decoder (drivers/input/quadrature) with two steps per detent,
// resting where A == B. The interrupt is the only writer of enc; the input task takes detents with enc_taken
#define ENC_STEPS	2
static quad_dec_t enc;
static int32_t enc_taken;

static void init(void)
{
	stm32_fsmc_keypad_init(&keypad);
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
	uint32_t raw = stm32_fsmc_keypad_scan(&keypad, NULL);
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

const hal_input_t uvf_input = { .init = init, .scan = scan, .encoder = encoder, .encoder_stats = encoder_stats };
