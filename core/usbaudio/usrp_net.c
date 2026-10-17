// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// USRP voice over UDP on the radio's network address (usrp_net.h)
#include "config.h"
#if CONFIG_NET_USRP	// the whole file
#include "usbaudio/usrp_net.h"
#include "usbaudio/usrp.h"
#include "usbaudio/audio_mux.h"
#include "dmr/ipGateway.h"
#include "radio/radio.h"
#include "board.h"
#include "hal_time.h"

#define VIEW_MS		20u	// the radio's state (squelch, mode) read this often
#define SEND_BUDGET_MS	20u	// one poll sends audio for at most this long (send_rx)

typedef struct {
	uint32_t sent;		// frames sent to the peer
	uint32_t send_busy;	// frames dropped: the USB IN endpoint was busy
	uint32_t refused;	// keyups not transmitted: hotspot mode, or FM on a board that can't take MCU audio
	uint32_t ptt_downs;	// transmissions started for the peer
} usrp_net_stats_t;

static usrp_link_t link;
static usrp_net_stats_t st;
static radio_view_t view;	// static: too large for the host link task's stack
static uint32_t view_ms;
static bool view_read;
static uint32_t peer_ip;	// learned from the last valid frame, 0 = none (the host's address then)
static uint32_t peer_ms;
static bool tx_on;		// we asked the radio to transmit
static bool cos_sent;		// the last frame we sent had keyup 1
static bool rx_on;
static int16_t pcm[USRP_FRAME_SAMPLES];

static bool dmr_mode(void)
{
	return view.ch.mode == RADIO_MODE_DMR;
}

// FM receive audio only from a board whose stream carries it: elsewhere the stream's input is the microphone
static audio_mux_rx_t rx_source(void)
{
	const hal_audio_tap_t *tap = board.audio_tap;
	if (view.hotspot) {
		return AUDIO_MUX_RX_OFF;
	}
	if (dmr_mode()) {
		return AUDIO_MUX_RX_OUTPUT;
	}
	return (tap != NULL && (tap->caps & AUDIO_TAP_FM_RX) != 0) ? AUDIO_MUX_RX_INPUT : AUDIO_MUX_RX_SILENT;
}

static void unkey(void)
{
	if (tx_on) {
		tx_on = false;
		audio_mux_host_tx(false, false);
		radio_req_ptt(false);
	}
}

static void key(void)
{
	if (tx_on) {
		return;
	}
	const hal_audio_tap_t *tap = board.audio_tap;
	bool fm_ok = tap != NULL && tap->fm_tx_inject != NULL && (tap->caps & AUDIO_TAP_FM_TX) != 0;
	if (view.hotspot || (!dmr_mode() && !fm_ok)) {
		st.refused++;
		return;
	}
	tx_on = true;
	st.ptt_downs++;
	audio_mux_host_tx(true, dmr_mode());
	radio_req_ptt_host(true);
}

static void event(usrp_event_t ev)
{
	switch (ev) {
	case USRP_EV_KEY:
		key();
		break;
	case USRP_EV_UNKEY:
	case USRP_EV_TIMEOUT:
	case USRP_EV_TX_LIMIT:
		unkey();
		break;
	default:
		break;
	}
}

static void udp_in(bool air, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data,
		   int length)
{
	(void)sport;
	(void)dst;
	(void)dport;
	if (air || length < 0) {
		return;
	}
	uint32_t now = hal_millis();
	uint32_t bad = link.stats.bad;
	bool voice = false;
	usrp_event_t ev = usrp_link_rx(&link, data, (size_t)length, now, pcm, &voice);
	if (link.stats.bad == bad) {
		peer_ip = src;		// a valid frame: answer this peer
		peer_ms = now;
	}
	event(ev);
	if (voice && link.keyed && tx_on) {
		audio_mux_host_push(pcm, USRP_FRAME_SAMPLES);
	}
}

void usrp_net_start(void)
{
	usrp_link_init(&link, NULL);
	ipGatewayUdpBind(USRP_PORT, udp_in);
}

static uint32_t peer(uint32_t now_ms)
{
	if (peer_ip != 0 && (now_ms - peer_ms) >= USRP_PEER_IDLE_MS) {
		peer_ip = 0;
	}
	return peer_ip != 0 ? peer_ip : ipGatewayHostIP();
}

