// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 vocoder: no DMR voice codec on this radio yet (docs/spec/hardware-gd77.md 5.6). Until there is one,
// present() is false and profile() NULL: the radio core then treats the
// radio as having no codec (DMR voice is neither played nor sent; DMR data and FM work), and the debug link reports no
// codec profile. encode and decode never run without present(); if called they give silence.
#include <string.h>
#include "gd77.h"
#include "config.h"

#if CONFIG_DMR
static bool present(void)
{
	return false;	// no codec hosting on this board yet
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

const hal_vocoder_t gd77_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
};
#endif
