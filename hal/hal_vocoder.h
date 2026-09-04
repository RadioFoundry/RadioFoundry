// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// AMBE+2 vocoder as a platform service. The vocoder itself is the radio's own stock-firmware codec, merged on the
// user's machine and never shipped (README, docs/spec/codec-*.md). Frames here are the 49 voice parameter bits; the
// AMBE+2 3600x2450 FEC and interleaving to and from the 72-bit air frame are done in core/dmr.
#ifndef HAL_VOCODER_H
#define HAL_VOCODER_H

#include <stdint.h>
#include <stdbool.h>

#define VOCODER_FRAME_SAMPLES	160	// 20 ms at 8 kHz
#define VOCODER_BITS		49

typedef struct {
	// True when the codec is present and matches the board's profile (signature / hash). False: FM only
	bool (*present)(void);
	void (*reset)(void);	// fresh encoder and decoder state (start of a call)
	// 49 bits, one per byte (0/1), MSB of the parameter frame first
	void (*encode)(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS]);
	void (*decode)(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES]);
	// Optional (may be NULL): the id of the codec profile present() recognized, e.g. "dm1701-v0206-csv", or NULL when
	// present() is false. Reported by the debug link's VERSION (codec=) so the flashing tool knows whether the codec
	// in flash can be kept (docs/dev/release.md, tools/odrflash). Read only: it never changes flash
	const char *(*profile)(void);
} hal_vocoder_t;

#endif
