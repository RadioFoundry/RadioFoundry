// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio/rx_agc.h"
#include "hal_audio.h"

static void tone(int16_t *pcm, int amp)
{
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		pcm[i] = (int16_t)(i % 8 < 4 ? amp : -amp);
}

static void settle(rx_agc_t *s, int amp, unsigned blocks)
{
	int16_t pcm[AUDIO_BLOCK_SAMPLES];
	for (unsigned i = 0; i < blocks; i++) {
		tone(pcm, amp);
		rx_agc_process(s, pcm, true);
	}
}

// Equal time at quiet and loud syllables has a stable average, independent of the current syllable.
static void test_syllable_average(void)
{
	rx_agc_t s;
	rx_agc_view_t v;
	rx_agc_reset();
	rx_agc_configure(RX_AGC_DMR, 5);
	rx_agc_begin(&s, RX_AGC_DMR, 70, 0);
	int low = 0, high = -96, out_low = 0, out_high = -96;
	int gain_low = 24 * 256, gain_high = -24 * 256;
	for (unsigned i = 0; i < 600; i++) {
		settle(&s, i % 10 < 5 ? 500 : 2000, 1);
		if (i < 300) continue;
		rx_agc_view(RX_AGC_DMR, &v);
		if (v.input_dbfs < low) low = v.input_dbfs;
		if (v.input_dbfs > high) high = v.input_dbfs;
		if (v.output_dbfs < out_low) out_low = v.output_dbfs;
		if (v.output_dbfs > out_high) out_high = v.output_dbfs;
		if (s.gain_q8 < gain_low) gain_low = s.gain_q8;
		if (s.gain_q8 > gain_high) gain_high = s.gain_q8;
	}
	printf("syllables: input %d..%d, output %d..%d, gain swing %d/256 dB\n",
		low, high, out_low, out_high, gain_high - gain_low);
	fflush(stdout);
	assert(high - low <= 2 && out_high - out_low <= 2);
	assert(low >= -28 && high <= -26); // RMS of the combined energy, not the average dB
	assert(out_low >= -26 && out_high <= -24);
	assert(gain_high - gain_low <= 128);
	int16_t gain = s.gain_q8;
	uint32_t input = s.input_power, output = s.output_power;
	int16_t pcm[AUDIO_BLOCK_SAMPLES] = { 0 };
	for (unsigned i = 0; i < 100; i++)
		rx_agc_process(&s, pcm, true);
	assert(s.gain_q8 == gain && s.input_power == input && s.output_power == output);
	rx_agc_begin(&s, RX_AGC_DMR, 70, 0);
	assert(s.cached && s.gain_q8 >= 1 * 256 && s.gain_q8 <= 3 * 256);
}

static void test_average_response(void)
{
	rx_agc_t s;
	rx_agc_reset();
	rx_agc_configure(RX_AGC_DMR, 5);
	rx_agc_begin(&s, RX_AGC_DMR, 71, 0);
	settle(&s, 1000, 100);
	int16_t start = s.gain_q8;
	for (unsigned i = 0; i < 75; i++) {
		int16_t before = s.gain_q8;
		settle(&s, 2000, 1);
		assert(s.gain_q8 >= before - 256 && s.gain_q8 <= before + 5);
	}
	// After 1.5 seconds the energy average has traversed about 63% of a sustained step.
	assert(s.input_q8 >= -27 * 256 && s.input_q8 <= -25 * 256);
	assert(s.gain_q8 < start);
	settle(&s, 2000, 600);
	assert(s.input_q8 >= -25 * 256 && s.input_q8 <= -24 * 256);
	// A lost loud frame must not alter either average, even when peak protection intervenes.
	uint32_t input = s.input_power, output = s.output_power;
	int16_t pcm[AUDIO_BLOCK_SAMPLES];
	tone(pcm, 32767);
	rx_agc_process(&s, pcm, false);
	assert(s.input_power == input && s.output_power == output);
	// With settled gain, amplitude ratios survive: this is gain control, not a speech filter.
	settle(&s, 1000, 600);
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		pcm[i] = (int16_t)(i % 4 == 0 ? 500 : i % 4 == 1 ? -500 : i % 4 == 2 ? 1000 : -1000);
	rx_agc_process(&s, pcm, false);
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i += 4) {
		assert(pcm[i] == -pcm[i + 1] && pcm[i + 2] == -pcm[i + 3]);
		assert(pcm[i + 2] >= 2 * pcm[i] - 1 && pcm[i + 2] <= 2 * pcm[i] + 1);
	}
}

