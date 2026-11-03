// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 sub-audio receive: CTCSS / DCS decode in software from QT_DQT_IN on PA2 (ADC2 IN2, docs/spec/hardware-md9600.md
// 19.9). ADC2 converts continuously into a circular DMA buffer (DMA2 stream 3, channel 1);
// each half-transfer interrupt sums groups of DECIM conversions into one sample for drivers/rf/subaudio, the DSP. The
// transmit side (the tone PWM on PC7) is not written.
#include "config.h"

#if CONFIG_SUBTONE
#include <stdbool.h>
#include "md9600.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/subaudio/subaudio_rx.h"

// The input: ADC IN2 (PA2, QT_DQT_IN). A 100.0 Hz tone on the air holds it at about 320 counts of amplitude; PC3 (IN13)
// carries the voice band with the tone filtered out (first radio, 2026-11-03)
#ifndef MD9600_SUBTONE_ADC_CH
#define MD9600_SUBTONE_ADC_CH	2u
#endif
#ifndef MD9600_SUBTONE_RX_INVERT
#define MD9600_SUBTONE_RX_INVERT	0
#endif
// Smallest tone amplitude accepted, in the summed samples' counts (DECIM x 12-bit): the detector's default of 8 counts
// of a 12-bit converter, scaled. A guess until a tone's level on the pin is measured
#ifndef MD9600_SUBTONE_MIN_AMP
#define MD9600_SUBTONE_MIN_AMP	(8.0f * DECIM)
#endif

// ADC clock PCLK2 (72 MHz) / 8, 480 + 12 cycles a conversion: 18293 Hz. Sums of 8: 2287 Hz, 15 bits
#define ADC_RATE_HZ		(72000000u / 8u / 492u)
#define DECIM			8u
#define FS_HZ			((ADC_RATE_HZ + DECIM / 2u) / DECIM)
#define HALF_OUT		16u				// samples a half buffer: an interrupt every 7 ms
#define HALF_RAW		(HALF_OUT * DECIM)
#define IRQ_PRIORITY		7				// no RTOS calls in the handler
#define LEVEL_N			256u				// samples in a level window, about 0.11 s

static volatile uint16_t raw[2u * HALF_RAW];
static subaudio_rx_t rx;
static bool started;
static volatile uint32_t samples;

// AC level of the input for the view: the mean and the rms over the last window, in 12-bit counts
static float lvl_dc;
static float lvl_acc;
static uint16_t lvl_n;
static volatile uint16_t lvl_rms;

static subaudio_kind_t kind_of(const subtone_t *t)
{
	switch (t->type) {
	case TONE_CTCSS: return SUBAUDIO_CTCSS;
	case TONE_DCS: return SUBAUDIO_DCS;
	case TONE_DCS_INV: return SUBAUDIO_DCS_INV;
	default: return SUBAUDIO_NONE;
	}
}

static void level(int32_t s)
{
	float x = (float)s / (float)DECIM;
	lvl_dc += (x - lvl_dc) * (1.0f / 64.0f);
	float ac = x - lvl_dc;
	lvl_acc += ac * ac;
	if (++lvl_n >= LEVEL_N) {
		float ms = lvl_acc / (float)LEVEL_N;
		// Square root by a few Newton steps: no libm in the interrupt
		float r = ms > 1.0f ? ms / 2.0f : 1.0f;
		for (int i = 0; i < 12; i++) {
			r = 0.5f * (r + ms / r);
		}
		lvl_rms = (uint16_t)(ms > 0.0f ? r : 0.0f);
		lvl_acc = 0.0f;
		lvl_n = 0;
	}
}

