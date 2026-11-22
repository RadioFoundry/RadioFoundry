// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion in the radio core (radio_ysf.h)
#include <string.h>
#include "radio/radio_ysf.h"

#if CONFIG_YSF
#include "board.h"
#include "radio/radio.h"
#include "radio/beep.h"
#include "radio/radio_mode_ram.h"
#include "radio/mic_gain.h"
#include "audio/miclimit/miclimit.h"
#include "radio/radio_flat.h"
#include "codeplug/codeplug_modes.h"
#include "ysf/ysf_engine.h"
#include "ysf/ysf_codeplug.h"
#include "dmr/ambe/ambeFec.h"
#if CONFIG_YSF_VW
#include "hal_time.h"
#include "ysf/ysf_vw.h"
#include "radio/radio_ysf_vw.h"
#endif

#if RADIO_YSF_ID != YSF_RADIO_ID_LEN
#error "RADIO_YSF_ID is CSD3's radio ID field"
#endif
#if (RADIO_YSF_VOICE_Q & (RADIO_YSF_VOICE_Q - 1u)) != 0 || RADIO_YSF_VOICE_Q > 128u
#error "RADIO_YSF_VOICE_Q must be a power of two up to 128"
#endif
#if (RADIO_YSF_TX_Q & (RADIO_YSF_TX_Q - 1u)) != 0 || RADIO_YSF_TX_Q > 128u
#error "RADIO_YSF_TX_Q must be a power of two up to 128"
#endif

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define QMASK		(RADIO_YSF_VOICE_Q - 1u)
#define TXQMASK		(RADIO_YSF_TX_Q - 1u)
#define FRAME_BYTES	7u	// 49 codec bits, packed most significant first
// A queue slot holds a DN frame or, with VW built, a full-rate frame: its 11 bytes, the corrections its codes made
// (byte 11) and the kind (byte 12). An over is all one or the other, but the slot says which, so the audio callback
// never needs to know what the receiver saw when
#if CONFIG_YSF_VW
#define SLOT_BYTES	13u
#define SLOT_ERRS	11u
#define SLOT_KIND	12u
#define KIND_FR		1u
#else
#define SLOT_BYTES	FRAME_BYTES
#endif
#define RX_CHUNK	128u	// samples per read from the board, on the radio task's stack
#define FLAT_RATE_DEF	8000u

static const hal_audio_t *aud;
static const hal_audio_tap_t *tap;

// In the area the radio core lends the mode that runs (radio_mode_ram.h): taken at start, given back at stop, so NULL
// whenever Fusion isn't running. D-STAR's engine and codec state use the same bytes while a D-STAR channel is tuned
static radio_ysf_area_t *area;
static miclimit_t cb_limit;	// DN's mic gain and limiter (radio_mic_pcm_q8; audio callback)
static ysf_engine_t *eng;
static ysf_path_t path;
static bool running, streaming, rx_on;
static radio_ysf_view_t view;
static bool view_dirty;
static radio_ysf_stats_t stats;
// How much of the live receiver's counting stats already holds. The radio task adds the receiver's progress into
// stats at each poll (rx_count), so radio_ysf_stats copies stats alone, as D-STAR's does. It used to read the receiver
// itself, on the debug link's task: the radio task, above that task, could stop Fusion and let D-STAR take the shared
// engine area between its read of eng and its read of the counters, which then came from D-STAR's bytes. 36 bytes
// here against a lock on every read; the counters lag the receiver by at most one poll
static struct {
	uint32_t syncs, late, fich_good, fich_bad, csd_good, csd_bad, order_all, sync_bits, sync_errors;
} seen;

static ysf_chan_t chan;
static ysf_csd_t csd_tx;	// what PTT sends
static uint8_t cm_tx;		// and its call mode (YSF_CM_*)
static uint8_t heard_sc;	// the latest FICH's squelch code (the DG-ID), for the voice filter

// Voice frames. RX: producer the radio task (the receiver's callback), consumer the audio callback. TX: producer the
// audio callback (the encoder), consumer the board's interrupt (the transmitter's voice pull)
static uint8_t rxq[RADIO_YSF_VOICE_Q][SLOT_BYTES];
static uint8_t rx_head, rx_tail;
static uint8_t txq[RADIO_YSF_TX_Q][SLOT_BYTES];
static uint8_t tx_head, tx_tail;
// The codec's frame for no sound: encoded from a silent block by the audio callback at the first transmission, so it
// is this vocoder's own (49 zero bits are not silence to every AMBE+2 decoder). The audio callback can only run the
// vocoder once the over has started, so on the first over the transmit interrupt is already pulling while the frame
// is written, and a pull in between sent half of it over the zeros. silence_ready is stored after the last bit and
// read before the first: until then the interrupt sends zeros, which is what the frame held before
static uint8_t silence[YSF_VOICE_BITS];
static bool silence_ready;

