// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family PCM audio: the shared I2S3 stream to the HR_C6000's built-in codec (platform/stm32f4/hrc/i2s_pcm.c,
// docs/spec/hardware-dm1701.md 3.2, 4.4, 8) with the board's speaker routing, the FM transmit audio for USRP and the
// audio probe.
#include "uvf.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "pins.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hrc/i2s_pcm.h"

#include <string.h>
#if CONFIG_AUDIO_PROBE
#include <math.h>
#endif
#if CONFIG_NET_USRP
#include "usbaudio/audio_mux.h"
#endif

#if CONFIG_AUDIO_PROBE
static int16_t probe_raw[2 * AUDIO_BLOCK_SAMPLES];	// the last block as received, left / right interleaved

static void probe_copy(const int16_t *lr)
{
	memcpy(probe_raw, lr, sizeof probe_raw);
}
#endif

// Clock idle high (spec 4.4)
static const stm32_i2s_pcm_cfg_t i2s_cfg = {
	.ckpol = true,
#if CONFIG_AUDIO_PROBE
	.raw_block = probe_copy,
#endif
};

// --- speaker routing ---

// The amp, its mute and the RX audio mux are shared with the FM path in radio_uvf.c. A board-level
// uvf_speaker_pcm() there (combining FM squelch and PCM requests) overrides this fallback, which saves the pins on
// and restores them on off.
void uvf_speaker_pcm(bool on);

static uint32_t saved_odr_b, saved_odr_d;
static bool pcm_on;

__attribute__((weak)) void uvf_speaker_pcm(bool on)
{
	const uint32_t mask_b = PIN_MASK(PIN_SPK_MUTE) | PIN_MASK(PIN_AMP_EN);
	const uint32_t mask_d = PIN_MASK(PIN_RX_AUDIO_MUX);
	if (on == pcm_on) {
		return;
	}
	pcm_on = on;
	if (on) {
		saved_odr_b = GPIOB->ODR & mask_b;
		saved_odr_d = GPIOD->ODR & mask_d;
		PIN_WRITE(PIN_RX_AUDIO_MUX, true);	// HR_C6000 line out (its DAC carries the I2S PCM) to the amp
		PIN_WRITE(PIN_AMP_EN, true);
		PIN_WRITE(PIN_SPK_MUTE, false);
	} else {
		GPIOB->BSRR = saved_odr_b | ((mask_b & ~saved_odr_b) << 16);
		GPIOD->BSRR = saved_odr_d | ((mask_d & ~saved_odr_d) << 16);
	}
}

static void speaker_pcm(bool on)
{
	uvf_speaker_pcm(on);
}

// --- hal_audio_t ---

static bool start(audio_block_cb_t cb, void *ctx)
{
	return stm32_i2s_pcm_start(&i2s_cfg, cb, ctx);
}

const hal_audio_t uvf_audio = { .start = start, .stop = stm32_i2s_pcm_stop, .speaker_pcm = speaker_pcm };

#if CONFIG_NET_USRP
// --- FM transmit audio: PWM on PC8 into the HR_C6000's microphone input (hardware-dm1701.md 21.3.1, "PWM audio") ---
//
// PC8 (the stock beep PWM, TIM8 CH3 on AF3) reaches MIC_P / MIC_N; radio_uvf.c switches those inputs on and the
// microphone (LINEIN1) off. The chip's codec samples at 8 kHz behind its own filter, so a carrier of 72 kHz (nine
// periods a sample) needs no filter of ours. The duty follows the samples, linearly interpolated over the nine periods,
// with the rounding error carried from period to period: the codec averages the periods, so this gains about three
// bits over the timer's 1000 steps. DMA2 stream 2 (TIM8 CH3's request) rewrites CCR3 every period from a circular
// buffer of two 5 ms halves, refilled from the interrupt with samples the core's pull returns.

