// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV in the radio core (CONFIG_DSTAR; docs/spec/dstar-modem.md 5, docs/spec/dstar-codeplug.md): a channel of the
// D-STAR kind is received and sent through the board's flat FM paths (hal_audio_tap.h) by the engine of core/dstar, with
// the software voice codec (dstar_codec.h) in the board's audio stream. The radio core (radio_sm.c) tunes the channel
// as FM and hands over to this module for the squelch, the speaker and the transmission's content; it keeps PTT, the
// time-out and the keying.
//
// Threading: everything here runs in the radio task, except the audio block callback (the board's audio task: the
// codec) and the transmit pull (the board's interrupt: the modulator). Voice frames cross between them through single
// producer, single consumer rings; the engine is touched by the interrupt only between tx_start and the radio task
// seeing tx_done, and by the radio task only outside that.
//
// Memory: the codec's state and scratch are not static. Only one mode runs at a time, so they live in the RAM the
// board's DMR vocoder lends while DMR is off (hal_vocoder_t.lend); without a loan D-STAR runs with no voice (headers,
// messages and the silence frame still go both ways) and the view says why.
#ifndef RADIO_DSTAR_H
#define RADIO_DSTAR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "codeplug/codeplug.h"
#include "hal_audio.h"
#if CONFIG_DSTAR
#include "dstar/dstar_engine.h"
#include "dstar/dstar_codec.h"
#endif

#define RADIO_DSTAR_CALL	8u	// a callsign field as on air
#define RADIO_DSTAR_SUFFIX	4u
#define RADIO_DSTAR_MSG		20u
#define RADIO_DSTAR_PREAMBLE	128u	// bits of 1010 before the frame sync (26.7 ms): key-up of the RF chain and the
					// receiver's DC and clock loops; twice the standard's minimum (dstar-modem.md 3.3)
#define RADIO_DSTAR_VOICE_Q	16u	// voice frames each way (320 ms), a power of two
#define RADIO_DSTAR_DRAIN_MS	(RADIO_DSTAR_VOICE_Q * 20u + 1000u)	// after PTT release, the longest the queue and
					// the end pattern may take before tx_done says done anyway (the board's latency on top)
#define RADIO_DSTAR_PRIME	4u	// frames queued before receive playback starts: halves arrive every 32 ms and the
					// radio task drains them every 25 ms, so up to 57 ms can pass between deliveries

// What the next over sends as UR (radio_req_dstar_ur): the helpers only set UR (dstar-codeplug.md, reflector commands)
typedef enum {
	RDSTAR_UR_CHANNEL = 0,	// back to the channel's own UR (CQCQCQ unless the codeplug says otherwise)
	RDSTAR_UR_CQ,		// CQCQCQ for now (until the channel changes)
	RDSTAR_UR_REPLY,	// the last caller's callsign for now
	RDSTAR_UR_LINK,		// one over: link to the reflector and module given (7 characters, "REF030C")
	RDSTAR_UR_UNLINK,	// one over: unlink
	RDSTAR_UR_ECHO,		// one over: the gateway's echo test
	RDSTAR_UR_INFO,		// one over: the gateway's link information
} radio_dstar_ur_t;

// The receive side's state (radio_dstar_view_t.rx)
typedef enum {
	RDSTAR_RX_IDLE = 0,	// nothing heard
	RDSTAR_RX_HEADER,	// a header (or a late entry) and voice frames coming
} radio_dstar_rx_t;

// Why there is no voice (radio_dstar_view_t.voice_off)
typedef enum {
	RDSTAR_VOICE_OK = 0,
	RDSTAR_VOICE_NO_RAM,	// the vocoder lent no RAM (busy, or the board has none to lend)
	RDSTAR_VOICE_NO_STREAM,	// the board's audio stream didn't start
} radio_dstar_voice_t;

