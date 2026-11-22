// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Key fill between radios (CONFIG_KEY_FILL, docs/spec/key-fill.md): the protocol engine. X25519 with a commitment, a
 * six-digit code the users compare, then the key encrypted and authenticated under HKDF keys from the exchange.
 *
 * Pure: no RTOS, no allocation, no clock of its own. The session is the caller's (the UI keeps it on the RTOS heap
 * while the Key fill screen is open); the caller feeds received messages (kf_rx), the user's answers (kf_confirm,
 * kf_cancel) and the time (kf_tick), and sends what kf_tx hands it. X25519 runs inside kf_listen, kf_send and kf_rx:
 * call them from a task that may take a few hundred milliseconds (the UI task), never the radio task.
 */
#ifndef RFOUNDRY_KEYFILL_H
#define RFOUNDRY_KEYFILL_H

#include <stdbool.h>
#include <stdint.h>
#include "support/x25519.h"
#include "support/sha256.h"

#define KF_VERSION		1
#define KF_PORT			4101	// UDP source and destination port (key-fill.md 3)
#define KF_HEADER		6	// version, type, session ID
#define KF_TAG_LEN		16
#define KF_PAYLOAD_MAX		48	// class 2 with an 8-octet identity and a 32-octet key
#define KF_MSG_MAX		(KF_HEADER + 1 + KF_PAYLOAD_MAX + KF_TAG_LEN)	// KEY, the longest: 71
#define KF_RANDOM_LEN		36	// a session's randomness: the private key (32) and, for a sender, the session ID (4)
#define KF_CODE_MOD		1000000u

// Message types (4)
enum { KF_COMMIT = 1, KF_RPUB, KF_SPUB, KF_READY, KF_KEY, KF_DONE, KF_ABORT };

// DONE results (4.4)
enum {
	KF_STORED = 0,
	KF_RES_NO_SYSTEM = 1,		// no system of that type and identity
	KF_RES_AMBIGUOUS = 2,		// an empty identity and more than one system of the type
	KF_RES_UNSUPPORTED = 3,		// a kind of key this build doesn't store
	KF_RES_BAD_PAYLOAD = 4,
	KF_RES_WRITE_FAILED = 5,
};

// ABORT reasons (4.4); also why a session failed here
enum {
	KF_WHY_NONE = 0,
	KF_WHY_CANCELED = 1,
	KF_WHY_MISMATCH = 2,		// the user said the codes differ
	KF_WHY_TIMEOUT = 3,
	KF_WHY_CHECK = 4,		// a tag, the commitment or the shared secret failed
	KF_WHY_BUSY = 5,		// the other radio has a session already
	KF_WHY_REFUSED = 6,		// local only: DONE with a result other than stored
	KF_WHY_SEND = 7,		// local only: a message couldn't be sent (the transport says)
};

typedef enum {
	KF_ST_IDLE,
	KF_ST_LISTEN,		// R: waiting for COMMIT
	KF_ST_SENT_COMMIT,	// S: waiting for RPUB
	KF_ST_SENT_RPUB,	// R: waiting for SPUB
	KF_ST_COMPARE,		// both: the code is up, waiting for the user (S also takes READY here)
	KF_ST_SENT_READY,	// R: the user said the codes match, waiting for KEY
	KF_ST_WAIT_READY,	// S: the user said the codes match, waiting for READY
	KF_ST_SENT_KEY,		// S: waiting for DONE
	KF_ST_STORING,		// R: KEY checked, the payload waits for the caller to store it (kf_payload, kf_stored)
	KF_ST_DONE,
	KF_ST_FAILED,
} kf_state_t;

// Times (5), ms
#define KF_LISTEN_MS		(5u * 60u * 1000u)
#define KF_STEP_MS		30000u
#define KF_COMPARE_MS		(2u * 60u * 1000u)

// The sender's payload, asked for only when KEY is built (5: the key leaves the store as late as it can): its length,
// at most cap octets into out; 0 or less fails the session
typedef int (*kf_payload_fn)(uint8_t *out, int cap, void *ctx);

typedef struct {
	uint32_t rx;		// messages handed to kf_rx
	uint32_t dropped;	// not for this state, another sender or session, or malformed
	uint32_t bad_tag;
} kf_stats_t;

