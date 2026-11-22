// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ui_svc.h calls for group key fill (docs/spec/group-key-fill.md): one session while the Group key screen is open,
// on the RTOS heap, wiped before it is freed on every way out. The sender builds the message once and hands the
// transport (keyfill/groupkey_api.h) its copies; a receiver checks each message under its own channel key and, on the
// user's OK, stores the key by a storage job on the housekeeping task. The emergency wipe clears a running session
// (ui_svc_gk_zeroize, called by zeroize_ram_hook in ui_svc_keyfill.c).
#include "config.h"

#if CONFIG_GROUP_KEY_FILL
#include <string.h>
#include "ui_svc.h"
#include "keyfill/groupkey.h"
#include "keyfill/groupkey_api.h"
#include "support/entropy.h"
#include "support/hkdf.h"
#include "support/kcv.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_records.h"
#include "codeplug/key_store.h"
#include "radio/radio.h"
#include "storage/storage.h"
#include "hal_time.h"
#include "FreeRTOS.h"

enum { JOB_NONE, JOB_RUNNING, JOB_DONE };

typedef struct {
	uint8_t state;			// UI_GK_*
	uint8_t why;			// UI_GK_WHY_*
	bool send;			// the sender's session
	bool ending;
	// Sender: the stored key as picked (class, record and check value must still match when it is read)
	uint16_t pick_record;
	uint8_t pick_kcv[KCV_LEN];
	uint8_t key_id;
	uint32_t own, tg;
	uint8_t copies;			// copies on air so far
	bool sending;			// a copy is with the transport
	uint32_t next_at;		// when the next copy goes
	uint8_t msg[GK_MSG_LEN];
	// Receiver
	uint32_t until;			// the window closes
	uint8_t chan_key[GK_KEY_LEN];	// the channel's key while the window is open
	uint8_t chan_system;
	gk_item_t got;			// the key shown, waiting for the user
	uint8_t seen[4][GK_SESSION_LEN];	// sessions already shown in this window
	uint8_t seen_n;
	uint8_t save_id;
	volatile uint8_t job;		// JOB_*: the store
	uint8_t job_result;
	key_put_t put;
	uint8_t kcv[KCV_LEN];
	bool kcv_ok;
	uint8_t seen_state;		// what the screen showed last
	uint32_t seen_s;
} gk_ctx_t;

static gk_ctx_t *gk;	// the UI task's; cleared before the block is freed
static volatile bool gk_wiped;	// an emergency wipe ran: a store job still to run stores nothing

static void finish(void)
{
	gk_ctx_t *c = gk;
	group_key_open(false);
	if (c->send && c->state != UI_GK_SENT)
		group_key_cancel();
	crypto_wipe(c, sizeof *c);
	gk = NULL;
	vPortFree(c);
}

// The channel on view: an AES channel with its key: the key into key, its system into *system
static bool channel_key(const radio_view_t *v, uint8_t key[GK_KEY_LEN], uint8_t *system)
{
	const channel_t *ch = &v->ch;
	if (ch->mode != RADIO_MODE_DMR || ch->kind != CODEPLUG_KIND_DMR_AES || ch->aes_ref == 0 || ch->aes_ref > 256)
		return false;
	*system = ch->system;
	return codeplug_traffic_key(ch->system, GK_ALG_AES256, (uint16_t)(ch->aes_ref - 1u), key, GK_KEY_LEN) ==
	       GK_KEY_LEN;
}

static void store_job(void *ctx)
{
	gk_ctx_t *c = ctx;
	if (gk_wiped) {
		c->job_result = KEY_PUT_WRITE_FAILED;
		c->job = JOB_DONE;
		return;
	}
	c->job_result = key_store_put(&c->put, c->kcv);
	c->job = JOB_DONE;
}

uint8_t ui_svc_gk_start(int32_t key, const radio_view_t *v)
{
	if (gk != NULL)
		return UI_GK_WHY_BUSY;
	uint8_t ck[GK_KEY_LEN], system = 0;
	bool have = channel_key(v, ck, &system);
	if (!have) {
		crypto_wipe(ck, sizeof ck);
		return UI_GK_WHY_CHANNEL;
	}
	uint32_t own = v->dmr.own_id;
	if (key >= 0 && (v->dmr.target_private || v->dmr.target_id == 0)) {
		crypto_wipe(ck, sizeof ck);
		return UI_GK_WHY_TARGET;
	}
	if (own == 0 || own > 0xFFFFFFu) {
		crypto_wipe(ck, sizeof ck);
		return UI_GK_WHY_NO_ID;
	}
	key_entry_t e;
	if (key >= 0 && (!ui_svc_key((uint16_t)key, &e) || e.cls != KCV_CLASS_TRAFFIC || e.alg != GK_ALG_AES256 ||
			 e.len != GK_KEY_LEN || !e.kcv_ok || e.key_id > 255)) {
		crypto_wipe(ck, sizeof ck);
		return UI_GK_WHY_KEY;
	}
	gk_ctx_t *c = pvPortMalloc(sizeof *c);
	if (c == NULL) {
		crypto_wipe(ck, sizeof ck);
		return UI_GK_WHY_BUSY;
	}
	memset(c, 0, sizeof *c);
	gk_wiped = false;
	c->own = own;
	if (key < 0) {
		c->state = UI_GK_LISTEN;
		memcpy(c->chan_key, ck, sizeof ck);
		c->chan_system = system;
		c->until = hal_millis() + GK_LISTEN_MS;
		group_key_open(true);
	} else {
		c->send = true;
		c->state = UI_GK_RANDOM;
		c->pick_record = e.record;
		memcpy(c->pick_kcv, e.kcv, KCV_LEN);
		memcpy(c->kcv, e.kcv, KCV_LEN);
		c->kcv_ok = true;
		c->key_id = (uint8_t)e.key_id;
		c->tg = v->dmr.target_id;
		memcpy(c->chan_key, ck, sizeof ck);	// wiped once the message is built
	}
	crypto_wipe(ck, sizeof ck);
	gk = c;
	return UI_GK_WHY_NONE;
}

