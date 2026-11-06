// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 speaker routing as pin levels (docs/spec/hardware-md9600.md 6.1). Two sources reach the volume control and
// the amplifier: FM audio from the band's IF chip (U_V_AF_SW picks the band, FM_MUTE high lets it through) and the
// HR_C6000's line out (DMR voice, beeps, prompts), which is always connected. The amplifier mute is AF_SW. PCM wins
// over FM: while it plays FM is held back, so a beep or a DMR call is never mixed with the discriminator's noise. Warm
// keeps the amplifier on with neither source let through (it takes about 60 ms to sound after an unmute). Pure, so
// the host test (boards/md9600/tests/md9600AudioTest.c) checks every case.
#ifndef SPEAKER_MD9600_H
#define SPEAKER_MD9600_H

#include <stdbool.h>

typedef struct {
	bool af_mute;		// PB6 AF_SW: high = amplifier muted
	bool fm_pass;		// PE7 FM_MUTE: high = FM audio to the volume control
	bool af_vhf;		// PC13 U_V_AF_SW: high = the VHF IF chip's audio
	bool codec_out;		// the HR_C6000 codec DAC to its line out
} md9600_speaker_t;

// After the FM squelch closes the receiver's noise still reaches the amplifier for about 150 ms with FM_MUTE low (a
// microphone at the speaker, UHF, the SDR bench, 2026-11-06: a hiss 38 dB over the floor fading out). The original
// firmware mutes the amplifier when the squelch closes; warm waits this long before it powers the amplifier again
#define MD9600_FM_HUSH_MS	200u

// fm: the core's FM squelch is open. pcm: PCM is playing. dmr: the radio is in DMR, where the IF chip's audio is never
// wanted. vhf: the receiver is on the VHF band. warm: the amplifier held on for a sound that may follow. hush: the
// FM squelch closed less than MD9600_FM_HUSH_MS ago (warm doesn't count then; PCM still plays)
static inline md9600_speaker_t md9600_speaker_lines(bool fm, bool pcm, bool dmr, bool vhf, bool warm, bool hush)
{
	md9600_speaker_t s;
	bool fm_on = fm && !dmr && !pcm;
	s.af_mute = !(fm_on || pcm || (warm && !hush));
	s.fm_pass = fm_on;
	s.af_vhf = vhf;
	s.codec_out = pcm;
	return s;
}

#endif