#define PWM_TOP			999u				// 72 MHz / 1000 = 72 kHz
#define PWM_OVERSAMPLE		9u				// carrier periods a sample
#define PWM_HALF_SAMPLES	40u				// 5 ms
#define PWM_HALF		(PWM_HALF_SAMPLES * PWM_OVERSAMPLE)
// Full scale (32767) to this many timer steps either side of the middle: 10 % of the range, about 2.6 kHz rms
// deviation for a full-scale sine with the mic input gain the radio sets (bench 2026-10-12, 4 % gave 1 kHz rms, 10 %
// 2.6 kHz rms, the limiter above). 0x34's limiter keeps the deviation inside the channel whatever the host sends
#define PWM_AMP_STEPS		50
#define PWM_IRQ_PRIORITY	6

static uint16_t pwm_buf[2 * PWM_HALF] __attribute__((aligned(4)));	// main SRAM: DMA
static audio_tap_pull_t volatile pwm_pull;
static int32_t pwm_prev_q8, pwm_err_q8;		// the last sample as a duty (x256), the carried rounding error

static void pwm_fill(uint16_t *out)
{
	int16_t pcm[PWM_HALF_SAMPLES];
	audio_tap_pull_t pull = pwm_pull;
	if (pull != NULL) {
		pull(pcm, PWM_HALF_SAMPLES);
	} else {
		memset(pcm, 0, sizeof pcm);
	}
	const int32_t mid_q8 = (int32_t)((PWM_TOP + 1u) / 2u) << 8;
	for (uint32_t i = 0; i < PWM_HALF_SAMPLES; i++) {
		int32_t next_q8 = mid_q8 + (((int32_t)pcm[i] * PWM_AMP_STEPS) >> 7);	// x256 / 32768
		int32_t step_q8 = (next_q8 - pwm_prev_q8) / (int32_t)PWM_OVERSAMPLE;
		int32_t v_q8 = pwm_prev_q8;
		for (uint32_t k = 0; k < PWM_OVERSAMPLE; k++) {
			v_q8 += step_q8;
			int32_t want = v_q8 + pwm_err_q8;
			int32_t duty = (want + 128) >> 8;
			pwm_err_q8 = want - (duty << 8);
			*out++ = (uint16_t)(duty < 0 ? 0 : duty > (int32_t)PWM_TOP ? (int32_t)PWM_TOP : duty);
		}
		pwm_prev_q8 = next_q8;
	}
}

void DMA2_Stream2_IRQHandler(void)
{
	uint32_t isr = DMA2->LISR;
	DMA2->LIFCR = isr & (DMA_LISR_HTIF2 | DMA_LISR_TCIF2 | DMA_LISR_TEIF2 | DMA_LISR_DMEIF2 | DMA_LISR_FEIF2);
	if (isr & DMA_LISR_HTIF2) {
		pwm_fill(&pwm_buf[0]);		// the first half has gone out; the DMA reads the second
	}
	if (isr & DMA_LISR_TCIF2) {
		pwm_fill(&pwm_buf[PWM_HALF]);
	}
}

static void pwm_stop(void)
{
	TIM8->CR1 = 0;
	TIM8->DIER = 0;
	DMA2_Stream2->CR &= ~DMA_SxCR_EN;
	while (DMA2_Stream2->CR & DMA_SxCR_EN) {
	}
	DMA2->LIFCR = 0x3Fu << 16;			// stream 2's flags
	pwm_pull = NULL;
	stm32_pin_out(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), false, false);	// back to the low output
}

