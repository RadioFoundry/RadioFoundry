// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
// The real probe command on a made-up PCM tap: wire bounds, snapshots and callback/host storage independence.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { if (failures++ < 12) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if CONFIG_AUDIO_PROBE
static char trace[32];
static unsigned traced, sequence;
static bool stream_running, dmr_voice, p25_voice, inject_callback, inside_callback;
static audio_block_cb_t block_callback;
static void *block_ctx;
static int16_t *host_left, *host_right;

static void note(char c) { if (traced < sizeof trace) trace[traced++] = c; }
static void trace_is(const char *expected)
{
	CHECK(traced == strlen(expected) && !memcmp(trace, expected, traced));
	traced = 0;
}

bool ambeVoiceStreaming(void) { note('D'); return dmr_voice; }
bool p25VoiceStreaming(void) { note('P'); return p25_voice; }
uint32_t hal_millis(void) { return 0; }
uint32_t hal_micros(void) { return 0; }
void hal_delay_ms(uint32_t ms) { (void)ms; }
void dlog(const char *fmt, ...) { (void)fmt; }

static int16_t sample(unsigned i, bool right)
{
	if (i == 0) return right ? INT16_MAX : INT16_MIN;
	if (i == 1) return right ? 0 : -1;
	if (i == 2) return right ? -1 : 0;
	return (int16_t)((int)((i * (right ? 197u : 137u) + sequence) % 65536u) - 32768);
}

static bool last_block(int16_t *left, int16_t *right, uint32_t *blocks)
{
	note(inside_callback ? 'C' : 'L');
	CHECK(left != right && (uintptr_t)left % _Alignof(int16_t) == 0 && (uintptr_t)right % _Alignof(int16_t) == 0);
	if (!stream_running) return false;
	if (inside_callback) {
		CHECK(left != host_left && left != host_right && right != host_left && right != host_right);
	} else if (inject_callback) {
		int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, speaker[AUDIO_BLOCK_SAMPLES];
		host_left = left; host_right = right;
		inside_callback = true;
		block_callback(mic, speaker, block_ctx);
		inside_callback = false;
		host_left = host_right = NULL;
	}
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		left[i] = inside_callback ? -1234 : sample(i, false);
		right[i] = inside_callback ? 2345 : sample(i, true);
	}
	*blocks = 0xFEDCBA98u + sequence;
	return true;
}

static bool start(audio_block_cb_t callback, void *ctx)
{
	note('S'); block_callback = callback; block_ctx = ctx;
	return true;
}
static void stop(void) { note('X'); block_callback = NULL; }
static hal_audio_tap_t tap = { .last_block = last_block };
static const hal_audio_t audio = { .start = start, .stop = stop };
const board_t board = { .audio_tap = &tap, .audio = &audio };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

#define SAMPLES_REPLY (5u + 4u * AUDIO_BLOCK_SAMPLES)
#define ROOM (SAMPLES_REPLY + 17u)
static uint8_t storage[ROOM + 32u];

static uint8_t *fresh(void)
{
	memset(storage, 0xA5, sizeof storage);
	traced = 0;
	return storage + 9;	// the wire payload need not be aligned
}
static void untouched(size_t used)
{
	for (unsigned i = 0; i < 9; i++) CHECK(storage[i] == 0xA5);
	for (size_t i = 9 + used; i < sizeof storage; i++) CHECK(storage[i] == 0xA5);
}

static void samples(void)
{
	const uint8_t req[] = { 5 };
	stream_running = true;
	for (size_t cap = 1; cap < SAMPLES_REPLY; cap++) {
		uint8_t *out = fresh();
		CHECK(debuglink_cmd_audio_probe(req, sizeof req, out, cap) == 1 && out[0] == DL_ST_UNAVAILABLE);
		trace_is(""); untouched(1);
	}
	for (unsigned seed = 0; seed < 2; seed++) {
		sequence = seed * 0x10203u;
		for (size_t cap = SAMPLES_REPLY; cap <= ROOM; cap++) {
			uint8_t *out = fresh();
			CHECK(debuglink_cmd_audio_probe(req, sizeof req, out, cap) == SAMPLES_REPLY && out[0] == DL_ST_OK);
			trace_is("L"); CHECK(rd32(out + 1) == 0xFEDCBA98u + sequence);
			for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				CHECK(rd16(out + 5 + 2 * i) == (uint16_t)sample(i, false));
				CHECK(rd16(out + 5 + 2 * (AUDIO_BLOCK_SAMPLES + i)) == (uint16_t)sample(i, true));
			}
			untouched(SAMPLES_REPLY);
		}
	}
	stream_running = false;
	uint8_t *out = fresh();
	CHECK(debuglink_cmd_audio_probe(req, sizeof req, out, ROOM) == 1 && out[0] == DL_ST_UNAVAILABLE);
	trace_is("L"); untouched(1);
	tap.last_block = NULL; out = fresh();
	CHECK(debuglink_cmd_audio_probe(req, sizeof req, out, ROOM) == 1 && out[0] == DL_ST_UNAVAILABLE);
	trace_is(""); untouched(1); tap.last_block = last_block;
}

