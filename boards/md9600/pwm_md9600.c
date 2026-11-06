// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 TIM8 PWM outputs besides the backlight (docs/spec/hardware-md9600.md 3, 19.9): CH2 on PC7 is the sub-audio
// tone into the modulator, CH3 on PC8 the beep. TIM8 runs for the backlight (display_md9600.c: / 10 / 256, about
// 28 kHz); these only add a compare channel. A tone or beep generator changes the duty at its sample rate. Duty 0 hands
// the pin back as a GPIO: the beep low, the tone released (open drain high). The tone line goes to the reference
// oscillator's modulation input: driven low it pulls the reference far enough that DMR never syncs (2.6, 19.9).
#include <string.h>
#include "config.h"
#include "md9600.h"
#include "pins.h"

void md9600_pwm_out(uint8_t ch, uint8_t duty)
{
	if (ch != MD9600_PWM_TONE && ch != MD9600_PWM_BEEP) {
		return;
	}
	bool tone = ch == MD9600_PWM_TONE;
	GPIO_TypeDef *port = tone ? PIN_PORT(PIN_TONE_PWM) : PIN_PORT(PIN_BEEP_PWM);
	uint16_t pin = tone ? PIN_MASK(PIN_TONE_PWM) : PIN_MASK(PIN_BEEP_PWM);
	if (duty == 0 || !(RCC->APB2ENR & RCC_APB2ENR_TIM8EN)) {
		TIM8->CCER &= tone ? ~TIM_CCER_CC2E : ~TIM_CCER_CC3E;
		stm32_pin_out(port, pin, tone, tone);	// the tone pin released, the beep pin low
		return;
	}
	if (tone) {
		TIM8->CCR2 = duty;
		if (!(TIM8->CCER & TIM_CCER_CC2E)) {
			TIM8->CCMR1 = (TIM8->CCMR1 & ~(TIM_CCMR1_OC2M | TIM_CCMR1_CC2S)) | (6u << TIM_CCMR1_OC2M_Pos) |
				      TIM_CCMR1_OC2PE;	// PWM mode 1, preload: a new duty starts with the next period
			TIM8->CCER |= TIM_CCER_CC2E;
		}
	} else {
		TIM8->CCR3 = duty;
		if (!(TIM8->CCER & TIM_CCER_CC3E)) {
			TIM8->CCMR2 = (TIM8->CCMR2 & ~(TIM_CCMR2_OC3M | TIM_CCMR2_CC3S)) | (6u << TIM_CCMR2_OC3M_Pos) |
				      TIM_CCMR2_OC3PE;
			TIM8->CCER |= TIM_CCER_CC3E;
		}
	}
	stm32_pin_af(port, pin, GPIO_AF3_TIM8, false, GPIO_NOPULL);
}

#if MD9600_PWM_AUDIO
// --- Audio as PWM on PC8 (TIM8 CH3) into the HR_C6000's microphone input (hardware-md9600.md 19.9) ---
//
// The beep net reaches MIC_P / MIC_N (test_probe_fm_tx_pin_tone). TIM8's timebase is the backlight's and the sub-audio
// tone's, so it stays: 225 carrier periods (8 ms at 28125 Hz) take 64 samples at 8 kHz, the duty linearly interpolated
// between samples with the rounding error carried from period to period. The chip's codec samples at 8 kHz behind its
// own filter, which takes the carrier away. DMA2 stream 2 (TIM8 CH3's request, channel 0) rewrites CCR3 once a period
// from a circular buffer of two halves; its interrupt refills the half that has gone out with samples from pull.
#define PA_PERIODS		225u	// carrier periods a half buffer
#define PA_SAMPLES		64u	// samples a half buffer
#define PA_MID			128	// 50 % of the 256 count period
#define PA_MAX			255	// a compare value past the period never matches, and the DMA would stop
#define PA_IRQ_PRIORITY		6	// no RTOS calls in the handler; the pull must not block

static uint16_t pa_buf[2u * PA_PERIODS];	// main SRAM: DMA
static audio_tap_pull_t volatile pa_pull;
static int32_t pa_amp;		// counts either side of the middle for a full-scale sample
static int16_t pa_last;		// the previous half's last sample: the interpolation's start
static int32_t pa_err_q8;	// the carried rounding error, x256

