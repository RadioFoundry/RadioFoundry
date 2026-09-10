// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Small text builders for the UI (no printf: less flash and stack)
#ifndef UI_FMT_H
#define UI_FMT_H

#include <stdint.h>
#include <stddef.h>
#include "hal_radio.h"

// Appends to a fixed buffer, always terminated, silently truncated
typedef struct {
	char *p;
	char *end;	// last usable byte (for the terminator)
} sbuf_t;

void sb_init(sbuf_t *b, char *buf, size_t len);
void sb_str(sbuf_t *b, const char *s);
void sb_chr(sbuf_t *b, char c);
// Unsigned decimal with at least min_digits digits (zero padded)
void sb_u32(sbuf_t *b, uint32_t v, int min_digits);
void sb_i32(sbuf_t *b, int32_t v);
// MHz with 3 decimals, more (up to 5) when the frequency needs them: 146.520, 446.00625
void sb_freq(sbuf_t *b, uint32_t hz);
// CTCSS 100.0, DCS D023N / D023I, empty for none
void sb_tone(sbuf_t *b, const subtone_t *t);
// m:ss
void sb_mmss(sbuf_t *b, uint32_t seconds);
// Elapsed time as a short age: 45s, 14m, 3h, 2d
void sb_age(sbuf_t *b, uint32_t ms);
// Degrees from 1e-7 degree units with 4 decimals: -84.3880
void sb_degrees(sbuf_t *b, int32_t e7);
// A distance and a compass point: "850 m NE", "12.3 km S", "240 km W"
void sb_range(sbuf_t *b, uint32_t meters, uint16_t bearing);

#endif