void DMA2_Stream3_IRQHandler(void)
{
	uint32_t isr = DMA2->LISR;
	DMA2->LIFCR = DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3 | DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CFEIF3;
	if (!(isr & (DMA_LISR_HTIF3 | DMA_LISR_TCIF3))) {
		return;
	}
	const volatile uint16_t *p = &raw[(isr & DMA_LISR_TCIF3) ? HALF_RAW : 0u];
	for (uint32_t o = 0; o < HALF_OUT; o++) {
		int32_t sum = 0;
		for (uint32_t k = 0; k < DECIM; k++) {
			sum += *p++;
		}
		subaudio_rx_feed(&rx, (int16_t)sum);
		level(sum);
	}
	samples += HALF_OUT;
}

static void start(void)
{
	stm32_pin_analog(PIN_PORT(PIN_DISC_AF), PIN_MASK(PIN_DISC_AF));
	stm32_pin_analog(PIN_PORT(PIN_QT_IN), PIN_MASK(PIN_QT_IN));
	RCC->APB2ENR |= RCC_APB2ENR_ADC2EN;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
	(void)RCC->APB2ENR;
	// The common prescaler (PCLK2 / 8) is the ADC1 scan's (adc_md9600.c), set at power-up

	DMA2_Stream3->CR = 0;
	while (DMA2_Stream3->CR & DMA_SxCR_EN) {
	}
	DMA2->LIFCR = DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3 | DMA_LIFCR_CDMEIF3 | DMA_LIFCR_CFEIF3;
	DMA2_Stream3->PAR = (uint32_t)&ADC2->DR;
	DMA2_Stream3->M0AR = (uint32_t)raw;
	DMA2_Stream3->NDTR = 2u * HALF_RAW;
	DMA2_Stream3->FCR = 0;	// direct mode
	// Channel 1 (ADC2), peripheral to memory, 16-bit both sides, memory increment, circular, both interrupts
	DMA2_Stream3->CR = DMA_SxCR_CHSEL_0 | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
			   DMA_SxCR_HTIE | DMA_SxCR_TCIE;
	HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
	DMA2_Stream3->CR |= DMA_SxCR_EN;

	ADC2->CR2 = 0;
	ADC2->SR = 0;
	ADC2->CR1 = 0;		// 12-bit, one channel
	ADC2->SMPR1 = 0x07FFFFFFu;	// 480 cycles on every channel
	ADC2->SMPR2 = 0x3FFFFFFFu;
	ADC2->SQR1 = 0;
	ADC2->SQR2 = 0;
	ADC2->SQR3 = MD9600_SUBTONE_ADC_CH;
	ADC2->CR2 = ADC_CR2_CONT | ADC_CR2_DMA | ADC_CR2_DDS | ADC_CR2_ADON;
	hal_delay_us(5);	// ADC stabilization time
	ADC2->CR2 |= ADC_CR2_SWSTART;
}

void md9600_subtone_rx_set(const subtone_t *tone)
{
	// The detector state is shared with the DMA interrupt: stop it while the detector is reset
	HAL_NVIC_DisableIRQ(DMA2_Stream3_IRQn);
	if (!started) {
		subaudio_rx_init(&rx, FS_HZ, MD9600_SUBTONE_MIN_AMP);
	}
	subaudio_rx_set(&rx, kind_of(tone), tone->value, MD9600_SUBTONE_RX_INVERT != 0);
	if (!started) {
		started = true;
		start();	// enables the interrupt
	} else {
		HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
	}
}

bool md9600_subtone_rx_match(void)
{
	return !started || subaudio_rx_match(&rx);
}

void md9600_subtone_stats(md9600_subtone_stats_t *s)
{
	s->samples = samples;
	s->fs_hz = FS_HZ;
	s->rms = lvl_rms;
	s->mean = (uint16_t)lvl_dc;
	s->kind = (uint8_t)rx.kind;
	s->match = started && subaudio_rx_match(&rx);
	// The ADC stopped (an overrun clears its DMA request) or the stream was disabled by an error
	s->fault = started && ((ADC2->SR & ADC_SR_OVR) || !(DMA2_Stream3->CR & DMA_SxCR_EN));
}
#endif
