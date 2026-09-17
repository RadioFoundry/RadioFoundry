// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 vocoder: not hosted yet. The AMBE+2 codec is inside the stock TYT firmware (D002.032 and later), but its
// code and table windows, entry points and state sizes have not been located (docs/spec/hardware-md380.md 14 says how
// to find them and what the profile needs). Flash sectors 7-11 are kept free for it (md380.ld). Until a profile exists,
// present() is false and the radio is FM only: the core then refuses DMR voice.
#include "md380.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include <string.h>

static bool present(void)
{
	return false;
}

static void reset(void)
{
}

static void encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	(void)pcm;
	memset(bits, 0, VOCODER_BITS);
}

static void decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
	(void)bits;
	memset(pcm, 0, VOCODER_FRAME_SAMPLES * sizeof(int16_t));
}

static const char *profile(void)
{
	return NULL;
}

const hal_vocoder_t md380_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
};

#endif	// CONFIG_DMR