// Overs. rx_seq bumps at each transmission the radio task sees begin and rx_end_seq takes its value at the end, so the
// audio callback sees an over's edges without a lock
static uint8_t rx_seq, rx_end_seq;
static volatile bool tx_on, tx_released, tx_done;
static uint32_t tx_done_ms;
static uint32_t tx_drain_ms;		// when the radio task first saw the release, 0 = not yet

static uint8_t cb_rx_seq;
static bool cb_playing, cb_primed, cb_speaker, cb_mixing, cb_tx;
static bool cb_fr;		// the over playing is VW: a missing frame is concealed by the full-rate decoder
#if CONFIG_YSF_VW
static uint32_t cb_dec_us;	// the full-rate decoder's time in this VW over so far (stats.vw_dec_*)
#endif

// The channel sends Voice FR (VW) rather than DN: its flag, where VW is built
static bool tx_vw(void)
{
#if CONFIG_YSF_VW
	return (chan.flags & YSF_CP_VW) != 0;
#else
	return false;
#endif
}

// --- small helpers ---

static void field_str(char *dst, const char *src, size_t n)
{
	size_t len = n;
	while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\0')) {
		len--;
	}
	for (size_t i = 0; i < len; i++) {
		char c = src[i];
		dst[i] = (c >= ' ' && c < 0x7F) ? c : ' ';	// a callsign from the air is shown, never trusted
	}
	dst[len] = '\0';
}