// Why D-STAR can't run on this channel (radio_dstar_view_t.fault)
typedef enum {
	RDSTAR_OK = 0,
	RDSTAR_NO_PATH,		// the board has no flat FM paths (AUDIO_TAP_FM_RX_RAW / AUDIO_TAP_FM_TX_FLAT)
	RDSTAR_RX_REFUSED,	// the board refused to start flat receive
	RDSTAR_NO_CAL,		// the board knows no deviation scale for its flat transmit path, and none is set
} radio_dstar_fault_t;

// Strings are NUL terminated with their trailing spaces removed
typedef struct {
	bool on;		// the channel is D-STAR and the engine runs
	uint8_t fault;		// radio_dstar_fault_t
	uint8_t voice_off;	// radio_dstar_voice_t
	uint8_t rx;		// radio_dstar_rx_t
	bool playing;		// received voice is playing now
	// The last transmission heard: its header (from the air or rebuilt from the slow data) and message
	bool heard;
	bool heard_slow;	// the header came from the slow data (a late entry)
	char my[RADIO_DSTAR_CALL + 1], suffix[RADIO_DSTAR_SUFFIX + 1];
	char ur[RADIO_DSTAR_CALL + 1], rpt1[RADIO_DSTAR_CALL + 1], rpt2[RADIO_DSTAR_CALL + 1];
	char msg[RADIO_DSTAR_MSG + 1];
	uint32_t heard_ms;	// hal_millis() at the header or the end of that transmission, 0 = none
	// What PTT sends
	char tx_my[RADIO_DSTAR_CALL + 1], tx_suffix[RADIO_DSTAR_SUFFIX + 1];
	char tx_ur[RADIO_DSTAR_CALL + 1], tx_rpt1[RADIO_DSTAR_CALL + 1], tx_rpt2[RADIO_DSTAR_CALL + 1];
	uint8_t ur_mode;	// radio_dstar_ur_t of tx_ur
	bool tx_repeater;	// flag 1 says via a repeater
	bool no_call;		// no MY callsign set anywhere: PTT is refused
} radio_dstar_view_t;

// Counters for the debug link (dstar.* keys)
typedef struct {
	uint32_t rx_frames;	// voice frames the deframer handed in
	uint32_t rx_played;	// decoded and played
	uint32_t rx_concealed;	// blocks played as the codec's concealment (repeat, comfort noise or mute) of a bad or
				// missing frame
	uint32_t rx_underruns;	// blocks with nothing queued while playing
	uint32_t rx_overflows;	// frames dropped, the queue full
	uint32_t rx_bad;	// frames the codec concealed by its bad-frame rule (C0 failed, or doubtful after a loss)
	uint32_t rx_bit_errors;	// bits the codec's FEC corrected
	uint32_t rx_samples;	// samples fed to the demodulator
	uint32_t rx_overs;	// transmissions heard (a header or a confirmed late entry)
	uint32_t tx_overs;
	uint32_t tx_frames;	// frames encoded
	uint32_t tx_underruns;	// frames the modulator wanted with none encoded: the silence frame went instead
	uint32_t tx_overflows;	// encoded frames dropped, the queue full
	uint32_t tx_samples;	// samples the board pulled
	uint32_t tx_peak;	// largest mic sample in the latest over
	uint32_t blocks;	// audio blocks the callback ran
	int8_t polarity;	// the receive polarity the deframer found last: +1, -1, 0 none yet
	uint16_t hdr_corrected;	// the last coded header's corrected bits
} radio_dstar_stats_t;

// A test signal the next transmission sends in place of D-STAR (the bench's transmit path tests,
// docs/testing/dstar-bench.md). The amplitude is the flat path's 1 kHz deviation (or level_pct of it)
typedef enum {
	RDSTAR_TEST_OFF = 0,
	RDSTAR_TEST_TONE,	// a sine at hz
	RDSTAR_TEST_SWEEP,	// a sine swept from 100 Hz to 3.6 kHz and back over 4 s
	RDSTAR_TEST_SQUARE,	// a square wave at hz (the high-pass and the droop it causes)
	RDSTAR_TEST_DOTS,	// D-STAR's preamble, 1010..., through the GMSK modulator only (the eye, the deviation)
	RDSTAR_TEST_SILENT,	// a full transmission whose voice frames are the standard's silence (no codec)
	RDSTAR_TEST_COUNT
} radio_dstar_test_t;

