// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Fusion's Voice FR (VW) in the radio core (core/radio/radio_ysf.c with radio_ysf_vw.c; docs/spec/ysf-modem.md 4.1, 5),
 * against ysfRadioTest's fake board: its flat paths, its fake AMBE+2 vocoder and the real codeplug. The harness is
 * ysfRadioTest.c itself, included with its main renamed, so the two tests share one fake board and VW's tests live
 * apart from DN's.
 *
 *  - another station's VW over is received, decoded by the full-rate codec and played: the samples are exactly what
 *    the codec makes of those frames, the board's vocoder is never asked, and it plays with no codec in flash at all
 *  - DN, VW and DN again on one channel: each over goes to its own decoder
 *  - a channel set to VW transmits VW: our encoder's frames, in order, with DT 3 and CSD3, PTT allowed without the
 *    board's codec, and DN channels still send DN
 *
 *   make -C tests/host run-ysfcp
 */
#define main ysf_radio_test_main
#include "ysfRadioTest.c"
#undef main

#include "ysf/ysf_vw.h"
#include "mbe/imbe.h"
#include "mbe/mbe_est.h"

// --- the codec outside the radio, as the reference ---

// A vowel (a pulse train through two resonators, a slow pitch glide), as ysfVwTest's: speech enough for the encoder
static void vowel(int16_t *out, uint32_t n)
{
	int32_t y1a = 0, y2a = 0, y1b = 0, y2b = 0;
	for (uint32_t t = 0; t < n; t++) {
		uint32_t period = 60u + (t / 400u) % 12u;
		int32_t x = (t % period) == 0 ? 8000 : 0;
		int32_t ya = x + ((2 * 13722 * y1a) >> 14) - ((15550 * y2a) >> 14);
		y2a = y1a;
		y1a = ya / 2;
		int32_t yb = ya / 4 + ((2 * 11019 * y1b) >> 14) - ((15550 * y2b) >> 14);
		y2b = y1b;
		y1b = yb / 2;
		out[t] = (int16_t)(yb > 20000 ? 20000 : yb < -20000 ? -20000 : yb);
	}
}

#define NFR		40u
static int16_t speech[NFR * 160u];
static uint8_t frames[NFR][YSF_VW_IMBE_BYTES];	// the reference encoder's frames of speech
static int16_t ref_pcm[NFR * 160u];		// and the reference decoder's samples of those

static struct {
	mbe_est_state_t est;
	imbe_dec_state_t st;
	mbe_params_t pr;
	mbe_state_t ms;
	mbe_frame_t prev, cur;
	int32_t buf[MBE_BUF_WORDS];
} rm;

static void reference(void)
{
	vowel(speech, NFR * 160u);
	imbe_enc_mem_t em = { &rm.st.q, &rm.est, &rm.pr, rm.buf };
	imbe_enc_init(&em);
	for (uint32_t i = 0; i < NFR; i++) {
		imbe_enc_frame(&em, speech + i * 160u, frames[i]);
	}
	imbe_mem_t dm = { &rm.st, &rm.pr, { &rm.ms, &rm.prev, &rm.cur, rm.buf } };
	imbe_dec_init(&dm);
	for (uint32_t i = 0; i < NFR; i++) {
		imbe_dec_frame(&dm, frames[i], 0, ref_pcm + i * 160u);
	}
}

// --- another station sending VW ---

static uint32_t vw_next;

static bool vw_pull(void *ctx, uint8_t imbe[YSF_VW_IMBE_BYTES])
{
	(void)ctx;
	if (vw_next >= NFR) {
		return false;
	}
	memcpy(imbe, frames[vw_next++], YSF_VW_IMBE_BYTES);
	return true;
}

// What the speaker got: every block the audio callback filled, from the over's first loud sample
static int16_t heard[(NFR + 40u) * 160u];
static uint32_t heard_n;

static void step_keep(void)
{
	int16_t mic[160], spk[160];
	memset(mic, 0, sizeof mic);
	radio_ysf_poll(now_ms);
	if (stream_cb != NULL) {
		stream_cb(mic, spk, NULL);
		bool loud = heard_n > 0;
		for (int i = 0; i < 160 && !loud; i++) {
			loud = spk[i] != 0;
		}
		if (loud && heard_n + 160u <= sizeof heard / sizeof heard[0]) {
			memcpy(heard + heard_n, spk, sizeof spk);
			heard_n += 160u;
		}
	}
	now_ms += 20;
}