typedef struct {
	// What the caller reads
	uint8_t state;		// kf_state_t
	uint8_t role;		// 0 sender, 1 receiver
	uint8_t why;		// KF_WHY_*: why the session failed
	uint8_t result;		// the DONE result (both sides)
	bool by_peer;		// the failure came as the other radio's ABORT
	bool kcv_ok;		// kcv holds the check value DONE carried (stored)
	uint8_t kcv[3];
	uint32_t own_id, peer_id;
	uint32_t code;		// the six-digit code, from Compare on
	kf_stats_t stats;

	// The engine's
	bool own_ok, peer_ready;
	uint8_t tx_len;
	uint8_t payload_len;
	uint32_t sid;
	uint32_t state_at, compare_at;
	uint32_t tx_dst;
	kf_payload_fn payload_fn;
	void *payload_ctx;
	uint8_t priv[X25519_LEN];
	uint8_t own_pub[X25519_LEN];
	uint8_t peer_pub[X25519_LEN];
	uint8_t commit[SHA256_LEN];	// R: the commitment COMMIT carried
	uint8_t t[SHA256_LEN];		// the transcript hash
	uint8_t k_mac[SHA256_LEN];
	uint8_t k_enc[SHA256_LEN];
	uint8_t payload[KF_PAYLOAD_MAX];	// R: the decrypted payload while Storing
	uint8_t tx[KF_MSG_MAX];
	x25519_ws_t ws;
} kf_session_t;

// R: wait for a sender. rnd: KF_RANDOM_LEN octets of the entropy pool (support/entropy.h)
void kf_listen(kf_session_t *s, uint32_t own_id, const uint8_t rnd[KF_RANDOM_LEN], uint32_t now);
// S: offer a key to peer_id. fn gives the payload when KEY is built. COMMIT waits in kf_tx. False (s idle) for an ID
// out of range or our own
bool kf_send(kf_session_t *s, uint32_t own_id, uint32_t peer_id, kf_payload_fn fn, void *ctx,
	     const uint8_t rnd[KF_RANDOM_LEN], uint32_t now);
// A datagram's payload from src, received on KF_PORT
void kf_rx(kf_session_t *s, uint32_t src, const uint8_t *m, int len, uint32_t now);
// The user's answer in Compare: same = the codes match
void kf_confirm(kf_session_t *s, bool same, uint32_t now);
// The user left: ABORT (canceled) to the other radio when there is one
void kf_cancel(kf_session_t *s, uint32_t now);
void kf_tick(kf_session_t *s, uint32_t now);
// The transport couldn't send the last message: the session fails (no ABORT: it wouldn't go either)
void kf_send_failed(kf_session_t *s);
// The message to send now, if any: its length (0: none), to *dst. Taken once
int kf_tx(kf_session_t *s, uint8_t out[KF_MSG_MAX], uint32_t *dst);

// R, in Storing: the decrypted payload (4.3), valid until kf_stored
const uint8_t *kf_payload(const kf_session_t *s, int *len);
// R: the store's answer: result (KF_STORED or a KF_RES_*) and, when stored, the check value of the stored key. DONE
// goes to the sender; the payload is wiped
void kf_stored(kf_session_t *s, uint8_t result, const uint8_t kcv[3], uint32_t now);

// Every secret and the rest of the session, zeroed
void kf_wipe(kf_session_t *s);
// The code as "482 913"
void kf_code_text(uint32_t code, char out[8]);

// --- The payload (4.3), for the caller that stores and the one that sends ---
typedef struct {
	uint8_t cls;		// 1 privacy slot, 2 traffic key
	uint8_t type;		// privacy type, or system type
	uint8_t slot;		// class 1
	uint8_t alg;		// class 2
	uint16_t key_id;
	uint8_t flags;
	uint8_t id_len;		// class 2: the system's identity
	uint8_t identity[8];
	uint8_t key_len;
	const uint8_t *key;	// into the payload parsed
} kf_item_t;
// Parse len octets (strict: every length must agree); false for a payload that doesn't
bool kf_item_parse(const uint8_t *p, int len, kf_item_t *it);
// Build a payload: its length (0 when it, or the key, doesn't fit)
int kf_item_build(const kf_item_t *it, uint8_t *out, int cap);

#endif
