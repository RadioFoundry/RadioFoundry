// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "audio/rx_agc.h"
#if CONFIG_RX_AGC
#include <stddef.h>
#include <string.h>
#include "hal_audio.h"

#define GAIN_LIMIT (24 * 256)
#define SPEECH_FLOOR (-55 * 256)
#define ACQUIRE_BLOCKS 15u
#define HOLD_BLOCKS 50u
#define AVERAGE_BLOCKS 75u // 1.5 seconds of active speech
#define GAIN_DEADBAND 128 // half a dB; preserve normal syllable variation
#define PEAK_LIMIT 29204u // -1 dBFS headroom
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

typedef struct {
	uint32_t source;
	uint16_t scope;
	int16_t input_q8;
	uint8_t mode;
} caller_t;
static caller_t callers[RX_AGC_CALLERS]; // most recently used first; audio task only
static int16_t fallback[RX_AGC_MODES];
static bool have_fallback[RX_AGC_MODES];
static uint8_t choices[RX_AGC_MODES];
static uint32_t views[RX_AGC_MODES];
static const int8_t targets[RX_AGC_TARGETS] = { 0, -36, -33, -30, -27, -25, -24, -21, -18, -15, -12 };
static const uint16_t scales[] = { 258, 290, 325, 365, 410, 460, 516, 579, 649, 728, 817, 917, 1029, 1154, 1295, 1453, 1631, 1830, 2053, 2303, 2584, 2900, 3254, 3651, 4096, 4596, 5157, 5786, 6492, 7284, 8173, 9170, 10289, 11544, 12953, 14533, 16306, 18296, 20529, 23034, 25844, 28997, 32536, 36506, 40960, 45958, 51566, 57858, 64917 };

int8_t rx_agc_target(uint8_t choice)
{
	return choice < RX_AGC_TARGETS ? targets[choice] : 0;
}

void rx_agc_configure(rx_agc_mode_t mode, uint8_t choice)
{
	if ((unsigned)mode < RX_AGC_MODES)
		STORE(choices[mode], choice < RX_AGC_TARGETS ? choice : 0);
}

static int16_t clamp_gain(int32_t v)
{
	return (int16_t)(v < -GAIN_LIMIT ? -GAIN_LIMIT : v > GAIN_LIMIT ? GAIN_LIMIT : v);
}

static uint16_t scale(int16_t gain)
{
	unsigned pos = (unsigned)(gain + GAIN_LIMIT), i = pos / 256u;
	if (i == 48u)
		return scales[i];
	return (uint16_t)(scales[i] + (uint32_t)(scales[i + 1] - scales[i]) * (pos % 256u) / 256u);
}

static uint32_t power(const int16_t *pcm, uint32_t *raw)
{
	int32_t mean = 0;
	uint64_t raw_energy = 0;
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mean += pcm[i];
		if (raw)
			raw_energy += (uint32_t)((int32_t)pcm[i] * pcm[i]);
	}
	if (raw)
		*raw = (uint32_t)((raw_energy + AUDIO_BLOCK_SAMPLES - 1u) / AUDIO_BLOCK_SAMPLES);
	mean /= AUDIO_BLOCK_SAMPLES;
	uint64_t energy = 0;
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		int32_t v = (int32_t)pcm[i] - mean;
		energy += (uint64_t)((int64_t)v * v);
	}
	return (uint32_t)(energy / AUDIO_BLOCK_SAMPLES);
}

// 10 log10(mean square / 32768^2), Q8. Binary logarithm avoids floating point in the audio callback.
static int16_t level(uint32_t power)
{
	if (power == 0)
		return RX_AGC_SILENT * 256;
	unsigned bit = 0;
	for (uint32_t v = power; v > 1; v >>= 1)
		bit++;
	uint32_t v = bit > 16 ? power >> (bit - 16) : power << (16 - bit);
	int32_t log = (int32_t)bit * 256;
	for (unsigned f = 128; f != 0; f >>= 1) {
		v = (uint32_t)(((uint64_t)v * v) >> 16);
		if (v >= 131072u) {
			v >>= 1;
			log += (int32_t)f;
		}
	}
	return (int16_t)((log - 30 * 256) * 771 / 256);
}

