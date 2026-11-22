// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV in the radio core (radio_dstar.h)
#include <string.h>
#include "radio/radio_dstar.h"

#if CONFIG_DSTAR
#include "board.h"
#include "radio/radio.h"
#include "radio/beep.h"
#include "radio/radio_mode_ram.h"
#include "radio/radio_flat.h"
#include "settings/settings.h"
#include "codeplug/codeplug_modes.h"
#include "dstar/dstar_engine.h"
#include "dstar/dstar_codec.h"
#include "dstar/dstar_codeplug.h"

#if (RADIO_DSTAR_VOICE_Q & (RADIO_DSTAR_VOICE_Q - 1u)) != 0 || RADIO_DSTAR_VOICE_Q > 128u
#error "RADIO_DSTAR_VOICE_Q must be a power of two up to 128"
#endif

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define QMASK		(RADIO_DSTAR_VOICE_Q - 1u)
#define RX_CHUNK	128u	// samples per read from the board, on the radio task's stack
#define FLAT_RATE_DEF	8000u

static const hal_audio_t *aud;
static const hal_audio_tap_t *tap;

// In the area the radio core lends the mode that runs (radio_mode_ram.h): taken at start, given back at stop, so NULL
// whenever D-STAR isn't running. Another mode's engine uses the same bytes while its channel is tuned
static radio_dstar_area_t *area;
static dstar_engine_t *eng;
static dstar_path_t path;
static dstar_codec_t *codec;		// area->codec while the vocoder lends the working memory; NULL: no voice
static bool running, streaming, rx_on;
static radio_dstar_view_t view;
static char view_msg_tx[RADIO_DSTAR_MSG];	// our slow data message, as on air
static bool view_dirty;
static radio_dstar_stats_t stats;
// The counters at the debug link's last reset, which radio_dstar_stats subtracts (dmr_call_reset_stats keeps its bases
// the same way). stats has three writers (the radio task, the audio task and the transmit interrupt), all above the
// debug link's task, so a reset that zeroed stats itself could be preempted part way: an over's played frame survived
// while the block it played in was zeroed, and an increment between a counter's bytes kept half of the old count. The
// reset now writes only here, which nothing else writes; 68 bytes, against a request each writer would have to serve
// (a mode not running serves none)
static radio_dstar_stats_t stats_base;

// The routing: the channel's (dstar_codeplug.h), and the one-over and for-now UR choices over it
static dstar_chan_t chan;
static char ur_temp[RADIO_DSTAR_CALL];	// for now (CQ, reply); [0] == 0: none
static char ur_once[RADIO_DSTAR_CALL];	// the next over only (reflector commands); [0] == 0: none
static uint8_t ur_temp_mode, ur_once_mode;
static uint16_t cur_number = 0xFFFFu;	// the channel they were chosen on (0: the VFO)

// Voice frames. RX: producer the radio task (the deframer's callback), consumer the audio callback. TX: producer the
// audio callback (the encoder), consumer the board's interrupt (the modulator's voice pull)
static uint8_t rxq[RADIO_DSTAR_VOICE_Q][DSTAR_VOICE_BYTES];
static uint8_t rx_head, rx_tail;
static uint8_t txq[RADIO_DSTAR_VOICE_Q][DSTAR_VOICE_BYTES];
static uint8_t tx_head, tx_tail;

// Overs. rx_seq bumps at each header the radio task delivers and rx_end_seq takes its value at the end, so the audio
// callback sees an over's edges without a lock; tx_on and tx_released are the radio task's, read by the others
static uint8_t rx_seq, rx_end_seq;
static volatile bool tx_on, tx_released, tx_done;
static uint32_t tx_done_ms;		// when the radio task first saw tx_done, 0 = not yet
static uint32_t tx_drain_ms;		// when the radio task first saw the release, 0 = not yet

// The audio callback's own state
static uint8_t cb_rx_seq;
static bool cb_playing, cb_primed, cb_speaker, cb_mixing, cb_tx;

// The bench's test signal (radio_dstar_test): set by another task between transmissions
static uint8_t test_kind;
static uint16_t test_hz;
static uint8_t test_level = 100;
// What the interrupt reads, copied by radio_dstar_tx_start before the pull is hooked and left alone until the next
// over. The interrupt read test_kind and path live before: radio_dstar_test's "never mid-over" was a check-then-write
// on another task (the radio task could key in between, and a switch from TONE to DOTS sent the interrupt into a
// modulator the tone path never set up), and radio_dstar_cal_reload, which runs in RS_TX too, zeroes path before
// refilling it (a sample in that window divided by path.rate = 0 in step_of)
static uint8_t tx_kind;
static uint32_t tx_rate;
static int32_t tx_amp_1khz;
static uint32_t test_phase, test_step;	// a 2^32 turn per cycle
static int32_t test_sweep_dir;
static uint32_t test_n;