// How send_vw_ex sends: at air step stall_at the audio callback runs stall_n extra blocks with no air between them
// (the speaker ahead of the receiver, as when the radio task falls behind); the first blank samples of the air are
// silence (a receiver that joins late: 800 a frame at 8 kHz); rem4: the radio ID in CSD3 (NULL: none); at step
// snap_at (above 0) the module's view is copied to mid
typedef struct {
	int stall_at, stall_n;
	uint32_t blank;
	const char *rem4;
	int snap_at;
} vw_opts_t;
static radio_ysf_view_t mid;

static void send_vw_ex(const char *src, const vw_opts_t *o);
static void send_vw(const char *src)
{
	send_vw_ex(src, NULL);
}

static void send_vw_ex(const char *src, const vw_opts_t *o)
{
	static ysf_engine_t e;
	int steps = 0;
	ysf_tx_config_t c;
	memset(&c, 0, sizeof c);
	ysf_csd_init(&c.csd, src, "ALL");
	if (o != NULL && o->rem4 != NULL) {
		memcpy(c.csd.rem[3], o->rem4, YSF_RADIO_ID_LEN);
	}
	c.headers = 2;
	c.vw = true;
	c.pull_fr = vw_pull;
	memcpy(c.silence_fr, imbe_silence, IMBE_BYTES);
	vw_next = 0;
	ysf_path_t p = { .rate = 8000, .tx_amp = 2700 };
	CHECK(ysf_engine_tx_start(&e, &p, &c), "VW tx");
	bool more = true;
	while (more) {
		if (vw_next >= NFR) {
			ysf_engine_tx_stop(&e);
		}
		int16_t pcm[160];
		more = ysf_engine_tx(&e, pcm, 160);
		for (int i = 0; i < 160; i++) {
			bool blank = o != NULL && (uint32_t)steps * 160u + (uint32_t)i < o->blank;
			rxbuf[rx_w++ % RXBUF] = blank ? 0 : pcm[i];
		}
		step_keep();
		if (o != NULL && o->snap_at > 0 && steps == o->snap_at) {
			radio_ysf_view(&mid);
		}
		if (o != NULL && steps++ == o->stall_at) {
			for (int k = 0; k < o->stall_n && stream_cb != NULL; k++) {
				int16_t mic[160] = { 0 }, spk[160];
				stream_cb(mic, spk, NULL);
			}
		}
	}
	for (int t = 0; t < 30; t++) {
		for (int i = 0; i < 160; i++) {
			rxbuf[rx_w++ % RXBUF] = 0;
		}
		step_keep();
	}
}

static void channel_vw(bool vw)
{
	channel(0, 0, "G4XYZ");
	ysf_chan_t yc;
	ysf_field_set(yc.dest, YSF_CALLSIGN_LEN, "ALL");
	yc.tx_dgid = yc.rx_dgid = 0;
	yc.flags = vw ? YSF_CP_VW : 0u;
	CHECK(ysf_chan_write(5, "C4FM", &yc, true), "tag");
	CHECK(codeplug_channel(5, &ch) && ch.kind == CODEPLUG_KIND_YSF, "kind");
}

static void test_receive_vw(bool codec)
{
	channel_vw(false);
	voc_present = codec;
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	dec_n = 0;
	heard_n = 0;
	radio_ysf_stats_t before, after;
	radio_ysf_stats(&before);
	send_vw("JA1YKX");
	radio_ysf_view_t v;
	radio_ysf_view(&v);
	radio_ysf_stats(&after);
	CHECK(v.heard && v.mode == RYSF_MODE_VW && strcmp(v.src, "JA1YKX") == 0, "heard VW from '%s' (mode %u)", v.src,
	      v.mode);
	CHECK(dec_n == 0, "the board's vocoder decoded %u frames of a VW over", dec_n);
	uint32_t played = after.rx_played - before.rx_played;
	CHECK(played >= NFR && played <= NFR + 4u, "codec %d: %u frames played (%u sent, then the last frame's silence)",
	      codec, played, NFR);
	CHECK(after.rx_vw - before.rx_vw >= NFR / 5u, "VW frames counted: %u", after.rx_vw - before.rx_vw);
	// Exactly what the codec makes of those frames: the playback starts with the first frame and nothing is lost
	uint32_t first = 0;
	while (first < NFR * 160u && ref_pcm[first] == 0) {
		first++;
	}
	CHECK(heard_n >= NFR * 160u - first, "%u samples heard", heard_n);
	uint32_t same = 0;
	for (uint32_t i = 0; first + i < NFR * 160u && i < heard_n; i++) {
		same += heard[i] == ref_pcm[first + i];
	}
	CHECK(same == NFR * 160u - first, "codec %d: %u of %u samples as the reference decoder's", codec, same,
	      NFR * 160u - first);
	CHECK(!speaker_on && !radio_ysf_receiving(), "the over ended");
	radio_ysf_stop();
	voc_present = true;
}