// The receiver's audio, one frame per 20 ms block, while the squelch is open and one keyup 0 frame after it closes
//
// A send that blocks (a serial link: a frame takes 30 ms to clock out, longer than the 20 ms of audio in it) must not
// keep this task in the loop: the audio arrives faster than it leaves, the loop never ends, and the host link never
// reads its port again or refreshes the squelch state. After SEND_BUDGET_MS the rest of the backlog is dropped.
static void send_rx(uint32_t now_ms)
{
	bool cos = view.state == RS_RX && !tx_on && !view.hotspot;
	uint32_t t0 = hal_millis();
	while (audio_mux_host_avail() >= USRP_FRAME_SAMPLES) {
		if ((hal_millis() - t0) >= SEND_BUDGET_MS) {
			while (audio_mux_host_avail() >= USRP_FRAME_SAMPLES) {
				audio_mux_host_read(pcm, USRP_FRAME_SAMPLES);
				st.send_busy++;
			}
			break;
		}
		audio_mux_host_read(pcm, USRP_FRAME_SAMPLES);
		if (!cos && !cos_sent) {
			continue;
		}
		uint32_t ip = peer(now_ms);
		if (ip == 0) {
			continue;
		}
		uint8_t *out = ipGatewayUdpPayload();
		size_t n = usrp_link_tx(&link, out, IPGW_UDP_PAYLOAD_MAX, cos ? pcm : NULL, cos, false);
		if (n != 0 && ipGatewayUdpSend(ip, USRP_PORT, USRP_PEER_PORT, out, (int)n)) {
			st.sent++;
			cos_sent = cos;
		} else {
			st.send_busy++;
		}
	}
}

void usrp_net_poll(uint32_t now_ms, bool up)
{
	if (!view_read || (now_ms - view_ms) >= VIEW_MS) {
		view_read = true;
		view_ms = now_ms;
		radio_get_view(&view);
	}
	if (!up) {
		unkey();
		if (rx_on) {
			rx_on = false;
			audio_mux_host_rx(AUDIO_MUX_RX_OFF);
		}
		cos_sent = false;
		return;
	}
	if (view.hotspot) {
		unkey();	// hotspot mode owns the modem (usb-audio.md 6.5)
	}
	event(usrp_link_tick(&link, now_ms));
	audio_mux_host_rx(rx_source());
	rx_on = !view.hotspot;
	send_rx(now_ms);
}

void usrp_net_view(dl_text_t *t)
{
	const usrp_link_stats_t *l = &link.stats;
	audio_mux_stats_t m;
	pcm_slip_stats_t s;

	audio_mux_stats(&m);
	audio_mux_slip_stats(&s);
	dl_text(t, "usrp.peer=%u.%u.%u.%u\nusrp.keyed=%u\nusrp.tx=%u\nusrp.cos=%u\nusrp.rx_on=%u\n",
		(unsigned)(peer_ip >> 24), (unsigned)((peer_ip >> 16) & 0xFFu), (unsigned)((peer_ip >> 8) & 0xFFu),
		(unsigned)(peer_ip & 0xFFu), link.keyed, tx_on, cos_sent, rx_on);
	dl_text(t, "usrp.frames=%u\nusrp.voice=%u\nusrp.lost=%u\nusrp.out_of_order=%u\nusrp.bad=%u\n",
		(unsigned)l->frames, (unsigned)l->voice, (unsigned)l->lost, (unsigned)l->out_of_order, (unsigned)l->bad);
	dl_text(t, "usrp.keys=%u\nusrp.timeouts=%u\nusrp.tx_limits=%u\nusrp.sent=%u\nusrp.send_busy=%u\nusrp.refused=%u\n",
		(unsigned)l->keys, (unsigned)l->timeouts, (unsigned)l->tx_limits, (unsigned)st.sent,
		(unsigned)st.send_busy, (unsigned)st.refused);
	dl_text(t, "usrp.ptt_downs=%u\nmux.blocks=%u\nmux.rx_samples=%u\nmux.rx_overflow=%u\nmux.tx_blocks=%u\n",
		(unsigned)st.ptt_downs, (unsigned)m.blocks, (unsigned)m.rx_samples, (unsigned)m.rx_overflow,
		(unsigned)m.tx_blocks);
	dl_text(t, "mux.fm_pulls=%u\nmux.starts=%u\nmux.start_fails=%u\nmux.tx_fill=%u\n", (unsigned)m.fm_pulls,
		(unsigned)m.starts, (unsigned)m.start_fails, (unsigned)audio_mux_host_tx_fill());
	dl_text(t, "slip.pushed=%u\nslip.underruns=%u\nslip.drops=%u\nslip.repeats=%u\nslip.primes=%u\n",
		(unsigned)s.pushed, (unsigned)s.underruns, (unsigned)s.drops, (unsigned)s.repeats, (unsigned)s.primes);
}

#endif	// CONFIG_NET_USRP
