// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key fill's protocol engine (keyfill/keyfill.h, docs/spec/key-fill.md)
#include "config.h"
#if CONFIG_KEY_FILL	// the whole file
#include <string.h>
#include "keyfill/keyfill.h"
#include "support/hkdf.h"

// The labels of 4 and 4.1, ASCII without a terminator
static const char L_COMMIT[] = "RFKF1 commit";
static const char L_TRANSCRIPT[] = "RFKF1 transcript";
static const char L_KEYS[] = "RFKF1 keys";
static const char L_PAYLOAD[] = "RFKF1 payload";
#define LABEL(l)	(l), (sizeof(l) - 1u)

#define PUB_MSG		(KF_HEADER + X25519_LEN)	// COMMIT, RPUB, SPUB: 38
#define READY_LEN	(KF_HEADER + KF_TAG_LEN)	// 22
#define DONE_LEN	(KF_HEADER + 4 + KF_TAG_LEN)	// 26
#define ABORT_LEN	(KF_HEADER + 1)			// 7
#define ID_MAX		0xFFFFFFu

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void enter(kf_session_t *s, kf_state_t st, uint32_t now)
{
	s->state = (uint8_t)st;
	s->state_at = now;
}

static int header(uint32_t sid, uint8_t type, uint8_t *out)
{
	out[0] = KF_VERSION;
	out[1] = type;
	put32(&out[2], sid);
	return KF_HEADER;
}

static void queue(kf_session_t *s, int len, uint32_t dst)
{
	s->tx_len = (uint8_t)len;
	s->tx_dst = dst;
}

// Everything secret the session holds; the public keys, the IDs and the outcome stay for the screen
static void wipe_secrets(kf_session_t *s)
{
	crypto_wipe(s->priv, sizeof s->priv);
	crypto_wipe(s->commit, sizeof s->commit);
	crypto_wipe(s->t, sizeof s->t);
	crypto_wipe(s->k_mac, sizeof s->k_mac);
	crypto_wipe(s->k_enc, sizeof s->k_enc);
	crypto_wipe(s->payload, sizeof s->payload);
	s->payload_len = 0;
	crypto_wipe(&s->ws, sizeof s->ws);
	s->payload_fn = NULL;
	s->payload_ctx = NULL;
}

static bool active(const kf_session_t *s)
{
	return s->state != KF_ST_IDLE && s->state != KF_ST_DONE && s->state != KF_ST_FAILED;
}

// The session ends here. tell: ABORT with why to the other radio, when there is one
static void fail(kf_session_t *s, uint8_t why, bool tell, uint32_t now)
{
	s->tx_len = 0;
	if (tell && s->peer_id != 0) {
		int n = header(s->sid, KF_ABORT, s->tx);
		s->tx[n++] = why;
		queue(s, n, s->peer_id);
	}
	s->why = why;
	wipe_secrets(s);
	enter(s, KF_ST_FAILED, now);
}

static void commit_hash(const uint8_t pub[X25519_LEN], uint8_t out[SHA256_LEN])
{
	sha256_t h;
	sha256_init(&h);
	sha256_update(&h, LABEL(L_COMMIT));
	sha256_update(&h, pub, X25519_LEN);
	sha256_final(&h, out);
}