bool ui_svc_gk_view(ui_gk_view_t *out)
{
	memset(out, 0, sizeof *out);
	if (gk == NULL)
		return false;
	out->state = gk->state;
	out->why = gk->why;
	out->send = gk->send;
	out->copies = gk->copies;
	uint32_t now = hal_millis();
	out->left_s = gk->send || (int32_t)(gk->until - now) <= 0 ? 0 : (gk->until - now + 999u) / 1000u;
	out->sender = gk->got.sender;
	out->key_id = gk->send ? gk->key_id : gk->got.key_id;
	out->save_id = gk->save_id;
	out->kcv_ok = gk->kcv_ok;
	memcpy(out->kcv, gk->kcv, sizeof out->kcv);
	out->tg = gk->tg;
	out->presses = gk->state == UI_GK_RANDOM && entropy_bits() < ENTROPY_FULL ?
		(uint8_t)((ENTROPY_FULL - entropy_bits() + ENTROPY_KEY_BITS - 1u) / ENTROPY_KEY_BITS) : 0;
	return true;
}

void ui_svc_gk_save_id(uint8_t id)
{
	if (gk != NULL && gk->state == UI_GK_GOT)
		gk->save_id = id;
}

void ui_svc_gk_save(void)
{
	if (gk == NULL || gk->state != UI_GK_GOT || gk->job != JOB_NONE)
		return;
	key_put_t *k = &gk->put;
	memset(k, 0, sizeof *k);
	k->cls = KCV_CLASS_TRAFFIC;
	k->type = CODEPLUG_SYSTYPE_DMR_AES;
	k->alg = GK_ALG_AES256;
	k->key_id = gk->save_id;
	int n = key_store_identity(gk->chan_system, k->identity, sizeof k->identity);
	if (n < 0) {
		gk->state = UI_GK_FAILED;
		gk->why = UI_GK_WHY_STORE;
		gk_wipe(&gk->got);
		return;
	}
	k->id_len = (uint8_t)n;
	k->key_len = GK_KEY_LEN;
	k->key = gk->got.key;
	gk->state = UI_GK_SAVING;
	gk->job = JOB_RUNNING;
	if (!storage_job_post(store_job, gk))
		gk->job = JOB_NONE;	// another job is running: the next tick tries again
}

void ui_svc_gk_drop(void)
{
	if (gk == NULL || gk->state != UI_GK_GOT)
		return;
	gk_wipe(&gk->got);	// its session stays seen: a later copy of it isn't shown again
	gk->kcv_ok = false;
	gk->state = UI_GK_LISTEN;
}

void ui_svc_gk_end(void)
{
	if (gk != NULL)
		gk->ending = true;
}

static bool seen(const uint8_t session[GK_SESSION_LEN])
{
	for (uint8_t i = 0; i < gk->seen_n; i++)
		if (memcmp(gk->seen[i], session, GK_SESSION_LEN) == 0)
			return true;
	return false;
}

static void remember(const uint8_t session[GK_SESSION_LEN])
{
	uint8_t i = gk->seen_n < 4 ? gk->seen_n++ : (uint8_t)(hal_millis() & 3u);
	memcpy(gk->seen[i], session, GK_SESSION_LEN);
}