#if CONFIG_DSTAR
// What D-STAR keeps in the radio core's mode area while it runs (radio_mode_ram.h): its engine, and its software
// codec's state from frame to frame. The codec's working memory (dstar_codec_scratch_t, 5.3 KB) is the DMR vocoder's,
// lent while DMR voice is off (hal_vocoder_t.lend): the whole codec (8.4 KB) no longer fits that 7 KB loan
typedef struct {
	dstar_engine_t eng;
	dstar_codec_t codec;
} radio_dstar_area_t;

// Once, at start-up: the board's PCM audio (the stream the codec runs in; may be wrapped by the caller)
void radio_dstar_init(const hal_audio_t *audio);
// The channel is of the D-STAR kind (a live tag, the kind built)
bool radio_dstar_kind(const channel_t *ch);
// Tuned to a D-STAR channel (after hal_radio_t.rx()): its routing and the station loaded, flat receive on, the audio
// stream started. number 0: the VFO (no tag: direct, CQCQCQ). False when the board can't (view.fault says why);
// the channel is then silent and never keyed
bool radio_dstar_start(uint16_t number, const channel_t *ch, uint32_t now_ms);
// Leaving it (before the next rx() or set_mode()): receive off, the stream stopped, the RAM given back
void radio_dstar_stop(void);
bool radio_dstar_running(void);
// Every radio poll: received samples to the engine, the view kept. True when the view changed
bool radio_dstar_poll(uint32_t now_ms);
// A transmission is being received (the squelch's place: RS_RX while true)
bool radio_dstar_receiving(void);
// Received voice is playing (the green LED)
bool radio_dstar_playing(void);
// PTT would be refused for a reason of D-STAR's own: TXDENY_* or TXDENY_NONE
uint8_t radio_dstar_tx_check(void);
// Keyed (after hal_radio_t.tx() succeeded): the over starts (preamble, header, voice). False when the board refused
// the flat path; the caller unkeys
bool radio_dstar_tx_start(uint32_t now_ms);
// PTT released: the end pattern follows the voice still queued
void radio_dstar_tx_release(void);
// The last sample of the over is on air (the board's latency waited out): the caller unkeys, then radio_dstar_rx_again.
// Also true RADIO_DSTAR_DRAIN_MS after the release when the board stopped pulling, and whenever no over is on
bool radio_dstar_tx_done(uint32_t now_ms);
// Cut short (time-out, a refusal after keying): no more samples
void radio_dstar_tx_abort(void);
// Back to receive after the caller's tx_off() (the board's tx_off restores its normal FM receive)
void radio_dstar_rx_again(uint32_t now_ms);
// The audio stream is ours: beeps are mixed into it (beep_play's shared)
bool radio_dstar_streaming(void);
// UR for the next over(s); text: the reflector and module for RDSTAR_UR_LINK (7 characters, cut and padded)
void radio_dstar_ur(uint8_t mode, const char *text);
// The temporary UR dropped (BACK on Home, radio_req_clear_temporary)
void radio_dstar_clear_temporary(void);
void radio_dstar_view(radio_dstar_view_t *out);
void radio_dstar_stats(radio_dstar_stats_t *out);
void radio_dstar_reset_stats(void);
// The bench: the flat paths' calibration in force (the board's, with the settings over it), a test signal for the
// next transmissions (level 1-100 % of the 1 kHz deviation; hz for the tone and the square)
void radio_dstar_cal_reload(void);
void radio_dstar_test(uint8_t kind, uint16_t hz, uint8_t level_pct);
uint8_t radio_dstar_test_kind(void);
// The audio block callback (exposed for the host tests)
void radio_dstar_audio_block(const int16_t *mic, int16_t *spk, void *ctx);
#else
static inline bool radio_dstar_kind(const channel_t *ch) { (void)ch; return false; }
static inline bool radio_dstar_streaming(void) { return false; }
static inline bool radio_dstar_running(void) { return false; }
#endif

#endif
