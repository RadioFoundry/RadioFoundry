// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family APRS AFSK (hal_afsk.h, docs/spec/aprs.md 5.2): the AT1846S FM TX tone generator as the voice source (as for
// the roger beep), retuned at every symbol boundary by the TIM6 interrupt (hardware-dm1701.md 3.1, 3.3: TIM6 is the
// AFSK bit clock, priority 5). One AT1846S register write per tone change (reg 0x35, about 0.1 ms on I2C3 at
// 400 kHz) out of an 833 us symbol; the radio core keeps the I2C3 bus free meanwhile (status() returns its cached
// reading while r.afsk is set).
//
// No receive path of its own (rx_start NULL): the HR_C6000 puts FM receive audio on the I2S stream (hardware-dm1701.md
// 21.3.1), so APRS takes it from there through the audio tap's AUDIO_TAP_FM_RX (aprs.md 5.3).
//
// Unverified on the bench: whether the tone generator's phase stays continuous across a retune, the deviation the
// level gives, and the flatness of mark vs space (aprs.md 13 HIL steps).
#include "config.h"
#if CONFIG_APRS
#include "uvf.h"
#include "stm32f4xx_hal.h"

#define AFSK_IRQ_PRIORITY	5	// hardware-dm1701.md 3.1 (no RTOS calls from the handler)

static const uint8_t *volatile tones;
static volatile uint16_t nbits;
static volatile uint16_t pos;		// next symbol
static uint16_t mark_hz, space_hz;
static volatile uint16_t cur_hz;
static volatile bool busy;
static volatile uint32_t skipped;	// tone writes that didn't happen (bus busy): a corrupt frame

static bool symbol(uint16_t i)
{
	return ((tones[i >> 3] >> (i & 7u)) & 1u) != 0;
}

static void timer_stop(void)
{
	TIM6->CR1 = 0;
	TIM6->DIER = 0;
	HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
	TIM6->SR = 0;
}

static void timer_start(uint16_t baud)
{
	__HAL_RCC_TIM6_CLK_ENABLE();
	uint32_t pclk = HAL_RCC_GetPCLK1Freq();
	uint32_t clk = ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) ? pclk : 2u * pclk;	// APB1 timer clock
	TIM6->CR1 = 0;
	TIM6->PSC = 1;					// 72 MHz / 2
	TIM6->ARR = clk / 2u / baud - 1u;		// 29999 at 1200 baud
	TIM6->CNT = 0;
	TIM6->EGR = TIM_EGR_UG;				// load PSC
	TIM6->SR = 0;
	TIM6->DIER = TIM_DIER_UIE;
	HAL_NVIC_SetPriority(TIM6_DAC_IRQn, AFSK_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
	TIM6->CR1 = TIM_CR1_CEN;
}

void TIM6_DAC_IRQHandler(void)
{
	if ((TIM6->SR & TIM_SR_UIF) == 0) {
		return;
	}
	TIM6->SR = (uint16_t)~TIM_SR_UIF;
	uint16_t i = pos;
	if (i >= nbits) {
		timer_stop();		// the last symbol has had its full time
		busy = false;
		return;
	}
	uint16_t hz = symbol(i) ? mark_hz : space_hz;
	if (hz != cur_hz) {
		if (uvf_afsk_tone(hz)) {
			cur_hz = hz;
		} else {
			skipped++;
		}
	}
	pos = (uint16_t)(i + 1u);
}

static bool tx_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	if (busy || n == 0 || baud == 0) {
		return false;
	}
	tones = t;
	nbits = n;
	mark_hz = mark;
	space_hz = space;
	cur_hz = symbol(0) ? mark : space;
	if (!uvf_afsk_begin(cur_hz, level)) {	// the first symbol's tone, at task level
		return false;
	}
	pos = 1;
	busy = true;
	timer_start(baud);
	return true;
}

static bool tx_busy(void)
{
	return busy;
}

static void tx_stop(void)
{
	timer_stop();
	busy = false;
	uvf_afsk_end();
}

const hal_afsk_t uvf_afsk = {
	.tx_start = tx_start,
	.tx_busy = tx_busy,
	.tx_stop = tx_stop,
};

#endif
