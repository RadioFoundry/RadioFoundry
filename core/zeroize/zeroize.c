// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The emergency wipe (zeroize.h, docs/spec/zeroize.md)
#include "zeroize/zeroize.h"
#if CONFIG_ZEROIZE
#include "board.h"
#include "hal_time.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "radio/radio.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "support/sha256.h"
#if CONFIG_CRASH_LOG
#include "crash/crash.h"
#endif

#define RADIO_WAIT_MS	1000u	// the radio core's answer to RREQ_ZEROIZE (4.2)

// Zeroes n octets so the compiler can't drop it as a dead store (a secret going out of scope)
static void wipe(void *p, size_t n)
{
	volatile uint8_t *v = p;
	while (n-- > 0) {
		*v++ = 0;
	}
}

void zeroize_hash(uint32_t salt, const char code[ZEROIZE_CODE_LEN], uint8_t out[ZEROIZE_HASH_LEN])
{
	static const char label[5] = { 'R', 'F', 'Z', 'Z', '1' };
	uint8_t uid[12] = { 0 }, s[4];
	hal_chip_t c;
	if (board.system != NULL && board.system->chip != NULL && board.system->chip(&c)) {
		for (unsigned i = 0; i < 12u; i++) {
			uid[i] = (uint8_t)(c.uid[i / 4u] >> (8u * (i % 4u)));
		}
	}
	for (unsigned i = 0; i < 4u; i++) {
		s[i] = (uint8_t)(salt >> (8u * i));
	}
	sha256_t h;
	uint8_t d[SHA256_LEN];
	sha256_init(&h);
	sha256_update(&h, label, sizeof(label));
	sha256_update(&h, uid, sizeof(uid));
	sha256_update(&h, s, sizeof(s));
	sha256_update(&h, code, ZEROIZE_CODE_LEN);
	sha256_final(&h, d);
	memcpy(out, d, ZEROIZE_HASH_LEN);
	wipe(&h, sizeof(h));	// the code was in its block
	wipe(d, sizeof(d));
}

static bool digits(const char code[ZEROIZE_CODE_LEN])
{
	for (unsigned i = 0; i < ZEROIZE_CODE_LEN; i++) {
		if (code[i] < '0' || code[i] > '9') {
			return false;
		}
	}
	return true;
}

bool zeroize_code_set(const char code[ZEROIZE_CODE_LEN])
{
	if (!digits(code)) {
		return false;
	}
	uint8_t h[ZEROIZE_HASH_LEN];
	settings_t *st = settings_begin();
	// Not random (the HAL has none): the uptime mixed into the last salt, so each setting differs (spec 2)
	uint32_t salt = (st->zz_salt ^ hal_millis()) * 2654435761u + 0x9E3779B9u;
	zeroize_hash(salt, code, h);
	st->zz_salt = salt;
	memcpy(st->zz_hash, h, sizeof(h));
	st->zz_flags |= SETTINGS_ZZ_SET;
	settings_end(hal_millis());
	return true;
}

void zeroize_code_clear(void)
{
	settings_t *st = settings_begin();
	st->zz_flags &= (uint8_t)~SETTINGS_ZZ_SET;
	st->zz_salt = 0;
	memset(st->zz_hash, 0, sizeof(st->zz_hash));
	settings_end(hal_millis());
}

bool zeroize_code_is_set(void)
{
	settings_t *st = settings_begin();
	bool set = (st->zz_flags & SETTINGS_ZZ_SET) != 0;
	settings_end(hal_millis());
	return set;
}

bool zeroize_code_check(const char code[ZEROIZE_CODE_LEN])
{
	uint8_t want[ZEROIZE_HASH_LEN], got[ZEROIZE_HASH_LEN];
	settings_t *st = settings_begin();
	bool set = (st->zz_flags & SETTINGS_ZZ_SET) != 0;
	uint32_t salt = st->zz_salt;
	memcpy(want, st->zz_hash, sizeof(want));
	settings_end(hal_millis());
	if (!set || !digits(code)) {
		return false;
	}
	zeroize_hash(salt, code, got);
	uint8_t diff = 0;
	for (unsigned i = 0; i < ZEROIZE_HASH_LEN; i++) {
		diff |= (uint8_t)(want[i] ^ got[i]);	// constant time: a timing gives nothing away
	}
	return diff == 0;
}

