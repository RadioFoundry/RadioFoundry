// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// AMBE+2 vocoder as a platform service. The vocoder itself is the radio's own stock-firmware codec, merged on the
// user's machine and never shipped (README, docs/spec/codec-*.md). Frames here are the 49 voice parameter bits; the
// AMBE+2 3600x2450 FEC and interleaving to and from the 72-bit air frame are done in core/dmr.
#ifndef HAL_VOCODER_H
#define HAL_VOCODER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VOCODER_FRAME_SAMPLES	160	// 20 ms at 8 kHz
#define VOCODER_BITS		49

// Encode options (set_options). Each is a codec feature a board may or may not have; options() says which
#define VOCODER_OPT_NOISE_REDUCTION	0x01u	// suppress steady background noise in the mic signal
#define VOCODER_OPT_TONE_FRAMES		0x02u	// send a steady tone or tone pair (DTMF) as a tone frame, not as speech
#define VOCODER_OPT_AGC			0x04u	// input level control: quiet speech raised, loud speech lowered
#define VOCODER_OPT_DEFAULT		(VOCODER_OPT_NOISE_REDUCTION | VOCODER_OPT_TONE_FRAMES)

typedef struct {
	// True when the codec is present and matches the board's profile (signature / hash). False: FM only
	bool (*present)(void);
	void (*reset)(void);	// fresh encoder and decoder state (start of a call)
	// 49 bits, one per byte (0/1), MSB of the parameter frame first
	void (*encode)(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS]);
	void (*decode)(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES]);
	// Optional (may be NULL): the id of the codec profile the board recognized in flash, e.g. "dm1701-v0206-csv", or
	// NULL when that check failed. It stays valid while a board has stopped its codec (present() false after the HR_C7000
	// stub entry parks it): it describes flash, not whether voice can run now. Reported by the debug link's VERSION
	// (codec=) so the flashing tool knows whether the codec in flash can be kept (docs/dev/release.md, tools/odrflash).
	// Read only: it never changes flash
	const char *(*profile)(void);
	// Optional (both NULL without encode options): the VOCODER_OPT_* bits this codec takes, and the options for every
	// encode from now on (bits it doesn't take are ignored). Kept across reset(); VOCODER_OPT_DEFAULT until set. Same
	// task as encode(), so one frame is always encoded with one set of options
	uint32_t (*options)(void);
	void (*set_options)(uint32_t opts);
	// Optional: the codec's own word for the options in force, for the debug link only (its meaning is per codec,
	// docs/spec/codec-*.md)
	uint32_t (*encode_flags)(void);
} hal_vocoder_t;

// The codec in flash as VERSION reports it (codec=, docs/spec/debuglink.md 3.1): the profile the board recognized,
// "absent" when the board has a vocoder but its check failed, "none" without one (FM-only build). The profile comes
// first: a codec the board has stopped for a stub upload is still in flash, and a flasher asking then must not see it
// absent. Read only
static inline const char *hal_vocoder_id(const hal_vocoder_t *v)
{
	if (v == NULL) {
		return "none";
	}
	const char *id = v->profile != NULL ? v->profile() : NULL;
	if (id != NULL) {
		return id;
	}
	return v->present() ? "present" : "absent";
}

#endif
