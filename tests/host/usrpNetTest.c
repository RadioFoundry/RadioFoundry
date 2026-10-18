// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The PCM stream shared with a host audio session (core/usbaudio/audio_mux.c) and USRP on the radio's network address
// (core/usbaudio/usrp_net.c), against a fake board stream, IP gateway and radio core (docs/spec/usb-audio.md)
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "dmr/ipGateway.h"
#include "radio/radio.h"
#include "usbaudio/audio_mux.h"
#include "usbaudio/usrp.h"
#include "usbaudio/usrp_net.h"

static int failures;
static int checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t now;
uint32_t hal_millis(void) { return now; }

// ---- the board's stream ----

static struct {
	int starts, stops;
	bool streaming;
	bool fail;		// start() returns false (no codec clock)
	audio_block_cb_t cb;
} hw;
static bool a_start(audio_block_cb_t cb, void *ctx) { (void)ctx; hw.starts++; if (hw.fail) { return false; } hw.cb = cb; hw.streaming = true; return true; }
static void a_stop(void) { hw.stops++; hw.streaming = false; hw.cb = NULL; }
static void a_pcm(bool on) { (void)on; }
static const hal_audio_t fake_audio = { a_start, a_stop, a_pcm };

static int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];

static void block(void)
{
	if (hw.streaming && hw.cb != NULL) {
		hw.cb(mic, spk, NULL);
	}
}

// A radio user's callback: copies what it got as the microphone, writes 7s to the speaker
static int16_t prim_mic[AUDIO_BLOCK_SAMPLES];
static int prim_blocks;
static void prim(const int16_t *m, int16_t *s, void *ctx)
{
	(void)ctx;
	memcpy(prim_mic, m, sizeof prim_mic);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		s[i] = 7;
	}
	prim_blocks++;
}

static bool tap_inject(audio_tap_pull_t pull, audio_tap_mode_t mode) { (void)pull; (void)mode; return true; }
static hal_audio_tap_t tap = { .caps = AUDIO_TAP_FM_RX | AUDIO_TAP_FM_TX | AUDIO_TAP_DMR, .fm_tx_inject = tap_inject };

const board_t board = { .name = "fake", .audio = &fake_audio, .audio_tap = &tap };

// ---- the IP gateway ----

static ipGatewayUdpHandler bound;
static uint16_t bound_port;
static uint8_t local[IPGW_LOCAL_MAX];
#define MAX_SENT 64
static struct {
	uint32_t ip;
	uint16_t sport, dport;
	int len;
	uint8_t data[USRP_MAX_LEN];
} sent[MAX_SENT];
static int nsent;
static bool send_busy;
static uint32_t send_cost_ms;	// a blocking send: the clock runs and the audio interrupt delivers a block per 20 ms
static int sends;

bool ipGatewayUdpBind(uint16_t port, ipGatewayUdpHandler handler) { bound_port = port; bound = handler; return true; }
uint8_t *ipGatewayLocalPayload(void) { return local; }
uint32_t ipGatewayHostIP(void) { return 0x0A000002u; }
bool ipGatewayUdpSend(uint32_t ip, uint16_t sport, uint16_t dport, const uint8_t *p, int len)
{
	if (send_busy) {
		return false;
	}
	if (nsent < MAX_SENT && len <= USRP_MAX_LEN) {
		sent[nsent].ip = ip;
		sent[nsent].sport = sport;
		sent[nsent].dport = dport;
		sent[nsent].len = len;
		memcpy(sent[nsent].data, p, (size_t)len);
	}
	nsent++;
	sends++;
	for (uint32_t t = 0; t < send_cost_ms; t += 5) {
		now += 5;
		if ((t % 20) == 15) {
			block();
		}
	}
	return true;
}

// ---- the radio core ----

static radio_view_t view;
static int ptt_downs, ptt_ups, host_downs;
static bool keyed;
void radio_get_view(radio_view_t *v) { *v = view; }
void radio_req_ptt(bool down) { if (down) { ptt_downs++; } else { ptt_ups++; } keyed = down; }
void radio_req_ptt_host(bool down) { if (down) { host_downs++; } keyed = down; }

