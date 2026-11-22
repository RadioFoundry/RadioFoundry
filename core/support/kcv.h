// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key check values (docs/spec/key-check.md): three octets, shown as six hex digits, that two radios (or a radio and the
// Codeplug Editor) compare to see they hold the same key without showing it. A block cipher's key gets the usual
// check value (the cipher's encryption of an all-zero block, its first three octets); any other key a truncated
// SHA-256 with a label, so the two kinds never give the same value by design
#ifndef RFOUNDRY_KCV_H
#define RFOUNDRY_KCV_H

#include <stdbool.h>
#include <stdint.h>

#define KCV_LEN		3u
#define KCV_TEXT	(2u * KCV_LEN + 1u)	// "3FA2C1" and its terminator

// What a key is (key-check.md 2), the first octet the hashed check value covers. Never renumbered
typedef enum {
	KCV_CLASS_PRIVACY = 1,	// a DMR basic privacy key slot: its type (dmrPrivacyType_t), its value
	KCV_CLASS_TRAFFIC = 2,	// a traffic key record (channel-modes.md 5.4): its system's type and algorithm, its octets
} kcv_class_t;

// The check value of n octets of key with the label's two octets a and b (the type, or the system type and the
// algorithm). Always true
bool kcv_hashed(kcv_class_t cls, uint8_t a, uint8_t b, const uint8_t *key, uint16_t n, uint8_t out[KCV_LEN]);
// The usual block cipher check value: AES (FIPS-197) of 16 zero octets under the key, first three octets. Only 32
// octet (AES-256) keys today; false for any other length or a build without AES
bool kcv_aes(const uint8_t *key, uint16_t n, uint8_t out[KCV_LEN]);

// A privacy key slot (settings-store.md 3.1): its type and value. The value is taken as four octets, big endian
bool kcv_privacy(uint8_t type, uint32_t key, uint8_t out[KCV_LEN]);
// A traffic key: AES for the algorithms a mode names as AES (key-check.md 3), the hashed form for every other. False
// when the key needs AES and the build has none, or its length isn't AES's
bool kcv_traffic(uint8_t sys_type, uint8_t alg, const uint8_t *key, uint16_t n, uint8_t out[KCV_LEN]);

// Six upper case hex digits
void kcv_text(const uint8_t kcv[KCV_LEN], char out[KCV_TEXT]);

#endif