static void pack(const uint8_t bits[YSF_VOICE_BITS], uint8_t f[SLOT_BYTES])
{
	memset(f, 0, SLOT_BYTES);
	for (uint32_t i = 0; i < YSF_VOICE_BITS; i++) {
		if (bits[i]) {
			f[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
		}
	}
}

static void unpack(const uint8_t f[FRAME_BYTES], uint8_t bits[YSF_VOICE_BITS])
{
	for (uint32_t i = 0; i < YSF_VOICE_BITS; i++) {
		bits[i] = (uint8_t)((f[i >> 3] >> (7u - (i & 7u))) & 1u);
	}
}

static bool codec_ok(void)
{
	return board.vocoder != NULL && board.vocoder->present();
}

// --- calibration: the board's flat paths ---

static void cal_load(void)
{
	audio_tap_flat_cal_t c;
	radio_flat_cal(tap, &c);	// the board's, with the bench's settings over it: the same as D-STAR's
	memset(&path, 0, sizeof path);
	path.rate = tap != NULL && tap->fm_rx_flat_rate_hz != 0 ? tap->fm_rx_flat_rate_hz : FLAT_RATE_DEF;
	// A +1 symbol: 900 Hz wide, 450 narrow (table 3-1); the board's scale is per kHz
	int32_t amp = (int32_t)c.tx_1khz * ((chan.flags & YSF_CP_NARROW) ? 45 : 90) / 100;
	path.tx_amp = (int16_t)(amp > 7000 ? 7000 : amp < -7000 ? -7000 : amp);	// the shaped peak is 4.5 x it
	path.tx_invert = c.tx_invert;
	path.rx_invert = c.rx_invert;
	path.tx_hpf_hz = c.tx_hpf_hz;
	path.rx_hpf_hz = c.rx_hpf_hz;
}

static uint16_t tx_latency_ms(void)
{
	const audio_tap_flat_cal_t *c = tap != NULL && tap->flat_cal != NULL ? tap->flat_cal() : NULL;
	return c != NULL ? c->tx_latency_ms : 0;
}

// Our callsign: the codeplug's Fusion record, else the radio name
static void station_load(void)
{
	ysf_station_t st;
	memset(&st, ' ', sizeof st);
	ysf_station_load(&st);
	char name[CODEPLUG_NAME_LEN + 1];
	bool blank = true;
	for (unsigned i = 0; i < YSF_CALLSIGN_LEN; i++) {
		blank = blank && st.call[i] == ' ';
	}
	if (blank) {
		codeplug_radio_name(name);
		ysf_field_set(st.call, YSF_CALLSIGN_LEN, name);
	}
	ysf_csd_init(&csd_tx, NULL, NULL);
	memcpy(csd_tx.src, st.call, YSF_CALLSIGN_LEN);
	memcpy(csd_tx.dest, chan.dest, YSF_CALLSIGN_LEN);
	memcpy(csd_tx.rem[3], st.radio_id, YSF_RADIO_ID_LEN);	// Rem4: the source station's radio ID (table 4-2)
	// A call to everyone goes as a Yaesu radio sends it: radio ID mode, the destination field "*****" and our radio
	// ID (an FT-5D's DN overs, every frame: ysf-modem.md 1.6, 3.5), also with no radio ID set, when that half and
	// Rem4 are spaces. Every Yaesu radio has a radio ID, so none sends the group / CQ form to ALL, and a Yaesu radio
	// (an FTM-510DE) showed no caller for it although the source callsign was where it always is. A channel naming
	// a station or group calls it by name in group mode as before
	cm_tx = YSF_CM_GROUP;
	if (memcmp(chan.dest, YSF_CP_DEST_ALL, YSF_CALLSIGN_LEN) == 0) {
		ysf_csd_radio_id(&csd_tx, YSF_RADIO_ID_ALL);
		cm_tx = YSF_CM_RADIO_ID;
	}
	field_str(view.tx_src, csd_tx.src, YSF_CALLSIGN_LEN);
	field_str(view.tx_dest, chan.dest, YSF_CALLSIGN_LEN);
	view.no_call = view.tx_src[0] == '\0';
	view.tx_dgid = chan.tx_dgid;
	view.rx_dgid = chan.rx_dgid;
	view.tx_narrow = (chan.flags & YSF_CP_NARROW) != 0;
	view.tx_vw = tx_vw();
}

// --- the receiver's callbacks (radio task) ---

static uint8_t mode_of(uint8_t dt)
{
	switch (dt) {
	case YSF_DT_VD2: return RYSF_MODE_DN;
	case YSF_DT_VD1: return RYSF_MODE_VD1;
	case YSF_DT_VOICE_FR: return RYSF_MODE_VW;
	default: return RYSF_MODE_DATA;
	}
}

static void on_frame(void *ctx, const ysf_call_t *call, bool first)
{
	(void)ctx;
	if (first) {
		stats.rx_overs++;
		STORE(rx_seq, (uint8_t)(LOAD(rx_seq) + 1u));
		view.src[0] = view.dest[0] = view.down[0] = view.up[0] = view.src_id[0] = '\0';
	}
	heard_sc = call->fich.sc;
	uint8_t m = mode_of(call->fich.dt);
	bool muted = chan.rx_dgid != 0 && call->fich.sc != chan.rx_dgid;
	if (first || m != view.mode || muted != view.muted || call->fich.sc != view.dgid) {
		view_dirty = true;
	}
	view.rx = RYSF_RX_CALL;
	view.heard = true;
	view.mode = m;
	view.narrow = call->fich.dev != 0;
	view.dgid = call->fich.sc;
	view.muted = muted;
	if (m == RYSF_MODE_VW) {
		stats.rx_vw++;
	}
}

static void on_call(void *ctx, const ysf_call_t *call)
{
	(void)ctx;
	if (call->have & YSF_HAVE_SRC) {
		field_str(view.src, call->csd.src, YSF_CALLSIGN_LEN);
	}
	if (call->have & YSF_HAVE_DEST) {
		ysf_dest_show(call->fich.cm, call->csd.dest, view.dest);	// radio ID mode: "ALL", not "*****" and an ID
	}
	if (call->have & YSF_HAVE_DOWN) {
		field_str(view.down, call->csd.down, YSF_CALLSIGN_LEN);
	}
	if (call->have & YSF_HAVE_UP) {
		field_str(view.up, call->csd.up, YSF_CALLSIGN_LEN);
	}
	if (call->have & YSF_HAVE_REM34) {
		field_str(view.src_id, call->csd.rem[3], YSF_RADIO_ID_LEN);	// Rem4: the source's radio ID (table 4-2)
	}
	view_dirty = true;
}

// The next free receive slot, or NULL when the frame isn't played (another DG-ID, the queue full); the caller fills it
// and calls rx_commit
static uint8_t *rx_slot(void)
{
	stats.rx_frames++;
	if (chan.rx_dgid != 0 && heard_sc != chan.rx_dgid) {
		stats.rx_muted++;	// another group's transmission: shown, not played
		return NULL;
	}
	uint8_t h = rx_head;
	if ((uint8_t)(h - LOAD(rx_tail)) >= RADIO_YSF_VOICE_Q) {
		stats.rx_overflows++;
		return NULL;
	}
	return rxq[h & QMASK];
}

static void rx_commit(void)
{
	STORE(rx_head, (uint8_t)(rx_head + 1u));
}

static void rx_queue(const uint8_t bits[YSF_VOICE_BITS])
{
	uint8_t *f = rx_slot();
	if (f != NULL) {
		pack(bits, f);
		rx_commit();
	}
}

static void on_voice(void *ctx, const uint8_t bits[YSF_VOICE_BITS], uint8_t quality)
{
	(void)ctx;
	(void)quality;
	rx_queue(bits);
}

// V/D mode 1 sends the codec's 72-bit frame with DVSI's own FEC, the frame DMR carries: the DMR FEC takes it to the 49
// bits (whether its bit order is DMR's is for the first V/D 1 capture to confirm, ysf-modem.md 3.4)
static void on_voice72(void *ctx, const uint8_t frame[9])
{
	(void)ctx;
	uint8_t bits[AMBE_PARAM_BITS];
	ambe_fec_result_t r;
	ambeFecDecode(frame, bits, &r);
	if (!r.c0_failed) {
		rx_queue(bits);
	}
}

#if CONFIG_YSF_VW
// Voice FR: the full-rate frame and its corrections, for the audio callback's decoder
static void on_voice_fr(void *ctx, const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t errs)
{
	(void)ctx;
	uint8_t *f = rx_slot();
	if (f != NULL) {
		memcpy(f, imbe, YSF_VW_IMBE_BYTES);
		f[SLOT_ERRS] = errs;
		f[SLOT_KIND] = KIND_FR;
		rx_commit();
	}
}
#endif

static void on_end(void *ctx, ysf_end_t why)
{
	(void)ctx;
	(void)why;
	view.rx = RYSF_RX_IDLE;
	STORE(rx_end_seq, LOAD(rx_seq));
	view_dirty = true;
}

static void rx_count(void);
static void rx_fold(void);

static void engine_rx(void)
{
	rx_fold();
	static const ysf_rx_cb_t cb = { on_frame, on_call, on_voice, on_voice72, on_end, NULL };
	ysf_engine_rx_start(eng, &path, &cb);
#if CONFIG_YSF_VW
	ysf_engine_rx_voice_fr(eng, on_voice_fr);
#endif
}

// --- the audio stream (the board's audio task) ---

static void speaker(bool on)
{
	if (on != cb_speaker) {
		cb_speaker = on;
		if (aud != NULL && aud->speaker_pcm != NULL) {
			aud->speaker_pcm(on);
		}
	}
}

#if CONFIG_YSF_VW
// Voice FR's codec memory (radio_ysf.h RADIO_YSF_VW_IN_AREA): with a lending vocoder, its state block for as long as
// VW runs, else the mode area's, bound from the start. Audio callback only, as every codec call is; radio_ysf_stop
// clears it once the stream has stopped
static bool vw_lent;

// The memory bound for a VW frame: true when it was taken just now, so the caller starts its encoder or decoder afresh
// (whatever it held went with the vocoder's last call). False when it was bound already, or can't be: the calls then
// play silence and send the silence frame, as with no memory
static bool vw_take(void)
{
#if !RADIO_YSF_VW_IN_AREA
	if (vw_lent) {
		return false;
	}
	size_t n = 0;
	void *m = board.vocoder != NULL && board.vocoder->lend != NULL ? board.vocoder->lend(&n) : NULL;
	if (m == NULL || n < sizeof(radio_ysf_vw_mem_t)) {
		return false;
	}
	radio_ysf_vw_bind((radio_ysf_vw_mem_t *)m);
	vw_lent = true;
	return true;
#else
	return false;
#endif
}

// Before every call of the board's vocoder: a lent block goes back, and the vocoder rebuilds its state in it
static void vw_give(void)
{
	if (vw_lent) {
		radio_ysf_vw_bind(NULL);
		vw_lent = false;
	}
}
#else
static inline void vw_give(void) {}
#endif

static void tx_block(const int16_t *mic)
{
	if (!LOAD(tx_on) || LOAD(tx_released)) {
		cb_tx = false;
		return;
	}
	uint8_t bits[YSF_VOICE_BITS];
	bool first = !cb_tx;
#if CONFIG_YSF_VW
	if (tx_vw()) {
		// Voice FR: the full-rate encoder on the CPU, whatever the board's vocoder; its silence frame is a constant
		if (vw_take() || first) {
			cb_tx = true;
			radio_ysf_vw_tx_start();
		}
		uint8_t h = tx_head;
		const int16_t *pcm = mic;
		if ((uint8_t)(h - LOAD(tx_tail)) >= RADIO_YSF_TX_Q) {
			uint8_t drop[YSF_VW_IMBE_BYTES];
			radio_ysf_vw_encode(pcm, drop);		// keeps the encoder's history
			stats.tx_frames++;
			stats.tx_overflows++;
			return;
		}
		radio_ysf_vw_encode(pcm, txq[h & TXQMASK]);
		stats.tx_frames++;
		STORE(tx_head, (uint8_t)(h + 1u));
		return;
	}
#endif
	vw_give();
	if (!cb_tx) {
		cb_tx = true;
		miclimit_reset(&cb_limit, radio_mic_pcm_q8());
		stats.tx_mic_peak = stats.tx_enc_peak = 0;
		stats.tx_mic_low_q8 = cb_limit.low_q8;
		if (codec_ok()) {
			board.vocoder->reset();		// a fresh encoder: the first frame of the over
			if (!LOAD(silence_ready)) {
				static const int16_t quiet[AUDIO_BLOCK_SAMPLES];
				board.vocoder->encode(quiet, silence);
				STORE(silence_ready, true);	// the whole frame, then the flag: the pull reads them in turn
				board.vocoder->reset();
			}
		}
	}
	if (!codec_ok()) {
		return;		// the pull sends the silence frame
	}
	// The mic gain, as DMR's (radio/mic_gain.h): the board's quiet PCM lifted, a peak limiter so a shout doesn't clip.
	// Without it one radio's DN came out 13 dB under a Yaesu radio's at the far end (measured off air), which its
	// listeners heard as faint; DMR from the same mic had the same deficit. Skipped at unity, so a board at its own
	// level encodes the samples as delivered
	const int16_t *pcm = mic;
	uint16_t g = radio_mic_pcm_q8();
	if (g != MICLIMIT_UNITY) {
		static int16_t lifted[AUDIO_BLOCK_SAMPLES];	// the audio task only (off its stack)
		miclimit_block(&cb_limit, g, mic, lifted, AUDIO_BLOCK_SAMPLES);
		pcm = lifted;
		stats.tx_mic_low_q8 = cb_limit.low_q8;
	}
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		uint32_t a = (uint32_t)(mic[i] < 0 ? -mic[i] : mic[i]), e = (uint32_t)(pcm[i] < 0 ? -pcm[i] : pcm[i]);
		stats.tx_mic_peak = a > stats.tx_mic_peak ? a : stats.tx_mic_peak;
		stats.tx_enc_peak = e > stats.tx_enc_peak ? e : stats.tx_enc_peak;
	}
	board.vocoder->encode(pcm, bits);
	stats.tx_frames++;
	uint8_t h = tx_head;
	if ((uint8_t)(h - LOAD(tx_tail)) >= RADIO_YSF_TX_Q) {
		stats.tx_overflows++;
		return;
	}
	pack(bits, txq[h & TXQMASK]);
	STORE(tx_head, (uint8_t)(h + 1u));
}