// ---- helpers ----

static const hal_audio_t *radio_audio;

static void reset(void)
{
	audio_mux_reset();
	memset(&hw, 0, sizeof hw);
	memset(&view, 0, sizeof view);
	view.ch.mode = RADIO_MODE_FM;
	view.state = RS_IDLE;
	nsent = 0;
	send_busy = false;
	send_cost_ms = 0;
	sends = 0;
	ptt_downs = ptt_ups = host_downs = 0;
	keyed = false;
	prim_blocks = 0;
	radio_audio = audio_mux_wrap(&fake_audio);
}

// One 20 ms block of time: the radio task's service, the stream's block, the host task's poll (every 5 ms)
static void tick(void)
{
	for (int i = 0; i < 4; i++) {
		now += 5;
		audio_mux_service(now);
		usrp_net_poll(now, true);
	}
	block();
}

static void peer_frame(uint32_t ip, bool keyup, uint32_t seq, int16_t level)
{
	static uint8_t buf[USRP_MAX_LEN];
	static int16_t pcm[USRP_FRAME_SAMPLES];
	for (int i = 0; i < USRP_FRAME_SAMPLES; i++) {
		pcm[i] = level;
	}
	usrp_header_t h = { .seq = seq, .keyup = keyup, .type = USRP_TYPE_VOICE };
	size_t n = usrp_pack(buf, sizeof buf, &h, pcm);
	bound(false, ip, 32001, 0x0A000001u, USRP_PORT, buf, (int)n);
}

// ---- tests ----

static void test_mux_passthrough(void)
{
	reset();
	// The radio's own user alone: start / stop reach the board as before, its callback runs
	CHECK(radio_audio != NULL && radio_audio != &fake_audio);
	CHECK(radio_audio->start(prim, NULL) && hw.starts == 1 && hw.streaming);
	block();
	CHECK(prim_blocks == 1 && spk[0] == 7);
	radio_audio->stop();
	CHECK(hw.stops == 1 && !hw.streaming);
	audio_mux_service(now);
	CHECK(hw.starts == 1);		// nothing for the host side

	// A host session keeps the stream running across the user's stop, and hands the receiver's audio over
	audio_mux_host_rx(AUDIO_MUX_RX_INPUT);
	audio_mux_service(now);
	CHECK(hw.starts == 2 && hw.streaming);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mic[i] = (int16_t)(100 + i);
	}
	block();
	CHECK(audio_mux_host_avail() == AUDIO_BLOCK_SAMPLES);
	int16_t got[AUDIO_BLOCK_SAMPLES];
	CHECK(audio_mux_host_read(got, AUDIO_BLOCK_SAMPLES) == AUDIO_BLOCK_SAMPLES && got[0] == 100 && got[159] == 259);
	CHECK(spk[0] == 0);		// nobody plays: silence out
	// A user starting now takes the callback without restarting the stream; its stop leaves it running
	CHECK(radio_audio->start(prim, NULL) && hw.starts == 2);
	block();
	CHECK(prim_blocks == 2 && spk[0] == 7);
	audio_mux_host_read(got, AUDIO_BLOCK_SAMPLES);
	radio_audio->stop();
	CHECK(hw.streaming && hw.stops == 1);
	// DMR: the host hears the decoded voice (the speaker side)
	audio_mux_host_rx(AUDIO_MUX_RX_OUTPUT);
	radio_audio->start(prim, NULL);
	block();
	audio_mux_host_read(got, AUDIO_BLOCK_SAMPLES);
	CHECK(got[0] == 7);
	radio_audio->stop();
	// Session over: the radio task stops the stream
	audio_mux_host_rx(AUDIO_MUX_RX_OFF);
	audio_mux_service(now);
	CHECK(!hw.streaming && hw.stops == 2);

	// A stream that won't start is retried once a second, not every poll
	hw.fail = true;
	audio_mux_host_rx(AUDIO_MUX_RX_INPUT);
	int s0 = hw.starts;
	audio_mux_service(now);
	audio_mux_service(now + 10);
	CHECK(hw.starts == s0 + 1);
	audio_mux_service(now + AUDIO_MUX_RETRY_MS);
	CHECK(hw.starts == s0 + 2);
	hw.fail = false;
	audio_mux_host_rx(AUDIO_MUX_RX_OFF);

	// The receive ring overflows by dropping the newest, never moving the reader
	reset();
	audio_mux_host_rx(AUDIO_MUX_RX_INPUT);
	audio_mux_service(now);
	for (int i = 0; i < 10; i++) {
		block();
	}
	audio_mux_stats_t st;
	audio_mux_stats(&st);
	CHECK(audio_mux_host_avail() == AUDIO_MUX_RX_CAP && st.rx_overflow == 10u * AUDIO_BLOCK_SAMPLES - AUDIO_MUX_RX_CAP);
}

