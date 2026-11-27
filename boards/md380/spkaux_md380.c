// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 speaker's second output (hal_spk_aux_t, core/radio/spk_aux.h): BEEP (PC8, TIM8 CH3) PWM into the audio
// amplifier, for while DMR is on air and the HR_C5000's codec output has no level (hardware-md380.md 6.1). TIM8 runs
// the backlight's 40 kHz period (display_md380.c), so the PWM rate is the sample rate: each 8 kHz PCM sample is
// interpolated over 5 periods in the update interrupt, which runs only while the output is powered.
#include "md380.h"
#include "config.h"
#include "pins.h"
#include "radio/spk_aux.h"

#if CONFIG_BEEP && CONFIG_DMR
#include <string.h>

#define RING		6u	// blocks (120 ms, 5 usable): the lead's 3 and a block either way for the stream's jitter
#define UP		5u	// PWM periods per 8 kHz sample (40 kHz)
// PCM full scale = GAIN_Q7 / 128 of the PWM half swing. 256 (twice the half swing) put the default beep level (50, a peak of 8000) at half
// swing, as loud as decoded voice with the codec's DAC gain at 0xC2 (hardware-md380.md 6.1). The DAC gain is now 0x93,
// about 7.6 dB lower (3.5 dB to 0x9C, then 9 steps of 0.46 dB), so the PWM path drops as much: 256 * 0.417 = 107
#define GAIN_Q7		107
#define IRQ_PRIORITY	14u	// below the RF and audio interrupts: a late period only repeats a sample

static int16_t ring[RING][AUDIO_BLOCK_SAMPLES] RFOUNDRY_CPU_RAM_NOINIT;	// CPU only: no DMA reads it
static volatile uint8_t wr, rd;		// write: the audio task; read: the interrupt
static volatile bool playing;		// SPK_AUX_OPEN: the interrupt takes samples from the ring
static uint16_t smp;			// the read block's sample (interrupt)
static uint8_t sub;			// the period within it (interrupt)

void TIM8_UP_TIM13_IRQHandler(void)
{
	TIM8->SR = (uint16_t)~TIM_SR_UIF;
	int32_t half = (int32_t)(TIM8->ARR + 1u) / 2;
	int32_t v = 0;
	uint8_t r = rd;
	if (playing && r != wr) {
		const int16_t *b = ring[r];
		int32_t s0 = b[smp], s1 = s0;
		if (smp + 1u < AUDIO_BLOCK_SAMPLES) {
			s1 = b[smp + 1u];
		} else if ((uint8_t)((r + 1u) % RING) != wr) {
			s1 = ring[(r + 1u) % RING][0];
		}
		v = s0 + (s1 - s0) * (int32_t)sub / (int32_t)UP;
		if (++sub == UP) {
			sub = 0;
			if (++smp == AUDIO_BLOCK_SAMPLES) {
				smp = 0;
				rd = (uint8_t)((r + 1u) % RING);
			}
		}
	}
	int32_t out = half + (v * GAIN_Q7 / 128) * half / 32768;
	TIM8->CCR3 = (uint32_t)(out < 0 ? 0 : out > 2 * half ? 2 * half : out);
}

static void pwm_on(void)
{
	TIM8->CCR3 = (TIM8->ARR + 1u) / 2u;	// at rest: the midpoint, while the speaker is still muted
	TIM8->CCMR2 = (TIM8->CCMR2 & ~(TIM_CCMR2_OC3M | TIM_CCMR2_CC3S)) | (6u << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;
	TIM8->CCER |= TIM_CCER_CC3E;
	stm32_pin_af(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), GPIO_AF3_TIM8, false, GPIO_NOPULL);
	TIM8->SR = (uint16_t)~TIM_SR_UIF;
	TIM8->DIER |= TIM_DIER_UIE;
	HAL_NVIC_SetPriority(TIM8_UP_TIM13_IRQn, IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(TIM8_UP_TIM13_IRQn);
}

static void pwm_off(void)
{
	TIM8->DIER &= ~TIM_DIER_UIE;
	HAL_NVIC_DisableIRQ(TIM8_UP_TIM13_IRQn);
	stm32_pin_out(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), false, false);	// low when off
	TIM8->CCER &= ~TIM_CCER_CC3E;
}

static uint8_t now_state;

static void power(uint8_t state)
{
	if (state == now_state) {
		return;
	}
	if (state == SPK_AUX_OPEN) {
		smp = 0;
		sub = 0;
		playing = true;
	} else {
		playing = false;	// the interrupt leaves rd from here
		rd = wr;		// what is left is silence (spk_aux.c mutes after it): a new lead starts empty
	}
	if (now_state == SPK_AUX_OFF) {
		md380_speaker_aux(SPK_AUX_ARMED);	// the amplifier first, the PWM's step to its midpoint muted
		pwm_on();
	}
	md380_speaker_aux(state);
	if (state == SPK_AUX_OFF) {
		pwm_off();		// after the mute and the amplifier: the step to low makes no click
	}
	now_state = state;
}

static void write(const int16_t *pcm)
{
	uint8_t w = wr, next = (uint8_t)((w + 1u) % RING);
	if (next == rd) {
		return;		// full: the output has stopped taking blocks (it can't, playing): drop
	}
	memcpy(ring[w], pcm, sizeof ring[w]);
	__DMB();
	wr = next;
}

const hal_spk_aux_t md380_spk_aux = { .active = md380_speaker_aux_active, .power = power, .write = write,
				      .lead_ms = 60, .tail_ms = 60, .core = SPK_AUX_CORE };

#endif
