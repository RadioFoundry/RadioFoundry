// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV audio synthesized on the MCU and fed to the RF chip's audio input: DTMF,
// APRS's AFSK (hal_afsk.h transmit), and P25 and NXDN transmit (the soft modem's samples, from a ring). TIM4 CH4 (the
// GD32 manual's TIMER3 CH3) runs PB9 as an 8-bit PWM (period 256 at the core clock, about 470 kHz); TIM5's (TIMER4's)
// update interrupt, about 26.1 kHz, writes the next duty from 16-bit phase accumulators and a quarter sine table. DTMF:
// the sum of the two sines, the low group at 39/63 of the high group's amplitude, a twist of about 4.2 dB. AFSK: one
// tone, phase continuous across the symbols (tone_synth.h; the core's modulator divides in 64 bits per sample, too slow
// for this interrupt). The radio code sets the RF chip up for its audio input pin before any starts here.
//
// Air transmit (CONFIG_AIR): the interrupt runs at the soft modem's rate and takes one sample a tick from a ring the
// transmit tap fills (rxtap_d578uv.c), holding the last when it runs dry.
#include "d578uv.h"
#include "config.h"
#include "pins.h"
#include "mcu_io.h"

#if CONFIG_DTMF || CONFIG_APRS || CONFIG_AIR
#include "tone_synth.h"
#if CONFIG_APRS
#include "aprs/afsk.h"
#endif

#define PWM_TIM_N		4u
#define SAMPLE_TIM_N		5u
#define PWM_TIMER		MCU_TIM(PWM_TIM_N)
#define SAMPLE_TIMER		MCU_TIM(SAMPLE_TIM_N)
#define TONE_AF			2u	// TIM4 CH4 on PB9 (the AT32's multiplexed function)
// The sample period: 0x11EF + 1 timer clocks, 26,132 Hz at 120 MHz
#define SAMPLE_CLOCKS		4592u
#define PWM_PERIOD		256u
#define AMP_HIGH		63
#define AMP_LOW			39
// No RTOS calls in the handler; above the syscall level so no critical section delays a sample (the DMR burst
// interrupt shares the level: the two never run together, the tones are FM only)
#define SAMPLE_IRQ_PRIORITY	3

static volatile uint16_t phase_lo, phase_hi, step_lo, step_hi;
static volatile bool afsk_on;

#if CONFIG_AIR
// The ring of PWM duties (the transmit tap's), read by the interrupt from head, written from tail
static uint8_t *air_ring;
static uint16_t air_mask;
static volatile uint16_t air_head, air_tail;
static volatile bool air_on;
static uint8_t air_last;
static int32_t air_gain;	// Q15
#endif

#if CONFIG_APRS
// The tones' PWM amplitude at the core's level 50 (its nominal deviation). Measured off air (UHF, 2026-11-13): 90
// gave a 2.0 kHz peak, 108 2.34 kHz, 126 2.74 kHz, with the twist the same (no clipping). Full scale is 127, so the
// path tops out near 2.75 kHz, short of the 3 kHz aimed for (aprs.md 13): level 50 sits there and a higher level
// changes nothing
#define AFSK_AMP_NOMINAL	125

static afsk_tx_t afsk;
static volatile bool afsk_busy;
#endif

void MCU_TIM5_HANDLER(void)
{
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
#if CONFIG_AIR
	if (air_on) {
		uint16_t h = air_head;

		if (h != air_tail) {
			air_last = air_ring[h];
			air_head = (uint16_t)((h + 1u) & air_mask);
		}
		MCU_TIM_CCR4(PWM_TIMER) = air_last;
		return;
	}
#endif
#if CONFIG_APRS
	if (afsk_on) {
		int32_t d = afsk_busy ? afsk_tx_next(&afsk) : -1;
		if (d < 0) {
			afsk_busy = false;	// every symbol out: the center (silence) until tx_stop
			d = 128;
		}
		MCU_TIM_CCR4(PWM_TIMER) = (uint32_t)d;
		return;
	}
#endif
	uint16_t lo = (uint16_t)(phase_lo + step_lo), hi = (uint16_t)(phase_hi + step_hi);
	phase_lo = lo;
	phase_hi = hi;
	MCU_TIM_CCR4(PWM_TIMER) = (uint32_t)(128 + tone_sine(lo, AMP_LOW) + tone_sine(hi, AMP_HIGH));
}

// Both timers running and PB9 on the PWM, the interrupt off; the caller sets its state and enables it. True when the
// timers were stopped (a new sound: its phases start at 0)
static bool synth_start(void)
{
	mcu_tim_clock_on(PWM_TIM_N);
	mcu_tim_clock_on(SAMPLE_TIM_N);
	NVIC_DisableIRQ(MCU_TIM5_IRQn);
	if (MCU_TIM_CR1(PWM_TIMER) & MCU_TIM_CR1_CEN) {
		return false;
	}
	MCU_TIM_PSC(PWM_TIMER) = 0;
	MCU_TIM_ARR(PWM_TIMER) = PWM_PERIOD - 1u;
	MCU_TIM_CCR4(PWM_TIMER) = PWM_PERIOD / 2u;
	MCU_TIM_CCMR2(PWM_TIMER) = MCU_TIM_CCMR2_OC4_PWM1 | MCU_TIM_CCMR2_OC4PE;
	MCU_TIM_CCER(PWM_TIMER) = MCU_TIM_CCER_CC4E;
	MCU_TIM_EGR(PWM_TIMER) = MCU_TIM_EGR_UG;
	MCU_TIM_CR1(PWM_TIMER) = MCU_TIM_CR1_ARPE | MCU_TIM_CR1_CEN;
	mcu_pin_af(PIN_TONE, TONE_AF, false);

	MCU_TIM_CR1(SAMPLE_TIMER) = MCU_TIM_CR1_URS;
	MCU_TIM_PSC(SAMPLE_TIMER) = 0;
	MCU_TIM_ARR(SAMPLE_TIMER) = SAMPLE_CLOCKS - 1u;
	MCU_TIM_EGR(SAMPLE_TIMER) = MCU_TIM_EGR_UG;
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
	MCU_TIM_DIER(SAMPLE_TIMER) = MCU_TIM_DIER_UIE;
	MCU_TIM_CR1(SAMPLE_TIMER) |= MCU_TIM_CR1_CEN;
	NVIC_SetPriority(MCU_TIM5_IRQn, SAMPLE_IRQ_PRIORITY);
	return true;
}