static void test_mux_dmr_tx(void)
{
	reset();
	int16_t host[AUDIO_BLOCK_SAMPLES];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		host[i] = 1234;
		mic[i] = -5;
	}
	radio_audio->start(prim, NULL);
	audio_mux_host_tx(true, true);
	// Not primed (60 ms): the encoder gets silence, never the microphone
	audio_mux_host_push(host, AUDIO_BLOCK_SAMPLES);
	block();
	CHECK(prim_mic[0] == 0);
	for (int i = 0; i < 3; i++) {
		audio_mux_host_push(host, AUDIO_BLOCK_SAMPLES);
	}
	block();
	CHECK(prim_mic[0] == 1234 && prim_mic[159] == 1234);
	// Host done: the microphone again
	audio_mux_host_tx(false, false);
	block();
	CHECK(prim_mic[0] == -5);
	audio_mux_stats_t st;
	audio_mux_stats(&st);
	CHECK(st.tx_blocks == 2 && st.tx_pushes == 4);
	radio_audio->stop();

	// FM: the board pulls in its own pieces; the push never overruns the jitter buffer
	audio_mux_host_tx(true, false);
	for (int i = 0; i < 20; i++) {
		audio_mux_host_push(host, AUDIO_BLOCK_SAMPLES);
	}
	CHECK(audio_mux_host_tx_fill() == PCM_SLIP_CAP);
	int16_t out[40];
	audio_mux_fm_pull(out, 40);
	CHECK(out[0] == 1234);
	CHECK(audio_mux_host_tx_fill() == PCM_SLIP_CAP - 41);	// far above the target: one sample slipped
}

static void test_usrp_rx(void)
{
	reset();
	usrp_net_start();
	CHECK(bound_port == USRP_PORT && bound != NULL);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mic[i] = 321;
	}
	// The link up: the stream runs for the host, but nothing is sent while the squelch is closed
	for (int i = 0; i < 5; i++) {
		tick();
	}
	CHECK(hw.streaming && nsent == 0);
	// Squelch open: one frame per block to the host's address (no peer yet), from 34001 to 32001, keyup set
	view.state = RS_RX;
	for (int i = 0; i < 5; i++) {
		tick();
	}
	CHECK(nsent >= 4 && nsent <= 5);
	CHECK(sent[0].ip == 0x0A000002u && sent[0].sport == USRP_PORT && sent[0].dport == USRP_PEER_PORT);
	usrp_header_t h;
	int16_t pcm[USRP_FRAME_SAMPLES];
	size_t ns;
	CHECK(usrp_parse(sent[1].data, (size_t)sent[1].len, &h, pcm, &ns) == USRP_PARSE_OK);
	CHECK(h.keyup && h.type == USRP_TYPE_VOICE && ns == USRP_FRAME_SAMPLES && pcm[0] == 321);
	usrp_parse(sent[0].data, (size_t)sent[0].len, &h, pcm, &ns);
	uint32_t seq0 = h.seq;
	usrp_parse(sent[1].data, (size_t)sent[1].len, &h, pcm, &ns);
	CHECK(h.seq == seq0 + 1);
	// Squelch closed: one keyup 0 frame of silence, then nothing
	int n0 = nsent;
	view.state = RS_IDLE;
	for (int i = 0; i < 5; i++) {
		tick();
	}
	CHECK(nsent == n0 + 1);
	usrp_parse(sent[n0].data, (size_t)sent[n0].len, &h, pcm, &ns);
	CHECK(!h.keyup && pcm[0] == 0);
	// A peer that sends us anything valid gets the audio from then on
	peer_frame(0x0A000063u, false, 1, 0);
	view.state = RS_RX;
	tick();
	tick();
	CHECK(nsent > n0 + 1 && sent[nsent - 1].ip == 0x0A000063u);
	// ... until it has been silent for a minute
	now += USRP_PEER_IDLE_MS;
	tick();
	tick();
	CHECK(sent[nsent - 1].ip == 0x0A000002u);
	// A board whose stream doesn't carry FM receive audio: the squelch as keyup, but silence, never the microphone
	tap.caps = AUDIO_TAP_DMR;
	tick();
	tick();
	usrp_parse(sent[nsent - 1].data, (size_t)sent[nsent - 1].len, &h, pcm, &ns);
	CHECK(h.keyup && pcm[0] == 0);
	tap.caps = AUDIO_TAP_FM_RX | AUDIO_TAP_FM_TX | AUDIO_TAP_DMR;
	// Hotspot mode: no audio to the host
	view.hotspot = true;
	n0 = nsent;
	tick();
	tick();
	CHECK(nsent <= n0 + 1);		// at most the closing keyup 0 frame
	// The link down: the stream is let go
	view.hotspot = false;
	usrp_net_poll(now, false);
	audio_mux_service(now);
	CHECK(!hw.streaming);
}