// --- small helpers ---

static bool field_blank(const char *f, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (f[i] != ' ' && f[i] != '\0') {
			return false;
		}
	}
	return true;
}

static void dirty(void)
{
	view_dirty = true;
}

// --- calibration ---

// The board's flat paths with the bench's settings over them (settings_t flat_*: 0 = the board's)
void radio_dstar_cal_reload(void)
{
	audio_tap_flat_cal_t c;
	radio_flat_cal(tap, &c);
	memset(&path, 0, sizeof path);
	path.rate = tap != NULL && tap->fm_rx_flat_rate_hz != 0 ? tap->fm_rx_flat_rate_hz : FLAT_RATE_DEF;
	// +-1200 Hz for a long run of one value (dstar-modem.md 2.1); the board's scale is per kHz
	int32_t amp = (int32_t)c.tx_1khz * 6 / 5;
	path.tx_amp = (int16_t)(amp > 32000 ? 32000 : amp < -32000 ? -32000 : amp);
	path.tx_invert = c.tx_invert;
	path.rx_invert = c.rx_invert;
	path.tx_hpf_hz = c.tx_hpf_hz;
	path.rx_hpf_hz = c.rx_hpf_hz;
	if (running) {
		view.fault = path.tx_amp == 0 ? RDSTAR_NO_CAL : view.fault == RDSTAR_NO_CAL ? RDSTAR_OK : view.fault;
		dirty();
	}
}

static uint16_t tx_latency_ms(void)
{
	const audio_tap_flat_cal_t *c = tap != NULL && tap->flat_cal != NULL ? tap->flat_cal() : NULL;
	return c != NULL ? c->tx_latency_ms : 0;
}

// --- the header we send ---

// MY: set on the radio (Settings), else the codeplug's D-STAR record, else the radio's callsign
static void station_load(void)
{
	dstar_station_t st;
	memset(&st, ' ', sizeof st);
	dstar_station_load(&st);
	settings_t *s = settings_begin();
	char my[RADIO_DSTAR_CALL], sfx[RADIO_DSTAR_SUFFIX], msg[RADIO_DSTAR_MSG];
	memcpy(my, s->dstar_my, sizeof my);
	memcpy(sfx, s->dstar_suffix, sizeof sfx);
	memcpy(msg, s->dstar_msg, sizeof msg);
	settings_end(0);
	if (!field_blank(my, sizeof my)) {
		memcpy(st.my, my, sizeof my);
		memcpy(st.suffix, sfx, sizeof sfx);	// the suffix belongs to the callsign it was set with
	}
	if (!field_blank(msg, sizeof msg)) {
		memcpy(st.msg, msg, sizeof msg);
	}
	if (field_blank(st.my, sizeof st.my)) {
		char name[CODEPLUG_NAME_LEN + 1];
		codeplug_radio_name(name);
		dstar_field_set(st.my, sizeof st.my, name);
	}
	dstar_field_text(view.tx_my, st.my, sizeof st.my);
	dstar_field_text(view.tx_suffix, st.suffix, sizeof st.suffix);
	view.no_call = view.tx_my[0] == '\0';
	memcpy(view_msg_tx, st.msg, sizeof st.msg);
}

// The UR in force: the one-over command, else the for-now choice, else the channel's
static const char *ur_now(uint8_t *mode)
{
	if (ur_once[0] != 0) {
		*mode = ur_once_mode;
		return ur_once;
	}
	if (ur_temp[0] != 0) {
		*mode = ur_temp_mode;
		return ur_temp;
	}
	*mode = RDSTAR_UR_CHANNEL;
	return chan.ur;
}

// RPT1 and RPT2 left blank on a direct channel go out as "DIRECT", what other radios send and show for a simplex over
static void rpt_field(char *dst, const char *src)
{
	if (field_blank(src, RADIO_DSTAR_CALL)) {
		memcpy(dst, "DIRECT  ", RADIO_DSTAR_CALL);
	} else {
		memcpy(dst, src, RADIO_DSTAR_CALL);
	}
}

