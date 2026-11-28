// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// YSF bench tool on the firmware's own code: the frame codec and call layer of core/ysf and the framer of core/air,
// built for the host. tools/bench/ysftool.py is its front end; this program writes JSON lines on stdout. Voice is
// opaque: the frames the call layer hands on, never speech.
//
//   ysftool decode IN.dibits [--unpacked]	symbols through the framer and the call layer
//   ysftool gen OUT.dibits CALL DGID FRAMES	a DN voice transmission as the transmit path builds it: CALL with
//						radio ID K7Q2Z, FRAMES voice frames (made-up patterns), then its end
//
// Dibit files: four symbols to a byte, the first in bits 7-6; 01 = +3, 00 = +1, 10 = -1, 11 = -3 (hal/hal_air.h).
// --unpacked: one symbol a byte, in its low two bits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_framer.h"
#include "air/air_syms.h"
#include "ysf/ysf.h"

static void die(const char *msg, const char *arg)
{
	fprintf(stderr, "ysftool: %s%s%s\n", msg, arg ? " " : "", arg ? arg : "");
	exit(2);
}

static uint8_t *read_syms(const char *path, int unpacked, size_t *nsyms)
{
	FILE *f = fopen(path, "rb");
	uint8_t *raw = NULL, *s;
	size_t cap = 0, n = 0, r;

	if (!f)
		die("can't open", path);
	do {
		if (n == cap) {
			cap = cap ? 2 * cap : 1 << 16;
			raw = realloc(raw, cap);
			if (!raw)
				die("out of memory", NULL);
		}
		r = fread(raw + n, 1, cap - n, f);
		n += r;
	} while (r);
	fclose(f);
	if (unpacked) {
		for (size_t i = 0; i < n; i++)
			raw[i] &= 3;
		*nsyms = n;
		return raw;
	}
	s = malloc(4 * n + 1);
	if (!s)
		die("out of memory", NULL);
	for (size_t i = 0; i < 4 * n; i++)
		s[i] = (uint8_t)((raw[i / 4] >> (6 - 2 * (i & 3))) & 3);
	free(raw);
	*nsyms = 4 * n;
	return s;
}

static ysf_call_rx_t g_call;
static air_framer_t g_fr;
static uint32_t g_voice_out;

static void call_json(const char *ev, const ysf_call_t *c)
{
	printf("{\"ev\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\",\"src_id\":\"%s\",\"dgid\":%u,\"dt\":%u,\"mr\":%u,"
	       "\"late\":%s}\n", ev, c->src, c->dst, c->src_id, c->dgid, c->dt, c->mr, c->late ? "true" : "false");
}

static void on_start(const ysf_call_t *c, void *ctx) { (void)ctx; call_json("call_start", c); }
static void on_update(const ysf_call_t *c, void *ctx) { (void)ctx; call_json("call_update", c); }
static void on_voice(const uint8_t v[YSF_VOICE_BYTES], void *ctx)
{
	(void)v;
	(void)ctx;
	g_voice_out++;
}
static void on_end(void *ctx) { (void)ctx; printf("{\"ev\":\"call_end\"}\n"); }

static const ysf_call_sink_t sink = { on_start, on_update, on_voice, on_end, NULL };

