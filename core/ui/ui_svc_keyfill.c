// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ui_svc.h calls for key fill between radios (docs/spec/key-fill.md): the session lives on the RTOS heap while the
// Key fill screen is open and is wiped before it is freed, on every way out. The engine (keyfill/keyfill.h) runs here
// on the UI task, X25519 included; its messages go through the transport (keyfill/keyfill_api.h), a received key is
// stored by a storage job on the housekeeping task. All the RAM is the session's: this file keeps one pointer.
// The emergency wipe (CONFIG_ZEROIZE) clears a running session from the housekeeping task: zeroize_ram_hook below.
#include "config.h"

#if CONFIG_KEY_FILL
#include <string.h>
#include "board.h"
#include "ui_svc.h"
#include "keyfill/keyfill.h"
#include "keyfill/keyfill_api.h"
#if CONFIG_GROUP_KEY_FILL
#include "keyfill/groupkey_api.h"
#endif
#include "support/entropy.h"
#include "support/hkdf.h"
#include "codeplug/codeplug.h"
#include "codeplug/key_store.h"
#include "radio/radio.h"
#include "storage/storage.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#if CONFIG_ZEROIZE
#include "task.h"
#include "zeroize/zeroize.h"
#endif

enum { JOB_NONE, JOB_RUNNING, JOB_DONE };

typedef struct {
	kf_session_t s;
	uint8_t out[KF_MSG_MAX];	// a message the transport hasn't taken yet
	uint8_t in[KF_MSG_MAX];		// the one received, while the engine reads it
	uint8_t out_len;
	uint32_t out_dst;
	int32_t key;			// sender: the stored key's index; receiver: -1
	// The sender's key as picked: its class, slot or record, and check value. KEY sends the entry that still matches
	// all of them, wherever the list has moved it since; none: the session fails rather than send another key
	uint8_t pick_cls, pick_slot;
	uint16_t pick_record;
	uint8_t pick_kcv[KCV_LEN];
	uint32_t own, dst;
	bool started;			// the engine runs (the randomness pool was full)
	bool ending;			// the screen was left: the ABORT goes, then the session is freed
	bool sending;			// our message is with the transport
	bool low;			// the radio holds the lowest TX power for us (radio_req_tx_low): direct mode only
	uint8_t seen_state, seen_presses;	// what the screen showed last
	volatile uint8_t job;		// JOB_*: the store (receiver)
	uint8_t job_result;
	uint8_t job_kcv[KCV_LEN];
	key_put_t put;			// the job's key: points into s.payload until kf_stored
#if KEY_STORE
	// The sender's key while KEY is built, here and not on the UI task's stack: the store's read is the deepest
	// path of that task (docs/dev/stack-check.md)
	key_entry_t e;
	uint8_t key_buf[KEY_STORE_MAX_LEN];
#endif
} kf_ctx_t;

static kf_ctx_t *kf;		// the UI task's; set only once the block is cleared, cleared before it is freed
#if CONFIG_ZEROIZE
static volatile bool wiped;	// an emergency wipe ran: a store job still to run stores nothing (zeroize_ram_hook)
#endif

// Freed, the pointer cleared first: the emergency wipe (another task) never writes to a freed block
static void release(void)
{
	kf_ctx_t *c = kf;
	kf = NULL;
	vPortFree(c);
}

static uint8_t presses_left(void)
{
	unsigned bits = entropy_bits();
	if (bits >= ENTROPY_FULL)
		return 0;
	return (uint8_t)((ENTROPY_FULL - bits + ENTROPY_KEY_BITS - 1u) / ENTROPY_KEY_BITS);
}

static void tx_low(bool on)
{
	if (kf->low != on) {
		kf->low = on;
		radio_req_tx_low(on);
	}
}

// Wiped and freed: every way out ends here
static void finish(void)
{
	key_fill_open(false);
	tx_low(false);
	kf_wipe(&kf->s);
	crypto_wipe(kf, sizeof *kf);
	release();
}

#if KEY_STORE
// The sender's payload, read from the store only now (key-fill.md 5) and wiped right after
static int send_payload(uint8_t *out, int cap, void *ctx)
{
	kf_ctx_t *c = ctx;
	const key_entry_t *e = &c->e;
	kf_item_t it;
	int n = 0;
	memset(&it, 0, sizeof it);
	bool found = false;
	for (uint16_t i = 0, n_keys = key_store_count(); i < n_keys && !found; i++) {
		found = key_store_get(i, &c->e, c->key_buf) && e->cls == c->pick_cls && e->kcv_ok &&
			(e->cls == KCV_CLASS_PRIVACY ? e->slot == c->pick_slot : e->record == c->pick_record) &&
			memcmp(e->kcv, c->pick_kcv, KCV_LEN) == 0;
	}
	if (found && e->len >= 1 && e->len <= KEY_STORE_MAX_LEN) {
		it.cls = e->cls;
		it.type = e->type;
		it.slot = e->slot;
		it.alg = e->alg;
		it.key_id = e->key_id;
		it.flags = e->flags;
		it.key_len = (uint8_t)e->len;
		it.key = c->key_buf;
		int id = e->cls == KCV_CLASS_TRAFFIC ? key_store_identity(e->system, it.identity, sizeof it.identity) : 0;
		if (id >= 0) {
			it.id_len = (uint8_t)id;
			n = kf_item_build(&it, out, cap);
		}
	}
	crypto_wipe(c->key_buf, sizeof c->key_buf);
	crypto_wipe(&c->e, sizeof c->e);
	crypto_wipe(&it, sizeof it);
	return n;
}