// 4.1: the shared secret, the transcript hash and the keys; the private key is wiped, used once. False: the shared
// secret is all zeros
static bool derive(kf_session_t *s)
{
	uint8_t shared[X25519_LEN], prk[SHA256_LEN], okm[2 * SHA256_LEN + 4];
	bool ok = x25519(shared, s->priv, s->peer_pub, &s->ws);
	crypto_wipe(s->priv, sizeof s->priv);
	if (!ok) {
		crypto_wipe(shared, sizeof shared);
		return false;
	}
	bool sender = s->role == 0;
	uint32_t id_s = sender ? s->own_id : s->peer_id, id_r = sender ? s->peer_id : s->own_id;
	uint8_t ids[10];
	put32(&ids[0], s->sid);
	ids[4] = (uint8_t)(id_s >> 16);
	ids[5] = (uint8_t)(id_s >> 8);
	ids[6] = (uint8_t)id_s;
	ids[7] = (uint8_t)(id_r >> 16);
	ids[8] = (uint8_t)(id_r >> 8);
	ids[9] = (uint8_t)id_r;
	sha256_t h;
	sha256_init(&h);
	sha256_update(&h, LABEL(L_TRANSCRIPT));
	sha256_update(&h, ids, sizeof ids);
	sha256_update(&h, sender ? s->own_pub : s->peer_pub, X25519_LEN);
	sha256_update(&h, sender ? s->peer_pub : s->own_pub, X25519_LEN);
	sha256_final(&h, s->t);
	hkdf_extract(s->t, sizeof s->t, shared, sizeof shared, prk);
	(void)hkdf_expand(prk, LABEL(L_KEYS), okm, sizeof okm);
	memcpy(s->k_mac, okm, SHA256_LEN);
	memcpy(s->k_enc, okm + SHA256_LEN, SHA256_LEN);
	s->code = get32(okm + 2 * SHA256_LEN) % KF_CODE_MOD;
	crypto_wipe(shared, sizeof shared);
	crypto_wipe(prk, sizeof prk);
	crypto_wipe(okm, sizeof okm);
	return true;
}

// 4.2: HMAC(k_mac, msg | T), first 16 octets
static void tag(const kf_session_t *s, const uint8_t *msg, int len, uint8_t out[KF_TAG_LEN])
{
	hmac_sha256_t h;
	uint8_t full[SHA256_LEN];
	hmac_sha256_init(&h, s->k_mac, sizeof s->k_mac);
	hmac_sha256_update(&h, msg, (size_t)len);
	hmac_sha256_update(&h, s->t, sizeof s->t);
	hmac_sha256_final(&h, full);
	memcpy(out, full, KF_TAG_LEN);
	crypto_wipe(full, sizeof full);
}

static bool tag_ok(kf_session_t *s, const uint8_t *msg, int len)
{
	uint8_t want[KF_TAG_LEN];
	tag(s, msg, len - KF_TAG_LEN, want);
	bool ok = crypto_equal(want, msg + len - KF_TAG_LEN, KF_TAG_LEN);
	if (!ok)
		s->stats.bad_tag++;
	return ok;
}

static void keystream(const kf_session_t *s, uint8_t *ks, int n)
{
	(void)hkdf_expand(s->k_enc, LABEL(L_PAYLOAD), ks, (size_t)n);	// k_enc is a uniform key: a PRK as it is
}

// S: the user and READY both said yes: the payload from the store, encrypted, tagged, to R
static void send_key(kf_session_t *s, uint32_t now)
{
	uint8_t pt[KF_PAYLOAD_MAX], ks[KF_PAYLOAD_MAX];
	int n = s->payload_fn != NULL ? s->payload_fn(pt, (int)sizeof pt, s->payload_ctx) : 0;
	if (n <= 0 || n > KF_PAYLOAD_MAX) {
		crypto_wipe(pt, sizeof pt);
		fail(s, KF_WHY_CANCELED, true, now);	// the key is gone from the store since the start
		return;
	}
	uint8_t *m = s->tx;
	int h = header(s->sid, KF_KEY, m);
	m[h] = (uint8_t)n;
	keystream(s, ks, n);
	for (int i = 0; i < n; i++)
		m[h + 1 + i] = (uint8_t)(pt[i] ^ ks[i]);
	tag(s, m, h + 1 + n, &m[h + 1 + n]);
	queue(s, h + 1 + n + KF_TAG_LEN, s->peer_id);
	crypto_wipe(pt, sizeof pt);
	crypto_wipe(ks, sizeof ks);
	crypto_wipe(s->k_enc, sizeof s->k_enc);
	enter(s, KF_ST_SENT_KEY, now);
}

void kf_listen(kf_session_t *s, uint32_t own_id, const uint8_t rnd[KF_RANDOM_LEN], uint32_t now)
{
	kf_wipe(s);
	s->role = 1;
	s->own_id = own_id;
	memcpy(s->priv, rnd, X25519_LEN);
	x25519_public(s->own_pub, s->priv, &s->ws);
	enter(s, KF_ST_LISTEN, now);
}