// DN, then VW, then DN on one channel
static void test_mixed(void)
{
	channel_vw(false);
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	dec_n = dec_match = 0;
	dec_want_base = 0;
	send_other("M0ABC", 0, 20);
	quiet(400);
	uint32_t dn1 = dec_match;
	heard_n = 0;
	radio_ysf_stats_t a, b;
	radio_ysf_stats(&a);
	send_vw("JA1YKX");
	radio_ysf_stats(&b);
	uint32_t dn_during_vw = dec_n - dn1;
	dec_want_base = 0;
	dec_n = dec_match = 0;
	send_other("M0ABC", 0, 20);
	quiet(400);
	CHECK(dn1 == 20, "first DN over: %u frames in order", dn1);
	CHECK(b.rx_played - a.rx_played >= NFR && heard_n >= (NFR - 2u) * 160u, "the VW over played: %u frames",
	      b.rx_played - a.rx_played);
	CHECK(dn_during_vw <= 5, "the board's vocoder during the VW over: %u frames (the first over's tail at most)",
	      dn_during_vw);
	CHECK(dec_match == 20, "DN after VW: %u frames in order", dec_match);
	radio_ysf_view_t v;
	radio_ysf_view(&v);
	CHECK(v.mode == RYSF_MODE_DN, "the last over was DN (%u)", v.mode);
	radio_ysf_stop();
}

// VW's codec memory where the vocoder lends its state block (radio_ysf.h): the block, taken for the VW over alone, so
// the mode area holds only the engine. A DN over first and after: the vocoder's calls write all over the block (the
// fake's scribble, as the real one rebuilds its state there), and the VW over between still plays exactly what the
// codec makes of its frames. Without a lend (RADIO_YSF_VW_IN_AREA) the area holds it and nothing is borrowed
static void test_vw_memory(void)
{
	channel_vw(false);
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	dec_n = dec_match = 0;
	dec_want_base = 0;
	uint32_t lends0 = voc_lends;
	send_other("M0ABC", 0, 20);
	quiet(400);
	CHECK(voc_lends == lends0, "a DN over borrowed the vocoder's block %u times", voc_lends - lends0);
	heard_n = 0;
	send_vw("JA1YKX");
	uint32_t first = 0;
	while (first < NFR * 160u && ref_pcm[first] == 0) {
		first++;
	}
	uint32_t same = 0;
	for (uint32_t i = 0; first + i < NFR * 160u && i < heard_n; i++) {
		same += heard[i] == ref_pcm[first + i];
	}
	CHECK(same == NFR * 160u - first, "VW after DN: %u of %u samples as the reference decoder's", same,
	      NFR * 160u - first);
#if RADIO_YSF_VW_IN_AREA
	CHECK(voc_lends == lends0, "VW borrowed the vocoder's block with no lend built");
	CHECK(radio_mode_ram_size() >= sizeof(radio_ysf_vw_mem_t), "the area (%zu B) lacks the codec", radio_mode_ram_size());
#else
	CHECK(voc_lends == lends0 + 1u, "the VW over took the vocoder's block %u times", voc_lends - lends0);
	CHECK(radio_mode_ram_size() < sizeof(radio_ysf_vw_mem_t), "the area (%zu B) still holds the codec's %zu",
	      radio_mode_ram_size(), sizeof(radio_ysf_vw_mem_t));
#endif
	uint32_t lends1 = voc_lends;
	dec_want_base = 0;
	dec_n = dec_match = 0;
	send_other("M0ABC", 0, 20);
	quiet(400);
	CHECK(dec_match == 20 && voc_lends == lends1, "DN after VW: %u frames in order, %u lends", dec_match,
	      voc_lends - lends1);
	radio_ysf_stop();
}

