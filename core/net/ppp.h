// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A minimal PPP (RFC 1661) in HDLC-like framing (RFC 1662) with IPCP (RFC 1332), for IPv4 over a serial host link
// (CONFIG_NET_PPP, docs/spec/ppp.md). One link, the radio as the peer that knows both addresses.
//
//   framing   flag 0x7E, control escape 0x7D, the async control character map (ACCM) both ways, FCS-16 (bitwise:
//             the link is 115200 baud, so the 512 byte table buys nothing); full Address / Control and Protocol fields
//   LCP       we ask for MRU PPP_MRU, ACCM 0 and a Magic-Number; we take the peer's MRU, ACCM and Magic-Number and
//             Configure-Reject everything else (authentication, quality, PFC, ACFC, ...); Echo-Request answered,
//             Terminate both ways, Code-Reject for unknown codes, Protocol-Reject for unknown protocols (IPv6CP, CCP)
//   IPCP      IP-Address only: ours is ppp_local_ip(), the peer is Nak'd to ppp_peer_ip() (the addresses of the USB
//             network link, so the gateway and the services behave the same); everything else Configure-Rejected
//   leaving   the link ends (ppp_running() false) when LCP finishes: a Terminate either way, the negotiation timing
//             out, PPP_DEAD_MS without a good frame (an open link sends Echo-Requests after PPP_ECHO_MS of quiet, so a
//             live peer always answers), or a frame that starts with another protocol's first byte
//             (ppp_foreign_start): the bytes from there on belong to that protocol
//
// No RTOS calls and no hardware: the host link task feeds bytes (ppp_input) and time (ppp_poll); the host tests do
// the same against scripted peers and pppd.
#ifndef RFOUNDRY_PPP_H
#define RFOUNDRY_PPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PPP_HEADROOM		14	// writable bytes before a packet given to ppp_ip_in (room for an Ethernet header)
#ifndef PPP_MRU
#define PPP_MRU			584	// a received frame fits a 600 byte buffer (the IP gateway's frame size)
#endif
#define PPP_RX_BUFFER		(PPP_HEADROOM + PPP_MRU + 2)	// headroom, information field, FCS
#define PPP_RESTART_MS		3000u
#define PPP_MAX_CONFIGURE	10
#define PPP_MAX_TERMINATE	2
#define PPP_MAX_FAILURE		5	// Configure-Naks in a row before the options are rejected instead
#define PPP_ECHO_MS		15000u	// an open link quiet this long sends an Echo-Request (again after as long)
#define PPP_DEAD_MS		60000u	// no good frame for this long: the link ends

// Protocol states (RFC 1661 4.2); PPP_INITIAL: the layer below isn't up (IPCP before LCP opens)
enum { PPP_INITIAL = 0, PPP_CLOSED = 2, PPP_STOPPED, PPP_CLOSING, PPP_STOPPING, PPP_REQ_SENT, PPP_ACK_RCVD,
       PPP_ACK_SENT, PPP_OPENED };

typedef struct {
	uint32_t rx_frames, tx_frames, rx_errors;	// good frames in, frames out, bad FCS / too long / bad header
	uint8_t lcp, ipcp;				// states
} ppp_stats_t;

// Start a link. active: send our Configure-Request now (asked for by the host through another protocol); passive:
// wait for the peer's (its first frame is arriving). salt varies the Magic-Number. rx: PPP_RX_BUFFER bytes for the
// frame being received, the engine's until the link ends (the host link lends it the USB network adapter's frame
// buffer, idle while PPP runs)
void ppp_start(bool active, uint32_t salt, uint32_t now_ms, uint8_t *rx);
// Bytes from the serial port: returns how many were taken. Fewer than len, or ppp_running() false afterwards: the link
// ended and the rest belongs to the other protocols
size_t ppp_input(const uint8_t *buf, size_t len, uint32_t now_ms);
// Timers: restarts, echo, the dead peer. Every few ms while running
void ppp_poll(uint32_t now_ms);
// End the link from our side: Terminate-Request, then finished on the Terminate-Ack or the time-out
void ppp_close(uint32_t now_ms);
bool ppp_running(void);
// IPCP is open: IPv4 goes both ways
bool ppp_ip_up(void);
// An IPv4 packet to the peer; false when IPCP isn't open or it is longer than the peer's MRU (dropped)
bool ppp_send_ip(const uint8_t *packet, int length);
void ppp_stats(ppp_stats_t *s);

// Provided by the user of the engine
size_t ppp_write(const uint8_t *buf, size_t len);	// to the serial port
void ppp_ip_in(uint8_t *packet, int length);		// an IPv4 packet; PPP_HEADROOM writable bytes before it
uint32_t ppp_local_ip(void);				// our address, 0 = none yet
uint32_t ppp_peer_ip(void);				// the address the peer gets, 0 = none yet
bool ppp_foreign_start(uint8_t b);			// b starts a request of another protocol on the port

#endif
