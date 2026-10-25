// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 front panel: the 4 x 3 key matrix, the channel encoder and the hand mic's PTT (docs/spec/hardware-md9600.md
// 9). Rows K0-K3 are open-drain outputs, driven low one at a time; columns K6, K5, K4 are inputs with pull-ups, low =
// pressed. The encoder's two lines interrupt on both edges and feed the shared quadrature decoder. The power key is a
// matrix position: its state goes to power_md9600.c, not to the key map. The hand mic's keypad is two analog levels
// (9.3) from the ADC scan, decoded by drivers/input/adckeys; a mic without a keypad leaves both levels idle.
#include "md9600.h"
#include "pins.h"
#include "hal_time.h"
#include "input/keymatrix/keymatrix.h"
#include "input/quadrature/quadrature.h"
#include "input/adckeys/adckeys.h"

#define ENC_IRQ_PRIORITY	6	// at or below the RTOS max syscall level 5: no RTOS calls in the handler anyway
#define ROWS			4u
#define COLS			3u
#define N			KEYMATRIX_NONE

// Row-major, columns K6, K5, K4 (column bits 0, 1, 2)
static const uint8_t matrix_map[ROWS * COLS] = {
	KEY_POWER, KEY_ALARM, KEY_P1,	// K0
	KEY_P2, KEY_P3, KEY_P4,		// K1
	KEY_BACK, KEY_DOWN, KEY_ENTER,	// K2
	KEY_UP, N, N,			// K3
};
static const keymatrix_t matrix = { ROWS, COLS, matrix_map, NULL };

// Hand-mic keypad: the windows of the row level (PA6) and the column level (PA7) in raw 12-bit counts, both at or
// above 3445 with no key down (unverified). A key counts once it has decoded the same on MIC_SCANS scans in a row, so
// a level passing through another key's window as a key goes down or up is never reported
#define MIC_SCANS		3
static const adckeys_window_t mic_rows[5] = {
	{ 2830, 3444 }, { 2215, 2705 }, { 1477, 2090 }, { 862, 1352 }, { 247, 737 },
};
static const adckeys_window_t mic_cols[4] = {
	{ 0, 1106 }, { 1231, 1844 }, { 1969, 2705 }, { 2830, 3320 },
};
static const uint8_t mic_map[5 * 4] = {
	KEY_MIC_1, KEY_MIC_2, KEY_MIC_3, KEY_MIC_A,
	KEY_MIC_4, KEY_MIC_5, KEY_MIC_6, KEY_MIC_B,
	KEY_MIC_7, KEY_MIC_8, KEY_MIC_9, KEY_MIC_C,
	KEY_MIC_STAR, KEY_MIC_0, KEY_MIC_HASH, KEY_MIC_D,
	ADCKEYS_NONE, KEY_MIC_AB, KEY_MIC_UP, KEY_MIC_DOWN,
};
static const adckeys_t mic = { mic_rows, 5, mic_cols, 4, mic_map, 3445 };
static adckeys_state_t mic_state;
static uint8_t mic_last = ADCKEYS_NONE;

static quad_dec_t enc;
static int32_t enc_taken;
static uint8_t raw_rows[ROWS];

static uint8_t columns_low(void)
{
	uint8_t low = 0;
	low |= PIN_READ(PIN_KEY_K6) ? 0u : 1u;
	low |= PIN_READ(PIN_KEY_K5) ? 0u : 2u;
	low |= PIN_READ(PIN_KEY_K4) ? 0u : 4u;
	return low;
}

static void row_drive(uint8_t r, bool low)
{
	switch (r) {
	case 0: PIN_WRITE(PIN_KEY_K0, !low); break;
	case 1: PIN_WRITE(PIN_KEY_K1, !low); break;
	case 2: PIN_WRITE(PIN_KEY_K2, !low); break;
	default: PIN_WRITE(PIN_KEY_K3, !low); break;
	}
}

