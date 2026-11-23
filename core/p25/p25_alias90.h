// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Alex Bloom
#ifndef P25_ALIAS90_H
#define P25_ALIAS90_H
#include <stdbool.h>
#include <stdint.h>
enum {
	P25_ALIAS90_WAIT, P25_ALIAS90_COMPLETE, P25_ALIAS90_BAD_HEADER,
	P25_ALIAS90_NO_HEADER, P25_ALIAS90_BAD_BLOCK, P25_ALIAS90_WRONG_SOURCE,
	P25_ALIAS90_BAD_CRC, P25_ALIAS90_BAD_TEXT
};
typedef struct {
	uint8_t data[88];
	uint16_t seen, tg;
	uint8_t count, seq, result;
} p25_alias90_t;
// Zero src/tg means unknown; recovered_src is written only after full CRC/text validation.
bool p25_alias90_lc(p25_alias90_t *a, const uint8_t raw[9], uint32_t src, uint16_t tg,
		    char name[15], uint32_t *recovered_src);
static inline bool p25_alias90_word(const uint8_t raw[9])
{
	return raw[1] == 0x90 && (raw[0] == 0x15 || raw[0] == 0x17);
}
// Motorola talk-complete ends speech while the channel can still send an alias.
// It cannot identify a caller by itself; a known caller must match its address.
static inline bool p25_alias90_talk_complete(const uint8_t raw[9], uint32_t src)
{
	uint32_t address = (uint32_t)raw[6] << 16 | (uint32_t)raw[7] << 8 | raw[8];
	return raw[0] == 0x0f && raw[1] == 0x90 && address && (!src || src == address);
}
#endif
