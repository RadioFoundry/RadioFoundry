// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Small text builders for the UI
#include "fmt.h"
#include "font.h"
#include "i18n.h"

void sb_hex(sbuf_t *b, uint32_t v, int digits)
{
	for (int sh = 4 * (digits - 1); sh >= 0; sh -= 4)
		sb_chr(b, "0123456789ABCDEF"[(v >> sh) & 0xFu]);
}

void sb_freq(sbuf_t *b, uint32_t hz)
{
	sb_u32(b, hz / 1000000u, 1);
	sb_chr(b, '.');
	uint32_t frac = hz % 1000000u;	// Hz
	sb_u32(b, frac / 1000u, 3);
	uint32_t rest = (frac % 1000u) / 10u;	// 10 Hz units, two more decimals at most
	if (rest) {
		sb_chr(b, (char)('0' + rest / 10u));
		if (rest % 10u)
			sb_chr(b, (char)('0' + rest % 10u));
	}
}

void sb_tone(sbuf_t *b, const subtone_t *t)
{
	switch (t->type) {
	case TONE_CTCSS:
		sb_u32(b, t->value / 10u, 1);
		sb_chr(b, '.');
		sb_u32(b, t->value % 10u, 1);
		break;
	case TONE_DCS:
	case TONE_DCS_INV:
		sb_chr(b, 'D');
		sb_chr(b, (char)('0' + ((t->value >> 6) & 7u)));
		sb_chr(b, (char)('0' + ((t->value >> 3) & 7u)));
		sb_chr(b, (char)('0' + (t->value & 7u)));
		sb_chr(b, t->type == TONE_DCS_INV ? 'I' : 'N');
		break;
	default:
		break;
	}
}

void sb_split(sbuf_t *b, uint32_t rx_hz, uint32_t tx_hz)
{
	if (tx_hz == rx_hz) {
		sb_str(b, TR(VALUE_OFF, "Off"));
		return;
	}
	sb_chr(b, tx_hz > rx_hz ? '+' : '-');
	uint32_t d = tx_hz > rx_hz ? tx_hz - rx_hz : rx_hz - tx_hz;
	// Below 1 MHz in kHz, else in MHz; three decimals at most, trailing zeros dropped: "+600k", "+12.5k", "-7.6M"
	bool mhz = d >= 1000000u;
	uint32_t unit = mhz ? 1000000u : 1000u;
	sb_u32(b, d / unit, 1);
	uint32_t frac = d % unit / (unit / 1000u);
	if (frac != 0) {
		sb_chr(b, '.');
		for (uint32_t div = 100u; frac != 0; div /= 10u) {
			sb_chr(b, (char)('0' + frac / div));
			frac %= div;
		}
	}
	sb_chr(b, mhz ? 'M' : 'k');
}

void sb_mmss(sbuf_t *b, uint32_t seconds)
{
	sb_u32(b, seconds / 60u, 1);
	sb_chr(b, ':');
	sb_u32(b, seconds % 60u, 2);
}

// The age's number and unit letter: 45 's', 14 'm', 3 'h', 2 'd'
// The unit goes to unit[0]: the caller's is a string of one letter
static uint32_t age_unit(uint32_t ms, char *unit)
{
	uint32_t s = ms / 1000u;
	if (s < 60u) {
		*unit = 's';
		return s;
	}
	if (s < 3600u) {
		*unit = 'm';
		return s / 60u;
	}
	if (s < 86400u) {
		*unit = 'h';
		return s / 3600u;
	}
	*unit = 'd';
	return s / 86400u;
}

void sb_age_long(sbuf_t *b, uint32_t ms)
{
	char unit[2] = "";
	uint32_t v = age_unit(ms, unit);
	TRF(b, VALUE_AGE_LONG, "%u%K ago", v, unit);
}

// "14m ago", "14m" as text alternates: the drawing keeps the longest that fits (font.h). One unit style everywhere,
// so two ages on one screen always read alike
void sb_age(sbuf_t *b, uint32_t ms)
{
	char unit[2] = "";
	uint32_t v = age_unit(ms, unit);
	TRF(b, VALUE_AGE, TEXT_ALT_OPEN "%1$u%2$K ago" TEXT_ALT_NEXT "%1$u%2$K" TEXT_ALT_CLOSE, v, unit);
}

void sb_degrees(sbuf_t *b, int32_t e7)
{
	uint32_t v = (uint32_t)(e7 < 0 ? -(int64_t)e7 : e7);
	v = (v + 500u) / 1000u;	// 1e-4 degree
	if (e7 < 0)
		sb_chr(b, '-');
	sb_u32(b, v / 10000u, 1);
	sb_chr(b, '.');
	sb_u32(b, v % 10000u, 4);
}

void sb_coord(sbuf_t *b, int32_t e7, char pos, char neg)
{
	uint32_t v = (uint32_t)(e7 < 0 ? -(int64_t)e7 : e7);
	v = (v + 5u) / 10u;	// 1e-6 degree
	sb_u32(b, v / 1000000u, 1);
	sb_chr(b, '.');
	sb_u32(b, v % 1000000u, 6);
	sb_chr(b, ' ');
	sb_chr(b, e7 < 0 ? neg : pos);
}

void sb_compass(sbuf_t *b, uint16_t bearing)
{
	static const char *const points[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	sb_str(b, points[((bearing % 360u) * 2u + 45u) / 90u % 8u]);
}

void sb_range(sbuf_t *b, uint32_t meters, uint16_t bearing)
{
	if (meters < 1000u) {
		sb_u32(b, meters, 1);
		sb_str(b, " m ");
	} else if (meters < 100000u) {
		uint32_t h = (meters + 50u) / 100u;	// 0.1 km
		sb_u32(b, h / 10u, 1);
		sb_chr(b, '.');
		sb_u32(b, h % 10u, 1);
		sb_str(b, " km ");
	} else {
		sb_u32(b, (meters + 500u) / 1000u, 1);
		sb_str(b, " km ");
	}
	sb_compass(b, bearing);
}