// A quiet lead-in must not lock in boost when the talker's normal voice is already above target.
static void test_loud_after_quiet_start(void)
{
	rx_agc_t s;
	rx_agc_view_t v;
	rx_agc_reset();
	rx_agc_configure(RX_AGC_DMR, 2); // -33 dBFS
	rx_agc_begin(&s, RX_AGC_DMR, 80, 0);
	settle(&s, 328, 15); // -40 dBFS lead-in finishes fast acquisition with positive gain
	int16_t pcm[AUDIO_BLOCK_SAMPLES];
	for (unsigned i = 0; i < 100; i++) {
		tone(pcm, 3277); // normal voice is -20 dBFS
		rx_agc_process(&s, pcm, true);
		if (i == 9) assert(s.gain_q8 <= 0); // positive gain is gone within 200 ms
	}
	rx_agc_view(RX_AGC_DMR, &v);
	printf("loud after quiet start: %d -> %d dBFS, gain %d/256 dB\n",
		v.input_dbfs, v.output_dbfs, s.gain_q8);
	fflush(stdout);
	assert(v.input_dbfs >= -22 && v.input_dbfs <= -20);
	assert(s.gain_q8 < 0);
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		assert(pcm[i] >= -925 && pcm[i] <= 925); // actual output is at most -31 dBFS after two seconds
	settle(&s, 3277, 500);
	rx_agc_view(RX_AGC_DMR, &v);
	assert(v.input_dbfs == -20 && v.output_dbfs >= -34 && v.output_dbfs <= -32);
}

static void test_stale_boost(void)
{
	for (unsigned mode = 0; mode < RX_AGC_MODES; mode++) {
		for (unsigned cached = 0; cached < 2; cached++) {
			rx_agc_t s;
			rx_agc_reset();
			rx_agc_configure((rx_agc_mode_t)mode, 2);
			rx_agc_begin(&s, (rx_agc_mode_t)mode, 81, 0);
			settle(&s, 66, 60); // near-gate audio teaches roughly +21 dB of boost
			assert(s.gain_q8 >= 20 * 256);
			int16_t pcm[AUDIO_BLOCK_SAMPLES] = { 0 };
			for (unsigned i = 0; i < 100; i++)
				rx_agc_process(&s, pcm, true);
			if (cached) {
				rx_agc_end(&s);
				rx_agc_begin(&s, (rx_agc_mode_t)mode, 81, 0);
				assert(s.cached && s.gain_q8 >= 20 * 256);
			}
			for (unsigned i = 0; i < 100; i++) {
				int16_t before = s.gain_q8;
				tone(pcm, 3277);
				rx_agc_process(&s, pcm, true);
				assert(s.gain_q8 <= before); // a level increase never requests more boost
				if (i == 29) assert(s.gain_q8 < 0);
			}
			for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
				assert(pcm[i] >= -925 && pcm[i] <= 925);
		}
	}
}

static uint64_t energy(const int16_t *pcm)
{
	uint64_t sum = 0;
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		sum += (uint32_t)((int32_t)pcm[i] * pcm[i]);
	return sum;
}

static void test_startup_garble(void)
{
	rx_agc_t s;
	rx_agc_reset();
	rx_agc_configure(RX_AGC_DMR, 2);
	rx_agc_begin(&s, RX_AGC_DMR, 90, 0);
	settle(&s, 66, 60); // stale +21 dB caller estimate
	rx_agc_end(&s);
	rx_agc_begin(&s, RX_AGC_DMR, 90, 0);
	int16_t pcm[AUDIO_BLOCK_SAMPLES];
	tone(pcm, 32767);
	rx_agc_process(&s, pcm, false); // concealed garble must still be protected
	printf("startup garble: energy %llu, target ceiling %llu\n",
		(unsigned long long)energy(pcm), (unsigned long long)733 * 733 * AUDIO_BLOCK_SAMPLES);
	fflush(stdout);
	assert(energy(pcm) <= (uint64_t)733 * 733 * AUDIO_BLOCK_SAMPLES);
}

static void corrupt_block(int16_t *pcm, unsigned pattern);