static uint8_t enc_ab(void)
{
	return QUAD_AB(PIN_READ(PIN_ENC_A), PIN_READ(PIN_ENC_B));
}

void EXTI15_10_IRQHandler(void)
{
	uint32_t pending = EXTI->PR & (EXTI_PR_PR10 | EXTI_PR_PR11);
	EXTI->PR = pending;
	(void)quad_sample(&enc, enc_ab());
}

static void init(void)
{
	stm32_pin_out(PIN_PORT(PIN_KEY_K0), PIN_MASK(PIN_KEY_K0) | PIN_MASK(PIN_KEY_K1), true, true);
	stm32_pin_out(PIN_PORT(PIN_KEY_K2), PIN_MASK(PIN_KEY_K2) | PIN_MASK(PIN_KEY_K3), true, true);
	stm32_pin_in(GPIOD, PIN_MASK(PIN_KEY_K4) | PIN_MASK(PIN_KEY_K5) | PIN_MASK(PIN_KEY_K6), GPIO_PULLUP);
	stm32_pin_in(PIN_PORT(PIN_PTT), PIN_MASK(PIN_PTT), GPIO_PULLUP);
	adckeys_init(&mic_state);

	// Encoder: pull-ups (the board's own are not known), EXTI10 / EXTI11 on port B, both edges. Two steps per detent
	// resting where both lines agree (unverified)
	stm32_pin_in(GPIOB, PIN_MASK(PIN_ENC_A) | PIN_MASK(PIN_ENC_B), GPIO_PULLUP);
	quad_init(&enc, 2, QUAD_REST_EQUAL, enc_ab());
	RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
	(void)RCC->APB2ENR;
	SYSCFG->EXTICR[2] = (SYSCFG->EXTICR[2] & ~(SYSCFG_EXTICR3_EXTI10 | SYSCFG_EXTICR3_EXTI11)) |
			    SYSCFG_EXTICR3_EXTI10_PB | SYSCFG_EXTICR3_EXTI11_PB;
	EXTI->RTSR |= EXTI_RTSR_TR10 | EXTI_RTSR_TR11;
	EXTI->FTSR |= EXTI_FTSR_TR10 | EXTI_FTSR_TR11;
	EXTI->PR = EXTI_PR_PR10 | EXTI_PR_PR11;
	EXTI->IMR |= EXTI_IMR_MR10 | EXTI_IMR_MR11;
	HAL_NVIC_SetPriority(EXTI15_10_IRQn, ENC_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

static uint32_t scan(void)
{
	uint8_t idle = columns_low();
	for (uint8_t r = 0; r < ROWS; r++) {
		row_drive(r, true);
		hal_delay_us(10);
		raw_rows[r] = columns_low();
		row_drive(r, false);
	}
	uint32_t keys = keymatrix_decode(&matrix, idle, raw_rows);
	md9600_power_key((keys & (1u << KEY_POWER)) != 0);
	keys &= ~(1u << KEY_POWER);
	if (!PIN_READ(PIN_PTT)) {
		keys |= 1u << KEY_PTT;
	}
	mic_last = adckeys_decode(&mic, md9600_adc_raw(MD9600_ADC_MICKEY_ROW), md9600_adc_raw(MD9600_ADC_MICKEY_COL));
	uint8_t k = adckeys_debounce(&mic_state, mic_last, MIC_SCANS);
	if (k != ADCKEYS_NONE) {
		keys |= 1u << k;
	}
	return keys;
}

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
	st->steps_per_detent = enc.steps;
}

uint8_t md9600_mic_key(void)
{
	return mic_last;
}

void md9600_input_raw(uint8_t rows[4])
{
	for (uint8_t r = 0; r < ROWS; r++) {
		rows[r] = raw_rows[r];
	}
}

const hal_input_t md9600_input = {
	.init = init,
	.scan = scan,
	.encoder = encoder,
	.encoder_stats = encoder_stats,
};
