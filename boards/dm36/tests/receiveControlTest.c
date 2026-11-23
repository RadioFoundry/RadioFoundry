// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Run the board's mode switch with a beep completion injected immediately after the RF table writes register 0x47.
#include <assert.h>
#include <stdio.h>
#include "platform.h"
#include "dm32.h"

static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t s) { (void)s; }
static uint32_t dac;
#define hrc_irq_save irq_save
#define hrc_irq_restore irq_restore
#undef DAC_DATA_B
#define DAC_DATA_B dac
#include "../../dm32/radio_dm32.c"

static uint16_t af, adc;
static bool end_beep, amplifier;

void fd6818_set_mode(fd6818_t *dev, fd6818_mode_t next)
{
	uint16_t table_af = next == FD6818_MODE_DMR ? 0x6943 : 0x6042;
	af = table_af;
	if (end_beep) {
		assert(pcm_on);
		speaker_pcm(false);
		assert(af == table_af);
	}
}

void fd6818_af_mute(fd6818_t *dev, bool mute) { af = mute ? 0x6042 : 0x6142; }
void fd6818_write(fd6818_t *dev, uint8_t reg, uint16_t val) { assert(reg == 0x48); }
void fd6818_rx_sleep(fd6818_t *dev, bool sleep, fd6818_mode_t m) {}
void hrc7000_fm_idle(hrc7000_t *d) {}
void hrc7000_dmr_enter(hrc7000_t *d, uint32_t bias, uint8_t gain) {}
void hrc7000_audio_bits(hrc7000_t *d, uint32_t set, uint32_t clear) {}
void hrc_audio_pcm_ring(hrc7000_t *d, bool on) {}
void hrc_pin_write(uint8_t pin, bool level) { if (pin == PIN_AMP_EN) amplifier = level; }
void hrc_dac_b_power(bool on) {}
void hrc_dmr_enter(void) {}
void hrc_dmr_leave(void) {}
void hrc_rfburst_disarm(void) {}
bool hrc_rfburst_armed(void) { return false; }
uint8_t bf_cal_tx_center(void) { return 128; }
uint16_t bf_cal_modcal(uint32_t hz) { return 0; }

void hrc_adc_init(void) {}
void hrc_pin_in(uint8_t pin) {}
void hrc_pin_out(uint8_t pin, bool on) {}
uint32_t hal_millis(void) { return 0; }
uint16_t hrc_adc_read(uint8_t ch) { assert(ch == KNOB_ADC_CH); return adc; }

static void test_mode_change(void)
{
	fd.bus = &fd_bus.bus;
	rx_audio = true;
	set_mode(RADIO_MODE_DMR);
	set_mode(RADIO_MODE_FM);
	speaker_pcm(true);
	assert(af == 0x6042);
	end_beep = true;
	set_mode(RADIO_MODE_DMR);
	end_beep = false;
	assert(!pcm_on && af == 0x6943);
	speaker_pcm(true);
	speaker_pcm(false);
	assert(af == 0x6943);
	speaker_pcm(true);
	end_beep = true;
	set_mode(RADIO_MODE_FM);
	end_beep = false;
	assert(!pcm_on && af == 0x6042);
	speaker_pcm(true);
	speaker_pcm(false);
	assert(af == 0x6142);
}

static void test_aprs_receive_mute(void)
{
	for (unsigned muted = 0; muted < 2; muted++) {
		vol_muted = muted;
		audio(true);
		assert(amplifier == !muted);
		aprs_listen_route(true);
		assert(amplifier == !muted && af == 0x6142);
		aprs_listen_route(false);
		assert(amplifier == !muted && af == 0x6142);
	}
	audio(false);
	aprs_listen_route(true);
	assert(!amplifier && af == 0x6142);
	aprs_listen_route(false);
	assert(!amplifier && af == 0x6042);
}

static void test_volume_knob(void)
{
	// Endpoints, calibration knots, interpolation on both slopes, and saturation above the last knot.
	static const struct { uint16_t raw; int percent; } points[] = {
		{ 0, 0 }, { 29, 12 }, { 43, 18 }, { 57, 25 }, { 86, 37 }, { 114, 50 }, { 178, 56 },
		{ 243, 62 }, { 387, 75 }, { 459, 81 }, { 532, 87 }, { 671, 99 }, { 672, 100 }, { 1023, 100 },
	};
	for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
		adc = points[i].raw;
		assert(board_power.volume() == points[i].percent);
	}
	int previous = 0;
	for (adc = 0; adc <= 1023; adc++) {
		int now = board_power.volume();
		assert(now >= previous && now <= previous + 1 && now <= 100);
		previous = now;
	}
}

static void frames(int16_t level, unsigned n)
{
	int16_t pcm[VOCODER_FRAME_SAMPLES] = { 0 };
	pcm[7] = level;
	while (n-- > 0) {
		hrc_vocoder_rx_frame(pcm);
	}
}

static void test_dmr_amp_gate(void)
{
	vol_muted = false;
	audio(true);
	speaker_pcm(true);
	assert(amplifier);
	frames(-900, 3);
	frames(400, AMP_GATE_HOLD - 1u);
	assert(amplifier);
	frames(72, 1);
	assert(!amplifier);	// 300 ms of quiet frames
	frames(0, 50);
	assert(!amplifier);
	frames(-401, 1);
	assert(amplifier);	// the first speech frame
	frames(0, AMP_GATE_HOLD);
	assert(!amplifier);
	amp(AMP_SIDE, true);
	assert(amplifier);	// any other user keeps it on
	amp(AMP_SIDE, false);
	assert(!amplifier);
	speaker_pcm(false);
	speaker_pcm(true);
	assert(amplifier);	// the next call starts on
	frames(0, AMP_GATE_HOLD);
	assert(!amplifier);
	speaker_pcm(false);
	audio(false);
	assert(!amplifier);
	frames(0, AMP_GATE_HOLD);
	amp(AMP_BCAST, true);
	assert(amplifier);	// no call: the broadcast receiver is untouched
	amp(AMP_BCAST, false);
}

int main(void)
{
	test_mode_change();
	test_aprs_receive_mute();
	test_volume_knob();
	test_dmr_amp_gate();
	puts("receiveControlTest: mode change, APRS receive mute, volume knob and DMR amp gate passed");
	return 0;
}
