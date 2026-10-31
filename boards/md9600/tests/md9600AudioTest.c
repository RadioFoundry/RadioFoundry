// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 speaker routing (boards/md9600/speaker_md9600.h, docs/spec/hardware-md9600.md 6.1): every combination of
// the FM squelch, PCM, DMR mode and band
#include <stdio.h>
#include "speaker_md9600.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	for (int i = 0; i < 16; i++) {
		bool fm = i & 1, pcm = i & 2, dmr = i & 4, vhf = i & 8;
		md9600_speaker_t s = md9600_speaker_lines(fm, pcm, dmr, vhf);
		// The band line always follows the receiver
		CHECK(s.af_vhf == vhf);
		// The codec drives the line out exactly while PCM plays
		CHECK(s.codec_out == pcm);
		// FM audio only for an open squelch in FM with no PCM playing: never mixed with a beep or DMR voice
		CHECK(s.fm_pass == (fm && !pcm && !dmr));
		// The amplifier is on exactly when something plays
		CHECK(s.af_mute == !(s.fm_pass || pcm));
	}
	// The cases by name
	md9600_speaker_t s = md9600_speaker_lines(false, false, false, false);
	CHECK(s.af_mute && !s.fm_pass && !s.codec_out);			// idle: muted
	s = md9600_speaker_lines(true, false, false, true);
	CHECK(!s.af_mute && s.fm_pass && !s.codec_out && s.af_vhf);	// FM receive on VHF
	s = md9600_speaker_lines(true, true, false, false);
	CHECK(!s.af_mute && !s.fm_pass && s.codec_out);			// a beep over FM: FM held back
	s = md9600_speaker_lines(true, false, true, false);
	CHECK(s.af_mute && !s.fm_pass);					// DMR: the squelch alone plays nothing
	s = md9600_speaker_lines(false, true, true, false);
	CHECK(!s.af_mute && !s.fm_pass && s.codec_out);			// DMR voice

	printf("md9600AudioTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
