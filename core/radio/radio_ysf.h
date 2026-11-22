// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion in the radio core (CONFIG_YSF; docs/spec/ysf-modem.md 5, docs/spec/ysf-codeplug.md): a channel of
// the Fusion kind is received and sent through the board's flat FM paths (hal_audio_tap.h) by the engine of core/ysf,
// with the board's own AMBE+2 vocoder (hal_vocoder.h: Fusion's DN voice is the DMR codec's 49 bits) in the board's
// audio stream. The radio core (radio_sm.c) tunes the channel as FM and hands over to this module for the squelch,
// the speaker and the transmission's content; it keeps PTT, the time-out and the keying.
//
// The same shape as D-STAR's module (radio_dstar.h on feature/dstar), so the radio core runs either through the same
// hooks: only one of them is running at a time, and both take the flat paths and the audio stream whole.
//
// Threading: everything here runs in the radio task, except the audio block callback (the board's audio task: the
// vocoder) and the transmit pull (the board's interrupt: the modulator). Voice frames cross between them through single
// producer, single consumer rings; the engine is touched by the interrupt only between tx_start and the radio task
// seeing tx_done, and by the radio task only outside that.
#ifndef RADIO_YSF_H
#define RADIO_YSF_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#if CONFIG_YSF
#include "ysf/ysf_engine.h"
#include "radio/radio_ysf_vw.h"
#endif
#include "codeplug/codeplug.h"
#include "hal_audio.h"

#define RADIO_YSF_CALL		10u	// a callsign field as on air
#define RADIO_YSF_ID		5u	// a radio ID field (CSD3)
#define RADIO_YSF_VOICE_Q	16u	// voice frames received (320 ms), a power of two
// Voice frames to send, a power of two. The encoder runs from PTT, while the air carries no voice until the preamble
// (50 ms), both headers (200 ms) and, with VW, the CSD3 frame's three data subframes (60 ms) have gone: about 16
// frames are queued before the first one leaves, and the audio path's own lead adds to that. 16 slots held it until the
// 240-symbol preamble came in, after which every VW over dropped one frame of its first word (ysfVwRadioTest caught
// it); 32 leaves room for a flat route's chunked pulls on top. 13 more bytes a slot with VW, 7 with DN only
#define RADIO_YSF_TX_Q		32u
#define RADIO_YSF_DRAIN_MS	(RADIO_YSF_TX_Q * 20u + 1000u)	// after PTT release, the longest the queue, the
					// frame it ends in and the terminator may take before tx_done says done anyway
#define RADIO_YSF_PRIME		6u	// frames queued before receive playback starts: they arrive five in 75 ms then
					// none for 25 ms (the sync and FICH), and the flat ring is drained every 25 ms
#define RADIO_YSF_PREAMBLE	240u	// symbols of alternating -3 / +3 before the first header: 50 ms, what a Yaesu
					// radio sends between its key-up and its header (ysf-modem.md 3.5)
#define RADIO_YSF_HEADERS	2u	// header frames before the voice: the standard recommends one and a Yaesu radio
					// sends one; the second covers a flat route's unmeasured key-up (ysf-modem.md 3.5)

// The receive side's state (radio_ysf_view_t.rx)
typedef enum {
	RYSF_RX_IDLE = 0,	// nothing heard
	RYSF_RX_CALL,		// a transmission is being received
} radio_ysf_rx_t;

// Why Fusion can't run on this channel (radio_ysf_view_t.fault)
typedef enum {
	RYSF_OK = 0,
	RYSF_NO_PATH,		// the board has no flat FM paths (AUDIO_TAP_FM_RX_RAW / AUDIO_TAP_FM_TX_FLAT)
	RYSF_RX_REFUSED,	// the board refused to start flat receive
	RYSF_NO_CAL,		// the board knows no deviation scale for its flat transmit path
} radio_ysf_fault_t;

// The heard transmission's voice: what its FICH says (radio_ysf_view_t.mode)
typedef enum {
	RYSF_MODE_NONE = 0,
	RYSF_MODE_DN,		// V/D mode 2: played
	RYSF_MODE_VD1,		// V/D mode 1: played where the DMR FEC is built (its 72-bit frames)
	RYSF_MODE_VW,		// Voice FR: played where CONFIG_YSF_VW builds the full-rate codec, else shown
	RYSF_MODE_DATA,		// Data FR: shown, nothing to play
} radio_ysf_mode_t;