#if CONFIG_DTMF
void d578uv_dtmf_start(uint16_t low_hz, uint16_t high_hz)
{
	bool fresh = synth_start() || afsk_on;
	afsk_on = false;
	step_lo = tone_step(low_hz, SAMPLE_CLOCKS, SystemCoreClock);
	step_hi = tone_step(high_hz, SAMPLE_CLOCKS, SystemCoreClock);
	if (fresh) {
		phase_lo = phase_hi = 0;
	}
	NVIC_EnableIRQ(MCU_TIM5_IRQn);
}
#endif

#if CONFIG_AIR
void d578uv_air_tx_ring(uint8_t *buf, uint16_t len, uint16_t gain_q15)
{
	air_on = false;
	air_gain = gain_q15;
	air_ring = buf;
	air_mask = (uint16_t)(len - 1u);
	air_head = air_tail = 0;
	air_last = PWM_PERIOD / 2u;
}

// 256 counts a duty step: the duty is 128 + s x gain / 256, rounded, within 1..255
uint16_t d578uv_air_tx_put(const int16_t *s, uint16_t n)
{
	uint16_t t = air_tail, room = (uint16_t)((air_head - t - 1u) & air_mask), k;

	if (air_ring == NULL) {
		return 0;
	}
	if (n > room) {
		n = room;
	}
	for (k = 0; k < n; k++) {
		int32_t d = 128 + (((s[k] * air_gain >> 15) + 128) >> 8);

		air_ring[t] = (uint8_t)(d < 1 ? 1 : d > 255 ? 255 : d);
		t = (uint16_t)((t + 1u) & air_mask);
	}
	air_tail = t;
	return n;
}

bool d578uv_air_tx_start(uint32_t rate_hz)
{
	if (air_ring == NULL || rate_hz == 0) {
		return false;
	}
	synth_start();
	afsk_on = false;
	MCU_TIM_ARR(SAMPLE_TIMER) = SystemCoreClock / rate_hz - 1u;
	air_on = true;
	NVIC_EnableIRQ(MCU_TIM5_IRQn);
	return true;
}

void d578uv_air_tx_stop(void)
{
	d578uv_tone_stop();
	air_ring = NULL;
}
#endif

// Plain register writes (d578uv_rf_safe calls it): both timers stopped, PB9 back to analog between tones
void d578uv_tone_stop(void)
{
	afsk_on = false;
#if CONFIG_AIR
	air_on = false;
#endif
#if CONFIG_APRS
	afsk_busy = false;
#endif
	if (!mcu_tim_clocked(PWM_TIM_N) || !mcu_tim_clocked(SAMPLE_TIM_N)) {
		return;
	}
	NVIC_DisableIRQ(MCU_TIM5_IRQn);
	MCU_TIM_DIER(SAMPLE_TIMER) = 0;
	MCU_TIM_CR1(SAMPLE_TIMER) = 0;
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
	MCU_TIM_CR1(PWM_TIMER) = 0;
	MCU_TIM_CCER(PWM_TIMER) = 0;
	NVIC_ClearPendingIRQ(MCU_TIM5_IRQn);
	mcu_pin_analog(PIN_TONE);
}

#if CONFIG_APRS
// hal_afsk.h transmit. The core has keyed the transmitter in FM; the RF chip goes to its audio input (the DTMF set-up),
// then the tones play from the interrupt. Bell 202 only, as the core sends
bool d578uv_afsk_tx_start(const uint8_t *tones, uint16_t nbits, uint16_t baud, uint16_t mark_hz, uint16_t space_hz,
			  uint8_t level)
{
	if (afsk_busy || nbits == 0 || baud != AFSK_BAUD || mark_hz != AFSK_MARK_HZ || space_hz != AFSK_SPACE_HZ) {
		return false;
	}
	if (!d578uv_line_in(true)) {
		return false;
	}
	uint32_t a = (uint32_t)(level > 100u ? 100u : level) * AFSK_AMP_NOMINAL / 50u;
	synth_start();
	afsk_tx_init(&afsk, tones, nbits, baud, mark_hz, space_hz, (uint8_t)(a > 127u ? 127u : a), SAMPLE_CLOCKS,
		     SystemCoreClock);
	afsk_busy = true;
	afsk_on = true;
	NVIC_EnableIRQ(MCU_TIM5_IRQn);
	return true;
}

bool d578uv_afsk_tx_busy(void)
{
	return afsk_busy;
}

void d578uv_afsk_tx_stop(void)
{
	d578uv_tone_stop();
	d578uv_line_in(false);
}
#endif

#endif