// pull non-NULL: start the PWM audio; NULL: stop it. Radio task (radio_uvf.c's fm_tx_inject)
void uvf_pwm_audio(audio_tap_pull_t pull)
{
	pwm_stop();
	if (pull == NULL) {
		return;
	}
	pwm_prev_q8 = (int32_t)((PWM_TOP + 1u) / 2u) << 8;
	pwm_err_q8 = 0;
	for (uint32_t i = 0; i < 2u * PWM_HALF; i++) {
		pwm_buf[i] = (uint16_t)((PWM_TOP + 1u) / 2u);	// silence until the first refill
	}
	pwm_pull = pull;
	__HAL_RCC_TIM8_CLK_ENABLE();
	__HAL_RCC_DMA2_CLK_ENABLE();
	TIM8->PSC = 0;					// 72 MHz counts
	TIM8->ARR = PWM_TOP;
	TIM8->CCR3 = pwm_buf[0];
	TIM8->CCMR2 = (6u << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;	// PWM mode 1, preload: a new duty each period
	TIM8->CCER = TIM_CCER_CC3E;
	TIM8->BDTR = TIM_BDTR_MOE;			// TIM8 is an advanced timer: main output enable
	TIM8->EGR = TIM_EGR_UG;
	DMA2_Stream2->PAR = (uint32_t)&TIM8->CCR3;
	DMA_RAM_OK(pwm_buf);
	DMA2_Stream2->M0AR = (uint32_t)pwm_buf;
	DMA2_Stream2->NDTR = 2u * PWM_HALF;
	DMA2_Stream2->FCR = 0;
	DMA2_Stream2->CR = (0u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 |
			   DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_DIR_0 | DMA_SxCR_HTIE | DMA_SxCR_TCIE;
	HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, PWM_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);
	DMA2_Stream2->CR |= DMA_SxCR_EN;
	TIM8->DIER = TIM_DIER_CC3DE;
	TIM8->CR1 = TIM_CR1_CEN;
	stm32_pin_af(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), GPIO_AF3_TIM8, false, GPIO_NOPULL);
}
#endif

#if CONFIG_AUDIO_PROBE
// The probe's view of the stream (hal_audio_tap.h). Called from the probe's block callback, i.e. the audio task right
// after probe_copy() copied the block, or from the debug link task (a torn block is fine for a diagnostic)
static bool probe_last_block(int16_t *left, int16_t *right, uint32_t *count)
{
	if (!stm32_i2s_pcm_running()) {
		return false;
	}
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		left[i] = probe_raw[2 * i];
		right[i] = probe_raw[2 * i + 1];
	}
	stm32_i2s_pcm_stats_t st;
	stm32_i2s_pcm_stats(&st);
	*count = st.blocks;
	return true;
}

// PC8 (the stock beep PWM, TIM8_CH3 on AF3): a 50 % square wave; off puts it back as the low output board.c makes it
static void probe_pin_tone(uint16_t hz)
{
	TIM8->CR1 = 0;
	TIM8->DIER = 0;
	DMA2_Stream2->CR = 0;				// the PWM audio's DMA, if it ran
	if (hz == 0) {
		stm32_pin_out(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), false, false);
		return;
	}
	__HAL_RCC_TIM8_CLK_ENABLE();
	TIM8->CR1 = 0;
	TIM8->PSC = 72 - 1;				// APB2 timer clock 72 MHz -> 1 MHz
	TIM8->ARR = 1000000u / hz - 1u;
	TIM8->CCR3 = 500000u / hz;
	TIM8->CCMR2 = 6u << TIM_CCMR2_OC3M_Pos;		// PWM mode 1
	TIM8->CCER = TIM_CCER_CC3E;
	TIM8->BDTR = TIM_BDTR_MOE;			// TIM8 is an advanced timer: main output enable
	TIM8->EGR = TIM_EGR_UG;
	TIM8->CR1 = TIM_CR1_CEN;
	stm32_pin_af(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), GPIO_AF3_TIM8, false, GPIO_NOPULL);
}

// PWM audio on PC8: TIM8 CH3, its compare request on DMA2 stream 2 channel 0 rewriting CCR3 from a circular table each
// carrier period. The table holds whole periods of the tone (as near hz as its length allows)
#define PWM_TABLE_MAX		1024u
static uint16_t pwm_table[PWM_TABLE_MAX];