// A link that sends slower than real time (115200 baud serial): one poll must return, not chase the audio
static void test_usrp_rx_slow_link(void)
{
	reset();
	usrp_net_start();
	for (int i = 0; i < 3; i++) {
		tick();
	}
	view.state = RS_RX;
	send_cost_ms = 31;
	// A backlog of ten blocks, then one poll
	for (int i = 0; i < 10; i++) {
		block();
	}
	now += 5;
	usrp_net_poll(now, true);
	CHECK(sends >= 1 && sends <= 2);
	CHECK(audio_mux_host_avail() < USRP_FRAME_SAMPLES);	// the backlog is gone, not kept for the next poll
	// The next polls come back at once and the state is refreshed: the squelch closing is seen
	send_cost_ms = 0;
	view.state = RS_IDLE;
	for (int i = 0; i < 4; i++) {
		tick();
	}
	CHECK(sends <= 4);
}

static void test_usrp_tx(void)
{
	reset();
	usrp_net_start();
	tick();
	// Keyup frames key the radio with the host's audio, which reaches the FM path once primed
	uint32_t seq = 100;
	for (int i = 0; i < 5; i++) {
		peer_frame(0x0A000063u, true, seq++, 2000);
		tick();
	}
	CHECK(host_downs == 1 && keyed);
	CHECK(audio_mux_host_tx_fill() >= AUDIO_MUX_TX_TARGET);
	int16_t out[40];
	audio_mux_fm_pull(out, 40);
	CHECK(out[0] == 2000 && out[39] == 2000);
	// While we transmit, the squelch isn't reported (our own receiver is off anyway)
	view.state = RS_RX;
	int n0 = nsent;
	tick();
	CHECK(nsent == n0);
	view.state = RS_IDLE;
	// Keyup 0: unkey
	peer_frame(0x0A000063u, false, seq++, 0);
	CHECK(!keyed && ptt_ups == 1);
	// The peer goes silent while keyed: the watchdog unkeys within USRP_KEYUP_TIMEOUT_MS
	peer_frame(0x0A000063u, true, seq++, 1000);
	CHECK(keyed && host_downs == 2);
	for (uint32_t t = 0; t < USRP_KEYUP_TIMEOUT_MS + 40u; t += 20) {
		tick();
	}
	CHECK(!keyed && ptt_ups == 2);
	// The link going down unkeys too
	peer_frame(0x0A000063u, true, seq++, 1000);
	CHECK(keyed);
	usrp_net_poll(now, false);
	CHECK(!keyed && ptt_ups == 3);

	// The peer still sends keyup: no new key-up from those (a transmission only starts on a keyup edge)
	peer_frame(0x0A000063u, true, seq++, 1000);
	CHECK(!keyed);
	peer_frame(0x0A000063u, false, seq++, 0);

	// Refused: hotspot mode, and FM on a board that can't take MCU audio; DMR always works
	tick();
	view.hotspot = true;
	tick();
	peer_frame(0x0A000063u, true, seq++, 1000);
	CHECK(!keyed);
	view.hotspot = false;
	peer_frame(0x0A000063u, false, seq++, 0);
	tick();
	tap.caps = AUDIO_TAP_DMR;
	peer_frame(0x0A000063u, true, seq++, 1000);
	CHECK(!keyed);
	peer_frame(0x0A000063u, false, seq++, 0);
	view.ch.mode = RADIO_MODE_DMR;
	tick();
	peer_frame(0x0A000063u, true, seq++, 1000);
	CHECK(keyed);
	peer_frame(0x0A000063u, false, seq++, 0);
	tap.caps = AUDIO_TAP_FM_RX | AUDIO_TAP_FM_TX | AUDIO_TAP_DMR;

	// Garbage counts as bad and doesn't change the peer
	static const uint8_t junk[8] = "NOTUSRP";
	bound(false, 0x0A0000AAu, 1, 0, USRP_PORT, junk, sizeof junk);
	char buf[1500];
	dl_text_t t;
	dl_text_init(&t, buf, sizeof buf);
	usrp_net_view(&t);
	buf[t.len] = '\0';
	CHECK(strstr(buf, "usrp.peer=10.0.0.99\n") != NULL && strstr(buf, "usrp.bad=1\n") != NULL);
	CHECK(strstr(buf, "usrp.refused=2\n") != NULL);
	if (failures) {
		printf("%s", buf);
	}
}