// Average energy before converting to dB, so quiet syllables do not bias the talker estimate downward.
static uint32_t average(uint32_t previous, uint32_t current, unsigned blocks)
{
	unsigned divisor = blocks < ACQUIRE_BLOCKS ? blocks + 1 : AVERAGE_BLOCKS;
	return (uint32_t)((int32_t)previous + ((int32_t)current - (int32_t)previous) / (int32_t)divisor);
}

// Round RMS up and the gain down: the guard must never underestimate a corrupt block's energy.
static uint32_t safety_ceiling(uint8_t choice, uint32_t raw_power, bool acquiring)
{
	static const uint16_t amplitudes[RX_AGC_TARGETS] = { 0, 519, 733, 1036, 1463, 1842, 2067, 2920, 4125, 5827, 8230 };
	if (raw_power == 0)
		return UINT16_MAX;
	uint32_t remainder = raw_power, root = 0, bit = 1u << 30;
	while (bit > remainder)
		bit >>= 2;
	while (bit != 0) {
		if (remainder >= root + bit) {
			remainder -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	if (root * root < raw_power)
		root++;
	uint32_t amplitude = amplitudes[choice];
	if (!acquiring)
		amplitude *= 2; // about 6 dB of headroom preserves normal syllable dynamics
	return amplitude * 4096u / root;
}

static int8_t whole(int16_t q8)
{
	return (int8_t)(q8 >= 0 ? (q8 + 128) / 256 : (q8 - 128) / 256);
}

static void publish(rx_agc_t *s)
{
	uint32_t flags = (s->choice != 0 ? 1u : 0u) | (s->measured && s->quiet_blocks < HOLD_BLOCKS ? 2u : 0u) |
		(s->speech_blocks < ACQUIRE_BLOCKS ? 4u : 0u) | (s->cached ? 8u : 0u);
	uint32_t packed = (uint8_t)whole(s->input_q8) | ((uint32_t)(uint8_t)whole(s->output_q8) << 8) |
		((uint32_t)(uint8_t)whole(s->measured ? s->output_q8 - s->input_q8 : s->gain_q8) << 16) | (flags << 24);
	STORE(views[s->mode], packed);
}

void rx_agc_view(rx_agc_mode_t mode, rx_agc_view_t *out)
{
	uint32_t v = (unsigned)mode < RX_AGC_MODES ? LOAD(views[mode]) : 0;
	out->input_dbfs = (int8_t)v;
	out->output_dbfs = (int8_t)(v >> 8);
	out->gain_db = (int8_t)(v >> 16);
	out->enabled = (v & (1u << 24)) != 0;
	out->valid = (v & (2u << 24)) != 0;
	out->acquiring = (v & (4u << 24)) != 0;
	out->cached = (v & (8u << 24)) != 0;
}

void rx_agc_begin(rx_agc_t *s, rx_agc_mode_t mode, uint32_t source, uint16_t scope)
{
	memset(s, 0, sizeof *s);
	s->mode = (uint8_t)mode;
	s->source = source;
	s->scope = scope;
	s->started = true;
	s->choice = LOAD(choices[mode]);
	int16_t input = fallback[mode];
	bool known = have_fallback[mode];
	for (unsigned i = 0; source != 0 && i < RX_AGC_CALLERS; i++) {
		if (callers[i].source == source && callers[i].scope == scope && callers[i].mode == mode) {
			input = callers[i].input_q8;
			known = s->cached = true;
			break;
		}
	}
	s->gain_q8 = s->choice && known ? clamp_gain(targets[s->choice] * 256 - input) : 0;
	s->scale_q12 = scale(s->gain_q8);
	s->input_q8 = s->output_q8 = RX_AGC_SILENT * 256;
	publish(s);
}

void rx_agc_identify(rx_agc_t *s, uint32_t source, uint16_t scope)
{
	// A late ID labels the running estimate; it never substitutes another caller's gain mid-word.
	if (source != 0) {
		s->source = source;
		s->scope = scope;
	}
}

static void remember(rx_agc_t *s, int16_t input)
{
	fallback[s->mode] = input;
	have_fallback[s->mode] = true;
	if (s->source == 0)
		return;
	unsigned at = RX_AGC_CALLERS - 1;
	for (unsigned i = 0; i < RX_AGC_CALLERS; i++) {
		if (callers[i].source == s->source && callers[i].scope == s->scope && callers[i].mode == s->mode) {
			at = i;
			break;
		}
	}
	memmove(callers + 1, callers, at * sizeof *callers);
	callers[0] = (caller_t){ .source = s->source, .scope = s->scope, .input_q8 = input, .mode = s->mode };
}

void rx_agc_process(rx_agc_t *s, int16_t *pcm, bool learn)
{
	uint8_t choice = LOAD(choices[s->mode]);
	if (choice != s->choice) {
		// The learned input level remains useful when the user changes targets.
		s->gain_q8 = choice && s->measured ? clamp_gain(targets[choice] * 256 - s->input_q8) : 0;
		s->choice = choice;
	}
	uint32_t raw_power;
	uint32_t input_power = power(pcm, &raw_power);
	bool acquiring = s->speech_blocks < ACQUIRE_BLOCKS;
	int16_t input = level(input_power);
	bool speech = learn && input >= SPEECH_FLOOR;
	if (speech) {
		s->input_power = average(s->input_power, input_power, s->speech_blocks);
		s->input_q8 = level(s->input_power);
		int16_t desired = clamp_gain(targets[choice] * 256 - s->input_q8);
		if (choice) {
			int32_t delta = desired - s->gain_q8;
			// Remove excessive gain promptly; only increases follow the gentle 1 dB/second rate.
			int32_t step = acquiring ? 3 * 256 : delta < 0 ? 256 : 5;
			if (!acquiring && delta >= -GAIN_DEADBAND && delta <= GAIN_DEADBAND)
				delta = 0;
			if (delta > step) delta = step;
			if (delta < -step) delta = -step;
			s->gain_q8 = clamp_gain(s->gain_q8 + delta);
		}
		s->quiet_blocks = 0;
		if (s->speech_blocks < UINT16_MAX)
			s->speech_blocks++;
		if (s->speech_blocks >= 5)
			remember(s, s->input_q8);
	} else if (s->quiet_blocks < HOLD_BLOCKS) {
		s->quiet_blocks++;
	}
	if (choice) {
		uint32_t peak = 0;
		for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			uint32_t a = pcm[i] < 0 ? (uint32_t)-(int32_t)pcm[i] : (uint32_t)pcm[i];
			if (a > peak) peak = a;
		}
		uint32_t ceiling = safety_ceiling(choice, raw_power, acquiring);
		uint32_t peak_ceiling = peak ? PEAK_LIMIT * 4096u / peak : UINT16_MAX;
		if (ceiling > peak_ceiling) ceiling = peak_ceiling;
		uint32_t to = scale(s->gain_q8), from = s->scale_q12;
		if (to > ceiling) to = ceiling;
		if (from > ceiling) from = ceiling;
		for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			int32_t g = (int32_t)from + ((int32_t)to - (int32_t)from) * (int32_t)(i + 1) / AUDIO_BLOCK_SAMPLES;
			pcm[i] = (int16_t)((int32_t)pcm[i] * g / 4096);
		}
		s->scale_q12 = (uint16_t)to;
	} else {
		s->gain_q8 = 0;
		s->scale_q12 = 4096;
	}
	if (speech) {
		s->output_power = average(s->output_power, power(pcm, NULL), s->speech_blocks - 1u);
		s->output_q8 = level(s->output_power);
		s->measured = true;
	}
	if (++s->display_blocks >= 12 || !s->measured) {
		s->display_blocks = 0;
		publish(s);
	}
}

void rx_agc_end(rx_agc_t *s)
{
	if (s->started) {
		s->started = false;
		s->measured = false;
		publish(s);
	}
}

void rx_agc_reset(void)
{
	memset(callers, 0, sizeof callers);
	memset(fallback, 0, sizeof fallback);
	memset(have_fallback, 0, sizeof have_fallback);
	memset(choices, 0, sizeof choices);
	memset(views, 0, sizeof views);
}
#endif