#if CONFIG_YSF_VW
// One block of the full-rate decoder timed (stats.vw_dec_*): it runs here, in the audio callback, and a long block
// holds up the board's speaker output, the stutter a VW over had on the radio before the decoder was made faster
// (ysf-modem.md 4.3). The counters are the audio task's alone; the debug link reads them as they stand
static void vw_timed(uint32_t t0)
{
	uint32_t us = hal_micros() - t0;
	cb_dec_us += us;
	stats.vw_dec_blocks++;
	stats.vw_dec_avg_us = cb_dec_us / stats.vw_dec_blocks;
	if (us > stats.vw_dec_max_us) {
		stats.vw_dec_max_us = us;
	}
	if (us > stats.vw_dec_worst_us) {
		stats.vw_dec_worst_us = us;
	}
}
#endif

static void rx_block(int16_t *spk)
{
	uint8_t seq = LOAD(rx_seq);
	if (seq != cb_rx_seq) {
		cb_rx_seq = seq;		// a new over: the decoder starts afresh, playback waits for the prime
		cb_playing = true;
		cb_primed = false;
		cb_fr = false;
		if (codec_ok() && !cb_tx) {
			vw_give();
			board.vocoder->reset();
		}
	}
	uint8_t t = rx_tail, n = (uint8_t)(LOAD(rx_head) - t);
	bool ended = LOAD(rx_end_seq) == seq;
	if (cb_playing && !cb_primed && (n >= RADIO_YSF_PRIME || (ended && n > 0))) {
		cb_primed = true;
	}
	if (!cb_playing || !cb_primed || cb_tx) {
		memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
		if (cb_tx && n > 0) {
			STORE(rx_tail, (uint8_t)(t + n));	// our own over: nothing heard is played
		}
		return;
	}
	if (n == 0) {
		if (ended) {
			cb_playing = false;	// the over is done and played out
		} else {
			stats.rx_concealed++;
			stats.rx_late++;	// mid-over and nothing queued: the receiver hasn't handed the frame in yet
#if CONFIG_YSF_VW
			if (cb_fr) {
				uint32_t t0 = hal_micros();
				radio_ysf_vw_lost(spk);		// the last good frame repeated, then muted
				vw_timed(t0);
				return;
			}
#endif
		}
		memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
		return;
	}
#if CONFIG_YSF_VW
	const uint8_t *slot = rxq[t & QMASK];
	if (slot[SLOT_KIND] == KIND_FR) {
		bool fresh = vw_take();
		if (!cb_fr) {
			cb_fr = true;		// this over's first VW frame: its own decode times from here
			cb_dec_us = 0;
			stats.vw_dec_max_us = stats.vw_dec_avg_us = stats.vw_dec_blocks = 0;
			fresh = true;
		}
		if (fresh) {
			radio_ysf_vw_rx_start();	// a fresh decoder: also undoes what a transmission's encoder left in it
		}
		uint32_t t0 = hal_micros();
		int bad = radio_ysf_vw_decode(slot, slot[SLOT_ERRS], spk);
		vw_timed(t0);
		if (bad == 0) {
			stats.rx_played++;
		} else {
			stats.rx_concealed++;
		}
		STORE(rx_tail, (uint8_t)(t + 1u));
		return;
	}
#endif
	uint8_t bits[YSF_VOICE_BITS];
	unpack(rxq[t & QMASK], bits);
	if (codec_ok()) {
		vw_give();
		board.vocoder->decode(bits, spk);
		stats.rx_played++;
	} else {
		memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
	}
	STORE(rx_tail, (uint8_t)(t + 1u));
}