#if CONFIG_APRS_RX
// APRS's own receive ring: the stream's input while it asks, independent of a host session, the stream kept running
// for it alone; a late reader loses whole blocks
static void test_mux_aprs(void)
{
	reset();
	audio_mux_aprs_rx(true);
	audio_mux_service(now);
	CHECK(hw.starts == 1 && hw.streaming);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mic[i] = (int16_t)(300 + i);
	}
	block();
	int16_t got[AUDIO_MUX_APRS_CAP];
	CHECK(audio_mux_aprs_read(got, 50) == 50 && got[0] == 300 && got[49] == 349);
	CHECK(audio_mux_aprs_read(got, AUDIO_MUX_APRS_CAP) == AUDIO_BLOCK_SAMPLES - 50 && got[0] == 350);
	CHECK(audio_mux_host_avail() == 0);	// the host's ring isn't filled for APRS
	// A host session too: both get the block
	audio_mux_host_rx(AUDIO_MUX_RX_INPUT);
	block();
	CHECK(audio_mux_host_avail() == AUDIO_BLOCK_SAMPLES && audio_mux_aprs_read(got, 1) == 1);
	audio_mux_host_rx(AUDIO_MUX_RX_OFF);
	audio_mux_service(now);
	CHECK(hw.streaming);			// APRS still wants it
	// A radio user's stop leaves it running too
	CHECK(radio_audio->start(prim, NULL));
	radio_audio->stop();
	CHECK(hw.streaming);
	// Full: whole blocks dropped (1024 samples hold 6 blocks)
	audio_mux_aprs_read(got, AUDIO_MUX_APRS_CAP);
	for (int i = 0; i < 10; i++) {
		block();
	}
	CHECK(audio_mux_aprs_read(got, AUDIO_MUX_APRS_CAP) == 6u * AUDIO_BLOCK_SAMPLES);
	// Off: the stream stops, and on again nothing stale is read
	block();
	audio_mux_aprs_rx(false);
	audio_mux_service(now);
	CHECK(!hw.streaming);
	audio_mux_aprs_rx(true);
	CHECK(audio_mux_aprs_read(got, AUDIO_MUX_APRS_CAP) == 0);
	audio_mux_aprs_rx(false);
}
#endif

int main(void)
{
	test_mux_passthrough();
	test_mux_dmr_tx();
	test_usrp_rx();
	test_usrp_rx_slow_link();
	test_usrp_tx();
#if CONFIG_APRS_RX
	test_mux_aprs();
#endif
	printf("# usrpNetTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