// What the debug link reports of a VW over's receive timing (ysf-modem.md 4.3): every decode timed by the clock, the
// over's longest and mean and its blocks, the longest since start; a block mid-over with nothing queued counted late
static void test_vw_timing(void)
{
	channel_vw(false);
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	radio_ysf_stats_t before, after;
	radio_ysf_stats(&before);
	micros_step = 1500;		// every decode takes 1.5 ms by the clock
	vw_opts_t o = { .stall_at = 30, .stall_n = 12, .snap_at = 0 };
	send_vw_ex("JA1YKX", &o);
	radio_ysf_stats(&after);
	uint32_t blocks = (after.rx_played - before.rx_played) + (after.rx_concealed - before.rx_concealed);
	uint32_t late = after.rx_late - before.rx_late;
	CHECK(after.vw_dec_max_us == 1500 && after.vw_dec_avg_us == 1500 && after.vw_dec_worst_us == 1500,
	      "decode time: longest %u, mean %u, since start %u us (1500 each)", after.vw_dec_max_us, after.vw_dec_avg_us,
	      after.vw_dec_worst_us);
	CHECK(after.vw_dec_blocks == blocks && blocks >= NFR, "%u blocks timed, %u played or concealed",
	      after.vw_dec_blocks, blocks);
	CHECK(late >= 2 && late <= (uint32_t)o.stall_n, "%u blocks late of the %d run ahead of the air", late, o.stall_n);
	// The next over has its own figures; the longest since start stays
	micros_step = 700;
	send_vw("JA1YKX");
	radio_ysf_stats(&before);
	CHECK(before.vw_dec_max_us == 700 && before.vw_dec_avg_us == 700 && before.vw_dec_worst_us == 1500,
	      "second over: longest %u, mean %u, since start %u us", before.vw_dec_max_us, before.vw_dec_avg_us,
	      before.vw_dec_worst_us);
	CHECK(before.rx_late == after.rx_late, "nothing late in an over the receiver kept up with (%u)",
	      before.rx_late - after.rx_late);
	micros_step = 0;
	radio_ysf_stop();
}

// A VW over joined after its header: the callsigns come only with the terminator, the caller's radio ID with the
// frame after the header (CSD3) if that was caught, nothing at all otherwise (ysf-modem.md 4.1)
static void test_vw_late_entry(void)
{
	channel_vw(false);
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	// The first header and most of the second lost, the CSD3 frame heard (1,000 to 1,600 samples blank do that; 1,800
	// to 2,400 lose the CSD3 frame as well, the receiver's sync search taking the rest of it)
	vw_opts_t o = { .stall_at = -1, .blank = 1200u, .rem4 = "E0abc", .snap_at = 20 };
	send_vw_ex("JA1YKX", &o);
	CHECK(mid.rx == RYSF_RX_CALL && mid.mode == RYSF_MODE_VW, "mid-over: receiving VW (rx %u, mode %u)", mid.rx,
	      mid.mode);
	CHECK(mid.src[0] == '\0' && mid.dest[0] == '\0', "mid-over: no callsigns yet ('%s', '%s')", mid.src, mid.dest);
	CHECK(strcmp(mid.src_id, "E0abc") == 0, "mid-over: the caller's radio ID from CSD3: '%s'", mid.src_id);
	radio_ysf_view_t v;
	radio_ysf_view(&v);
	CHECK(strcmp(v.src, "JA1YKX") == 0 && strcmp(v.src_id, "E0abc") == 0, "after the terminator: '%s', ID '%s'", v.src,
	      v.src_id);
	// The CSD3 frame lost too: nothing names the caller until the terminator; the last over's ID is not kept
	o.blank = 2000u;
	send_vw_ex("JA1YKX", &o);
	CHECK(mid.rx == RYSF_RX_CALL && mid.src[0] == '\0' && mid.src_id[0] == '\0', "mid-over: '%s', ID '%s'", mid.src,
	      mid.src_id);
	// No radio ID set (spaces): none shown
	o.blank = 1200u;
	o.rem4 = NULL;
	send_vw_ex("JA1YKX", &o);
	CHECK(mid.rx == RYSF_RX_CALL && mid.src_id[0] == '\0', "no radio ID sent: ID '%s'", mid.src_id);
	radio_ysf_stop();
}

// --- our VW transmission, heard by an independent receiver ---