static void routing_view(void)
{
	uint8_t mode;
	const char *ur = ur_now(&mode);
	dstar_field_text(view.tx_ur, ur, RADIO_DSTAR_CALL);
	view.ur_mode = mode;
	char r1[RADIO_DSTAR_CALL], r2[RADIO_DSTAR_CALL];
	rpt_field(r1, chan.rpt1);
	rpt_field(r2, chan.rpt2);
	dstar_field_text(view.tx_rpt1, r1, sizeof r1);
	dstar_field_text(view.tx_rpt2, r2, sizeof r2);
	view.tx_repeater = (chan.flags & DSTAR_CP_VIA_RPT) != 0;
	dirty();
}

// --- receive: the deframer's callbacks (radio task) ---

static void rx_header(void *ctx, const dstar_header_t *h, bool from_slow)
{
	(void)ctx;
	dstar_field_text(view.my, h->my, sizeof h->my);
	dstar_field_text(view.suffix, h->suffix, sizeof h->suffix);
	dstar_field_text(view.ur, h->ur, sizeof h->ur);
	dstar_field_text(view.rpt1, h->rpt1, sizeof h->rpt1);
	dstar_field_text(view.rpt2, h->rpt2, sizeof h->rpt2);
	if (!from_slow || view.rx == RDSTAR_RX_IDLE) {
		view.msg[0] = '\0';	// a new over; a header rebuilt from the slow data mid-over keeps its message
	}
	view.heard = true;
	view.heard_slow = from_slow;
	if (view.rx == RDSTAR_RX_IDLE) {
		view.rx = RDSTAR_RX_HEADER;
		stats.rx_overs++;
		STORE(rx_seq, (uint8_t)(rx_seq + 1u));
	}
	dirty();
}

static void rx_voice(void *ctx, const uint8_t v[DSTAR_VOICE_BYTES], uint8_t frame)
{
	(void)ctx;
	(void)frame;
	if (view.rx == RDSTAR_RX_IDLE) {
		// a confirmed late entry hands voice over before any header (dstar-modem.md 3.4): an over all the same
		view.rx = RDSTAR_RX_HEADER;
		stats.rx_overs++;
		STORE(rx_seq, (uint8_t)(rx_seq + 1u));
		dirty();
	}
	stats.rx_frames++;
	uint8_t h = rx_head;
	if ((uint8_t)(h - LOAD(rx_tail)) >= RADIO_DSTAR_VOICE_Q) {
		stats.rx_overflows++;
		return;
	}
	memcpy(rxq[h & QMASK], v, DSTAR_VOICE_BYTES);
	STORE(rx_head, (uint8_t)(h + 1u));
}

static void rx_message(void *ctx, const char msg[DSTAR_MSG_LEN])
{
	(void)ctx;
	dstar_field_text(view.msg, msg, DSTAR_MSG_LEN);
	dirty();
}

static void rx_end(void *ctx, dstar_end_t why)
{
	(void)ctx;
	(void)why;
	if (view.rx != RDSTAR_RX_IDLE) {
		view.rx = RDSTAR_RX_IDLE;
		STORE(rx_end_seq, LOAD(rx_seq));
		dirty();
	}
}

static const dstar_rx_cb_t rx_cb = { rx_header, rx_voice, rx_message, rx_end, NULL };

static bool engine_rx(void)
{
	if (!dstar_engine_rx_start(eng, &path, &rx_cb)) {
		return false;
	}
	return true;
}

// --- the codec's RAM and the audio stream ---

// The codec's state is in the mode area beside the engine; its working memory is the DMR vocoder's state block, lent
// while DMR voice is off (the radio core stops DMR before it tunes a D-STAR channel). The vocoder rebuilds its own
// state at its next call, so leaving gives nothing back: the codec only lets go of the block
static void codec_take(void)
{
	codec = NULL;
	view.voice_off = RDSTAR_VOICE_NO_RAM;
	const hal_vocoder_t *v = board.vocoder;
	if (area != NULL && v != NULL && v->lend != NULL) {
		size_t n = 0;
		void *p = v->lend(&n);
		if (p != NULL && n >= sizeof(dstar_codec_scratch_t)) {
			codec = &area->codec;
			dstar_codec_bind(codec, p);
			view.voice_off = RDSTAR_VOICE_OK;
		}
	}
}

static void codec_give_back(void)
{
	if (codec != NULL) {
		dstar_codec_bind(codec, NULL);
	}
	codec = NULL;
}

