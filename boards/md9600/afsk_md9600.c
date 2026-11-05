// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 APRS AFSK (hal_afsk.h, docs/spec/aprs.md 5).
//
// Transmit (CONFIG_MD9600_TX): the HR_C6000's tone generator in place of the microphone, as for the 1750 Hz burst,
// retuned at every symbol boundary by the TIM6 interrupt (priority 5, the RTOS syscall level: each configuration
// port transaction masks it, so a retune never lands inside one). A retune is two table entries, four register
// writes, about 40 us of an 833 us symbol. Unverified on air: the generator's phase across a retune and the
// deviation per level.
//
// Receive: the discriminator audio on PC3 at 9146 Hz, from the capture APRS shares with the PCM stream's FM receive
// tap (disc_md9600.c). It runs while the core listens.
#include "config.h"

#if CONFIG_APRS && (CONFIG_APRS_RX || CONFIG_MD9600_TX)
#include <stdbool.h>
#include "md9600.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"

#if CONFIG_MD9600_TX
#define TX_IRQ_PRIORITY		5	// the RTOS syscall level (no RTOS calls in the handler)

static const uint8_t *volatile tx_tones;
static volatile uint16_t tx_nbits;
static volatile uint16_t tx_pos;	// next symbol
static uint16_t tx_mark, tx_space;
static volatile uint16_t tx_hz;
static volatile bool tx_running;
static volatile uint32_t tx_missed;	// retunes that failed: a corrupt frame

static bool tx_symbol(uint16_t i)
{
	return ((tx_tones[i >> 3] >> (i & 7u)) & 1u) != 0;
}

static void tx_timer_stop(void)
{
	TIM6->CR1 = 0;
	TIM6->DIER = 0;
	HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
	TIM6->SR = 0;
}

static void tx_timer_start(uint16_t baud)
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
	HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TX_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
	TIM6->CR1 = TIM_CR1_CEN;
}

void TIM6_DAC_IRQHandler(void)
{
	if ((TIM6->SR & TIM_SR_UIF) == 0) {
		return;
	}
	TIM6->SR = (uint16_t)~TIM_SR_UIF;
	uint16_t i = tx_pos;
	if (i >= tx_nbits) {
		tx_timer_stop();	// the last symbol has had its full time
		tx_running = false;
		return;
	}
	uint16_t hz = tx_symbol(i) ? tx_mark : tx_space;
	if (hz != tx_hz) {
		if (md9600_c6000_afsk_tone(hz)) {
			tx_hz = hz;
		} else {
			tx_missed++;
		}
	}
	tx_pos = (uint16_t)(i + 1u);
}

static bool tx_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	if (tx_running || n == 0 || baud == 0) {
		return false;
	}
	tx_tones = t;
	tx_nbits = n;
	tx_mark = mark;
	tx_space = space;
	tx_hz = tx_symbol(0) ? mark : space;
	if (!md9600_c6000_afsk_begin(tx_hz, level)) {	// the first symbol's tone, at task level
		return false;
	}
	tx_pos = 1;
	tx_running = true;
	tx_timer_start(baud);
	return true;
}

static bool tx_busy(void)
{
	return tx_running;
}

static void tx_stop(void)
{
	tx_timer_stop();
	tx_running = false;
	(void)md9600_c6000_tx_pair(0, 0);	// the microphone back
}
#endif

#if CONFIG_APRS_RX
static uint32_t tail;	// this reader's place on the capture's ring

static bool rx_start(void)
{
	md9600_disc_start(MD9600_DISC_APRS);
	tail = md9600_disc_head();
	return true;
}

static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	const volatile int16_t *ring = md9600_disc_ring();
	uint32_t h = md9600_disc_head();
	if (h - tail > MD9600_DISC_RING) {
		tail = h - MD9600_DISC_RING;	// fell behind: the oldest are gone
	}
	uint16_t n = 0;
	while (n < max && tail != h) {
		buf[n++] = ring[tail % MD9600_DISC_RING];
		tail++;
	}
	return n;
}

static void rx_stop(void)
{
	md9600_disc_stop(MD9600_DISC_APRS);
}

#endif	// CONFIG_APRS_RX

const hal_afsk_t md9600_afsk = {
#if CONFIG_MD9600_TX
	.tx_start = tx_start,
	.tx_busy = tx_busy,
	.tx_stop = tx_stop,
#endif
#if CONFIG_APRS_RX
	.rx_rate_hz = MD9600_DISC_RATE_HZ,
	.rx_start = rx_start,
	.rx_read = rx_read,
	.rx_stop = rx_stop,
#endif
};
#endif
