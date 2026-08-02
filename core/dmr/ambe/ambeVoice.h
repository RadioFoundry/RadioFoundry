// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR voice between the call stack (core/dmr/call, 72-bit air frames) and the board's PCM audio and vocoder
 * (hal_audio_t, hal_vocoder_t, 49 parameter bits).
 *
 *  RX: the call stack's voice sink FEC-decodes each frame (ambeFec) into a small queue; the audio block callback takes
 *      one frame per 20 ms, decodes it with the vocoder and plays it. Playback starts once a burst's worth (3 frames)
 *      is queued, so the 60 ms burst rhythm doesn't underrun. Frames with an uncorrectable C0, lost frames and
 *      underruns repeat the last good frame up to 3 times (60 ms), then play silence.
 *  TX: while the voice source is active, the audio callback encodes each mic block and FEC-encodes it into a queue
 *      that the source's next() empties at the air rate.
 *
 * Threading: the sink, source and ambeVoiceStop() run in the call stack's task; the vocoder runs only in the audio
 * callback (the board's audio task). The queues are single producer / single consumer, and call boundaries are
 * counters each side only reads from the other, so no locks. hal_audio_t.speaker_pcm() is called from the audio
 * callback.
 *
 * Audio streams from the first begin until ambeVoiceStop() (leaving DMR); the speaker PCM route is on only while a
 * call plays.
 */
#ifndef ODR_AMBE_VOICE_H
#define ODR_AMBE_VOICE_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_audio.h"
#include "hal_vocoder.h"
#include "dmr/call/dmr_call.h"

#define AMBE_VOICE_QUEUE	8	// frames each way (160 ms), power of two
#define AMBE_VOICE_PRIME	3	// frames queued before RX playback starts
#define AMBE_VOICE_REPEAT	3	// bad / missing frames concealed by repeating the last good one, then silence

typedef struct {
	uint32_t rx_frames;	// frames decoded and played
	uint32_t rx_bad;	// frames with an uncorrectable C0
	uint32_t rx_lost;	// frames the call stack reported missing
	uint32_t rx_underruns;	// blocks with nothing queued during a call
	uint32_t rx_overflows;	// frames dropped, queue full
	uint32_t rx_bit_errors;	// bits corrected by the FEC
	uint32_t tx_frames;	// frames encoded
	uint32_t tx_overflows;	// encoded frames dropped, queue full (the stack didn't pull)
} ambe_voice_stats_t;

// audio and vocoder from the board; vocoder->present() false: frames are still queued and counted, but play silence
// and TX sends the vocoder's output (silence)
void ambeVoiceInit(const hal_audio_t *audio, const hal_vocoder_t *vocoder);
const dmr_voice_sink_t *ambeVoiceSink(void);
const dmr_voice_source_t *ambeVoiceSource(void);
// Stop the audio stream (leaving DMR mode); the next begin restarts it
void ambeVoiceStop(void);
void ambeVoiceGetStats(ambe_voice_stats_t *out);

// The audio block callback (exposed for tests): one 20 ms block in, one out
void ambeVoiceAudioBlock(const int16_t *mic, int16_t *spk, void *ctx);

#endif