static uint32_t far_vw, far_vw_match, far_vw_silence, far_dn;
static uint8_t far_dt;
static bool far_rem;

static void fv_frame(void *ctx, const ysf_call_t *c, bool first)
{
	(void)ctx;
	(void)first;
	far_dt = c->fich.dt;
}

static void fv_call(void *ctx, const ysf_call_t *c)
{
	(void)ctx;
	far_rem = far_rem || ((c->have & YSF_HAVE_REM34) && memcmp(c->csd.rem[3], "E0123", 5) == 0);
}

static void fv_voice(void *ctx, const uint8_t bits[49], uint8_t q)
{
	(void)ctx;
	(void)bits;
	(void)q;
	far_dn++;
}

static void fv_voice_fr(void *ctx, const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t errs)
{
	(void)ctx;
	CHECK(errs == 0, "a clean path: nothing to correct");
	if (memcmp(imbe, imbe_silence, IMBE_BYTES) == 0) {
		far_vw_silence++;
		return;
	}
	far_vw_match += far_vw_match < NFR && memcmp(imbe, frames[far_vw_match], IMBE_BYTES) == 0;
	far_vw++;
}

static void test_transmit_vw(void)
{
	channel_vw(true);
	voc_present = false;		// VW needs no codec in flash
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	radio_ysf_view_t v;
	radio_ysf_view(&v);
	CHECK(v.tx_vw, "the view says PTT sends VW");
	CHECK(radio_ysf_tx_check() == TXDENY_NONE, "VW: PTT allowed without the board's codec (%u)", radio_ysf_tx_check());
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");

	static ysf_engine_t rx;
	far_vw = far_vw_match = far_vw_silence = far_dn = 0;
	far_rem = false;
	static const ysf_rx_cb_t cb = { fv_frame, fv_call, fv_voice, NULL, NULL, NULL };
	ysf_path_t p = { .rate = 8000 };
	ysf_engine_rx_start(&rx, &p, &cb);
	ysf_engine_rx_voice_fr(&rx, fv_voice_fr);

	enc_n = 0;
	uint32_t block = 0;
	bool done = false;
	for (int t = 0; t < 400 && !done; t++) {
		int16_t mic[160], spk[160];
		memset(mic, 0, sizeof mic);
		if (block < NFR) {
			memcpy(mic, speech + block * 160u, sizeof mic);
			block++;
		} else if (block == NFR) {
			radio_ysf_tx_release();
			block++;
		}
		stream_cb(mic, spk, NULL);
		int16_t pcm[160];
		tx_pull_fn(pcm, 160);
		ysf_engine_rx(&rx, pcm, 160);
		now_ms += 20;
		done = radio_ysf_tx_done(now_ms);
	}
	CHECK(done, "tx done after the terminator");
	// The transmit queue holds everything spoken before the first voice subframe: the preamble, both headers and the
	// CSD3 frame's data. One frame too few showed only as a word missing a frame at the far end
	radio_ysf_stats_t st;
	radio_ysf_stats(&st);
	CHECK(st.tx_overflows == 0 && st.tx_frames == NFR, "%u frames encoded, %u dropped with the queue full",
	      st.tx_frames, st.tx_overflows);
	CHECK(far_dt == YSF_DT_VOICE_FR && far_dn == 0, "the far end heard DT %u, %u DN frames", far_dt, far_dn);
	CHECK(far_vw_match == NFR && far_vw == NFR, "the far end: %u frames, %u of them our encoder's in order (%u silence)",
	      far_vw, far_vw_match, far_vw_silence);
	CHECK(far_rem, "CSD3 carried our radio ID");
	CHECK(enc_n == 0, "the board's vocoder encoded %u blocks", enc_n);
	radio_ysf_rx_again(now_ms);
	radio_ysf_stop();
	voc_present = true;

	// A DN channel without the codec still can't transmit
	channel_vw(false);
	voc_present = false;
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_check() == TXDENY_NO_CODEC, "DN without a codec: refused");
	radio_ysf_view(&v);
	CHECK(!v.tx_vw, "a DN channel's view");
	radio_ysf_stop();
	voc_present = true;
}

int main(void)
{
	reference();
	test_receive_vw(true);
	test_receive_vw(false);
	test_mixed();
	test_vw_memory();
	test_vw_timing();
	test_vw_late_entry();
	test_transmit_vw();
	sim_flash_free();
	printf("%s ysfVwRadioTest: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}