// Strings are NUL terminated with their trailing spaces removed
typedef struct {
	bool on;		// the channel is Fusion and the engine runs
	uint8_t fault;		// radio_ysf_fault_t
	bool codec;		// the vocoder is there: voice plays and PTT works
	uint8_t rx;		// radio_ysf_rx_t
	bool playing;		// received voice is playing now
	// The transmission heard last: its mode and callsigns (from its header, terminator or DCH)
	bool heard;
	uint8_t mode;		// radio_ysf_mode_t
	bool narrow;		// its FICH says narrow deviation
	uint8_t dgid;		// its FICH squelch code (DG-ID)
	bool muted;		// heard but not played: another DG-ID than the channel's
	char src[RADIO_YSF_CALL + 1], dest[RADIO_YSF_CALL + 1];
	char down[RADIO_YSF_CALL + 1], up[RADIO_YSF_CALL + 1];
	// The caller's radio ID (CSD3's Rem4), "" when not heard: who is calling when the callsigns are not, as on a VW
	// over joined after its header, whose first frame after the header has CSD3 and no other frame a callsign
	char src_id[RADIO_YSF_ID + 1];
	uint32_t heard_ms;	// hal_millis() at its last frame, 0 = none
	// What PTT sends
	char tx_src[RADIO_YSF_CALL + 1], tx_dest[RADIO_YSF_CALL + 1];
	uint8_t tx_dgid, rx_dgid;
	bool tx_narrow;
	bool tx_vw;		// PTT sends Voice FR (VW), not DN: the channel's flag (ysf-codeplug.md 2.1)
	bool no_call;		// no callsign anywhere: PTT is refused
} radio_ysf_view_t;

// Counters for the debug link (ysf.* keys) and the bench (docs/testing/ysf-bench.md)
typedef struct {
	uint32_t rx_samples;	// samples fed to the demodulator
	uint32_t rx_overs;	// transmissions heard
	uint32_t rx_frames;	// voice frames the receiver handed in (DN, VD1 and VW)
	uint32_t rx_played;	// decoded and played
	uint32_t rx_concealed;	// blocks played as silence for a missing frame
	uint32_t rx_overflows;	// frames dropped, the queue full
	uint32_t rx_muted;	// frames of another DG-ID, not played
	uint32_t rx_vw;		// Voice FR (VW) frames heard (their voice is in rx_frames where VW is built)
	uint32_t rx_late;	// blocks in an over with no frame queued yet (the receiver behind the speaker): in rx_concealed
	// The full-rate decoder's time per block, microseconds, frames and concealments alike: the latest VW over's
	// longest and mean and its blocks, and the longest since start. It runs in the audio callback, so a long one
	// holds up the board's speaker output (hal_audio_t.stream_stats: audio.cb_max_us, audio.cb_long)
	uint32_t vw_dec_max_us, vw_dec_avg_us, vw_dec_blocks, vw_dec_worst_us;
	uint32_t tx_overs;
	uint32_t tx_frames;	// frames encoded
	uint32_t tx_underruns;	// frames the transmitter wanted with none encoded: the silence frame went instead
	uint32_t tx_overflows;	// encoded frames dropped, the queue full
	uint32_t tx_samples;	// samples the board pulled
	// DN's mic in the latest over (radio/mic_gain.h): the largest sample the board delivered, the largest the encoder
	// got after the gain, and the lowest gain the limiter used, Q8 (the set gain when it never cut). Not VW's
	uint32_t tx_mic_peak, tx_enc_peak, tx_mic_low_q8;
	uint32_t blocks;	// audio blocks the callback ran
	// The receiver's (ysf_frame.h ysf_rx_t), kept across overs and brought up to date at each radio poll: what the
	// first radio tests settle
	uint32_t syncs, late, fich_good, fich_bad, csd_good, csd_bad;
	uint32_t order_all;	// DCH units whose CRC matched the whitening of data and CRC together (ysf_fec.h)
	uint32_t sync_bits, sync_errors;	// a raw bit error rate from the frame syncs of locked frames
	int32_t ppm, offset_hz;	// the demodulator's symbol clock and carrier offset as it has them
	bool rx_invert;		// the receiver found the levels inverted
} radio_ysf_stats_t;

