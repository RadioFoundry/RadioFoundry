// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 PCM audio (I2S3 slave to the HR_C6000 codec, DMA): STUB, to be replaced
#include "dm1701.h"

static bool start(audio_block_cb_t cb, void *ctx) { (void)cb; (void)ctx; return false; }
static void stop(void) {}
static void speaker_pcm(bool on) { (void)on; }

const hal_audio_t dm1701_audio = { .start = start, .stop = stop, .speaker_pcm = speaker_pcm };