static void speaker(bool on)
{
	if (on != cb_speaker) {
		cb_speaker = on;
		if (aud != NULL && aud->speaker_pcm != NULL) {
			aud->speaker_pcm(on);
		}
	}
}

// Transmit side of a block: the mic into a frame for the modulator's queue
static void tx_block(const int16_t *mic)
{
	if (!LOAD(tx_on) || LOAD(tx_released)) {
		cb_tx = false;
		return;
	}
	if (!cb_tx) {
		cb_tx = true;
		if (codec != NULL) {
			dstar_codec_start(codec, true);	// a fresh encoder: the first frame of the over
		}
		stats.tx_peak = 0;
	}
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		uint32_t a = (uint32_t)(mic[i] < 0 ? -mic[i] : mic[i]);
		if (a > stats.tx_peak) {
			stats.tx_peak = a;
		}
	}
	uint8_t frame[DSTAR_VOICE_BYTES];
	if (codec == NULL || !dstar_codec_encode(codec, mic, frame)) {
		memcpy(frame, dstar_voice_silence, DSTAR_VOICE_BYTES);
	}
	stats.tx_frames++;
	uint8_t h = tx_head;
	if ((uint8_t)(h - LOAD(tx_tail)) >= RADIO_DSTAR_VOICE_Q) {
		stats.tx_overflows++;
		return;
	}
	memcpy(txq[h & QMASK], frame, DSTAR_VOICE_BYTES);
	STORE(tx_head, (uint8_t)(h + 1u));
}

// Receive side of a block: one frame decoded, or the codec's concealment, or silence between overs
static void rx_block(int16_t *spk)
{
	uint8_t seq = LOAD(rx_seq);
	if (seq != cb_rx_seq) {
		cb_rx_seq = seq;		// a new over: the decoder starts afresh, playback waits for the prime
		cb_playing = true;
		cb_primed = false;
		if (codec != NULL && !cb_tx) {
			dstar_codec_start(codec, false);
		}
	}
	uint8_t t = rx_tail, n = (uint8_t)(LOAD(rx_head) - t);
	bool ended = LOAD(rx_end_seq) == seq;
	if (cb_playing && !cb_primed && (n >= RADIO_DSTAR_PRIME || (ended && n > 0))) {
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
			memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
			return;
		}
		stats.rx_underruns++;
		stats.rx_concealed++;
		if (codec != NULL) {
			dstar_codec_lost(codec, spk);
		} else {
			memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
		}
		return;
	}
	const uint8_t *f = rxq[t & QMASK];
	if (codec != NULL && dstar_codec_decode(codec, f, spk)) {
		uint8_t e = dstar_codec_errors(codec);
		if (e >= DSTAR_CODEC_FAILED) {
			stats.rx_bad++;
			stats.rx_concealed++;
		} else {
			stats.rx_bit_errors += e;
			stats.rx_played++;
		}
	} else {
		memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
	}
	STORE(rx_tail, (uint8_t)(t + 1u));
}

void radio_dstar_audio_block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	stats.blocks++;
	// Half duplex: transmit first (a fresh over takes the codec's RAM for the encoder), then playback
	tx_block(mic);
	rx_block(spk);
	bool voice = cb_playing && cb_primed && !cb_tx;
	view.playing = voice;
	if (beep_mix(spk)) {
		cb_mixing = true;
		speaker(true);
	} else if (cb_mixing) {
		cb_mixing = false;
		speaker(voice);
	} else {
		speaker(voice);
	}
}