void radio_ysf_audio_block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	stats.blocks++;
	tx_block(mic);
	rx_block(spk);
	bool voice = cb_playing && cb_primed && !cb_tx;
	view.playing = voice;
	if (beep_mix(spk)) {
		cb_mixing = true;
		speaker(true);
	} else {
		cb_mixing = false;
		speaker(voice);
	}
}

static void stream_start(void)
{
	streaming = aud != NULL && aud->start != NULL && aud->start(radio_ysf_audio_block, NULL);
	cb_rx_seq = LOAD(rx_seq);
	cb_playing = cb_primed = cb_tx = cb_mixing = false;
}

static void stream_stop(void)
{
	if (streaming) {
		aud->stop();
		streaming = false;
	}
	if (cb_speaker || cb_mixing) {
		cb_speaker = cb_mixing = false;
		if (aud != NULL && aud->speaker_pcm != NULL) {
			aud->speaker_pcm(false);
		}
	}
	view.playing = false;
}

// --- transmit: the transmitter's sources (the board's interrupt) ---

// The next encoded frame; the codec's silence covers a late encoder. After PTT release the frames still queued go
// out (the last words of the over are in them), then the frame being sent is finished with silence and the terminator
// follows: the stop is asked from here, where the queue is seen empty
static bool voice_pull(void *ctx, uint8_t bits[YSF_VOICE_BITS])
{
	(void)ctx;
	uint8_t t = tx_tail;
	if (t != LOAD(tx_head)) {
		unpack(txq[t & TXQMASK], bits);
		STORE(tx_tail, (uint8_t)(t + 1u));
		return true;
	}
	if (tx_released) {
		ysf_engine_tx_stop(eng);
	} else {
		stats.tx_underruns++;
	}
	if (LOAD(silence_ready)) {
		memcpy(bits, silence, YSF_VOICE_BITS);
	} else {
		memset(bits, 0, YSF_VOICE_BITS);	// the audio callback may be part way through writing it
	}
	return true;
}

