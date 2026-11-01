// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 sub-audio: CTCSS / DCS decode in software from QT_DQT_IN (PA2, ADC1 IN2, one rank of the board's scan) and
// encode as PWM on CTC/DCS_OUT (PC7, TIM3 CH2) for the modulator (docs/spec/hardware-md380.md 6.4). The DSP is
// drivers/rf/subaudio; this file is the sampling and PWM glue. Nothing here has run on a radio.
#include "config.h"

#if CONFIG_SUBTONE
#include <stdbool.h>
#include "md380.h"
#include "pins.h"
#include "rf/subaudio/subaudio_rx.h"
#include "rf/subaudio/subaudio_tx.h"

// Bench knobs (docs/testing/md380-bringup.md): the tone's share of the PWM swing, and whether the analog path inverts
// the waveform (DCS polarity only: a CTCSS tone does not care). Both are guesses until a radio shows the deviation and
// polarity
#ifndef MD380_SUBTONE_PERCENT
#define MD380_SUBTONE_PERCENT	30
#endif
#ifndef MD380_SUBTONE_RX_INVERT
#define MD380_SUBTONE_RX_INVERT	0
#endif
#ifndef MD380_SUBTONE_TX_INVERT
#define MD380_SUBTONE_TX_INVERT	0
#endif
// Smallest tone amplitude the detector accepts, in ADC counts (12 bit): a guess until the level on PA2 is measured
#ifndef MD380_SUBTONE_MIN_AMP
#define MD380_SUBTONE_MIN_AMP	0	// 0 = the detector's default
#endif

// PWM: TIM3 counts to 1023 (10 bits) at the timer clock / 4 (about 17.6 kHz with a 72 MHz timer clock); the update
// interrupt fires every period and every TX_DIV-th one computes the next duty (about 4.4 kHz)
#define PWM_FULL	1023u
#define PWM_PSC		4u
#define TX_DIV		4u
#define TX_IRQ_PRIORITY	6

static subaudio_rx_t rx;
static bool rx_ready;
static volatile uint32_t samples_seen;

static subaudio_tx_t tx;
static volatile uint32_t tx_count;
static bool pwm_ready;

static subaudio_kind_t kind_of(const subtone_t *t)
{
	switch (t->type) {
	case TONE_CTCSS: return SUBAUDIO_CTCSS;
	case TONE_DCS: return SUBAUDIO_DCS;
	case TONE_DCS_INV: return SUBAUDIO_DCS_INV;
	default: return SUBAUDIO_NONE;
	}
}

// --- Receive ---

void md380_subtone_sample(uint16_t raw)
{
	samples_seen++;
	subaudio_rx_feed(&rx, (int16_t)raw);
}

uint32_t md380_subtone_samples(void)
{
	return samples_seen;
}

void md380_subtone_rx_set(const subtone_t *tone)
{
	// The detector state is shared with the DMA interrupt: stop it while the detector is reset
	HAL_NVIC_DisableIRQ(DMA2_Stream4_IRQn);
	if (!rx_ready) {
		subaudio_rx_init(&rx, md380_adc_scan_rate_hz(), (float)MD380_SUBTONE_MIN_AMP);
		rx_ready = true;
	}
	subaudio_rx_set(&rx, kind_of(tone), tone->value, MD380_SUBTONE_RX_INVERT != 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream4_IRQn);
}

bool md380_subtone_rx_match(void)
{
	return !rx_ready || subaudio_rx_match(&rx);
}

// --- Transmit ---

static uint32_t timer_clock_hz(void)
{
	uint32_t pclk = HAL_RCC_GetPCLK1Freq();
	return (RCC->CFGR & RCC_CFGR_PPRE1) >= RCC_CFGR_PPRE1_2 ? 2u * pclk : pclk;	// x2 when APB1 is divided
}

void TIM3_IRQHandler(void)
{
	TIM3->SR = 0;
	if (++tx_count >= TX_DIV) {
		tx_count = 0;
		TIM3->CCR2 = subaudio_tx_next(&tx);
	}
}

static void pwm_setup(void)
{
	__HAL_RCC_TIM3_CLK_ENABLE();
	TIM3->CR1 = 0;
	TIM3->PSC = PWM_PSC - 1u;
	TIM3->ARR = PWM_FULL;
	TIM3->CCR2 = (PWM_FULL + 1u) / 2u;
	TIM3->CCMR1 = (6u << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;	// PWM mode 1, preload
	TIM3->CCER = TIM_CCER_CC2E;
	TIM3->DIER = 0;
	HAL_NVIC_SetPriority(TIM3_IRQn, TX_IRQ_PRIORITY, 0);
	pwm_ready = true;
}

void md380_subtone_tx_stop(void)
{
	if (!pwm_ready) {
		return;
	}
	HAL_NVIC_DisableIRQ(TIM3_IRQn);
	TIM3->CR1 = 0;
	TIM3->DIER = 0;
	TIM3->SR = 0;
	stm32_pin_out(PIN_PORT(PIN_CTCSS_PWM), PIN_MASK(PIN_CTCSS_PWM), true, true);	// released, as board.c leaves it
}

bool md380_subtone_tx_tail(bool on)
{
	if (!pwm_ready) {
		return false;
	}
	HAL_NVIC_DisableIRQ(TIM3_IRQn);		// the generator runs in its interrupt
	bool ok = subaudio_tx_tail(&tx, on);
	if ((TIM3->CR1 & TIM_CR1_CEN) != 0) {
		HAL_NVIC_EnableIRQ(TIM3_IRQn);
	}
	return ok && (TIM3->CR1 & TIM_CR1_CEN) != 0;
}

void md380_subtone_tx_start(const subtone_t *tone)
{
	md380_subtone_tx_stop();
	subaudio_kind_t kind = kind_of(tone);
	if (kind == SUBAUDIO_NONE) {
		return;
	}
	if (!pwm_ready) {
		pwm_setup();
	}
	uint32_t rate = timer_clock_hz() / PWM_PSC / (PWM_FULL + 1u) / TX_DIV;
	subaudio_tx_init(&tx, rate, PWM_FULL, MD380_SUBTONE_PERCENT);
	subaudio_tx_start(&tx, kind, tone->value, MD380_SUBTONE_TX_INVERT != 0);
	tx_count = 0;
	TIM3->CCR2 = subaudio_tx_next(&tx);
	TIM3->CNT = 0;
	TIM3->SR = 0;
	TIM3->EGR = TIM_EGR_UG;
	TIM3->SR = 0;
	stm32_pin_af(PIN_PORT(PIN_CTCSS_PWM), PIN_MASK(PIN_CTCSS_PWM), GPIO_AF2_TIM3, false, GPIO_NOPULL);
	TIM3->DIER = TIM_DIER_UIE;
	HAL_NVIC_EnableIRQ(TIM3_IRQn);
	TIM3->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

#endif
