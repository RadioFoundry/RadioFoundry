// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV keys and encoder (docs/spec/hardware-d578uv.md 3.2). The front keys sit on two resistor ladders read by the
// ADC, four keys each, with the stock firmware's windows (at or below 500, 1500, 2500 and 3500 counts; above: none);
// one key has a pin of its own (PA1), low when pressed. The encoder's lines are PC8 and PC9, sampled on every edge of
// either (EXTI 8 and 9). PTT and the microphone's Up and Down keys come from the plain hand microphone's two lines,
// PA9 and PA10, read as the stock firmware reads them when its "Hand Type" isn't the serial microphone (front_io.h).
// Pull-ups hold both lines high without a microphone, so nothing keys the transmitter at power-on. The serial keypad
// microphone (the same pins as USART0) isn't read. Debouncing is the core's, as for the other keys.
#include "d578uv.h"
#include "front_io.h"
#include "pins.h"
#include "mcu_io.h"
#include "input/quadrature/quadrature.h"

static const uint16_t ladder_max[4] = { 500, 1500, 2500, 3500 };
#define ENC_LINES	((1u << 8) | (1u << 9))	// EXTI 8 and 9: PC8, PC9

static quad_dec_t enc;
static int32_t enc_taken;

static uint8_t enc_ab(void)
{
	return QUAD_AB(mcu_pin_read(PIN_ENC_A), mcu_pin_read(PIN_ENC_B));
}

void MCU_EXTI9_5_HANDLER(void)
{
	mcu_exti_clear(ENC_LINES);
	quad_sample(&enc, enc_ab());
}

static void init(void)
{
	mcu_pin_analog(PIN_KEY_LADDER0);
	mcu_pin_analog(PIN_KEY_LADDER1);
	mcu_pin_in(PIN_KEY_SINGLE, MCU_PULL_UP);
	mcu_pin_in(PIN_MIC_LINE9, MCU_PULL_UP);
	mcu_pin_in(PIN_MIC_LINE10, MCU_PULL_UP);
	mcu_pin_in(PIN_ENC_A, MCU_PULL_UP);
	mcu_pin_in(PIN_ENC_B, MCU_PULL_UP);
	mcu_adc_init();
	// Two steps per detent, resting where the lines are equal: the stock decoder counts once per edge of A (to be
	// checked on the radio)
	quad_init(&enc, 2, QUAD_REST_EQUAL, enc_ab());
	mcu_exti_route(GPIOC, 8);
	mcu_exti_route(GPIOC, 9);
	mcu_exti_both_edges(ENC_LINES);
	mcu_exti_clear(ENC_LINES);
	mcu_exti_enable(ENC_LINES);
	NVIC_SetPriority(MCU_EXTI9_5_IRQn, 6);
	NVIC_EnableIRQ(MCU_EXTI9_5_IRQn);
}

// The ladder position pressed (0..3), or -1
static int8_t ladder(uint8_t ch)
{
	uint16_t v = mcu_adc_read(ch);
	for (int8_t i = 0; i < 4; i++) {
		if (v <= ladder_max[i]) {
			return i;
		}
	}
	return -1;
}

static uint32_t scan(void)
{
	uint32_t m = 0;
	int8_t k = ladder(ADC_KEY_LADDER0);
	if (k >= 0) {
		m |= 1u << (KEY_UP + k);
	}
	k = ladder(ADC_KEY_LADDER1);
	if (k >= 0) {
		m |= 1u << (KEY_C17 + k);
	}
	if (!mcu_pin_read(PIN_KEY_SINGLE)) {
		m |= 1u << KEY_C1E;
	}
	switch (mic_key_decode(!mcu_pin_read(PIN_MIC_LINE9), !mcu_pin_read(PIN_MIC_LINE10))) {
	case MIC_KEY_PTT:
		m |= 1u << KEY_PTT;
		break;
	case MIC_KEY_UP:
		m |= 1u << KEY_MIC_UP;
		break;
	case MIC_KEY_DOWN:
		m |= 1u << KEY_MIC_DOWN;
		break;
	case MIC_KEY_NONE:
		break;
	}
	return m;
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

const hal_input_t d578uv_input = {
	.init = init,
	.scan = scan,
	.encoder = encoder,
	.encoder_stats = encoder_stats,
};