bool kf_send(kf_session_t *s, uint32_t own_id, uint32_t peer_id, kf_payload_fn fn, void *ctx,
	     const uint8_t rnd[KF_RANDOM_LEN], uint32_t now)
{
	kf_wipe(s);
	if (peer_id == 0 || peer_id > ID_MAX || peer_id == own_id || own_id == 0 || own_id > ID_MAX || fn == NULL)
		return false;
	s->role = 0;
	s->own_id = own_id;
	s->peer_id = peer_id;
	s->payload_fn = fn;
	s->payload_ctx = ctx;
	memcpy(s->priv, rnd, X25519_LEN);
	s->sid = get32(rnd + X25519_LEN);
	x25519_public(s->own_pub, s->priv, &s->ws);
	int n = header(s->sid, KF_COMMIT, s->tx);
	commit_hash(s->own_pub, &s->tx[n]);
	queue(s, PUB_MSG, peer_id);
	enter(s, KF_ST_SENT_COMMIT, now);
	return true;
}

// A COMMIT from a third radio while this one is busy: ABORT "busy" to it, under its own session ID, when nothing of
// ours waits to go
static void busy(kf_session_t *s, uint32_t src, uint32_t sid)
{
	if (s->tx_len != 0)
		return;
	int n = header(sid, KF_ABORT, s->tx);
	s->tx[n++] = KF_WHY_BUSY;
	queue(s, n, src);
}

void kf_rx(kf_session_t *s, uint32_t src, const uint8_t *m, int len, uint32_t now)
{
	s->stats.rx++;
	if (m == NULL || len < KF_HEADER || len > KF_MSG_MAX || m[0] != KF_VERSION || src == 0 || src > ID_MAX ||
	    src == s->own_id || !active(s)) {
		s->stats.dropped++;
		return;
	}
	uint8_t type = m[1];
	uint32_t sid = get32(&m[2]);
	bool ours = s->peer_id != 0 && src == s->peer_id && sid == s->sid;
	if (type == KF_ABORT) {
		if (len != ABORT_LEN || !ours) {
			s->stats.dropped++;
			return;
		}
		s->tx_len = 0;
		s->by_peer = true;
		s->why = m[KF_HEADER];
		wipe_secrets(s);
		enter(s, KF_ST_FAILED, now);
		return;
	}
	if (type == KF_COMMIT && s->state != KF_ST_LISTEN) {
		if (!ours && len == PUB_MSG)
			busy(s, src, sid);
		s->stats.dropped++;
		return;
	}
	if (s->state != KF_ST_LISTEN && !ours) {
		s->stats.dropped++;
		return;
	}
	switch (s->state) {
	case KF_ST_LISTEN:	// R
		if (type != KF_COMMIT || len != PUB_MSG)
			break;
		s->peer_id = src;
		s->sid = sid;
		memcpy(s->commit, &m[KF_HEADER], SHA256_LEN);
		memcpy(&s->tx[header(s->sid, KF_RPUB, s->tx)], s->own_pub, X25519_LEN);
		queue(s, PUB_MSG, src);
		enter(s, KF_ST_SENT_RPUB, now);
		return;
	case KF_ST_SENT_COMMIT:	// S
		if (type != KF_RPUB || len != PUB_MSG)
			break;
		memcpy(s->peer_pub, &m[KF_HEADER], X25519_LEN);
		if (!derive(s)) {
			fail(s, KF_WHY_CHECK, true, now);
			return;
		}
		memcpy(&s->tx[header(s->sid, KF_SPUB, s->tx)], s->own_pub, X25519_LEN);
		queue(s, PUB_MSG, s->peer_id);
		s->compare_at = now;
		enter(s, KF_ST_COMPARE, now);
		return;
	case KF_ST_SENT_RPUB: {	// R
		if (type != KF_SPUB || len != PUB_MSG)
			break;
		uint8_t h[SHA256_LEN];
		commit_hash(&m[KF_HEADER], h);
		bool kept = crypto_equal(h, s->commit, SHA256_LEN);
		memcpy(s->peer_pub, &m[KF_HEADER], X25519_LEN);
		if (!kept || !derive(s)) {
			fail(s, KF_WHY_CHECK, true, now);
			return;
		}
		s->compare_at = now;
		enter(s, KF_ST_COMPARE, now);
		return;
	}
	case KF_ST_COMPARE:	// S takes READY before its own user answers
	case KF_ST_WAIT_READY:
		if (s->role != 0 || type != KF_READY || len != READY_LEN)
			break;
		if (!tag_ok(s, m, len)) {
			fail(s, KF_WHY_CHECK, true, now);
			return;
		}
		s->peer_ready = true;
		if (s->state == KF_ST_WAIT_READY)
			send_key(s, now);
		return;
	case KF_ST_SENT_READY: {	// R
		if (type != KF_KEY || len < KF_HEADER + 1 + KF_TAG_LEN)
			break;
		int n = m[KF_HEADER];
		if (n < 1 || n > KF_PAYLOAD_MAX || len != KF_HEADER + 1 + n + KF_TAG_LEN)
			break;
		if (!tag_ok(s, m, len)) {
			fail(s, KF_WHY_CHECK, true, now);
			return;
		}
		uint8_t ks[KF_PAYLOAD_MAX];
		keystream(s, ks, n);
		for (int i = 0; i < n; i++)
			s->payload[i] = (uint8_t)(m[KF_HEADER + 1 + i] ^ ks[i]);
		s->payload_len = (uint8_t)n;
		crypto_wipe(ks, sizeof ks);
		crypto_wipe(s->k_enc, sizeof s->k_enc);
		enter(s, KF_ST_STORING, now);
		return;
	}
	case KF_ST_SENT_KEY:	// S
		if (type != KF_DONE || len != DONE_LEN)
			break;
		if (!tag_ok(s, m, len)) {
			fail(s, KF_WHY_CHECK, true, now);
			return;
		}
		s->result = m[KF_HEADER];
		memcpy(s->kcv, &m[KF_HEADER + 1], sizeof s->kcv);
		s->kcv_ok = s->result == KF_STORED;
		wipe_secrets(s);
		if (s->result == KF_STORED) {
			enter(s, KF_ST_DONE, now);
		} else {
			s->why = KF_WHY_REFUSED;
			enter(s, KF_ST_FAILED, now);
		}
		return;
	default:
		break;
	}
	s->stats.dropped++;	// not the message this state waits for: a repeat, or out of turn
}

