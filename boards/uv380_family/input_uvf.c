// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family keypad, side keys, PTT and encoder (docs/spec/hardware-dm1701.md section 10), or the MD-2017's top key
// and navigation ring in place of the encoder (hardware-md2017.md 5); the matrix is the board's (uvf_key_matrix)
// The keypad columns are the LCD data lines D0..D7: the shared scan (platform/stm32f4/fsmc_keypad) borrows them.
#include "uvf.h"
#include "pins.h"
#include "hal_time.h"
#include "fsmc_keypad/fsmc_keypad.h"
#ifdef PIN_ENC_A
#include "input/quadrature/quadrature.h"
#endif
#ifdef PIN_NAV_UP
#include "input/steplines/steplines.h"
#endif

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

#ifdef PIN_ENC_A
// Encoder (spec 10.3): both lines on both edges, a table decoder (drivers/input/quadrature) with two steps per detent,
// resting where A == B. The interrupt is the only writer of enc; the input task takes detents with enc_taken
#define ENC_STEPS	2
static quad_dec_t enc;
static int32_t enc_taken;

static void encoder_init(void)
{
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
	HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6, 0);	// spec 3.1; must stay numerically >= 4 for FreeRTOS
	HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

// Detents since the last call (the running total is one 32-bit word the interrupt writes: no lock needed)
static int8_t encoder(void)
{
	int8_t n = quad_take(&enc, &enc_taken);
	return uvf_variant.enc_reverse ? (int8_t)-n : n;
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
#endif

#ifdef PIN_NAV_UP
// The navigation ring (hardware-md2017.md 5): each of its four lines changes level once per step in its direction.
// Sampled with the keys; each step becomes one press of that direction's arrow key (drivers/input/steplines)
#define NAV_LINES	4
static const struct { GPIO_TypeDef *port; uint16_t pin; uint8_t key; } nav[NAV_LINES] = {
	{ PIN_PORT(PIN_NAV_UP), PIN_MASK(PIN_NAV_UP), KEY_UP },
	{ PIN_PORT(PIN_NAV_DOWN), PIN_MASK(PIN_NAV_DOWN), KEY_DOWN },
	{ PIN_PORT(PIN_NAV_LEFT), PIN_MASK(PIN_NAV_LEFT), KEY_LEFT },
	{ PIN_PORT(PIN_NAV_RIGHT), PIN_MASK(PIN_NAV_RIGHT), KEY_RIGHT },
};
static steplines_t ring;

static uint8_t nav_sample(void)
{
	uint8_t v = 0;
	for (uint8_t i = 0; i < NAV_LINES; i++) {
		if (stm32_pin_read(nav[i].port, nav[i].pin)) {
			v |= (uint8_t)(1u << i);
		}
	}
	return v;
}

static void nav_init(void)
{
	for (uint8_t i = 0; i < NAV_LINES; i++) {
		stm32_pin_in(nav[i].port, nav[i].pin, GPIO_PULLUP);
	}
	stm32_pin_in(PIN_PORT(PIN_KEY_TOP), PIN_MASK(PIN_KEY_TOP), GPIO_PULLUP);
	hal_delay_us(10);	// let the pull-ups settle before the first sample
	steplines_init(&ring, NAV_LINES, nav_sample());
}

static uint32_t nav_scan(void)
{
	uint8_t pressed = steplines_scan(&ring, nav_sample());
	uint32_t keys = 0;
	for (uint8_t i = 0; i < NAV_LINES; i++) {
		if (pressed & (1u << i)) {
			keys |= 1u << nav[i].key;
		}
	}
	return keys;
}
#endif

static void init(void)
{
	stm32_fsmc_keypad_init(&keypad);
	stm32_pin_in(PIN_PORT(PIN_PTT), PIN_MASK(PIN_PTT) | PIN_MASK(PIN_PTT_EXT), GPIO_PULLUP);
#ifdef PIN_ENC_A
	encoder_init();
#endif
#ifdef PIN_NAV_UP
	nav_init();
#endif
}

// Called every 10 ms: a change is reported once two scans in a row agree
static uint32_t scan(void)
{
	uint32_t raw = stm32_fsmc_keypad_scan(&keypad, NULL);
	if (!PIN_READ(PIN_PTT) || !PIN_READ(PIN_PTT_EXT)) {
		raw |= 1u << KEY_PTT;
	}
#ifdef PIN_KEY_TOP
	if (!PIN_READ(PIN_KEY_TOP)) {
		raw |= 1u << KEY_TOP;
	}
#endif
	if (raw == last_raw) {
		stable = raw;
	}
	last_raw = raw;
#ifdef PIN_NAV_UP
	return stable | nav_scan();
#else
	return stable;
#endif
}

#ifdef PIN_ENC_A
const hal_input_t uvf_input = { .init = init, .scan = scan, .encoder = encoder, .encoder_stats = encoder_stats };
#else
const hal_input_t uvf_input = { .init = init, .scan = scan };
#endif