static void test_target_sweep(void)
{
	// Square-wave RMS amplitudes for every whole dB from -40 through -15 dBFS.
	static const int amplitudes[] = { 328, 368, 413, 463, 519, 583, 654, 734, 823, 923, 1036, 1163, 1305,
		1464, 1642, 1843, 2068, 2320, 2603, 2920, 3277, 3677, 4125, 4629, 5193, 5827 };
	for (unsigned mode = 0; mode < RX_AGC_MODES; mode++) {
		for (unsigned a = 0; a < sizeof amplitudes / sizeof amplitudes[0]; a++) {
			for (unsigned history = 0; history < 5; history++) {
				rx_agc_t s;
				rx_agc_view_t v;
				int16_t pcm[AUDIO_BLOCK_SAMPLES];
				rx_agc_reset();
				rx_agc_configure((rx_agc_mode_t)mode, 2);
				if (history && history < 4) {
					rx_agc_begin(&s, (rx_agc_mode_t)mode, 100, 0);
					settle(&s, history == 1 ? 66 : history == 2 ? 30000 : amplitudes[a], 60);
					rx_agc_end(&s);
				}
				rx_agc_begin(&s, (rx_agc_mode_t)mode, history == 2 ? 101 : 100, 0);
				assert(s.cached == (history == 1 || history == 3));
				if (history == 4) {
					for (unsigned block = 0; block < 30; block++) {
						corrupt_block(pcm, 5);
						rx_agc_process(&s, pcm, false);
						assert(energy(pcm) <= (uint64_t)733 * 733 * AUDIO_BLOCK_SAMPLES);
					}
					assert(s.speech_blocks == 0 && s.input_power == 0);
				}
				for (unsigned block = 0; block < 15; block++) {
					tone(pcm, amplitudes[a]);
					rx_agc_process(&s, pcm, true);
					assert(energy(pcm) <= (uint64_t)733 * 733 * AUDIO_BLOCK_SAMPLES);
				}
				assert(energy(pcm) >= (uint64_t)690 * 690 * AUDIO_BLOCK_SAMPLES);
				int16_t gain = s.gain_q8;
				for (unsigned block = 0; block < 100; block++) {
					tone(pcm, amplitudes[a]);
					rx_agc_process(&s, pcm, true);
					assert(s.gain_q8 == gain); // constant talker level must not wander after acquisition
					assert(energy(pcm) >= (uint64_t)690 * 690 * AUDIO_BLOCK_SAMPLES);
					assert(energy(pcm) <= (uint64_t)778 * 778 * AUDIO_BLOCK_SAMPLES);
				}
				rx_agc_view((rx_agc_mode_t)mode, &v);
				assert(v.input_dbfs >= (int)a - 41 && v.input_dbfs <= (int)a - 39);
				assert(v.output_dbfs >= -34 && v.output_dbfs <= -32);
			}
		}
	}
	puts("target sweep: -40..-15 -> -33 dBFS within 300 ms; all modes and five startup histories PASS");
}

static void corrupt_block(int16_t *pcm, unsigned pattern)
{
	uint32_t random = 1;
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		random = random * 1664525u + 1013904223u;
		switch (pattern) {
		case 0: pcm[i] = i % 2 ? 32767 : -32768; break;
		case 1: pcm[i] = 32767; break;
		case 2: pcm[i] = -32768; break;
		case 3: pcm[i] = i == 0 ? -32768 : 0; break;
		case 4: pcm[i] = 0; break;
		case 5: pcm[i] = (int16_t)((int32_t)(random >> 16) - 32768); break;
		default: pcm[i] = i % 2 ? 32767 : 0; break;
		}
	}
}

