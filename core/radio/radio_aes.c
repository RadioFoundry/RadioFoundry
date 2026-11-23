// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "config.h"
#if CONFIG_DMR_AES
#include <string.h>
#include "board.h"
#include "codeplug/codeplug_records.h"
#include "dmr/dmrAes.h"
#include "dmr/dmrPrivacy.h"
#include "radio/radio.h"
#include "radio/radio_aes.h"
#include "settings/settings.h"

static dmrAesState_t channel_keys;
static uint16_t channel_key_ref;
static uint8_t channel_key_system;
static bool channel_tx_allowed;
static dmrAesSeq_t aes_seq;		// the radio's MI sequence, every key

void radio_aes_start(uint32_t saved)
{
	dmrAesSeqStart(&aes_seq, saved);	// TX waits for the next reservation
}

uint8_t radio_aes_tx_deny(const channel_t *ch)
{
	if (!ch->aes_ref || channel_key_ref != ch->aes_ref || channel_key_system != ch->system || !channel_tx_allowed)
		return TXDENY_AES_KEY;
	return aes_seq.left == 0 ? TXDENY_AES_WAIT : TXDENY_NONE;
}

bool radio_aes_configure(const channel_t *ch, dmr_call_config_t *cfg)
{
	// The call stack refuses an over it can't encrypt, even if it gets past the radio task's TX check.
	cfg->tx_crypt_required = ch->kind == CODEPLUG_KIND_DMR_AES;
	if (!cfg->tx_crypt_required) {
		if (channel_key_ref != 0 || dmrAesHasKeys(&channel_keys)) {
			dmrAesClearKeys(&channel_keys);
			channel_key_ref = 0;
			channel_tx_allowed = false;
		}
		return false;
	}

	codeplug_system_t sys;
	uint8_t block[CODEPLUG_SYS_HDR], key[DMR_AES_KEY_BYTES];
	uint8_t id = (uint8_t)(ch->aes_ref - 1);
	channel_tx_allowed = false;
	channel_key_ref = 0;
	channel_key_system = ch->system;
	dmrAesSetKey(&channel_keys, 0, 0, 0, NULL);
	if (ch->aes_ref && ch->aes_ref <= 256 && codeplug_system(ch->system, &sys, block, 0) >= 0 &&
	    sys.type == CODEPLUG_SYSTYPE_DMR_AES &&
	    codeplug_traffic_key(ch->system, DMR_AES_ALG_AES256, id, key, sizeof key) == sizeof key) {
		dmrAesSetKey(&channel_keys, 0, DMR_AES_ALG_AES256, id, key);
		dmrAesSetTx(&channel_keys, 0);
		channel_key_ref = ch->aes_ref;
		channel_tx_allowed = (sys.flags & CODEPLUG_SYS_TX) != 0;
	}
	dmrAesWipe(key, sizeof key);
	channel_keys.seq = &aes_seq;
	if (!dmrAesHasKeys(&channel_keys))
		return false;
	cfg->crypt = dmrAesCallHook;
	cfg->crypt_ctx = &channel_keys;
	if (!dmrAesTxKey(&channel_keys, &cfg->tx_crypt_alg, &cfg->tx_crypt_key_id))
		return false;
	cfg->crypt_mi = dmrAesNewMi;
	cfg->tx_fid = DMR_PRIVACY_FID;
	cfg->tx_svc |= DMR_PRIVACY_SERVICE_OPTION;
	return true;
}

void radio_aes_poll(const channel_t *ch, uint32_t own_id, uint32_t now_ms)
{
	dmrAesSeqSettle(&aes_seq);
	uint32_t stored = SETTINGS_FIELD(aes_mi_res);

	if (aes_seq.pending != 0) {
		if (stored != aes_seq.pending) {
			dmrAesSeqDropPending(&aes_seq);		// overwritten by a restore: ask again
		} else if (settings_u32_is_saved(offsetof(settings_t, aes_mi_res), stored)) {
			dmrAesSeqReserved(&aes_seq, stored);
		}
		return;
	}
	// Repair a restored reservation even off AES channels, without rewinding or allocating positions.
	if (aes_seq.end != 0 && stored != aes_seq.end) {
		settings_t *st = settings_begin();
		st->aes_mi_res = aes_seq.end;
		settings_end(now_ms);
		return;
	}
	if (ch->kind != CODEPLUG_KIND_DMR_AES)
		return;
	if (aes_seq.next == 0) {
		if (channel_key_ref == 0)
			return;
		hal_chip_t chip;
		memset(&chip, 0, sizeof(chip));
		if (board.system == NULL || board.system->chip == NULL || !board.system->chip(&chip))
			memset(chip.uid, 0, sizeof(chip.uid));
		aes_seq.next = dmrAesSeqSeed(channel_keys.keys[0].key, chip.uid, own_id, now_ms);
	}
	uint32_t r = dmrAesSeqReserve(&aes_seq);
	if (r != 0) {
		settings_t *st = settings_begin();
		st->aes_mi_res = r;
		settings_end(now_ms);
	}
}
#endif
