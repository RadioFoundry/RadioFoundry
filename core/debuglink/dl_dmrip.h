// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link over the air (docs/spec/debuglink.md 7): the datagram that carries a debug link frame to one radio
// through the DMR IP data path and back, its authentication, the replay check, which commands the air may send, and
// the detour runner (temporary settings a deadline reverts). Pure code: the glue is debuglink_dmrip.c.
//
// A datagram on UDP DL_DMRIP_PORT, both ways:
//   0  u8   version DL_DMRIP_VERSION
//   1  u8   flags DL_DMRIP_F_*
//   2  u24  the radio's DMR ID (little endian): the one addressed, or the one answering
//   5  u32  the session nonce the radio chose (a HELLO request: 0)
//   9  u32  the request counter (a reply: the request's; a HELLO reply: the last counter taken)
//   13 u8   request: chunks wanted again (bit i = chunk i, 0 = all); reply: chunk index << 4 | chunk count
//   14 ...  body: a debug link frame (A5 5A ...), or one chunk of the reply frame; a HELLO's: the host's challenge
//   n-8     SipHash-2-4 tag of bytes 0 .. n-9 under the radio's key
#ifndef DL_DMRIP_H
#define DL_DMRIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "debuglink/debuglink_proto.h"

// Little-endian fields of the datagrams and the detour's replies
static inline uint32_t dl_get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void dl_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

#define DL_DMRIP_PORT		3335u
#define DL_DMRIP_VERSION	0xD1u
#define DL_DMRIP_HDR		14u
#define DL_DMRIP_TAG		8u
#define DL_DMRIP_KEY		16u
#define DL_DMRIP_CHUNK		224u	// reply frame bytes in one datagram (the IP packet stays under 280 bytes)
#define DL_DMRIP_DGRAM_MAX	(DL_DMRIP_HDR + DL_DMRIP_CHUNK + DL_DMRIP_TAG)
#define DL_DMRIP_FRAME_MAX	(DL_HDR + DL_REPLY_MAX + 2u)
#define DL_DMRIP_CHUNKS_MAX	((DL_DMRIP_FRAME_MAX + DL_DMRIP_CHUNK - 1u) / DL_DMRIP_CHUNK)
#define DL_DMRIP_HELLO_MAX	16u	// challenge bytes a HELLO carries and its reply echoes

#define DL_DMRIP_F_REPLY	0x01u	// from the radio
#define DL_DMRIP_F_HELLO	0x02u	// session start: the reply gives the nonce and the last counter
#define DL_DMRIP_F_GONE		0x04u	// reply to a repeat whose reply the radio no longer holds: send it as new

_Static_assert(DL_DMRIP_CHUNKS_MAX <= 15, "chunk index and count share a byte");

typedef struct {
	uint8_t flags;
	uint32_t id;		// 24 bits
	uint32_t nonce;
	uint32_t counter;
	uint8_t chunk;
	const uint8_t *body;
	uint16_t body_len;
} dl_dmrip_msg_t;

// SipHash-2-4 (Aumasson and Bernstein), 64-bit output
uint64_t dl_siphash(const uint8_t key[DL_DMRIP_KEY], const uint8_t *p, size_t n);

// The radio's default key from its chip UID: two SipHash runs under a fixed public key, so a host that read the UID
// over the cable (INFO_CHIP) derives the same key
void dl_dmrip_kdf(const uint8_t *uid, size_t n, uint8_t key[DL_DMRIP_KEY]);

// Header and tag around m's body, which the caller placed at out + DL_DMRIP_HDR already (m->body is ignored).
// Returns the datagram's length
size_t dl_dmrip_seal(const uint8_t key[DL_DMRIP_KEY], const dl_dmrip_msg_t *m, uint8_t *out);

// A datagram checked: version, length, tag. m->body points into d
bool dl_dmrip_open(const uint8_t key[DL_DMRIP_KEY], const uint8_t *d, size_t n, dl_dmrip_msg_t *m);

// A request as it came in, for this radio (my_id): the tag holds, it is a request, and the ID it names is this radio's.
// A group, a broadcast or another radio's ID is DL_DMRIP_NOT_OURS. The IP destination was checked before: the gateway
// hands over only datagrams to the radio's own ID and address (ipGatewayUdpBindAir). m filled when DL_DMRIP_ACCEPT
enum { DL_DMRIP_ACCEPT, DL_DMRIP_BAD, DL_DMRIP_NOT_OURS };
int dl_dmrip_check(const uint8_t key[DL_DMRIP_KEY], const uint8_t *d, size_t n, uint32_t my_id, dl_dmrip_msg_t *m);

// Replay check: a counter above the last one taken is new (and taken), the last one again with the same request a
// repeat (the host lost the reply), anything else old: another request on the last counter is a second host in the
// session, whose request never ran and gets no answer. The nonce must be the session's. body: the request's
// fingerprint (dl_siphash of the body)
typedef struct {
	uint32_t nonce;		// 0: no session yet (only HELLO is answered)
	uint32_t last;
	uint64_t body;		// the last request taken
} dl_dmrip_replay_t;

