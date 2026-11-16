// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Small text builders for the UI: the values a sentence shows (frequencies, times, places). The buffer and the
// formatter are textfmt.h
#ifndef UI_FMT_H
#define UI_FMT_H

#include <stdint.h>
#include <stddef.h>
#include "hal_radio.h"
#include "textfmt.h"

// A buffer of its own for a value a helper makes: SB_LOCAL(f, 12); sb_freq(&f, hz); then f_text is the value
#define SB_LOCAL(name, size) \
	char name##_text[size]; \
	sbuf_t name; \
	sb_init(&name, name##_text, sizeof name##_text)
// Upper-case hex, exactly digits digits (a P25 NAC: 293, F7E)
void sb_hex(sbuf_t *b, uint32_t v, int digits);
// MHz with 3 decimals, more (up to 5) when the frequency needs them: 146.520, 446.00625
void sb_freq(sbuf_t *b, uint32_t hz);
// CTCSS 100.0, DCS D023N / D023I, empty for none
void sb_tone(sbuf_t *b, const subtone_t *t);
// TX against RX: "Off" when they are the same, else the signed offset, "+600k", "-5M", "+7.6M"
void sb_split(sbuf_t *b, uint32_t rx_hz, uint32_t tx_hz);
// m:ss
void sb_mmss(sbuf_t *b, uint32_t seconds);
// Elapsed time as a short age: 45s, 14m, 3h, 2d
void sb_age(sbuf_t *b, uint32_t ms);		// text alternates, longest first (font.h)
void sb_age_long(sbuf_t *b, uint32_t ms);	// "14m ago", no alternates (for wrapped text)
// Degrees from 1e-7 degree units with 4 decimals: -84.3880
void sb_degrees(sbuf_t *b, int32_t e7);
// A latitude or longitude with 6 decimals (0.1 m) and its hemisphere: "33.990512 N"
void sb_coord(sbuf_t *b, int32_t e7, char pos, char neg);
// The nearest of the 8 compass points to a bearing in degrees: "NE"
void sb_compass(sbuf_t *b, uint16_t bearing);
// A distance and a compass point: "850 m NE", "12.3 km S", "240 km W"
void sb_range(sbuf_t *b, uint32_t meters, uint16_t bearing);

#endif