// The housekeeping task: the received key into the store
static void store_job(void *ctx)
{
	kf_ctx_t *c = ctx;
#if CONFIG_ZEROIZE
	if (wiped) {
		c->job_result = KF_RES_WRITE_FAILED;	// the wipe cleared the key it pointed at: nothing goes to the flash
		c->job = JOB_DONE;
		return;
	}
#endif
	c->job_result = key_store_put(&c->put, c->job_kcv);
	c->job = JOB_DONE;
}
#endif

uint8_t ui_svc_kf_start(int32_t key, uint32_t own, uint32_t dst, bool low)
{
	if (own == 0 || own > 0xFFFFFFu)
		return UI_KF_NO_ID;
	if (key >= 0 && (!KEY_STORE || dst == 0 || dst > CODEPLUG_DMR_ID_MAX || dst == own))
		return UI_KF_BAD_PEER;
	if (kf != NULL) {
		if (kf->job == JOB_RUNNING)
			return UI_KF_BUSY;	// a key being stored: the session goes once it is
		finish();
	}
	kf_ctx_t *c = pvPortMalloc(sizeof *c);
	if (c == NULL)
		return UI_KF_BUSY;
	memset(c, 0, sizeof *c);
	kf = c;
#if CONFIG_ZEROIZE
	wiped = false;
#endif
	kf->key = key;
#if KEY_STORE
	if (key >= 0) {
		if (!key_store_get((uint16_t)key, &kf->e, NULL) || !kf->e.kcv_ok) {
			crypto_wipe(kf, sizeof *kf);
			release();
			return UI_KF_BAD_PEER;	// the key went from the list under the user
		}
		kf->pick_cls = kf->e.cls;
		kf->pick_slot = kf->e.slot;
		kf->pick_record = kf->e.record;
		memcpy(kf->pick_kcv, kf->e.kcv, KCV_LEN);
		crypto_wipe(&kf->e, sizeof kf->e);
	}
#endif
	kf->own = own;
	kf->dst = dst;
	kf->seen_state = 0xFF;
	key_fill_open(true);
	tx_low(low);	// held until the session ends, on every way out (finish)
	return UI_KF_OK;
}

bool ui_svc_kf_view(ui_kf_view_t *out)
{
	memset(out, 0, sizeof *out);
	if (kf == NULL || kf->ending)
		return false;
	const kf_session_t *s = &kf->s;
	out->state = s->state;
	out->role = kf->key < 0;
	out->why = s->why;
	out->result = s->result;
	out->by_peer = s->by_peer;
	out->kcv_ok = s->kcv_ok;
	memcpy(out->kcv, s->kcv, sizeof out->kcv);
	out->presses = kf->started ? 0 : presses_left();
	out->own_id = kf->own;
	out->peer_id = kf->key < 0 ? s->peer_id : kf->dst;
	out->code = s->code;
	return true;
}

void ui_svc_kf_confirm(bool same)
{
	if (kf != NULL && !kf->ending)
		kf_confirm(&kf->s, same, hal_millis());
}

void ui_svc_kf_end(void)
{
	if (kf == NULL || kf->ending)
		return;
	if (kf->started)
		kf_cancel(&kf->s, hal_millis());	// ABORT, when there is another radio, goes from the next tick
	kf->ending = true;
}

// Our messages to the transport, one at a time
static void transport(void)
{
	if (kf->sending) {
		kf_send_state_t st = key_fill_send_state();
		if (st == KF_SEND_SENT || st == KF_SEND_FAILED || st == KF_SEND_IDLE) {
			kf->sending = false;
			if (st == KF_SEND_FAILED && !kf->ending)
				kf_send_failed(&kf->s);
		}
	}
	if (kf->out_len == 0 && kf->started)
		kf->out_len = (uint8_t)kf_tx(&kf->s, kf->out, &kf->out_dst);
	if (kf->out_len != 0 && !kf->sending) {
		if (key_fill_send(kf->out_dst, kf->out, kf->out_len))
			kf->sending = true;
		else if (!kf->ending)
			kf_send_failed(&kf->s);		// the air queue has no room
		kf->out_len = 0;
	}
}