enum { DL_DMRIP_NEW, DL_DMRIP_REPEAT, DL_DMRIP_OLD };
int dl_dmrip_replay(dl_dmrip_replay_t *r, uint32_t nonce, uint32_t counter, uint64_t body);

// A debug link frame in a body: true when whole and its CRC holds
bool dl_dmrip_frame(const uint8_t *b, size_t n, uint8_t *cmd, uint8_t *seq, const uint8_t **payload, uint16_t *len);

// Whether a request may come over the air (7.3). Reads (not the chip UID or memory) and the detour pass; a command that can cut the link (RADIO,
// PTT, KEY) only as an item of a detour; writes that no deadline undoes (registers, flash, codeplug, stored keys) and
// everything that restarts or hands the radio over never
bool dl_dmrip_air_allowed(uint8_t cmd, const uint8_t *req, uint16_t len);
// Where the answer to a request from DMR ID id with IPv4 source src_ip goes: src_ip (the sender's own address
// 12.<ID>, its host's 13.<ID>, or a host on a network the sender routes), so the answer takes the way back the request
// came; a source no answer can reach (0, multicast, broadcast, loopback) gives the sender's own address
uint32_t dl_dmrip_reply_ip(uint32_t id, uint32_t src_ip);

// --- the detour (7.4) ---

#define DL_DETOUR_RUN		1u	// seconds u16, then items: cmd u8, len u8, payload; cmd 0 waits (ms u16)
#define DL_DETOUR_EXTEND	2u	// seconds u16: the deadline moves to now + seconds
#define DL_DETOUR_RETURN	3u	// back now
#define DL_DETOUR_STATUS	4u
#define DL_DETOUR_MAX_S		300u	// the longest deadline from now
#define DL_DETOUR_BACKSTOP_MS	10000u	// the radio core's own deadline comes this much later
#define DL_DETOUR_ITEM_WAIT	0u	// an item's cmd: wait ms u16 before the next item
#define DL_DETOUR_BUSY_MS	1000u	// an item answered BUSY is tried again this long
#define DL_DETOUR_ARM_MS	2000u	// the radio core arms within this, else the run fails with BUSY
#define DL_DETOUR_CLEAR_MS	3000u	// over the air the first item waits for the RUN reply to go out, this long at most
#define DL_DETOUR_REPOST_MS	500u	// the return is asked again until the core says it is done

// An item a detour may run: the changes the return undoes (the list in 7.4)
bool dl_detour_item_allowed(uint8_t cmd, const uint8_t *p, uint8_t len);
// A RUN's items, checked whole: DL_ST_OK, DL_ST_BAD_ARG (malformed), DL_ST_REFUSED (an item not allowed)
uint8_t dl_detour_items_check(const uint8_t *items, uint16_t n);

enum {
	DL_DETOUR_IDLE,
	DL_DETOUR_ARMING,	// the core is asked to keep the home state
	DL_DETOUR_CLEAR,	// waiting for the RUN reply to leave (over the air)
	DL_DETOUR_RUNNING,	// items, then the wait for the deadline
	DL_DETOUR_RETURNING,	// the core is asked to go back
};

// What the runner needs from the radio. dispatch runs one item through the debug link's handler (DL_ST_*)
typedef struct {
	uint8_t (*dispatch)(uint8_t cmd, const uint8_t *p, uint8_t len);
	void (*mark)(uint32_t backstop_ms);	// RADIO_DETOUR_MARK (again: only the backstop moves)
	void (*ret)(void);			// RADIO_DETOUR_RETURN
	bool (*armed)(void);			// the core holds a home state
	void (*release)(void);			// an injected PTT up
	bool (*clear)(void);			// the RUN reply is out (NULL: no wait)
} dl_detour_ops_t;

typedef struct {
	uint8_t phase;
	uint8_t items[DL_REQ_MAX];
	uint8_t n, at;			// items' bytes, the next one's offset
	uint8_t index;			// the next item's number
	uint8_t result;			// DL_ST_* of the last run: OK, BUSY (not armed), or the failed item's status
	uint8_t failed;			// the failed item's number + 1, 0 none
	bool asked;			// the return was asked (RETURN), not the deadline
	bool clear_wait;		// the first item waits for the RUN reply to leave (over the air)
	uint32_t deadline_ms;
	uint32_t phase_ms;
	uint32_t wait_until;
	uint32_t busy_since;		// the current item answered BUSY since then (0 none)
	uint32_t runs;			// RUNs taken since start
} dl_detour_t;

void dl_detour_init(dl_detour_t *d);
// A DETOUR request: the reply payload in out (status first), its length. clear_wait: over the air
size_t dl_detour_request(dl_detour_t *d, const dl_detour_ops_t *o, const uint8_t *req, uint16_t len, bool clear_wait,
			 uint8_t *out, size_t cap, uint32_t now_ms);
// Often (the host link task's loop): arming, items, the deadline, the return
void dl_detour_poll(dl_detour_t *d, const dl_detour_ops_t *o, uint32_t now_ms);

#endif
