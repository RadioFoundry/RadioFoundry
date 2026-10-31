// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV keys and encoder (docs/spec/hardware-d578uv.md 3.2). The front keys sit on two resistor ladders read by the
// ADC, four keys each, with the stock firmware's windows (at or below 500, 1500, 2500 and 3500 counts; above: none);
// one key has a pin of its own (PA1), low when pressed. The encoder's lines are PC8 and PC9, sampled on every edge of
// either (EXTI 8 and 9). PTT and the microphone's Up and Down keys come from the plain hand microphone's two lines,
// PA9 and PA10, read as the stock firmware reads them when its "Hand Type" isn't the serial microphone (front_io.h).
// Pull-ups hold both lines high without a microphone, so nothing keys the transmitter at power-on.
//
// The serial keypad microphone (the stock default "Hand Type") sends frames on PA10 at 115200 (anytone_mic.h). The
// USART listens on PA10 all the time, so either microphone works without a setting: once a frame is accepted the keys
// come from the frames and PA9 becomes the USART's TX for the answers; when the frames stop for
// ANYTONE_MIC_TIMEOUT_MS, PA9 is an input again. While bytes arrive (MIC_ACTIVITY_MS after the last) the two lines
// aren't read as keys, so a frame's start bits never look like PTT. A plain microphone's PTT shows the USART a break
// (a framing error), which doesn't count as a byte. Debouncing is the core's, as for the other keys.
#include "d578uv.h"
#include "front_io.h"
#include "pins.h"
#include "mcu_io.h"
#include "hal_time.h"
#include "input/quadrature/quadrature.h"
#include "input/anytone_mic/anytone_mic.h"

#define MIC_BAUD		115200u
#define MIC_ACTIVITY_MS		50u
#define MIC_IRQ_PRIORITY	6

static const uint16_t ladder_max[4] = { 500, 1500, 2500, 3500 };
#define ENC_LINES	((1u << 8) | (1u << 9))	// EXTI 8 and 9: PC8, PC9

static quad_dec_t enc;
static int32_t enc_taken;
static anytone_mic_t mic;
static volatile uint32_t mic_byte_ms;	// when the USART last received a byte
static volatile bool mic_byte_seen;
static volatile bool mic_tx;		// PA9 is the USART's TX

// The radio's key bit for a microphone key code, -1 for none
static int8_t mic_key_bit(uint8_t code)
{
	if (code >= ANYTONE_MIC_KEY_1 && code <= ANYTONE_MIC_KEY_1 + 8u) {
		return (int8_t)(KEY_MIC_1 + (code - ANYTONE_MIC_KEY_1));
	}
	switch (code) {
	case ANYTONE_MIC_KEY_0:
		return KEY_MIC_0;
	case ANYTONE_MIC_KEY_STAR:
		return KEY_MIC_STAR;
	case ANYTONE_MIC_KEY_HASH:
		return KEY_MIC_HASH;
	case ANYTONE_MIC_KEY_AB:
		return KEY_MIC_AB;
	case ANYTONE_MIC_KEY_MENU:
		return KEY_MENU;
	case ANYTONE_MIC_KEY_EXIT:
		return KEY_EXIT;
	case ANYTONE_MIC_KEY_UP:
		return KEY_MIC_UP;
	case ANYTONE_MIC_KEY_DOWN:
		return KEY_MIC_DOWN;
	case ANYTONE_MIC_KEY_PA:
	case ANYTONE_MIC_KEY_PB:
	case ANYTONE_MIC_KEY_PC:
	case ANYTONE_MIC_KEY_PD:
		return (int8_t)(KEY_MIC_PA + (code - ANYTONE_MIC_KEY_PA));
	default:
		return -1;
	}
}

// No RTOS calls: the answer is one byte, written straight into the empty transmit register
void MIC_USART_HANDLER(void)
{
	int32_t c = mcu_uart_take(MIC_USART);
	if (c == MCU_UART_ERROR) {
		anytone_mic_error(&mic);
		return;
	}
	if (c == MCU_UART_NONE) {
		return;
	}
	uint32_t now = hal_millis();
	mic_byte_ms = now;
	mic_byte_seen = true;
	if (anytone_mic_byte(&mic, (uint8_t)c, now)) {
		if (!mic_tx) {
			mcu_pin_af(PIN_MIC_LINE9, MIC_TX_AF, false);
			mic_tx = true;
		}
		mcu_uart_put(MIC_USART, ANYTONE_MIC_ACK);
	}
}

void d578uv_hand_mic(bool *present, uint32_t *frames, uint32_t *errors, uint8_t *key)
{
	*present = mic.present;
	*frames = mic.frames;
	*errors = mic.errors;
	*key = mic.key;
}

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
	anytone_mic_init(&mic);
	mcu_uart_start(MIC_USART, MIC_BAUD, true);
	NVIC_SetPriority(MIC_USART_IRQn, MIC_IRQ_PRIORITY);
	NVIC_EnableIRQ(MIC_USART_IRQn);
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

// The microphone's keys: from the frames while the serial microphone is present, else from the two lines unless bytes
// are arriving
static uint32_t mic_scan(void)
{
	uint32_t now = hal_millis();
	NVIC_DisableIRQ(MIC_USART_IRQn);
	bool present = anytone_mic_poll(&mic, now);
	bool ptt = mic.ptt;
	uint8_t key = mic.key;
	bool busy = mic_byte_seen && now - mic_byte_ms < MIC_ACTIVITY_MS;
	if (!present && mic_tx) {
		mcu_pin_in(PIN_MIC_LINE9, MCU_PULL_UP);
		mic_tx = false;
	}
	NVIC_EnableIRQ(MIC_USART_IRQn);
	uint32_t m = 0;
	if (present) {
		int8_t bit = mic_key_bit(key);
		if (bit >= 0) {
			m |= 1u << bit;
		}
		return ptt ? m | 1u << KEY_PTT : m;
	}
	if (busy) {
		return 0;
	}
	switch (mic_key_decode(!mcu_pin_read(PIN_MIC_LINE9), !mcu_pin_read(PIN_MIC_LINE10))) {
	case MIC_KEY_PTT:
		return 1u << KEY_PTT;
	case MIC_KEY_UP:
		return 1u << KEY_MIC_UP;
	case MIC_KEY_DOWN:
		return 1u << KEY_MIC_DOWN;
	case MIC_KEY_NONE:
		break;
	}
	return 0;
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
	return m | mic_scan();
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