static void pa_fill(uint16_t *out)
{
	int16_t pcm[PA_SAMPLES + 1u];
	pcm[0] = pa_last;
	audio_tap_pull_t pull = pa_pull;
	if (pull != NULL) {
		pull(&pcm[1], PA_SAMPLES);
	} else {
		memset(&pcm[1], 0, PA_SAMPLES * sizeof pcm[0]);
	}
	pa_last = pcm[PA_SAMPLES];
	for (uint32_t k = 0; k < PA_PERIODS; k++) {
		uint32_t pos = k * PA_SAMPLES;	// in 1/PA_PERIODS of a sample: exact, no drift
		uint32_t i = pos / PA_PERIODS;
		int32_t f = (int32_t)(pos % PA_PERIODS);
		int32_t a = pcm[i];
		int32_t v = a + ((int32_t)pcm[i + 1u] - a) * f / (int32_t)PA_PERIODS;
		int32_t want = (PA_MID << 8) + ((v * pa_amp) >> 7) + pa_err_q8;	// v / 32768 x amp x 256
		int32_t duty = (want + 128) >> 8;
		pa_err_q8 = want - (duty << 8);
		*out++ = (uint16_t)(duty < 0 ? 0 : duty > PA_MAX ? PA_MAX : duty);
	}
}

void DMA2_Stream2_IRQHandler(void)
{
	uint32_t isr = DMA2->LISR;
	DMA2->LIFCR = isr & (DMA_LISR_HTIF2 | DMA_LISR_TCIF2 | DMA_LISR_TEIF2 | DMA_LISR_DMEIF2 | DMA_LISR_FEIF2);
	if (isr & DMA_LISR_HTIF2) {
		pa_fill(&pa_buf[0]);		// the first half has gone out; the DMA reads the second
	}
	if (isr & DMA_LISR_TCIF2) {
		pa_fill(&pa_buf[PA_PERIODS]);
	}
}

void md9600_pwm_audio(audio_tap_pull_t pull, int16_t amp)
{
	TIM8->DIER &= ~TIM_DIER_CC3DE;
	DMA2_Stream2->CR &= ~DMA_SxCR_EN;
	while (DMA2_Stream2->CR & DMA_SxCR_EN) {
	}
	HAL_NVIC_DisableIRQ(DMA2_Stream2_IRQn);
	DMA2->LIFCR = DMA_LIFCR_CTCIF2 | DMA_LIFCR_CHTIF2 | DMA_LIFCR_CTEIF2 | DMA_LIFCR_CDMEIF2 | DMA_LIFCR_CFEIF2;
	pa_pull = NULL;
	md9600_pwm_out(MD9600_PWM_BEEP, 0);	// the pin a low output
	if (pull == NULL || !(RCC->APB2ENR & RCC_APB2ENR_TIM8EN)) {
		return;
	}
	for (uint32_t i = 0; i < 2u * PA_PERIODS; i++) {
		pa_buf[i] = PA_MID;		// silence until the first refill
	}
	pa_last = 0;
	pa_err_q8 = 0;
	pa_amp = amp;
	pa_pull = pull;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
	(void)RCC->AHB1ENR;
	DMA2_Stream2->PAR = (uint32_t)&TIM8->CCR3;
	DMA2_Stream2->M0AR = (uint32_t)pa_buf;
	DMA2_Stream2->NDTR = 2u * PA_PERIODS;
	DMA2_Stream2->FCR = 0;	// direct mode
	// Channel 0 (TIM8 CH3), memory to peripheral, 16-bit both sides, memory increment, circular, both interrupts
	DMA2_Stream2->CR = DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
			   DMA_SxCR_DIR_0 | DMA_SxCR_HTIE | DMA_SxCR_TCIE;
	HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, PA_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);
	DMA2_Stream2->CR |= DMA_SxCR_EN;
	md9600_pwm_out(MD9600_PWM_BEEP, PA_MID);	// CH3 in PWM mode with preload, the pin on TIM8
	TIM8->DIER |= TIM_DIER_CC3DE;
}
#endif