static int cmd_decode(const char *in, int unpacked)
{
	static uint8_t frame[YSF_FRAME_BYTES];
	static int8_t soft[YSF_FRAME_SYMS];
	uint8_t hard[AIR_CHUNK_SYMS_MAX / 4], z = 0;
	size_t n;
	uint8_t *s = read_syms(in, unpacked, &n);
	air_chunk_t c;

	ysf_call_rx_init(&g_call, &sink, NULL);
	air_framer_init(&g_fr, &ysf_air_mode, &g_call.rx, frame, soft, YSF_FRAME_SYMS);
	for (size_t at = 0; at < n; at += AIR_CHUNK_SYMS_MAX) {
		uint16_t k = (uint16_t)(n - at < AIR_CHUNK_SYMS_MAX ? n - at : AIR_CHUNK_SYMS_MAX);

		memset(&c, 0, sizeof c);
		for (uint16_t i = 0; i < k; i++)
			air_sym_put(hard, i, s[at + i]);
		c.t_us = (uint32_t)(((uint64_t)at * 1000000u) / ysf_air_mode.profile.sym_rate);
		c.nsyms = k;
		c.sync_at = -1;
		c.flags = at ? 0 : AIR_CHUNK_GAP;
		c.hard = hard;
		air_framer_rx(&g_fr, &c);
	}
	// The end of the input is the end of the signal
	memset(&c, 0, sizeof c);
	c.nsyms = 1;
	c.sync_at = -1;
	c.flags = AIR_CHUNK_GAP | AIR_CHUNK_NO_SIGNAL;
	c.hard = &z;
	air_framer_rx(&g_fr, &c);
	const ysf_call_stats_t *st = &g_call.stats;

	printf("{\"ev\":\"stats\",\"syms\":%zu,\"framer\":{\"syncs\":%u,\"frames\":%u},\"call\":{\"calls\":%u,"
	       "\"frames\":%u,\"fich_fail\":%u,\"csd_fail\":%u,\"dch_fail\":%u,\"voice\":%u,\"fec_bits\":%u,"
	       "\"vch_fixed\":%u,\"data_frames\":%u},\"voice_out\":%u}\n", n, g_fr.stats.syncs, g_fr.stats.frames,
	       st->calls, st->frames, st->fich_fail, st->csd_fail, st->dch_fail, st->voice, st->fec_bits, st->vch_fixed,
	       st->data_frames, g_voice_out);
	free(s);
	return 0;
}

static int cmd_gen(const char *out, const char *call, int dgid, int frames)
{
	static ysf_call_tx_t tx;
	ysf_tx_id_t id = { .radio_id = "K7Q2Z", .dgid = (uint8_t)dgid, .mr = YSF_MR_DIRECT };
	uint8_t v[YSF_VOICE_BYTES], silence[YSF_VOICE_BYTES] = { 0 }, byte = 0;
	uint16_t first;
	size_t total = 0;
	int made = 0;
	FILE *f = fopen(out, "wb");

	if (!f)
		die("can't write", out);
	snprintf(id.callsign, sizeof id.callsign, "%s", call);
	ysf_call_tx_begin(&tx, &id);
	while (!ysf_call_tx_done(&tx) && total < 4000000u) {
		uint16_t n = ysf_call_tx_ready(&tx, &first);

		if (!n) {
			if (made < frames) {
				for (unsigned i = 0; i < sizeof v; i++)
					v[i] = (uint8_t)(made * 37 + i * 11);
				if (!ysf_call_tx_voice(&tx, v))
					die("voice frame refused", NULL);
				made++;
			} else {
				ysf_call_tx_end(&tx, silence);
			}
			continue;
		}
		for (uint16_t i = 0; i < n; i++, total++) {
			byte = (uint8_t)(byte << 2 | air_sym_get(ysf_call_tx_syms(&tx), (uint16_t)(first + i)));
			if ((total & 3) == 3)
				fputc(byte, f);
		}
		ysf_call_tx_sent(&tx, n);
	}
	while (total & 3) {
		byte = (uint8_t)(byte << 2);
		if ((++total & 3) == 0)
			fputc(byte, f);
	}
	fclose(f);
	printf("{\"ev\":\"gen\",\"syms\":%zu,\"voice\":%d}\n", total, made);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 6 && !strcmp(argv[1], "gen"))
		return cmd_gen(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
	if (argc < 3 || strcmp(argv[1], "decode") != 0) {
		fprintf(stderr, "usage: ysftool decode IN [--unpacked] | gen OUT CALL DGID FRAMES  (tools/bench/ysftool.py)\n");
		return 2;
	}
	return cmd_decode(argv[2], argc > 3 && !strcmp(argv[3], "--unpacked"));
}
