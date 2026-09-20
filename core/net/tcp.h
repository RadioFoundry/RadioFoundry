// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A minimal TCP (RFC 9293 subset) for the radio's own services on the USB network link (docs/spec/tcp-services.md).
// Never over RF: the IP gateway hands it only segments to the radio's own address (ipGatewaySetTCP).
//
//   - TCP_CONNS connections in all, TCP_LISTENERS listening ports, active open for the radio's own clients
//   - MSS TCP_MSS (the gateway's frames are 600 bytes); no window scaling, SACK or timestamps; the MSS option only
//   - one transmit buffer per connection (TCP_TXBUF bytes from SND.UNA on): unsent and unacknowledged data, sent
//     again from SND.UNA when the retransmission timer expires (go back N, RFC 6298 RTO with Karn's rule and backoff)
//   - in-order receive only: a segment that doesn't start at RCV.NXT is dropped and acknowledged (the peer resends)
//   - RST for segments to no connection; RFC 5961 checks for RST and SYN on a connection (challenge ACK)
//   - a short TIME_WAIT (the oldest one is reused when a SYN needs its slot), keepalive probes on idle connections
//
// No RTOS calls and no hardware: the caller feeds segments (tcp_input) and time (tcp_poll) from one task, the host
// tests from a scripted peer. Applications are callbacks; they write with tcp_write (copied into the transmit buffer)
// and are told when room frees up (sent). The connection handle is an index, valid from opened (or tcp_connect)
// until closed or the application's own tcp_close / tcp_abort; after those the application hears nothing more.
#ifndef ODR_TCP_H
#define ODR_TCP_H

#include <stdbool.h>
#include <stdint.h>

#ifndef TCP_CONNS
#define TCP_CONNS		4	// connections in all states, TIME_WAIT included
#endif
#define TCP_LISTENERS		4
#define TCP_MSS			536	// 14 + 20 + 20 + 536 = 590 bytes, within the gateway's 600 byte frames
#ifndef TCP_TXBUF
#define TCP_TXBUF		(2 * TCP_MSS)	// two segments in flight: the peer's delayed ACK answers every second one
#endif
#define TCP_RTO_INIT_MS		1000u
#define TCP_RTO_MIN_MS		200u	// below RFC 6298's 1 s: a USB link's round trip is about a millisecond
#define TCP_RTO_MAX_MS		16000u
#define TCP_RETRIES		7	// retransmissions of data before the connection is dropped
#define TCP_SYN_RETRIES		4	// of our SYN (active open): about 31 s
#define TCP_TIME_WAIT_MS	2000u	// short: a local link, and the slots are few
#define TCP_FIN_WAIT_2_MS	10000u	// our side closed, the peer never sends its FIN
#define TCP_KEEPALIVE_MS	60000u	// first probe after this long without a segment from the peer
#define TCP_KEEPALIVE_INTVL_MS	10000u
#define TCP_KEEPALIVE_PROBES	4
#define TCP_HEADER		20
#define TCP_SEGMENT_MAX		(TCP_HEADER + 4 + TCP_MSS)	// with the MSS option on a SYN

typedef enum {
	TCP_CLOSED, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT,
	TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
} tcp_state_t;

typedef struct {
	void (*opened)(int c);				// passive open established, or an active open connected
	void (*recv)(int c, const uint8_t *data, int len);	// in order; never more than space() said
	int (*space)(int c);				// receive window to offer; NULL: 2 * TCP_MSS (takes all)
	void (*sent)(int c);				// data was acknowledged: room in the transmit buffer
	void (*peer_closed)(int c);			// the peer's FIN: no more data comes; close when done
	void (*closed)(int c, bool error);		// gone without our close: reset, timed out, refused (error), or
							// an orderly end the application didn't close itself (false)
} tcp_app_t;

// What TCP needs from the IP layer
typedef struct {
	uint8_t *(*buffer)(void);		// where to build a segment, TCP_SEGMENT_MAX bytes
	bool (*send)(uint32_t dst, int length);	// the segment in buffer() to dst from local_ip(); false: not sent
	uint32_t (*local_ip)(void);		// our address (pseudo header), 0 while there is none
} tcp_io_t;

typedef struct {
	uint32_t segs_in, segs_out, retransmits, rst_out, bad_checksum, accepted, refused, no_slot, timeouts;
} tcp_stats_t;

// Forget every connection and listener; salt varies the initial sequence numbers (e.g. the chip's unique ID)
void tcp_init(const tcp_io_t *io, uint32_t salt);
bool tcp_listen(uint16_t port, const tcp_app_t *app);
// Active open to ip:port: the handle, or -1 when no slot is free. opened() follows, or closed(c, true)
int tcp_connect(uint32_t ip, uint16_t port, const tcp_app_t *app, uint32_t now_ms);
// Copy up to len bytes into the transmit buffer; returns what was taken (0 when full or not open for sending)
int tcp_write(int c, const void *data, int len);
int tcp_space(int c);			// what tcp_write would take now
int tcp_unacked(int c);			// bytes still in the transmit buffer (sent or not)
void tcp_close(int c);			// FIN after the buffered data; the application is done with c
void tcp_abort(int c);			// RST now; the application is done with c
// The link went away: every connection is dropped without a segment (closed(c, true) for the open ones)
void tcp_abort_all(void);
// A segment from src (the IP payload), and the timers / pending output
void tcp_input(uint32_t src, const uint8_t *seg, int len, uint32_t now_ms);
void tcp_poll(uint32_t now_ms);

uint8_t tcp_state(int c);		// tcp_state_t
uint32_t tcp_remote_ip(int c);
uint16_t tcp_remote_port(int c);
uint16_t tcp_local_port(int c);
void tcp_get_stats(tcp_stats_t *s);
// Open connections (any state but CLOSED and TIME_WAIT)
int tcp_open_count(void);

// The Internet checksum over a segment with the IPv4 pseudo header (exposed for the tests)
uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, int len);

#endif