#if CONFIG_YSF_VW
// The same for Voice FR: the next full-rate frame, or the transmitter's silence frame (false)
static bool voice_pull_fr(void *ctx, uint8_t imbe[YSF_VW_IMBE_BYTES])
{
	(void)ctx;
	uint8_t t = tx_tail;
	if (t != LOAD(tx_head)) {
		memcpy(imbe, txq[t & TXQMASK], YSF_VW_IMBE_BYTES);
		STORE(tx_tail, (uint8_t)(t + 1u));
		return true;
	}
	if (tx_released) {
		ysf_engine_tx_stop(eng);
	} else {
		stats.tx_underruns++;
	}
	return false;
}
#endif

static void tx_pull(int16_t *pcm, uint16_t n)
{
	stats.tx_samples += n;
	if (tx_done) {
		memset(pcm, 0, n * sizeof(int16_t));
	} else if (!ysf_engine_tx(eng, pcm, n)) {
		// The block the terminator finished in is sent as the engine filled it, zeros after the end. Zeroing it as
		// well cut the terminator's last data unit, up to a board's whole transmit block, and the far end lost the
		// callsigns it carries
		tx_done = true;
	}
}

// --- the radio core's calls ---

void radio_ysf_init(const hal_audio_t *audio)
{
	aud = audio;
	tap = board.audio_tap;
}

bool radio_ysf_kind(const channel_t *ch)
{
	return ch->kind == CODEPLUG_KIND_YSF && codeplug_kind_built(ch->kind);
}

static bool path_ok(void)
{
	uint32_t need = AUDIO_TAP_FM_RX_RAW | AUDIO_TAP_FM_TX_FLAT;
	return tap != NULL && (tap->caps & need) == need && tap->fm_rx_flat_start != NULL &&
	       tap->fm_rx_flat_read != NULL && tap->fm_rx_flat_stop != NULL && tap->fm_tx_inject != NULL;
}

static void rx_flat_on(void)
{
	if (rx_on) {
		// Ours already: a tune at the end of an over (a retune that waited for it) starts the mode again before the
		// caller's rx_again. The board refuses a second start, and taking that as the answer stopped all reading
		return;
	}
	rx_on = path_ok() && tap->fm_rx_flat_start();
	if (!rx_on && view.fault == RYSF_OK) {
		view.fault = path_ok() ? RYSF_RX_REFUSED : RYSF_NO_PATH;
	}
}

static void rx_flat_off(void)
{
	if (rx_on) {
		tap->fm_rx_flat_stop();
		rx_on = false;
	}
}