static uint8_t flags(void)
{
	settings_t *st = settings_begin();
	uint8_t f = st->zz_flags;
	settings_end(hal_millis());
	return f;
}

static void set_flags(uint8_t clear, uint8_t set)
{
	settings_t *st = settings_begin();
	st->zz_flags = (uint8_t)((st->zz_flags & ~clear) | set);
	settings_end(hal_millis());
}

bool zeroize_keys_only(void) { return (flags() & SETTINGS_ZZ_KEYS_ONLY) != 0; }
bool zeroize_confirm(void) { return (flags() & SETTINGS_ZZ_NO_CONFIRM) == 0; }
uint8_t zeroize_tries(void) { return (uint8_t)(flags() >> SETTINGS_ZZ_TRIES_SHIFT); }
void zeroize_set_keys_only(bool on) { set_flags(SETTINGS_ZZ_KEYS_ONLY, on ? SETTINGS_ZZ_KEYS_ONLY : 0); }
void zeroize_set_confirm(bool on) { set_flags(SETTINGS_ZZ_NO_CONFIRM, on ? 0 : SETTINGS_ZZ_NO_CONFIRM); }

void zeroize_set_tries(uint8_t n)
{
	n = n > ZEROIZE_TRIES_MAX ? ZEROIZE_TRIES_MAX : n;
	set_flags((uint8_t)(0x0Fu << SETTINGS_ZZ_TRIES_SHIFT), (uint8_t)(n << SETTINGS_ZZ_TRIES_SHIFT));
}

__attribute__((weak)) void zeroize_ram_hook(void)
{
}

// The radio core's answer to RREQ_ZEROIZE, up to RADIO_WAIT_MS (the housekeeping task may wait: nothing else runs on it)
static bool radio_cleared(void)
{
	uint32_t t0 = hal_millis();
	while (radio_zeroize_busy()) {
		if (hal_millis() - t0 >= RADIO_WAIT_MS) {
			return false;
		}
		hal_delay_ms(10);
	}
	return true;
}

uint8_t zeroize_run(bool keys_only)
{
	uint8_t failed = 0;
	bool flash = storage_ready();
	// First, without waiting: the radio core drops its key, so it can't write it back into the settings while
	// they are erased (it saves the key in use with the channel, settings-store.md 2)
	radio_req_zeroize();
	// 1. The keys in the flash: the codeplug settings area's first, then the privacy key slots
	if (flash) {
		static const uint16_t secrets[] = {
			CODEPLUG_KEY_SYS_KEY, CODEPLUG_KEY_SYS_KEY + 0xFFu,		// system secrets
			CODEPLUG_KEY_TRAFFIC, CODEPLUG_KEY_ZONE_TAG - 1u,		// traffic keys
		};
		bool ok = keys_only ? codeplug_kv_remove_ranges(secrets, 2) : codeplug_wipe_keys();
		failed |= ok ? 0 : ZEROIZE_FAIL_KEYS;
	}
	failed |= radio_cleared() ? 0 : ZEROIZE_FAIL_RADIO;
	settings_t *st = settings_begin();
	if (keys_only) {
		memset(st->privacy_key, 0, sizeof(st->privacy_key));
		memset(st->privacy_type, 0, sizeof(st->privacy_type));
		st->privacy_active = 0;
	} else {
		settings_defaults(st);		// as a factory reset: the wipe code goes too
	}
	settings_end(hal_millis());
	if (flash && !settings_scrub()) {
		failed |= ZEROIZE_FAIL_SETTINGS;
	}
	// 2. The codeplug
	if (flash && !keys_only) {
		failed |= codeplug_wipe() ? 0 : ZEROIZE_FAIL_CODEPLUG;
		failed |= storage_native_erase() == STORAGE_NATIVE_FAILED ? ZEROIZE_FAIL_NATIVE : 0;
	}
	// 3. What a copy on write flash still keeps of all that
	if (flash && !storage_purge()) {
		failed |= ZEROIZE_FAIL_PURGE;
	}
	// 4. RAM: the radio core again (a key set meanwhile), what other features hold, the crash record's stack words
	radio_req_zeroize();
	zeroize_ram_hook();
#if CONFIG_CRASH_LOG
	crash_clear();
#endif
	failed |= radio_cleared() ? 0 : ZEROIZE_FAIL_RADIO;
	return failed;
}
#endif