void kf_confirm(kf_session_t *s, bool same, uint32_t now)
{
	if (s->state != KF_ST_COMPARE)
		return;
	if (!same) {
		fail(s, KF_WHY_MISMATCH, true, now);
		return;
	}
	if (s->role == 1) {
		int n = header(s->sid, KF_READY, s->tx);
		tag(s, s->tx, n, &s->tx[n]);
		queue(s, READY_LEN, s->peer_id);
		enter(s, KF_ST_SENT_READY, now);
		return;
	}
	s->own_ok = true;
	if (s->peer_ready)
		send_key(s, now);
	else
		enter(s, KF_ST_WAIT_READY, now);
}

void kf_cancel(kf_session_t *s, uint32_t now)
{
	if (active(s))
		fail(s, KF_WHY_CANCELED, s->state != KF_ST_LISTEN, now);
}

void kf_tick(kf_session_t *s, uint32_t now)
{
	uint32_t until;
	switch (s->state) {
	case KF_ST_LISTEN: until = s->state_at + KF_LISTEN_MS; break;
	case KF_ST_SENT_COMMIT:
	case KF_ST_SENT_RPUB:
	case KF_ST_SENT_KEY: until = s->state_at + KF_STEP_MS; break;
	case KF_ST_COMPARE:
	case KF_ST_WAIT_READY: until = s->compare_at + KF_COMPARE_MS; break;
	case KF_ST_SENT_READY: until = s->compare_at + KF_COMPARE_MS + KF_STEP_MS; break;	// the sender's user may be slow
	default: return;
	}
	if ((int32_t)(now - until) >= 0)
		fail(s, KF_WHY_TIMEOUT, s->state != KF_ST_LISTEN, now);
}

void kf_send_failed(kf_session_t *s)
{
	if (active(s))
		fail(s, KF_WHY_SEND, false, s->state_at);
}

