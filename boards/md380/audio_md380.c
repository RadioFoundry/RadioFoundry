// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 PCM audio: the shared I2S3 stream to the HR_C5000's built-in codec (platform/stm32f4/hrc/i2s_pcm.c,
// docs/spec/hardware-md380.md 6.2). The format is the MD-380's own (8 kHz, Philips, 16 bits in a 32-bit channel, clock
// idle low); which data line carries which direction is unconfirmed (bring-up stage 6). The HR_C5000's set-ups leave
// its I2S clocks off: the stream switches them with itself (md380_i2s_clock in radio_md380.c).
#include "md380.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/i2s_pcm.h"

// Bench knobs (docs/testing/md380-bringup.md): -DMD380_I2S_CKPOL=1 for the DM-1701's clock polarity,
// -DMD380_I2S_MASTER for an HR_C5000 that doesn't drive its I2S (issue 3)
#ifndef MD380_I2S_CKPOL
#define MD380_I2S_CKPOL	0
#endif
#ifdef MD380_I2S_MASTER
#define I2S_MASTER	true
#else
#define I2S_MASTER	false
#endif

static const stm32_i2s_pcm_cfg_t i2s_cfg = {
	.ckpol = MD380_I2S_CKPOL != 0,
	.master_if_free = I2S_MASTER,
	.clock = md380_i2s_clock,
};

// radio_md380.c owns the amplifier and the HR_C5000 audio path
static void speaker_pcm(bool on)
{
	md380_speaker_pcm(on);
}

static bool start(audio_block_cb_t cb, void *ctx)
{
	return stm32_i2s_pcm_start(&i2s_cfg, cb, ctx);
}

const hal_audio_t md380_audio = { .start = start, .stop = stm32_i2s_pcm_stop, .speaker_pcm = speaker_pcm };

#endif	// CONFIG_DMR