static void status(void)
{
	const uint8_t req[] = { 0 };
	sequence = 0;
	for (unsigned flags = 0; flags < 8; flags++) {
		stream_running = (flags & 1u) != 0;
		dmr_voice = (flags & 2u) != 0; p25_voice = (flags & 4u) != 0;
		uint8_t *out = fresh();
		CHECK(debuglink_cmd_audio_probe(req, sizeof req, out, 18) == 18 && out[0] == DL_ST_OK);
		trace_is("LDP");
		CHECK(out[1] == ((stream_running ? 2u : 0u) | (dmr_voice ? 8u : 0u) | (p25_voice ? 16u : 0u)));
		CHECK(rd32(out + 2) == (stream_running ? 0xFEDCBA98u : 0u));
		for (unsigned i = 6; i < 18; i++) CHECK(out[i] == 0);
		untouched(18);
	}
	tap.last_block = NULL;
	uint8_t *out = fresh();
	CHECK(debuglink_cmd_audio_probe(req, sizeof req, out, ROOM) == 18 && out[0] == DL_ST_OK);
	trace_is("DP"); CHECK(out[1] == 28u && rd32(out + 2) == 0); untouched(18);
	tap.last_block = last_block;
}

static void interleaved_callback(void)
{
	const uint8_t begin[] = { 3, 0, 0, 0, 0, 0 }, samples_req[] = { 5 }, status_req[] = { 0 }, end[] = { 4 };
	dmr_voice = p25_voice = false; stream_running = true; sequence = 0;
	uint8_t *out = fresh();
	CHECK(debuglink_cmd_audio_probe(begin, sizeof begin, out, ROOM) == 1 && out[0] == DL_ST_OK);
	trace_is("DPS"); CHECK(block_callback != NULL);
	inject_callback = true;
	out = fresh();
	CHECK(debuglink_cmd_audio_probe(samples_req, sizeof samples_req, out, ROOM) == SAMPLES_REPLY && out[0] == DL_ST_OK);
	trace_is("LC");
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		CHECK(rd16(out + 5 + 2 * i) == (uint16_t)sample(i, false));
		CHECK(rd16(out + 5 + 2 * (AUDIO_BLOCK_SAMPLES + i)) == (uint16_t)sample(i, true));
	}
	untouched(SAMPLES_REPLY);
	out = fresh();
	CHECK(debuglink_cmd_audio_probe(status_req, sizeof status_req, out, ROOM) == 18 && out[0] == DL_ST_OK);
	trace_is("LCDP"); CHECK(out[1] == 3u && rd32(out + 2) == 0xFEDCBA98u);
	CHECK(rd16(out + 6) == 1234 && rd16(out + 8) == 1234 && rd16(out + 10) == 1234);
	CHECK(rd16(out + 12) == 2345 && rd16(out + 14) == 2345 && rd16(out + 16) == 2345);
	untouched(18);
	inject_callback = false; out = fresh();
	CHECK(debuglink_cmd_audio_probe(end, sizeof end, out, ROOM) == 1 && out[0] == DL_ST_OK);
	trace_is("X");
}
#endif

int main(void)
{
#if CONFIG_AUDIO_PROBE
	samples(); status(); interleaved_callback();
#else
	uint8_t req[] = { 0 }, out[2] = { 0xA5, 0xA5 };
	CHECK(debuglink_cmd_audio_probe(req, sizeof req, out, 1) == 1 && out[0] == DL_ST_UNAVAILABLE && out[1] == 0xA5);
#endif
	printf("audioProbeTest: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
