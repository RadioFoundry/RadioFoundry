// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Shares the board's one PCM stream (hal_audio_t) between the radio's own users (DMR voice, beeps) and a host audio
 * session (USRP over the network, docs/spec/usb-audio.md 5.1). The radio's users get a wrapped hal_audio_t
 * (audio_mux_wrap) and keep their start / stop / callback logic: the mux keeps the board's stream running while
 * either side wants it and calls their callback from its own. The host side gets:
 *
 *   - receive audio: in FM the stream's input on boards with AUDIO_TAP_FM_RX (silence on the others: their stream's
 *     input is the microphone), in DMR its output (the decoded voice), in a ring the host task reads;
 *   - transmit audio: a jitter buffer (pcm_slip) the host task fills (audio_mux_host_push). In DMR, while the host
 *     transmits, its samples replace the microphone's on the way to the encoder; in FM the board pulls them through
 *     hal_audio_tap_t.fm_tx_inject (audio_mux_fm_pull).
 *
 * APRS (CONFIG_APRS_RX) has a receive ring of its own: the stream's input while it asks for it, on boards with
 * AUDIO_TAP_FM_RX, for the AFSK demodulator in the radio task (docs/spec/aprs.md 5.3).
 *
 * Threads: audio_mux_service and the wrapped start / stop in the radio task (the only places that start or stop the
 * board's stream); the block callback in the board's audio task; audio_mux_host_* in the host link task;
 * audio_mux_fm_pull in the board's PWM interrupt. Each ring has one producer and one consumer.
 */
#ifndef RFOUNDRY_AUDIO_MUX_H
#define RFOUNDRY_AUDIO_MUX_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_audio.h"

#define AUDIO_MUX_RX_CAP	1024u	// receive ring, samples (128 ms), power of two
#define AUDIO_MUX_TX_TARGET	480u	// jitter buffer target, samples (60 ms)
#define AUDIO_MUX_RETRY_MS	1000u	// a stream that wouldn't start (no codec clock) is tried again this often

typedef struct {
	uint32_t blocks;	// stream blocks seen
	uint32_t rx_samples;	// samples put in the receive ring
	uint32_t rx_overflow;	// receive samples dropped: the host task didn't read them in time
	uint32_t tx_pushes;	// host transmit pushes (the radio task's watchdog sees them move)
	uint32_t tx_blocks;	// DMR blocks whose microphone was replaced by host audio
	uint32_t fm_pulls;	// samples handed to the FM transmit path
	uint32_t starts;	// times the mux started the board's stream for the host
	uint32_t start_fails;	// ... and it didn't start
} audio_mux_stats_t;

// The board's stream (NULL: no PCM audio; the mux then does nothing). Returns the stream the radio's users take
const hal_audio_t *audio_mux_wrap(const hal_audio_t *hw);

// Host side (host link task)
// Where a host session's receive audio comes from
typedef enum {
	AUDIO_MUX_RX_OFF,	// no session: the stream isn't kept running for the host
	AUDIO_MUX_RX_SILENT,	// a session, but this mode's receive audio isn't on the stream (FM on a board without
				// AUDIO_TAP_FM_RX, where the stream's input is the microphone): silence
	AUDIO_MUX_RX_INPUT,	// the stream's input (FM receive audio on boards with AUDIO_TAP_FM_RX)
	AUDIO_MUX_RX_OUTPUT,	// the stream's output (decoded DMR, what the speaker gets)
} audio_mux_rx_t;
void audio_mux_host_rx(audio_mux_rx_t src);
// Receive samples waiting; read takes up to n of them, returns how many
uint32_t audio_mux_host_avail(void);
uint32_t audio_mux_host_read(int16_t *pcm, uint32_t n);
// Transmit audio from the host into the jitter buffer; whatever doesn't fit is dropped
void audio_mux_host_push(const int16_t *pcm, uint32_t n);
// The host transmits (true) or stopped (false): resets the jitter buffer at the start; dmr: replace the microphone in
// the stream's blocks while it lasts
void audio_mux_host_tx(bool on, bool dmr);
uint32_t audio_mux_host_tx_fill(void);
// Host transmit pushes so far: the radio task unkeys a host transmission whose audio stopped coming
uint32_t audio_mux_host_pushes(void);
#define AUDIO_MUX_HOST_STALE_MS	1000u

// APRS receive audio (radio task): on keeps the stream running and fills a ring of its own with the stream's input
// (FM receive audio on boards with AUDIO_TAP_FM_RX); read takes up to n samples, returns how many
#define AUDIO_MUX_APRS_CAP	1024u	// samples (128 ms), power of two
void audio_mux_aprs_rx(bool on);
uint32_t audio_mux_aprs_read(int16_t *pcm, uint32_t n);

// Radio task: start or stop the board's stream as the host side needs (every poll)
void audio_mux_service(uint32_t now_ms);

// FM transmit audio for hal_audio_tap_t.fm_tx_inject (the board's interrupt)
void audio_mux_fm_pull(int16_t *pcm, uint16_t n);

void audio_mux_stats(audio_mux_stats_t *out);
// The jitter buffer's counters (pcm_slip_stats_t), for the debug link
#include "usbaudio/pcm_slip.h"
void audio_mux_slip_stats(pcm_slip_stats_t *out);

// Back to the state after start-up (tests)
void audio_mux_reset(void);

#endif
