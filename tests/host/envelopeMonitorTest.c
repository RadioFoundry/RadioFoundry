// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Local envelope playback without a host; prompt priority, raw host audio and lifecycle.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "usbaudio/audio_mux.h"
#include "usbaudio/envelope_filter.h"

static int failures, starts, stops;
static bool speaker;
static audio_block_cb_t callback;
static int16_t input[AUDIO_BLOCK_SAMPLES], output[AUDIO_BLOCK_SAMPLES];
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static bool start(audio_block_cb_t cb, void *ctx) { (void)ctx; callback = cb; starts++; return true; }
static void stop(void) { callback = NULL; stops++; }
static void route(bool on) { speaker = on; }
static void prompt(const int16_t *in, int16_t *out, void *ctx)
{
	(void)in; (void)ctx;
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { out[i] = 777; }
}
static const hal_audio_t hw = { .start = start, .stop = stop, .speaker_pcm = route };

static double response(double hz, double amplitude)
{
	envelope_filter_t f = { 0 };
	double in_energy = 0, out_energy = 0;
	for (unsigned i = 0; i < 16000; i++) {
		int16_t x = (int16_t)lround(amplitude * sin(2 * 3.141592653589793 * hz * i / 8000));
		int16_t y = envelope_filter_sample(&f, x);
		if (i >= 8000) { in_energy += (double)x*x; out_energy += (double)y*y; }
	}
	return sqrt(out_energy / in_energy);
}

int main(void)
{
	// Preserve weak speech-band energy without a noise threshold; reject upper-band hiss and low rumble.
	double voice = response(1000, 4000), weak = response(1000, 20);
	CHECK(voice > 1.8 && voice < 2.1);
	CHECK(weak / voice > .94 && weak / voice < 1.06);
	CHECK(response(300, 1000) > 1.7 && response(2000, 1000) > 1.8);
	CHECK(response(3200, 1000) < .1 && response(3700, 1000) < .01);
	CHECK(response(50, 1000) < .15);
	audio_mux_reset();
	const hal_audio_t *audio = audio_mux_wrap(&hw);
	audio_mux_envelope_monitor(true);
	audio_mux_service(0);
	CHECK(starts == 1 && callback != NULL && speaker);
	// A steady envelope must settle to silence rather than send DC to the speaker.
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { input[i] = 12000; }
	for (unsigned n = 0; n < 20; n++) { callback(input, output, NULL); }
	CHECK(output[AUDIO_BLOCK_SAMPLES - 1] < 20 && output[AUDIO_BLOCK_SAMPLES - 1] > -20);
	static const int16_t tone[] = { 0, 7071, 10000, 7071, 0, -7071, -10000, -7071 };
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { input[i] = tone[i % 8]; }
	audio_mux_host_rx(AUDIO_MUX_RX_INPUT);
	callback(input, output, NULL);
	int peak = 0;
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		int v = output[i] < 0 ? -output[i] : output[i];
		if (v > peak) { peak = v; }
	}
	CHECK(peak > 5000 && peak < 32767);
	int16_t raw[AUDIO_BLOCK_SAMPLES];
	CHECK(audio_mux_host_read(raw, AUDIO_BLOCK_SAMPLES) == AUDIO_BLOCK_SAMPLES);
	CHECK(memcmp(raw, input, sizeof raw) == 0); // local filtering does not alter the host recording
	CHECK(audio->start(prompt, NULL));
	audio->speaker_pcm(true);
	callback(input, output, NULL);
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { CHECK(output[i] == 777); }
	audio->speaker_pcm(false);
	audio->stop();
	CHECK(speaker && stops == 0); // envelope playback survives the prompt ending
	callback(input, output, NULL);
	CHECK(output[1] != 777);
	audio_mux_host_rx(AUDIO_MUX_RX_OFF);
	audio_mux_service(20);
	CHECK(callback != NULL && speaker); // disconnecting the host must not stop local listening
	audio_mux_envelope_monitor(false);
	audio_mux_service(40);
	CHECK(!speaker && callback == NULL && stops == 1);
	printf("envelopeMonitorTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