static void test_corrupt_blocks(void)
{
	for (unsigned mode = 0; mode < RX_AGC_MODES; mode++) {
		for (unsigned pattern = 0; pattern < 7; pattern++) {
			for (unsigned learn = 0; learn < 2; learn++) {
				rx_agc_t s;
				int16_t pcm[AUDIO_BLOCK_SAMPLES];
				rx_agc_reset();
				rx_agc_configure((rx_agc_mode_t)mode, 2);
				rx_agc_begin(&s, (rx_agc_mode_t)mode, 200, 0);
				settle(&s, 66, 60);
				rx_agc_end(&s);
				rx_agc_begin(&s, (rx_agc_mode_t)mode, 200, 0);
				int16_t gain = s.gain_q8;
				corrupt_block(pcm, pattern);
				rx_agc_process(&s, pcm, learn != 0);
				assert(energy(pcm) <= (uint64_t)733 * 733 * AUDIO_BLOCK_SAMPLES);
				if (!learn)
					assert(s.gain_q8 == gain && s.input_power == 0 && s.output_power == 0);
				settle(&s, 66, 60);
				corrupt_block(pcm, pattern);
				rx_agc_process(&s, pcm, learn != 0);
				assert(energy(pcm) <= (uint64_t)1466 * 1466 * AUDIO_BLOCK_SAMPLES);
				for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
					assert(pcm[i] >= -29204 && pcm[i] <= 29204);
			}
		}
	}
	puts("corrupt blocks: rails, impulses, random data and concealment stay inside raw-RMS ceilings PASS");
}

static void test_guard_target_changes(void)
{
	rx_agc_t s;
	int16_t pcm[AUDIO_BLOCK_SAMPLES], copy[AUDIO_BLOCK_SAMPLES];
	rx_agc_reset();
	rx_agc_configure(RX_AGC_DMR, 10);
	rx_agc_begin(&s, RX_AGC_DMR, 300, 0);
	settle(&s, 66, 60);
	rx_agc_configure(RX_AGC_DMR, 2);
	corrupt_block(pcm, 0);
	rx_agc_process(&s, pcm, false);
	assert(energy(pcm) <= (uint64_t)1466 * 1466 * AUDIO_BLOCK_SAMPLES);
	rx_agc_configure(RX_AGC_DMR, 1); // lowest target: -36, with sustain headroom
	corrupt_block(pcm, 2);
	rx_agc_process(&s, pcm, false);
	assert(energy(pcm) <= (uint64_t)1038 * 1038 * AUDIO_BLOCK_SAMPLES);
	rx_agc_configure(RX_AGC_DMR, 10); // highest target: -12
	corrupt_block(pcm, 0);
	rx_agc_process(&s, pcm, false);
	assert(energy(pcm) <= (uint64_t)16460 * 16460 * AUDIO_BLOCK_SAMPLES);
	rx_agc_configure(RX_AGC_DMR, 0);
	corrupt_block(pcm, 5);
	memcpy(copy, pcm, sizeof pcm);
	rx_agc_process(&s, pcm, false);
	assert(memcmp(copy, pcm, sizeof pcm) == 0);
}