static void listen_tick(uint32_t now)
{
	if (gk->state == UI_GK_SAVING) {
		if (gk->job == JOB_NONE) {
			gk->job = JOB_RUNNING;
			if (!storage_job_post(store_job, gk))
				gk->job = JOB_NONE;
		} else if (gk->job == JOB_DONE) {
			gk->job = JOB_NONE;
			gk_wipe(&gk->got);
			if (gk->job_result == KEY_PUT_STORED) {
				gk->state = UI_GK_SAVED;
				gk->kcv_ok = true;
			} else {
				gk->state = UI_GK_FAILED;
				gk->why = UI_GK_WHY_STORE;
			}
			group_key_open(false);
			crypto_wipe(gk->chan_key, sizeof gk->chan_key);
		}
		return;
	}
	if (gk->state != UI_GK_LISTEN && gk->state != UI_GK_GOT)
		return;
	uint8_t in[GK_MSG_LEN];
	uint32_t src;
	int n = group_key_take(in, &src);
	if (n > 0 && gk->state == UI_GK_LISTEN) {
		gk_item_t it;
		if (gk_open(in, n, src, gk->chan_key, &it) == GK_OK && !seen(it.session)) {
			remember(it.session);
			gk->got = it;
			gk->save_id = it.key_id;
			gk->kcv_ok = kcv_traffic(CODEPLUG_SYSTYPE_DMR_AES, GK_ALG_AES256, it.key, GK_KEY_LEN, gk->kcv);
			gk->state = UI_GK_GOT;
		}
		gk_wipe(&it);
	}
	crypto_wipe(in, sizeof in);
	if (gk->state == UI_GK_LISTEN && (int32_t)(now - gk->until) >= 0) {
		gk->state = UI_GK_FAILED;
		gk->why = UI_GK_WHY_TIMEOUT;
		group_key_open(false);
		crypto_wipe(gk->chan_key, sizeof gk->chan_key);
	}
}

// The stored key read again only now and checked against the pick; the message built and the channel key wiped
static bool build(void)
{
	key_entry_t e;
	uint8_t key[KEY_STORE_MAX_LEN];
	bool found = false;
	for (uint16_t i = 0, n = key_store_count(); i < n && !found; i++)
		found = key_store_get(i, &e, key) && e.cls == KCV_CLASS_TRAFFIC && e.record == gk->pick_record &&
			e.kcv_ok && memcmp(e.kcv, gk->pick_kcv, KCV_LEN) == 0 && e.len == GK_KEY_LEN;
	if (found) {
		uint8_t session[GK_SESSION_LEN];
		entropy_take(session, sizeof session);
		gk_build(gk->msg, gk->chan_key, session, gk->own, gk->key_id, key);
		crypto_wipe(session, sizeof session);
	}
	crypto_wipe(key, sizeof key);
	crypto_wipe(&e, sizeof e);
	crypto_wipe(gk->chan_key, sizeof gk->chan_key);
	return found;
}

static void send_tick(uint32_t now)
{
	if (gk->state == UI_GK_RANDOM) {
		if (entropy_bits() < ENTROPY_FULL)
			return;
		if (!build()) {
			gk->state = UI_GK_FAILED;
			gk->why = UI_GK_WHY_KEY;
			return;
		}
		gk->state = UI_GK_SENDING;
		gk->next_at = now;
	}
	if (gk->state != UI_GK_SENDING)
		return;
	if (gk->sending) {
		gk_send_state_t st = group_key_send_state();
		if (st == GK_SEND_SENDING)
			return;
		gk->sending = false;
		if (st != GK_SEND_SENT) {
			gk->state = UI_GK_FAILED;
			gk->why = UI_GK_WHY_SEND;
			crypto_wipe(gk->msg, sizeof gk->msg);
			return;
		}
		gk->copies++;
		gk->next_at = now + GK_COPY_GAP_MS;
		if (gk->copies >= GK_COPIES) {
			gk->state = UI_GK_SENT;
			crypto_wipe(gk->msg, sizeof gk->msg);
			return;
		}
	}
	if ((int32_t)(now - gk->next_at) >= 0) {
		if (group_key_send(gk->tg, gk->msg)) {
			gk->sending = true;
		} else {
			gk->state = UI_GK_FAILED;
			gk->why = UI_GK_WHY_SEND;
			crypto_wipe(gk->msg, sizeof gk->msg);
		}
	}
}

bool ui_svc_gk_tick(const radio_view_t *v, uint32_t now)
{
	(void)v;
	if (gk == NULL)
		return false;
	if (gk->ending) {
		if (gk->job != JOB_RUNNING)
			finish();
		return false;
	}
	if (gk->send)
		send_tick(now);
	else
		listen_tick(now);
	uint32_t s = gk->send || (int32_t)(gk->until - now) <= 0 ? 0 : (gk->until - now) / 1000u;
	bool changed = gk->state != gk->seen_state || s != gk->seen_s;
	gk->seen_state = gk->state;
	gk->seen_s = s;
	return changed;
}

// The emergency wipe's RAM step (zeroize.md 4.2): called by zeroize_ram_hook with the scheduler suspended. The session
// is wiped in place and left ending; the UI task frees it on its next tick
void ui_svc_gk_zeroize(void)
{
	gk_ctx_t *c = gk;
	if (c != NULL) {
		uint8_t job = c->job;
		crypto_wipe(c, sizeof *c);
		c->job = job;
		c->ending = true;
	}
	gk_wiped = true;
}

#endif	// CONFIG_GROUP_KEY_FILL