static void stream_start(void)
{
	streaming = aud != NULL && aud->start != NULL && aud->start(radio_dstar_audio_block, NULL);
	if (!streaming) {
		view.voice_off = RDSTAR_VOICE_NO_STREAM;
	}
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

// --- transmit: the modulator's sources (the board's interrupt) ---

// The voice pull: the next encoded frame. The silence frame covers a late encoder; after PTT release the queue is
// played out, then the end pattern
static bool voice_pull(void *ctx, uint8_t frame[DSTAR_VOICE_BYTES])
{
	(void)ctx;
	uint8_t t = tx_tail;
	if (t != LOAD(tx_head)) {
		memcpy(frame, txq[t & QMASK], DSTAR_VOICE_BYTES);
		STORE(tx_tail, (uint8_t)(t + 1u));
		return true;
	}
	if (tx_released) {
		return false;
	}
	stats.tx_underruns++;
	memcpy(frame, dstar_voice_silence, DSTAR_VOICE_BYTES);
	return true;
}

// One period of a sine, Q15 (64 points; the tone is interpolated between them)
static const int16_t sine64[64] = {
	0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
	32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329, 23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
	0, -3212, -6393, -9512, -12539, -15446, -18204, -20787, -23170, -25329, -27245, -28898, -30273, -31356, -32137,
	-32609, -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329, -23170, -20787, -18204, -15446, -12539,
	-9512, -6393, -3212,
};

static int32_t sine(uint32_t phase)
{
	uint32_t i = phase >> 26, frac = (phase >> 10) & 0xFFFFu;
	int32_t a = sine64[i], b = sine64[(i + 1u) & 63u];
	return a + (int32_t)(((int64_t)(b - a) * (int32_t)frac) >> 16);
}

static uint32_t step_of(uint32_t hz)
{
	return (uint32_t)(((uint64_t)hz << 32) / tx_rate);
}

#define SWEEP_LO_HZ	100u
#define SWEEP_HI_HZ	3600u
#define SWEEP_S		2u	// each way

static void test_fill(int16_t *pcm, uint16_t n)
{
	int32_t amp = tx_amp_1khz;
	for (uint16_t i = 0; i < n; i++) {
		int32_t v;
		if (tx_kind == RDSTAR_TEST_SQUARE) {
			v = (test_phase & 0x80000000u) ? -amp : amp;
		} else {
			v = (int32_t)(((int64_t)sine(test_phase) * amp) >> 15);
		}
		pcm[i] = (int16_t)v;
		test_phase += test_step;
		if (tx_kind == RDSTAR_TEST_SWEEP) {
			// a linear sweep: the step moves by a constant each sample, turning at the ends
			test_n++;
			uint32_t lo = step_of(SWEEP_LO_HZ), hi = step_of(SWEEP_HI_HZ);
			uint32_t d = (hi - lo) / (SWEEP_S * tx_rate);
			test_step = test_sweep_dir > 0 ? test_step + d : test_step - d;
			if (test_step >= hi) {
				test_sweep_dir = -1;
			} else if (test_step <= lo) {
				test_sweep_dir = 1;
			}
		}
	}
}

static int dots_bit(void *ctx)
{
	(void)ctx;
	return (int)(test_n++ & 1u);
}

static void tx_pull(int16_t *pcm, uint16_t n)
{
	stats.tx_samples += n;
	if (tx_done) {
		memset(pcm, 0, n * sizeof(int16_t));
		return;
	}
	switch (tx_kind) {
	case RDSTAR_TEST_TONE:
	case RDSTAR_TEST_SWEEP:
	case RDSTAR_TEST_SQUARE:
		if (tx_released) {
			memset(pcm, 0, n * sizeof(int16_t));
			tx_done = true;
		} else {
			test_fill(pcm, n);
		}
		return;
	case RDSTAR_TEST_DOTS: {
		size_t got = tx_released ? 0 : dstar_gmsk_mod(&eng->u.tx.mod, pcm, n);
		if (got < n) {
			memset(&pcm[got], 0, (n - got) * sizeof(int16_t));
			tx_done = true;
		}
		return;
	}
	default:
		if (!dstar_engine_tx(eng, pcm, n)) {
			tx_done = true;
		}
		return;
	}
}

// --- the radio core's calls ---

void radio_dstar_init(const hal_audio_t *audio)
{
	aud = audio;
	tap = board.audio_tap;
	radio_dstar_cal_reload();
}

bool radio_dstar_kind(const channel_t *ch)
{
	return ch->kind == CODEPLUG_KIND_DSTAR && codeplug_kind_built(ch->kind);
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
	if (!rx_on && view.fault == RDSTAR_OK) {
		view.fault = path_ok() ? RDSTAR_RX_REFUSED : RDSTAR_NO_PATH;
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
	view.rx = RDSTAR_RX_IDLE;
	STORE(rx_end_seq, LOAD(rx_seq));
}

bool radio_dstar_start(uint16_t number, const channel_t *ch, uint32_t now_ms)
{
	(void)now_ms;
	if (running) {
		radio_dstar_stop();
	}
	memset(&chan, 0, sizeof chan);
	if (number == 0 || !dstar_chan_load(number, ch, &chan)) {
		dstar_chan_decode(NULL, 0, &chan);	// the defaults: direct, CQCQCQ
	}
	// UR chosen for now lasts until the channel changes: a retune of the same one (after an over, a trim) keeps it
	if (number != cur_number) {
		ur_temp[0] = ur_once[0] = 0;
		cur_number = number;
	}
	view.on = true;
	view.fault = RDSTAR_OK;
	tap = board.audio_tap;
	radio_dstar_cal_reload();
	station_load();
	routing_view();
	running = true;
	if (!path_ok()) {
		view.fault = RDSTAR_NO_PATH;
		dirty();
		return false;
	}
	area = radio_mode_ram_take(RADIO_MODE_RAM_DSTAR, sizeof *area);
	eng = area != NULL ? &area->eng : NULL;
	if (eng == NULL) {
		// Another mode still holds the area: the core starts a mode only after stopping the other, so this is a bug
		// in that order. Refused rather than taken, since that mode's transmit interrupt may still be pulling from it
		view.fault = RDSTAR_RX_REFUSED;
		dirty();
		return false;
	}
	if (path.tx_amp == 0) {
		view.fault = RDSTAR_NO_CAL;	// receive still runs: only transmit needs the scale
	}
	codec_take();
	rx_queue_reset();
	engine_rx();
	rx_flat_on();
	stream_start();
	dirty();
	return rx_on;
}

void radio_dstar_stop(void)
{
	if (!running) {
		return;
	}
	if (tx_on) {
		radio_dstar_tx_abort();
	}
	rx_flat_off();
	stream_stop();
	if (eng != NULL) {
		dstar_engine_idle(eng);
	}
	codec_give_back();
	radio_mode_ram_give(RADIO_MODE_RAM_DSTAR);	// after the engine is idle and the board's pull stopped (tx_abort)
	eng = NULL;
	area = NULL;
	running = false;
	view.on = false;
	view.rx = RDSTAR_RX_IDLE;
	dirty();
}

bool radio_dstar_running(void)
{
	return running;
}

bool radio_dstar_poll(uint32_t now_ms)
{
	if (running && rx_on && !tx_on) {
		int16_t buf[RX_CHUNK];
		uint16_t n;
		while ((n = tap->fm_rx_flat_read(buf, RX_CHUNK)) > 0) {
			stats.rx_samples += n;
			dstar_engine_rx(eng, buf, n);
		}
		if (eng->mode == DSTAR_ENGINE_RX) {
			stats.polarity = eng->u.rx.fr.polarity;
			stats.hdr_corrected = eng->u.rx.fr.last_corrected;
		}
		if (view.rx != RDSTAR_RX_IDLE) {
			view.heard_ms = now_ms != 0 ? now_ms : 1;
		}
	}
	bool was = view_dirty;
	view_dirty = false;
	return was;
}

bool radio_dstar_receiving(void)
{
	return running && view.rx != RDSTAR_RX_IDLE;
}

bool radio_dstar_playing(void)
{
	return running && view.playing;
}

uint8_t radio_dstar_tx_check(void)
{
	if (!running || eng == NULL || view.fault == RDSTAR_NO_PATH) {
		return TXDENY_MODE;
	}
	if (path.tx_amp == 0) {
		return TXDENY_REFUSED;		// no deviation scale: never guess at the deviation on air
	}
	if (view.no_call && test_kind == RDSTAR_TEST_OFF) {
		return TXDENY_NO_CALL;
	}
	return TXDENY_NONE;
}

bool radio_dstar_tx_start(uint32_t now_ms)
{
	(void)now_ms;
	if (!running || eng == NULL) {
		return false;
	}
	rx_flat_off();
	rx_queue_reset();
	tx_done = false;
	tx_released = false;
	tx_done_ms = tx_drain_ms = 0;
	STORE(tx_tail, LOAD(tx_head));	// nothing left over from the last over
	tx_kind = test_kind;
	tx_rate = path.rate;
	tx_amp_1khz = (int32_t)(path.tx_amp * 5 / 6) * test_level / 100;	// the 1 kHz scale
	if (tx_rate == 0) {
		return false;	// never seen (cal_reload sets a rate), but step_of divides by it
	}
	if (tx_kind == RDSTAR_TEST_OFF || tx_kind == RDSTAR_TEST_SILENT) {
		uint8_t mode;
		const char *ur = ur_now(&mode);
		char r1[RADIO_DSTAR_CALL], r2[RADIO_DSTAR_CALL], my[RADIO_DSTAR_CALL + 1];
		rpt_field(r1, chan.rpt1);
		rpt_field(r2, chan.rpt2);
		char urs[RADIO_DSTAR_CALL + 1], r1s[RADIO_DSTAR_CALL + 1], r2s[RADIO_DSTAR_CALL + 1];
		memcpy(urs, ur, RADIO_DSTAR_CALL);
		urs[RADIO_DSTAR_CALL] = '\0';
		memcpy(r1s, r1, RADIO_DSTAR_CALL);
		r1s[RADIO_DSTAR_CALL] = '\0';
		memcpy(r2s, r2, RADIO_DSTAR_CALL);
		r2s[RADIO_DSTAR_CALL] = '\0';
		strcpy(my, view.no_call ? "NOCALL" : view.tx_my);
		dstar_header_t h;
		uint8_t flag1 = view.tx_repeater ? DSTAR_FLAG1_REPEATER : 0;
		dstar_header_set(&h, flag1, r2s, r1s, urs, my, view.tx_suffix);
		char msg[RADIO_DSTAR_MSG + 1];
		memcpy(msg, view_msg_tx, RADIO_DSTAR_MSG);
		msg[RADIO_DSTAR_MSG] = '\0';
		bool silent = tx_kind == RDSTAR_TEST_SILENT;
		if (!dstar_engine_tx_start(eng, &path, &h, field_blank(msg, RADIO_DSTAR_MSG) ? NULL : msg,
					   RADIO_DSTAR_PREAMBLE, silent ? NULL : voice_pull, NULL)) {
			engine_rx();
			rx_flat_on();
			return false;
		}
		if (ur_once[0] != 0) {
			ur_once[0] = 0;		// a reflector command goes out once: the next over is the for-now or channel UR
			routing_view();
		}
	} else {
		// the bench's signals go round the framer; the dots still need the modulator
		test_phase = 0;
		test_n = 0;
		test_sweep_dir = 1;
		test_step = step_of(tx_kind == RDSTAR_TEST_SWEEP ? SWEEP_LO_HZ : test_hz);
		if (tx_kind == RDSTAR_TEST_DOTS &&
		    !dstar_gmsk_mod_init(&eng->u.tx.mod, path.rate, path.tx_amp, dots_bit, NULL)) {
			engine_rx();
			rx_flat_on();
			return false;
		}
		eng->mode = DSTAR_ENGINE_TX;
	}
	STORE(tx_on, true);
	if (!tap->fm_tx_inject(tx_pull, AUDIO_TAP_FLAT)) {
		STORE(tx_on, false);
		engine_rx();
		rx_flat_on();
		return false;
	}
	stats.tx_overs++;
	dirty();
	return true;
}

void radio_dstar_tx_release(void)
{
	if (tx_on) {
		tx_released = true;
		if (tx_kind == RDSTAR_TEST_SILENT) {
			dstar_engine_tx_stop(eng);
		}
	}
}

bool radio_dstar_tx_done(uint32_t now_ms)
{
	if (!tx_on) {
		return true;
	}
	if (!tx_done) {
		if (!tx_released) {
			return false;
		}
		// Only the board's interrupt sets tx_done, so a route that stops asking for halves (a request the modem
		// never raises, a hook the glue dropped: nothing of it is measured on a radio yet) would hold the carrier
		// after PTT release until the time-out, or for good without one. Past the longest the queue and the end
		// pattern can take, the over is over: the pull is stopped here and the caller unkeys
		if (tx_drain_ms == 0) {
			tx_drain_ms = now_ms != 0 ? now_ms : 1;
		}
		if (now_ms - tx_drain_ms < RADIO_DSTAR_DRAIN_MS + tx_latency_ms()) {
			return false;
		}
		radio_dstar_tx_abort();
		return true;
	}
	if (tx_done_ms == 0) {
		tx_done_ms = now_ms != 0 ? now_ms : 1;
	}
	return now_ms - tx_done_ms >= tx_latency_ms();
}

void radio_dstar_tx_abort(void)
{
	if (!tx_on) {
		return;
	}
	tx_done = true;
	tx_released = true;
	tap->fm_tx_inject(NULL, AUDIO_TAP_FLAT);
	STORE(tx_on, false);
}

void radio_dstar_rx_again(uint32_t now_ms)
{
	(void)now_ms;
	if (!running) {
		return;
	}
	if (tx_on) {
		tap->fm_tx_inject(NULL, AUDIO_TAP_FLAT);
		STORE(tx_on, false);
	}
	if (eng == NULL) {
		return;		// started without a path or the area: nothing to receive with
	}
	rx_queue_reset();
	engine_rx();
	rx_flat_on();
	dirty();
}

bool radio_dstar_streaming(void)
{
	return running && streaming;
}

void radio_dstar_ur(uint8_t mode, const char *text)
{
	switch (mode) {
	case RDSTAR_UR_CHANNEL:
		ur_temp[0] = ur_once[0] = 0;
		break;
	case RDSTAR_UR_CQ:
		memcpy(ur_temp, "CQCQCQ  ", RADIO_DSTAR_CALL);
		ur_temp_mode = mode;
		break;
	case RDSTAR_UR_REPLY:
		if (!view.heard || view.my[0] == '\0') {
			return;
		}
		dstar_field_set(ur_temp, RADIO_DSTAR_CALL, view.my);
		ur_temp_mode = mode;
		break;
	case RDSTAR_UR_LINK: {
		// the reflector padded to six, its module in the seventh place, 'L' in the eighth ("REF030CL")
		char ref[RADIO_DSTAR_CALL + 1];
		dstar_field_set(ref, RADIO_DSTAR_CALL, text != NULL ? text : "");
		size_t len = strnlen(text != NULL ? text : "", 7);
		char out[RADIO_DSTAR_CALL];
		memset(out, ' ', sizeof out);
		if (len >= 2) {
			memcpy(out, ref, len - 1u > 6u ? 6u : len - 1u);	// the name: all but the module letter
			out[6] = ref[len - 1u];
		} else {
			return;
		}
		out[7] = 'L';
		memcpy(ur_once, out, RADIO_DSTAR_CALL);
		ur_once_mode = mode;
		break;
	}
	case RDSTAR_UR_UNLINK:
		memcpy(ur_once, "       U", RADIO_DSTAR_CALL);
		ur_once_mode = mode;
		break;
	case RDSTAR_UR_ECHO:
		memcpy(ur_once, "       E", RADIO_DSTAR_CALL);
		ur_once_mode = mode;
		break;
	case RDSTAR_UR_INFO:
		memcpy(ur_once, "       I", RADIO_DSTAR_CALL);
		ur_once_mode = mode;
		break;
	default:
		return;
	}
	routing_view();
}

void radio_dstar_clear_temporary(void)
{
	if (ur_temp[0] != 0 || ur_once[0] != 0) {
		ur_temp[0] = ur_once[0] = 0;
		routing_view();
	}
}

void radio_dstar_view(radio_dstar_view_t *out)
{
	*out = view;
}

void radio_dstar_stats(radio_dstar_stats_t *out)
{
	*out = stats;
	// The counts since the reset. The polarity and the header's corrected bits are readings, not counts, and stand as
	// they are; the latest over's peak reads 0 until an over after the reset (dstar_bench.py's over takes it as the
	// microphone's level of the over it just keyed)
	out->rx_frames -= stats_base.rx_frames;
	out->rx_played -= stats_base.rx_played;
	out->rx_concealed -= stats_base.rx_concealed;
	out->rx_underruns -= stats_base.rx_underruns;
	out->rx_overflows -= stats_base.rx_overflows;
	out->rx_bad -= stats_base.rx_bad;
	out->rx_bit_errors -= stats_base.rx_bit_errors;
	out->rx_samples -= stats_base.rx_samples;
	out->rx_overs -= stats_base.rx_overs;
	out->tx_overs -= stats_base.tx_overs;
	out->tx_frames -= stats_base.tx_frames;
	out->tx_underruns -= stats_base.tx_underruns;
	out->tx_overflows -= stats_base.tx_overflows;
	out->tx_samples -= stats_base.tx_samples;
	out->blocks -= stats_base.blocks;
	if (out->tx_overs == 0) {
		out->tx_peak = 0;
	}
}

void radio_dstar_reset_stats(void)
{
	stats_base = stats;	// never stats itself: see stats_base
}

void radio_dstar_test(uint8_t kind, uint16_t hz, uint8_t level_pct)
{
	if (tx_on || kind >= RDSTAR_TEST_COUNT) {
		return;		// not mid-over (the over keeps the kind it started with: tx_kind)
	}
	test_kind = kind;
	test_hz = hz != 0 ? hz : 1000u;
	test_level = level_pct == 0 ? 100u : level_pct > 100u ? 100u : level_pct;
}

uint8_t radio_dstar_test_kind(void)
{
	return test_kind;
}
#endif