int kf_tx(kf_session_t *s, uint8_t out[KF_MSG_MAX], uint32_t *dst)
{
	int n = s->tx_len;
	if (n == 0)
		return 0;
	memcpy(out, s->tx, (size_t)n);
	*dst = s->tx_dst;
	s->tx_len = 0;
	crypto_wipe(s->tx, sizeof s->tx);
	return n;
}

const uint8_t *kf_payload(const kf_session_t *s, int *len)
{
	if (s->state != KF_ST_STORING) {
		*len = 0;
		return NULL;
	}
	*len = s->payload_len;
	return s->payload;
}

void kf_stored(kf_session_t *s, uint8_t result, const uint8_t kcv[3], uint32_t now)
{
	if (s->state != KF_ST_STORING)
		return;
	uint8_t *m = s->tx;
	int n = header(s->sid, KF_DONE, m);
	m[n] = result;
	if (result == KF_STORED && kcv != NULL)
		memcpy(&m[n + 1], kcv, 3);
	else
		memset(&m[n + 1], 0, 3);
	tag(s, m, n + 4, &m[n + 4]);
	queue(s, DONE_LEN, s->peer_id);
	s->result = result;
	s->kcv_ok = result == KF_STORED;
	memcpy(s->kcv, &m[n + 1], 3);
	wipe_secrets(s);
	if (result == KF_STORED) {
		enter(s, KF_ST_DONE, now);
	} else {
		s->why = KF_WHY_REFUSED;
		enter(s, KF_ST_FAILED, now);
	}
}

void kf_wipe(kf_session_t *s)
{
	crypto_wipe(s, sizeof *s);
}

void kf_code_text(uint32_t code, char out[8])
{
	code %= KF_CODE_MOD;
	for (int i = 6; i >= 0; i--) {
		if (i == 3) {
			out[i] = ' ';
			continue;
		}
		out[i] = (char)('0' + code % 10u);
		code /= 10u;
	}
	out[7] = '\0';
}

bool kf_item_parse(const uint8_t *p, int len, kf_item_t *it)
{
	memset(it, 0, sizeof *it);
	if (p == NULL || len < 1)
		return false;
	it->cls = p[0];
	if (it->cls == 1) {
		if (len != 7 || p[2] < 1 || p[2] > 4)
			return false;
		it->type = p[1];
		it->slot = p[2];
		it->key_len = 4;
		it->key = &p[3];
		return true;
	}
	if (it->cls != 2 || len < 9)
		return false;
	it->type = p[1];
	it->alg = p[2];
	it->key_id = (uint16_t)(p[3] | p[4] << 8);
	it->flags = p[5];
	it->id_len = p[6];
	if (it->id_len > sizeof it->identity || len < 8 + it->id_len)
		return false;
	memcpy(it->identity, &p[7], it->id_len);
	it->key_len = p[7 + it->id_len];
	if (it->key_len < 1 || it->key_len > 32 || len != 8 + it->id_len + it->key_len)
		return false;
	it->key = &p[8 + it->id_len];
	return true;
}

int kf_item_build(const kf_item_t *it, uint8_t *out, int cap)
{
	if (it->cls == 1) {
		if (cap < 7 || it->key_len != 4 || it->slot < 1 || it->slot > 4)
			return 0;
		out[0] = 1;
		out[1] = it->type;
		out[2] = it->slot;
		memcpy(&out[3], it->key, 4);
		return 7;
	}
	int n = 8 + it->id_len + it->key_len;
	if (it->cls != 2 || it->id_len > sizeof it->identity || it->key_len < 1 || it->key_len > 32 || n > cap)
		return 0;
	out[0] = 2;
	out[1] = it->type;
	out[2] = it->alg;
	out[3] = (uint8_t)it->key_id;
	out[4] = (uint8_t)(it->key_id >> 8);
	out[5] = it->flags;
	out[6] = it->id_len;
	memcpy(&out[7], it->identity, it->id_len);
	out[7 + it->id_len] = it->key_len;
	memcpy(&out[8 + it->id_len], it->key, it->key_len);
	return n;
}

#endif	// CONFIG_KEY_FILL