// Where Voice FR's codec state (radio_ysf_vw_mem_t, about 4 KB) lives. On a platform whose DMR vocoder lends its
// state block (HAL_VOCODER_LEND_BYTES), that block, for one VW over at a time: radio_ysf.c takes the lend at an over's
// first VW frame and lets it go before the vocoder's next call, which rebuilds its own state then (hal_vocoder_t.lend).
// A Fusion channel runs one over at a time, DN on the vocoder or VW on the CPU, never both, so the two take turns with
// the bytes. Elsewhere it is in the mode area beside the engine. On the HR_C7000 this took 3.9 KB out of every build
// with VW: one radio's image that kept the IP gateway and added Fusion had 624 B of RAM left (PR #833)
#if CONFIG_YSF_VW && !defined(HAL_VOCODER_LEND_BYTES)
#define RADIO_YSF_VW_IN_AREA	1
#else
#define RADIO_YSF_VW_IN_AREA	0
#endif

#if CONFIG_YSF
// What Fusion keeps in the radio core's mode area while it runs (radio_mode_ram.h): its engine, and Voice FR's codec
// state where the vocoder lends none (above). D-STAR's engine and codec state use the same bytes while a D-STAR channel
// is tuned
typedef struct {
	ysf_engine_t eng;
#if RADIO_YSF_VW_IN_AREA
	radio_ysf_vw_mem_t vw;
#endif
} radio_ysf_area_t;
#endif

#if CONFIG_YSF
// Once, at start-up: the board's PCM audio (the stream the vocoder runs in)
void radio_ysf_init(const hal_audio_t *audio);
// The channel is of the Fusion kind (a live tag, the kind built)
bool radio_ysf_kind(const channel_t *ch);
// Tuned to a Fusion channel (after hal_radio_t.rx()): its fields and the station loaded, flat receive on, the audio
// stream started. False when the board can't (view.fault says why); the channel is then silent and never keyed
bool radio_ysf_start(uint16_t number, const channel_t *ch, uint32_t now_ms);
// Leaving it (before the next rx() or set_mode()): receive off, the stream stopped
void radio_ysf_stop(void);
bool radio_ysf_running(void);
// Every radio poll: received samples to the engine, the view kept. True when the view changed
bool radio_ysf_poll(uint32_t now_ms);
// A transmission is being received (the squelch's place: RS_RX while true)
bool radio_ysf_receiving(void);
bool radio_ysf_playing(void);
// PTT would be refused for a reason of Fusion's own: TXDENY_* or TXDENY_NONE
uint8_t radio_ysf_tx_check(void);
// Keyed (after hal_radio_t.tx() succeeded): the transmission starts (headers, voice). False when the board refused
// the flat path; the caller unkeys
bool radio_ysf_tx_start(uint32_t now_ms);
// PTT released: the voice still queued, the frame it ends in finished with silence, then the terminator
void radio_ysf_tx_release(void);
// The terminator is on air (the board's latency waited out): the caller unkeys, then radio_ysf_rx_again. Also true
// RADIO_YSF_DRAIN_MS after the release when the board stopped pulling, and whenever no transmission is on
bool radio_ysf_tx_done(uint32_t now_ms);
// Cut short (time-out, a refusal after keying, the mode stopped): no more samples, no transmission on
void radio_ysf_tx_abort(void);
// Back to receive after the caller's tx_off()
void radio_ysf_rx_again(uint32_t now_ms);
// The audio stream is ours: beeps are mixed into it
bool radio_ysf_streaming(void);
void radio_ysf_view(radio_ysf_view_t *out);
void radio_ysf_stats(radio_ysf_stats_t *out);
// The audio block callback (exposed for the host tests)
void radio_ysf_audio_block(const int16_t *mic, int16_t *spk, void *ctx);
#else
static inline bool radio_ysf_kind(const channel_t *ch) { (void)ch; return false; }
static inline bool radio_ysf_streaming(void) { return false; }
static inline bool radio_ysf_running(void) { return false; }
#endif

#endif
