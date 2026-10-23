// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C7000 codec slot: its header, its checks and the code's move to the board's codec window
// (docs/spec/codec-placement.md, "HR_C7000 radios"). No hardware here, so the host tests run it
// (tests/host/hrc7000CodecSlotTest.c); platform/hrc7000/vocoder.c does the copies.
//
// tools/hrc7000_codec.py and the web flasher (c7000.js) write the slot; keep the three in step.
// Version 2 (the only one: a version 1 slot, from before the code moved, is refused and the codec merged again):
// header, code, tables, then the relocation list: rel_count code offsets of the 32-bit words that hold addresses
// inside the code (the codec map's self_refs). The code runs at the window's start; each listed word gets
// (window start - stock run address) added. The code and tables in the slot stay the stock bytes.
#ifndef ODR_HRC7000_CODEC_SLOT_H
#define ODR_HRC7000_CODEC_SLOT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define CODEC_SLOT_MAGIC	0x43524444u	// "DDRC" little-endian: ODR HR_C7000 codec slot
#define CODEC_SLOT_V2		2u
#define CODEC_SLOT_ENTRIES	7u
#define CODEC_SLOT_REL_MAX	64u		// more self references than any known build (3) by far: a bad list is refused

// Code follows at +code_off, tables at +tab_off, the relocation list at +rel_off
typedef struct {
	uint32_t magic, version;
	uint32_t code_off, code_len, code_ram;	// code_ram: the stock run address
	uint32_t tab_off, tab_len, tab_ram;
	// Entry points, offsets from the code start
	uint32_t decode, encode, dec_init, dec_set, enc_init, enc_set, fec_init;
	uint32_t stock_tag;			// which stock file: its version number (the board's codec_maps)
	uint32_t crc;				// CRC-32 of code, tables and the relocation list, as in the slot
	uint32_t rel_off, rel_count;
	uint32_t reserved[13];
} codec_slot_t;
_Static_assert(sizeof(codec_slot_t) == 128, "codec slot header layout");

// The board's windows: where the code may run (RAM) and where the tables go (modem SRAM)
typedef struct {
	uint32_t run_lo, run_hi, tab_lo, tab_hi;
} codec_windows_t;

static inline uint32_t codec_slot_crc32(const uint8_t *p, uint32_t len, uint32_t c)
{
	static const uint32_t nib[16] = {
		0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
		0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
	};
	c = ~c;
	while (len--) {
		c ^= *p++;
		c = (c >> 4) ^ nib[c & 15];
		c = (c >> 4) ^ nib[c & 15];
	}
	return ~c;
}

static inline bool codec_slot_within(uint32_t a, uint32_t len, uint32_t lo, uint32_t hi)
{
	return a >= lo && a <= hi && len <= hi - a;
}

static inline uint32_t codec_slot_word(const uint8_t *p)
{
	uint32_t w;
	memcpy(&w, p, 4);
	return w;
}

// Header sane, every part inside the slot, the code and tables inside the board's windows, entries and relocations
// inside the code, CRC right. *run: where the code must run. Doesn't touch RAM.
static inline bool codec_slot_check(const uint8_t *slot, uint32_t room, const codec_windows_t *win, uint32_t *run)
{
	codec_slot_t s;
	if (room < sizeof s) {
		return false;
	}
	memcpy(&s, slot, sizeof s);
	if (s.magic != CODEC_SLOT_MAGIC || s.version != CODEC_SLOT_V2) {
		return false;
	}
	if (s.code_off < sizeof s || s.code_off > room || s.code_len < 8 || s.code_len > room - s.code_off ||
		s.tab_off > room || s.tab_len > room - s.tab_off ||
		!codec_slot_within(s.tab_ram, s.tab_len, win->tab_lo, win->tab_hi)) {
		return false;
	}
	const uint32_t ep[CODEC_SLOT_ENTRIES] = { s.decode, s.encode, s.dec_init, s.dec_set, s.enc_init, s.enc_set,
						  s.fec_init };
	for (uint32_t i = 0; i < CODEC_SLOT_ENTRIES; i++) {
		if (ep[i] >= s.code_len || (ep[i] & 1u) != 0) {
			return false;
		}
	}
	uint32_t crc = codec_slot_crc32(slot + s.tab_off, s.tab_len, codec_slot_crc32(slot + s.code_off, s.code_len, 0));
	if (s.rel_count > CODEC_SLOT_REL_MAX || (s.rel_off & 3u) != 0 || s.rel_off > room ||
		s.rel_count * 4u > room - s.rel_off ||
		!codec_slot_within(win->run_lo, s.code_len, win->run_lo, win->run_hi) ||
		((win->run_lo - s.code_ram) & 3u) != 0) {	// literal pools are found by PC & ~3: keep the alignment
		return false;
	}
	for (uint32_t i = 0; i < s.rel_count; i++) {
		uint32_t off = codec_slot_word(slot + s.rel_off + 4u * i);
		if ((off & 3u) != 0 || off > s.code_len - 4u || off < 8u) {	// never the signature words
			return false;
		}
		// the word must hold an address inside the stock code: a list for another build stops here
		uint32_t w = codec_slot_word(slot + s.code_off + off);
		if (w < s.code_ram || w - s.code_ram >= s.code_len) {
			return false;
		}
	}
	crc = codec_slot_crc32(slot + s.rel_off, s.rel_count * 4u, crc);
	*run = win->run_lo;
	return crc == s.crc;
}

// After the code was copied to dst (its run address): add the move to every listed word
static inline void codec_slot_relocate(const uint8_t *slot, uint8_t *dst, uint32_t run)
{
	codec_slot_t s;
	memcpy(&s, slot, sizeof s);
	uint32_t delta = run - s.code_ram;
	for (uint32_t i = 0; i < s.rel_count; i++) {
		uint32_t off = codec_slot_word(slot + s.rel_off + 4u * i);
		uint32_t w = codec_slot_word(dst + off) + delta;
		memcpy(dst + off, &w, 4);
	}
}

#endif