int main(void)
{
	test_startup_garble();
	test_target_sweep();
	test_corrupt_blocks();
	test_guard_target_changes();
	test_loud_after_quiet_start();
	test_stale_boost();
	test_syllable_average();
	test_average_response();
	rx_agc_t s;
	rx_agc_view_t v;
	int16_t pcm[AUDIO_BLOCK_SAMPLES], copy[AUDIO_BLOCK_SAMPLES];
	rx_agc_reset();
	assert(rx_agc_target(5) == -25 && rx_agc_target(255) == 0);
	rx_agc_begin(&s, RX_AGC_DMR, 1, 0);
	tone(pcm, 1000);
	memcpy(copy, pcm, sizeof pcm);
	rx_agc_process(&s, pcm, true);
	assert(memcmp(copy, pcm, sizeof pcm) == 0); // Off is bit-exact, even at full scale
	pcm[0] = -32768; pcm[1] = 32767;
	memcpy(copy, pcm, sizeof pcm);
	rx_agc_process(&s, pcm, true);
	assert(memcmp(copy, pcm, sizeof pcm) == 0);

	rx_agc_reset();
	rx_agc_configure(RX_AGC_DMR, 5);
	rx_agc_begin(&s, RX_AGC_DMR, 1, 0);
	settle(&s, 328, 15); // -40 dBFS to -25 dBFS within the acquisition window
	assert(s.gain_q8 >= 14 * 256 && s.gain_q8 <= 16 * 256);
	settle(&s, 328, 21);
	rx_agc_view(RX_AGC_DMR, &v);
	assert(v.enabled && v.valid && !v.acquiring);
	assert(v.input_dbfs == -40 && v.output_dbfs >= -26 && v.output_dbfs <= -24);
	assert(s.gain_q8 >= 14 * 256 && s.gain_q8 <= 16 * 256);
	int16_t gain = s.gain_q8;
	memset(pcm, 0, sizeof pcm);
	for (unsigned i = 0; i < 60; i++)
		rx_agc_process(&s, pcm, true);
	rx_agc_view(RX_AGC_DMR, &v);
	assert(s.gain_q8 == gain && !v.valid); // pauses never turn up the gain
	tone(pcm, 12000);
	rx_agc_process(&s, pcm, false);
	assert(s.gain_q8 == gain); // concealment does not teach the controller
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		assert(pcm[i] >= -29204 && pcm[i] <= 29204);

	rx_agc_end(&s);
	rx_agc_begin(&s, RX_AGC_DMR, 1, 0);
	assert(s.cached && s.gain_q8 == gain);
	rx_agc_begin(&s, RX_AGC_DMR, 99, 0);
	assert(!s.cached && s.gain_q8 == gain); // unknown uses the last stable input, per mode
	rx_agc_configure(RX_AGC_DMR, 8); // -18: history survives target changes
	rx_agc_begin(&s, RX_AGC_DMR, 1, 0);
	assert(s.cached && s.gain_q8 > gain);
	rx_agc_configure(RX_AGC_NXDN, 5);
	rx_agc_begin(&s, RX_AGC_NXDN, 1, 0);
	assert(!s.cached && s.gain_q8 == 0); // IDs never collide across modes
	rx_agc_begin(&s, RX_AGC_DMR, 1, 123);
	assert(!s.cached); // or known network scopes

	rx_agc_reset();
	rx_agc_configure(RX_AGC_DMR, 5);
	for (unsigned id = 1; id <= 10; id++) {
		rx_agc_begin(&s, RX_AGC_DMR, id, 0);
		settle(&s, 1000, 6);
	}
	rx_agc_begin(&s, RX_AGC_DMR, 1, 0);
	assert(s.cached);
	settle(&s, 1000, 6); // refresh the oldest caller
	rx_agc_begin(&s, RX_AGC_DMR, 11, 0);
	settle(&s, 1000, 6);
	rx_agc_begin(&s, RX_AGC_DMR, 2, 0);
	assert(!s.cached);
	rx_agc_begin(&s, RX_AGC_DMR, 1, 0);
	assert(s.cached);

	rx_agc_begin(&s, RX_AGC_DMR, 0, 0);
	settle(&s, 2000, 6);
	gain = s.gain_q8;
	rx_agc_identify(&s, 42, 7);
	assert(s.gain_q8 == gain);
	settle(&s, 2000, 6);
	rx_agc_begin(&s, RX_AGC_DMR, 42, 7);
	assert(s.cached);

	// Abrupt full-scale speech is bounded even with a large remembered boost; no signed overflow.
	rx_agc_begin(&s, RX_AGC_DMR, 90, 0);
	settle(&s, 66, 40);
	assert(s.gain_q8 <= 24 * 256);
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		pcm[i] = i % 2 ? 32767 : -32768;
	rx_agc_process(&s, pcm, true);
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		assert(pcm[i] >= -29204 && pcm[i] <= 29204);
	settle(&s, 30000, 3000); // sustained talker changes now move gently, about 1 dB/second
	rx_agc_view(RX_AGC_DMR, &v);
	assert(v.output_dbfs >= -26 && v.output_dbfs <= -24);
	// Enabling during a live bypassed call uses the input level, not a target-relative attenuation.
	rx_agc_configure(RX_AGC_DMR, 0);
	settle(&s, 328, 1200);
	rx_agc_configure(RX_AGC_DMR, 5);
	assert(s.input_q8 >= -41 * 256 && s.input_q8 <= -39 * 256);
	settle(&s, 328, 1);
	assert(s.gain_q8 >= 14 * 256 && s.gain_q8 <= 16 * 256);
	settle(&s, 328, 299); // the displayed output average catches up to the new gain
	rx_agc_view(RX_AGC_DMR, &v);
	assert(v.output_dbfs >= -26 && v.output_dbfs <= -24);
	// Bypass takes effect on the next block, including after a target change or malformed setting.
	rx_agc_configure(RX_AGC_DMR, 255);
	tone(pcm, 1000); memcpy(copy, pcm, sizeof pcm);
	rx_agc_process(&s, pcm, true);
	assert(memcmp(copy, pcm, sizeof pcm) == 0);
	puts("rxAgcTest: PASS");
	return 0;
}
