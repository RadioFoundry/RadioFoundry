// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 vocoder: the stock V02.06 AMBE+2 codec run in place (docs/spec/codec-dm1701.md): STUB, to be replaced
#include "dm1701.h"

static bool present(void) { return false; }
static void reset(void) {}
static void encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS]) { (void)pcm; (void)bits; }
static void decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES]) { (void)bits; (void)pcm; }

const hal_vocoder_t dm1701_vocoder = { .present = present, .reset = reset, .encode = encode, .decode = decode };
