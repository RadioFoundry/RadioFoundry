// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV APRS (hal_afsk.h). Transmit: the tones on PB9 into the RF chip's audio input (tone_d578uv.c). Receive
// (CONFIG_APRS_RX, docs/spec/hardware-d578uv.md 3.4): the demodulated receive audio on PC0, ADC channel 10, sampled
// at 9600 Hz (eight samples a bit at 1200 baud) for the core's AFSK demodulator, where the stock firmware samples it
// for its DTMF and selective call decoders.
//
// TIM6 (the GD32 manual's TIMER5, the stock firmware's sampling timer) interrupts at the rate. Its handler reads the
// conversion it started on the last tick and starts the next on the ADC's sampling channel (mcu_io.h: a channel with
// its own trigger and data register, so the key ladders' and knobs' reads go on beside it), and puts the reading in a
// ring (drivers/audio/adc_pcm) that the radio task reads as PCM with the bias removed. A DMA scan isn't used: the
// regular group the DMA would take is the one mcu_adc_read shares between tasks.
//
// The timer runs only between the core's rx_start and rx_stop, which it calls while the radio receives FM on the APRS
// frequency. Readings reach the core only while side A is receiving FM (d578uv_fm_rx_a): while it transmits or side B
// has the AM receiver, the ring is emptied instead. That PC0 carries side A's audio while both sides receive is
// inferred (hardware-d578uv.md 3.4).
#include "d578uv.h"
#include "config.h"

#if CONFIG_APRS
#if CONFIG_APRS_RX
#include "pins.h"
#include "mcu_io.h"
#include "audio/adc_pcm/adc_pcm.h"

#define SAMPLE_TIM_N		6u
#define SAMPLE_TIMER		MCU_TIM(SAMPLE_TIM_N)
#define SAMPLE_HZ		9600u
// 12,500 timer clocks at 120 MHz (board.c's core clock; the APB1 timers run at it)
#define SAMPLE_CLOCKS		(120000000u / SAMPLE_HZ)
// No RTOS calls in the handler; above the syscall level so no critical section delays a sample, which would show as
// jitter on the tones. The DMR burst and DTMF interrupts share the level: they run only while transmitting
#define SAMPLE_IRQ_PRIORITY	3
// 1024 readings: 107 ms, more than the 100 ms hal_afsk.h asks for between the radio task's reads (every 25 ms)
#define RING_LEN		1024u

static uint16_t ring_buf[RING_LEN];
static adc_pcm_t ring;

void MCU_TIM6_HANDLER(void)
{
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
	adc_pcm_put(&ring, mcu_adc_sample_value());
	mcu_adc_sample_start();
}

static void timer_stop(void)
{
	NVIC_DisableIRQ(MCU_TIM6_IRQn);
	MCU_TIM_DIER(SAMPLE_TIMER) = 0;
	MCU_TIM_CR1(SAMPLE_TIMER) = 0;
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
	NVIC_ClearPendingIRQ(MCU_TIM6_IRQn);
}

static bool rx_start(void)
{
	static bool pin_ready;
	if (!pin_ready) {
		mcu_pin_analog(PIN_RX_AUDIO);
		mcu_adc_sample_init(ADC_RX_AUDIO);
		pin_ready = true;
	}
	mcu_tim_clock_on(SAMPLE_TIM_N);
	timer_stop();
	adc_pcm_init(&ring, ring_buf, RING_LEN, 12, 2048);
	mcu_adc_sample_start();		// the first tick reads a real conversion
	MCU_TIM_CR1(SAMPLE_TIMER) = MCU_TIM_CR1_URS;
	MCU_TIM_PSC(SAMPLE_TIMER) = 0;
	MCU_TIM_ARR(SAMPLE_TIMER) = SAMPLE_CLOCKS - 1u;
	MCU_TIM_EGR(SAMPLE_TIMER) = MCU_TIM_EGR_UG;
	MCU_TIM_SR(SAMPLE_TIMER) = 0;
	MCU_TIM_DIER(SAMPLE_TIMER) = MCU_TIM_DIER_UIE;
	NVIC_SetPriority(MCU_TIM6_IRQn, SAMPLE_IRQ_PRIORITY);
	NVIC_EnableIRQ(MCU_TIM6_IRQn);
	MCU_TIM_CR1(SAMPLE_TIMER) |= MCU_TIM_CR1_CEN;
	return true;
}

static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	if (!d578uv_fm_rx_a()) {
		adc_pcm_flush(&ring);
		return 0;
	}
	return adc_pcm_read(&ring, buf, max);
}

static void rx_stop(void)
{
	if (mcu_tim_clocked(SAMPLE_TIM_N)) {
		timer_stop();
	}
}

#endif	// CONFIG_APRS_RX

const hal_afsk_t d578uv_afsk = {
	.tx_start = d578uv_afsk_tx_start,
	.tx_busy = d578uv_afsk_tx_busy,
	.tx_stop = d578uv_afsk_tx_stop,
#if CONFIG_APRS_RX
	.rx_rate_hz = SAMPLE_HZ,
	.rx_start = rx_start,
	.rx_read = rx_read,
	.rx_stop = rx_stop,
#endif
};

#endif	// CONFIG_APRS