static void rx_queue_reset(void)
{
	STORE(rx_tail, LOAD(rx_head));
	view.rx = RYSF_RX_IDLE;
	STORE(rx_end_seq, LOAD(rx_seq));
}

bool radio_ysf_start(uint16_t number, const channel_t *ch, uint32_t now_ms)
{
	(void)now_ms;
	if (running) {
		radio_ysf_stop();
	}
	if (number == 0 || !ysf_chan_load(number, ch, &chan)) {
		ysf_chan_decode(NULL, 0, &chan);	// the defaults: ALL, DG-ID 0, wide
	}
	view.on = true;
	view.fault = RYSF_OK;
	view.codec = codec_ok();
	tap = board.audio_tap;
	cal_load();
	station_load();
	running = true;
	if (!path_ok()) {
		view.fault = RYSF_NO_PATH;
		view_dirty = true;
		return false;
	}
	area = radio_mode_ram_take(RADIO_MODE_RAM_YSF, sizeof *area);
	eng = area != NULL ? &area->eng : NULL;
	if (eng == NULL) {
		// D-STAR still holds the area: the core starts a mode only after stopping the other, so this is a bug in
		// that order. Refused rather than taken, since D-STAR's transmit interrupt may still be pulling from it
		view.fault = RYSF_RX_REFUSED;
		view_dirty = true;
		return false;
	}
	if (path.tx_amp == 0) {
		view.fault = RYSF_NO_CAL;	// receive still runs: only transmit needs the scale
	}
#if RADIO_YSF_VW_IN_AREA
	radio_ysf_vw_bind(&area->vw);	// before the stream starts: the audio callback runs the codec
#endif
	rx_queue_reset();
	engine_rx();
	rx_flat_on();
	stream_start();
	view_dirty = true;
	return rx_on;
}

void radio_ysf_stop(void)
{
	if (!running) {
		return;
	}
	if (tx_on) {
		radio_ysf_tx_abort();
	}
	rx_flat_off();
	stream_stop();
	rx_fold();
	if (eng != NULL) {
		ysf_engine_idle(eng);
	}
#if CONFIG_YSF_VW
	radio_ysf_vw_bind(NULL);	// the stream has stopped: no callback runs the codec now
	vw_lent = false;		// a lent block is the vocoder's again (it rebuilds at its next call)
#endif
	radio_mode_ram_give(RADIO_MODE_RAM_YSF);	// after the engine is idle and the board's pull stopped (tx_abort)
	eng = NULL;
	area = NULL;
	running = false;
	view.on = false;
	view.rx = RYSF_RX_IDLE;
	view_dirty = true;
}

bool radio_ysf_running(void)
{
	return running;
}

bool radio_ysf_poll(uint32_t now_ms)
{
	if (running && rx_on && !tx_on) {
		int16_t buf[RX_CHUNK];
		uint16_t n;
		while ((n = tap->fm_rx_flat_read(buf, RX_CHUNK)) > 0) {
			stats.rx_samples += n;
			ysf_engine_rx(eng, buf, n);
		}
		rx_count();
		if (view.rx != RYSF_RX_IDLE) {
			view.heard_ms = now_ms != 0 ? now_ms : 1;
		}
	}
	bool was = view_dirty;
	view_dirty = false;
	return was;
}

bool radio_ysf_receiving(void)
{
	return running && view.rx != RYSF_RX_IDLE;
}

bool radio_ysf_playing(void)
{
	return running && view.playing;
}

uint8_t radio_ysf_tx_check(void)
{
	if (!running || view.fault == RYSF_NO_PATH || view.fault == RYSF_RX_REFUSED || view.fault == RYSF_NO_CAL) {
		return TXDENY_MODE;	// the board can't carry it: the same refusal as a mode not built
	}
	if (!codec_ok() && !tx_vw()) {
		return TXDENY_NO_CODEC;		// VW runs on the CPU's own codec
	}
	if (view.no_call) {
		return TXDENY_NO_CALL;
	}
	return TXDENY_NONE;
}

bool radio_ysf_tx_start(uint32_t now_ms)
{
	(void)now_ms;
	if (!running || eng == NULL) {
		return false;	// tx_check refuses these first; the engine area is not ours to write without the take
	}
	rx_flat_off();
	rx_fold();
	STORE(tx_tail, LOAD(tx_head));
	ysf_tx_config_t cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.fich.cm = cm_tx;
	cfg.fich.dev = (chan.flags & YSF_CP_NARROW) ? 1u : 0u;
	cfg.fich.mr = YSF_MR_DIRECT;
	cfg.fich.sql = 0;
	cfg.fich.sc = chan.tx_dgid;
	cfg.csd = csd_tx;
	cfg.preamble = RADIO_YSF_PREAMBLE;
	cfg.headers = RADIO_YSF_HEADERS;
	cfg.order = YSF_WHITEN_DATA;
	if (LOAD(silence_ready)) {
		memcpy(cfg.silence, silence, sizeof cfg.silence);	// else zeros (the memset), as the pull sends
	}
	cfg.pull = voice_pull;
#if CONFIG_YSF_VW
	if (tx_vw()) {
		cfg.vw = true;
		cfg.pull_fr = voice_pull_fr;
		memcpy(cfg.silence_fr, radio_ysf_vw_silence, YSF_VW_IMBE_BYTES);
	}
#endif
	tx_released = false;
	tx_done = false;
	tx_done_ms = tx_drain_ms = 0;
	if (!ysf_engine_tx_start(eng, &path, &cfg)) {
		return false;
	}
	tx_on = true;
	if (!tap->fm_tx_inject(tx_pull, AUDIO_TAP_FLAT)) {
		tx_on = false;
		return false;
	}
	stats.tx_overs++;
	return true;
}

