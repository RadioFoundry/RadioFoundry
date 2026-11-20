// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The test voice set's full-rate frames (tests/data/voice/README.md): the speech file through the firmware's own encoder
// (core/mbe, as p25Voice.c runs it) to 11-byte frames, and those frames through the decoder to a WAV of what a radio
// plays. Both are fixed point, so the output is the same on every host.
//
//   imbeVoiceGen IN.pcm OUT.imbe OUT.wav	write the frames and the decoded speech (make -C tests/host voice-data)
//   imbeVoiceGen --check IN.pcm IN.imbe	encode again and compare with the committed frames (make -C tests/host)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/mbe/imbe.h"
#include "../../core/mbe/mbe_est.h"

#define MAX_FRAMES	1000

static int16_t pcm[MAX_FRAMES * MBE_FRAME_LEN];
static uint8_t bits[MAX_FRAMES][IMBE_BYTES];

static size_t read_pcm(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t b[2];
	size_t n = 0;

	if (!f) {
		fprintf(stderr, "imbeVoiceGen: can't read %s\n", path);
		exit(2);
	}
	// s16le, whatever the host's byte order
	while (n < sizeof pcm / sizeof pcm[0] && fread(b, 1, 2, f) == 2)
		pcm[n++] = (int16_t)(b[0] | b[1] << 8);
	if (!feof(f) && fgetc(f) != EOF) {
		fprintf(stderr, "imbeVoiceGen: %s holds more than %d frames\n", path, MAX_FRAMES);
		exit(2);
	}
	fclose(f);
	if (n == 0 || n % MBE_FRAME_LEN) {
		fprintf(stderr, "imbeVoiceGen: %s: %zu samples, not whole 20 ms frames\n", path, n);
		exit(2);
	}
	return n / MBE_FRAME_LEN;
}

static void encode(size_t frames)
{
	static imbe_state_t qs;
	static mbe_est_state_t est;
	static mbe_params_t p;
	static int32_t buf[MBE_BUF_WORDS];
	imbe_enc_mem_t m = { &qs, &est, &p, buf };

	imbe_enc_init(&m);
	for (size_t i = 0; i < frames; i++)
		imbe_enc_frame(&m, pcm + i * MBE_FRAME_LEN, bits[i]);
}

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void write_wav(const char *path, size_t frames)
{
	static imbe_dec_state_t ds;
	static mbe_params_t dp;
	static mbe_state_t ms;
	static mbe_frame_t prev, cur;
	static int32_t dbuf[MBE_BUF_WORDS];
	imbe_mem_t d = { &ds, &dp, { &ms, &prev, &cur, dbuf } };
	uint32_t data = (uint32_t)(frames * MBE_FRAME_LEN * 2);
	uint8_t h[45] = "RIFF....WAVEfmt \x10\0\0\0\1\0\1\0........\2\0\x10\0data....";
	FILE *f = fopen(path, "wb");

	if (!f) {
		fprintf(stderr, "imbeVoiceGen: can't write %s\n", path);
		exit(2);
	}
	put32(h + 4, 36 + data);
	put32(h + 24, 8000);
	put32(h + 28, 16000);
	put32(h + 40, data);
	fwrite(h, 1, 44, f);
	imbe_dec_init(&d);
	for (size_t i = 0; i < frames; i++) {
		int16_t out[MBE_FRAME_LEN];

		imbe_dec_frame(&d, bits[i], 0, out);
		for (int k = 0; k < MBE_FRAME_LEN; k++) {
			fputc(out[k] & 0xFF, f);
			fputc((out[k] >> 8) & 0xFF, f);
		}
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--check")) {
		size_t frames = read_pcm(argv[2]);
		uint8_t want[IMBE_BYTES];
		FILE *f = fopen(argv[3], "rb");
		size_t i = 0;

		encode(frames);
		if (!f) {
			printf("FAIL: can't read %s\n", argv[3]);
			return 1;
		}
		for (; i < frames && fread(want, 1, sizeof want, f) == sizeof want; i++)
			if (memcmp(want, bits[i], IMBE_BYTES)) {
				printf("FAIL: %s frame %zu differs from the encoder's: run make -C tests/host voice-data\n",
				       argv[3], i);
				return 1;
			}
		if (i != frames || fgetc(f) != EOF) {
			printf("FAIL: %s does not hold %zu frames: run make -C tests/host voice-data\n", argv[3], frames);
			return 1;
		}
		fclose(f);
		printf("# imbeVoiceGen: %s matches the encoder (%zu frames)\n", argv[3], frames);
		return 0;
	}
	if (argc != 4) {
		fprintf(stderr, "usage: imbeVoiceGen IN.pcm OUT.imbe OUT.wav | imbeVoiceGen --check IN.pcm IN.imbe\n");
		return 2;
	}
	size_t frames = read_pcm(argv[1]);
	FILE *f = fopen(argv[2], "wb");

	encode(frames);
	if (!f || fwrite(bits, IMBE_BYTES, frames, f) != frames) {
		fprintf(stderr, "imbeVoiceGen: can't write %s\n", argv[2]);
		return 2;
	}
	fclose(f);
	write_wav(argv[3], frames);
	printf("%s: %zu frames of %d bytes; %s: the frames decoded\n", argv[2], frames, IMBE_BYTES, argv[3]);
	return 0;
}