// Receiver, in Storing: the payload to the store, then its answer to the engine
static void storing(uint32_t now)
{
	static const uint8_t none[KCV_LEN];
	if (kf->job == JOB_DONE) {
		kf->job = JOB_NONE;
		kf_stored(&kf->s, kf->job_result, kf->job_result == KF_STORED ? kf->job_kcv : none, now);
		return;
	}
	if (kf->job != JOB_NONE)
		return;
	int len;
	const uint8_t *p = kf_payload(&kf->s, &len);
	kf_item_t it;
	if (p == NULL || !kf_item_parse(p, len, &it)) {
		kf_stored(&kf->s, KF_RES_BAD_PAYLOAD, none, now);
		return;
	}
#if KEY_STORE
	key_put_t *k = &kf->put;
	k->cls = it.cls;
	k->type = it.type;
	k->slot = it.slot;
	k->alg = it.alg;
	k->key_id = it.key_id;
	k->flags = it.flags;
	k->id_len = it.id_len;
	memcpy(k->identity, it.identity, sizeof k->identity);
	k->key_len = it.key_len;
	k->key = it.key;
	kf->job = JOB_RUNNING;
	if (!storage_job_post(store_job, kf))
		kf->job = JOB_NONE;	// another job is running: the next tick tries again
#else
	kf_stored(&kf->s, KF_RES_UNSUPPORTED, none, now);	// nowhere to store a key in this build
#endif
	crypto_wipe(&it, sizeof it);
}

// The randomness pool full: the session's private key (and, for a sender, its ID) from it
static void begin(uint32_t now)
{
	struct {
		uint32_t own;
		hal_chip_t chip;
	} id;
	memset(&id, 0, sizeof id);
	id.own = kf->own;
	if (board.system != NULL && board.system->chip != NULL)
		(void)board.system->chip(&id.chip);
	entropy_stir(&id, sizeof id, 0);
	uint8_t rnd[KF_RANDOM_LEN];
	entropy_take(rnd, sizeof rnd);
	kf->started = true;
	if (kf->key < 0) {
		kf_listen(&kf->s, kf->own, rnd, now);
	} else {
#if KEY_STORE
		if (!kf_send(&kf->s, kf->own, kf->dst, send_payload, kf, rnd, now))
#endif
		{
			kf->s.state = KF_ST_FAILED;	// checked by ui_svc_kf_start: not reached
			kf->s.why = KF_WHY_CHECK;
		}
	}
	crypto_wipe(rnd, sizeof rnd);
}

bool ui_svc_kf_tick(const radio_view_t *v, uint32_t now)
{
	if (kf == NULL)
		return false;
	if (kf->ending) {
		transport();
		if (!kf->sending && kf->out_len == 0 && kf->job != JOB_RUNNING)
			finish();
		return false;
	}
	// Each tick on the screen: the clock (entropy_stir adds it), the RSSI and the battery, credited nothing (6)
	struct {
		int16_t rssi;
		uint16_t mv;
		uint8_t noise;
	} mix = { v->rssi_dbm, v->battery_mv, v->noise };
	entropy_stir(&mix, sizeof mix, 0);
	if (!kf->started && entropy_bits() >= ENTROPY_FULL)
		begin(now);
	if (kf->started) {
		uint32_t src;
		int n = key_fill_take(kf->in, &src);
		if (n > 0)
			kf_rx(&kf->s, src, kf->in, n, now);
		kf_tick(&kf->s, now);
		if (kf->s.state == KF_ST_STORING)
			storing(now);
	}
	transport();
	// Done or failed, and the last message gone: the user's power level is back (2.1)
	if ((kf->s.state == KF_ST_DONE || kf->s.state == KF_ST_FAILED) && !kf->sending && kf->out_len == 0)
		tx_low(false);
	uint8_t presses = kf->started ? 0 : presses_left();
	bool changed = kf->s.state != kf->seen_state || presses != kf->seen_presses;
	kf->seen_state = kf->s.state;
	kf->seen_presses = presses;
	return changed;
}

#if CONFIG_ZEROIZE
// The emergency wipe's RAM step (zeroize.h, docs/spec/zeroize.md 4.2), on the housekeeping task while the session and
// the randomness pool are the UI task's. Both are wiped in place with the scheduler suspended, so the UI task never
// runs in the middle (nor frees the block: kf is cleared before vPortFree). The session is left ending with nothing to
// send: the UI task frees it on its next tick (finish) and the screen shows it gone. A store job still to run stores
// nothing (wiped). Then, with the scheduler running again (one takes the data service's lock, the other posts to the
// radio task, neither allowed while suspended): the transport's held message, the port closed, and the radio core's
// low power override. No storage lock, no long wait
void zeroize_ram_hook(void)
{
	vTaskSuspendAll();
	kf_ctx_t *c = kf;
	if (c != NULL) {
		uint8_t job = c->job;	// a store job still to run keeps the block until it has (ui_svc_kf_tick)
		kf_wipe(&c->s);
		crypto_wipe(c, sizeof *c);
		c->job = job;
		c->ending = true;
	}
	wiped = true;
#if CONFIG_GROUP_KEY_FILL
	ui_svc_gk_zeroize();
#endif
	entropy_wipe();
	(void)xTaskResumeAll();
	key_fill_open(false);
#if CONFIG_GROUP_KEY_FILL
	group_key_open(false);
#endif
	radio_req_tx_low(false);
}
#endif
#endif