void radio_ysf_tx_release(void)
{
	if (tx_on) {
		tx_released = true;	// the pull stops the transmitter once the queue is played out
	}
}

bool radio_ysf_tx_done(uint32_t now_ms)
{
	// Nothing on: done, as D-STAR answers. A mode restarted under the core's RS_TX (a tune in mid-transmission) has
	// nothing on; false here would leave the core keyed with nothing but the time-out to end it
	if (!tx_on) {
		return true;
	}
	if (!tx_done) {
		if (!tx_released) {
			return false;
		}
		// Only the board's interrupt ends the transmission, so a route that stops asking for halves would hold the
		// carrier after PTT release until the time-out. Past the longest the queue and the terminator can take, it
		// is over: the pull stopped here, and the caller unkeys (radio_dstar.c does the same)
		if (tx_drain_ms == 0) {
			tx_drain_ms = now_ms != 0 ? now_ms : 1;
		}
		if ((uint32_t)(now_ms - tx_drain_ms) < RADIO_YSF_DRAIN_MS + tx_latency_ms()) {
			return false;
		}
		radio_ysf_tx_abort();
		return true;
	}
	if (tx_done_ms == 0) {
		tx_done_ms = now_ms != 0 ? now_ms : 1;
	}
	return (uint32_t)(now_ms - tx_done_ms) >= tx_latency_ms();
}

void radio_ysf_tx_abort(void)
{
	tx_released = true;
	tx_done = true;
	if (tx_on && tap != NULL) {
		tap->fm_tx_inject(NULL, AUDIO_TAP_FLAT);
	}
	// Cleared with the pull stopped, as D-STAR's abort does. Left set, a stop in mid-transmission (radio_ysf_stop
	// aborts) carried it into the next start, whose poll then read nothing from the receiver until the next over
	tx_on = false;
}

void radio_ysf_rx_again(uint32_t now_ms)
{
	(void)now_ms;
	if (tx_on && tap != NULL) {
		tap->fm_tx_inject(NULL, AUDIO_TAP_FLAT);
	}
	tx_on = false;
	if (!running || eng == NULL) {
		return;		// not started, or started without a path or the area: nothing to receive with
	}
	rx_queue_reset();
	engine_rx();
	rx_flat_on();
	view_dirty = true;
}

bool radio_ysf_streaming(void)
{
	return streaming;
}

void radio_ysf_view(radio_ysf_view_t *out)
{
	*out = view;
	out->codec = codec_ok();
}

void radio_ysf_stats(radio_ysf_stats_t *out)
{
	*out = stats;	// never eng: the area may be D-STAR's by the time this task reads it (seen)
}

// The radio task: what the live receiver counted since the last call into the totals, and its readings as they are
static void rx_count(void)
{
	const ysf_rx_t *r = eng != NULL ? ysf_engine_receiver(eng) : NULL;
	if (r == NULL) {
		return;
	}
#define COUNT(total, rx)	do { stats.total += r->rx - seen.total; seen.total = r->rx; } while (0)
	COUNT(syncs, syncs);
	COUNT(late, late_syncs);
	COUNT(fich_good, fich_good);
	COUNT(fich_bad, fich_bad);
	COUNT(csd_good, csd_good);
	COUNT(csd_bad, csd_bad);
	COUNT(order_all, order_all);
	COUNT(sync_bits, sync_bits);
	COUNT(sync_errors, sync_errors);
#undef COUNT
	stats.rx_invert = r->invert;
	stats.ppm = ysf_c4fm_demod_ppm(&eng->u.rx.dm);
	stats.offset_hz = ysf_c4fm_demod_offset_hz(&eng->u.rx.dm);
}

// The receiver restarts at every over we send and every retune: its counters go into the totals first, the next one
// counts from zero, and its readings are gone with it (zero while nothing receives, as they always read)
static void rx_fold(void)
{
	rx_count();
	memset(&seen, 0, sizeof seen);
	stats.rx_invert = false;
	stats.ppm = stats.offset_hz = 0;
}

#endif