static uint32_t probe_pin_pwm(uint16_t top, uint16_t hz, uint16_t amp_permille)
{
	DMA2_Stream2->CR = 0;
	while (DMA2_Stream2->CR & DMA_SxCR_EN) {
	}
	if (hz == 0 || top < 15u) {
		probe_pin_tone(0);
		return 0;
	}
	uint32_t fs = 72000000u / (top + 1u);
	uint32_t k = (PWM_TABLE_MAX * hz) / fs;		// whole tone periods that fit the table
	if (k == 0) {
		k = 1;
	}
	uint32_t n = (fs * k + hz / 2u) / hz;
	if (n > PWM_TABLE_MAX || n < 4u) {
		return 0;
	}
	float half = (float)(top + 1u) / 2.0f, a = half * (float)(amp_permille > 1000u ? 1000u : amp_permille) / 1000.0f;
	for (uint32_t i = 0; i < n; i++) {
		int32_t v = (int32_t)(half + a * sinf(6.2831853f * (float)(i * k) / (float)n) + 0.5f);
		pwm_table[i] = (uint16_t)(v < 0 ? 0 : v > (int32_t)top ? top : v);
	}
	__HAL_RCC_TIM8_CLK_ENABLE();
	__HAL_RCC_DMA2_CLK_ENABLE();
	TIM8->CR1 = 0;
	TIM8->DIER = 0;
	TIM8->PSC = 0;					// 72 MHz counts
	TIM8->ARR = top;
	TIM8->CCR3 = pwm_table[0];
	TIM8->CCMR2 = (6u << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;	// PWM mode 1, preload: a new duty each period
	TIM8->CCER = TIM_CCER_CC3E;
	TIM8->BDTR = TIM_BDTR_MOE;
	TIM8->EGR = TIM_EGR_UG;
	DMA2->LIFCR = 0x3Fu << 16;			// stream 2's flags
	DMA2_Stream2->PAR = (uint32_t)&TIM8->CCR3;
	DMA2_Stream2->M0AR = (uint32_t)pwm_table;
	DMA2_Stream2->NDTR = n;
	DMA2_Stream2->FCR = 0;
	DMA2_Stream2->CR = (0u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 |
			   DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_DIR_0;
	DMA2_Stream2->CR |= DMA_SxCR_EN;
	TIM8->DIER = TIM_DIER_CC3DE;
	TIM8->CR1 = TIM_CR1_CEN;
	stm32_pin_af(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), GPIO_AF3_TIM8, false, GPIO_NOPULL);
	uint32_t ndtr = DMA2_Stream2->NDTR;
	uint32_t t0 = hal_millis();
	while (DMA2_Stream2->NDTR == ndtr && hal_millis() - t0 < 2u) {
	}
	return DMA2_Stream2->NDTR != ndtr ? fs : 0;	// the DMA must be moving
}

// Sample GPIOA-E's inputs back to back for ms; output pins read their own level, so a diff of two scans is the answer
static uint8_t probe_pin_scan(uint16_t ms, uint16_t *changed, uint8_t max)
{
	GPIO_TypeDef *const ports[] = { GPIOA, GPIOB, GPIOC, GPIOD, GPIOE };
	uint8_t n = max < 5 ? max : 5;
	uint16_t first[5];
	for (uint8_t i = 0; i < n; i++) {
		first[i] = (uint16_t)ports[i]->IDR;
		changed[i] = 0;
	}
	uint32_t t0 = hal_millis();
	while (hal_millis() - t0 < ms) {
		for (uint8_t i = 0; i < n; i++) {
			changed[i] |= (uint16_t)(ports[i]->IDR ^ first[i]);
		}
	}
	return n;
}

#endif

#if CONFIG_AUDIO_PROBE || CONFIG_NET_USRP || CONFIG_APRS_RX
const hal_audio_tap_t uvf_audio_tap = {
	.caps = AUDIO_TAP_DMR | AUDIO_TAP_FM_RX
#if CONFIG_NET_USRP
		| AUDIO_TAP_FM_TX,
	.fm_tx_inject = uvf_fm_tx_inject,
	.fm_tx_nominal_peak = 13800,	// 21 of PWM_AMP_STEPS: about 1.5 kHz peak deviation (bench 2026-10-12, 4 % = 1 kHz rms)
#else
		,
#endif
#if CONFIG_AUDIO_PROBE
	.modem_reg = uvf_modem_reg,
	.modem_ram = uvf_modem_ram,
	.modem_burst = uvf_modem_burst,
	.last_block = probe_last_block,
	.pin_tone = probe_pin_tone,
	.pin_scan = probe_pin_scan,
	.pin_pwm = probe_pin_pwm,
#endif
};
#endif

#endif	// CONFIG_DMR
