// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// APRS information fields (aprs_fmt.h). Written from the APRS Protocol Reference 1.0.1: chapter 6 (time and position
// formats), 7 (data extensions, altitude), 8 (position reports), 9 (compressed), 10 (Mic-E), 11 (objects and items),
// 14 (messages), 16 (status); the 1.1 addendum for the reply-ack form of message numbers.
#include <string.h>
#include "aprs/aprs_fmt.h"

#define PI_F	3.14159265f
#define EARTH_M	6371000.0f

// --- Compressed course and speed, and altitude: no libm (logf and powf cost several KB of flash) ---

// The compressed speed byte is round(log(knots + 1) / log(1.08)), at most 89 (chapter 9). speed_step[j - 1] is the
// lowest speed whose byte is j or more. The float formula's nearest tie is 0.0007 of a step away (at 264 knots), far
// beyond any logf's error, so this is the same byte for every speed from 0 to 65535 (tests/host/aprsTest.c)
static const uint16_t speed_step[89] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 19, 20, 22, 24, 26, 28, 30, 33, 35, 38, 41, 45, 48, 52, 56, 61, 66, 71, 77, 83, 90, 97, 105, 113, 122,
	132, 143, 154, 166, 180, 194, 210, 227, 245, 264, 286, 309, 333, 360, 389, 420, 454, 490, 529, 572, 617, 667,
	720, 778, 840, 907,
};

// A power of 1.002 or 1.08 (the float constants the formula has always used: 1.002f is 1.0019999743) as a Q24 number,
// by squaring a table: base^(2^k) as a 32-bit mantissa m and exponent e, value m * 2^(e - 31). The 2^k terms of n
// multiply, each step rounding by 2^-32, so the result is good to a few parts in 1e8
static const uint32_t alt_mant[14] = {
	0x80418900u, 0x8083338Eu, 0x8106ED97u, 0x820FF744u, 0x84287041u, 0x8873744Cu, 0x9175BF45u,
	0xA54D31AAu, 0xD5793297u, 0xB202E791u, 0xF790141Cu, 0xEF6757E4u, 0xDFE21FD4u, 0xC3CBBB30u,
};
static const uint8_t alt_exp[14] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 5, 11, 23 };
static const uint32_t spd_mant[7] = {
	0x8A3D7100u, 0x954C9926u, 0xAE248263u, 0xECEB4CE8u, 0xDB42AE12u, 0xBBCB2730u, 0x89C26C78u,
};
static const uint8_t spd_exp[7] = { 0, 0, 0, 0, 1, 3, 7 };

static uint64_t pow_q24(const uint32_t *tm, const uint8_t *te, uint32_t n)
{
	uint32_t m = 0x80000000u;	// 1.0
	int e = 0;
	for (uint32_t k = 0; n >> k; k++) {
		if (!((n >> k) & 1u)) {
			continue;
		}
		uint64_t q = (uint64_t)m * tm[k];	// 2^62 .. 2^64: the top bit decides the shift
		uint32_t sh = q >> 63 ? 32u : 31u;
		q = (q + (1ull << (sh - 1u))) >> sh;
		e += te[k] + (int)(sh - 31u);
		if (q >> 32) {	// rounded up to 2^32
			q >>= 1;
			e++;
		}
		m = (uint32_t)q;
	}
	// value = m * 2^(e - 31), in Q24 that is m * 2^(e - 7)
	return e >= 7 ? (uint64_t)m << (e - 7) : (uint64_t)m >> (7 - e);
}

// --- Small text helpers ---

typedef struct {
	char *p;
	size_t n, max;
	bool full;
} out_t;

static void put_c(out_t *o, char c)
{
	if (o->n + 1u >= o->max) {
		o->full = true;
		return;
	}
	o->p[o->n++] = c;
}

static void put_s(out_t *o, const char *s, size_t limit)
{
	for (size_t i = 0; s != NULL && s[i] && i < limit; i++) {
		put_c(o, s[i]);
	}
}

static void put_u(out_t *o, uint32_t v, uint8_t width)
{
	char d[10];
	uint8_t n = 0;
	do {
		d[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0 && n < sizeof(d));
	while (n < width) {
		d[n++] = '0';
	}
	while (n) {
		put_c(o, d[--n]);
	}
}

static size_t finish(out_t *o)
{
	if (o->full || o->max == 0) {
		if (o->max != 0) {
			o->p[0] = '\0';
		}
		return 0;
	}
	o->p[o->n] = '\0';
	return o->n;
}

void aprs_base91(uint32_t v, uint8_t digits, char *out)
{
	for (int i = digits - 1; i >= 0; i--) {
		out[i] = (char)(33 + v % 91u);
		v /= 91u;
	}
}

uint32_t aprs_base91_value(const char *s, uint8_t digits)
{
	uint32_t v = 0;
	for (uint8_t i = 0; i < digits; i++) {
		v = v * 91u + (uint32_t)((uint8_t)s[i] - 33u);
	}
	return v;
}

// --- Building ---

// "DDMM.hh" / "DDDMM.hh" with the hemisphere letter, ambiguity digits blanked
static void put_coord(out_t *o, int32_t v, bool lon, uint8_t ambiguity)
{
	char hemi = lon ? (v < 0 ? 'W' : 'E') : (v < 0 ? 'S' : 'N');
	uint32_t a = (uint32_t)(v < 0 ? -(int64_t)v : v);
	uint32_t deg = a / (uint32_t)APRS_DEG;
	uint32_t hm = (uint32_t)(((uint64_t)(a % (uint32_t)APRS_DEG) * 6000u + (uint32_t)APRS_DEG / 2u) / (uint32_t)APRS_DEG);
	if (hm >= 6000u) {
		hm -= 6000u;
		deg++;
	}
	char d[9];
	out_t t = { d, 0, sizeof(d), false };
	put_u(&t, deg, lon ? 3 : 2);
	put_u(&t, hm / 100u, 2);
	put_c(&t, '.');
	put_u(&t, hm % 100u, 2);
	// blank from the right: hundredths (2), then minutes units and tens, skipping the '.'
	static const uint8_t blank_lat[4] = { 6, 5, 3, 2 }, blank_lon[4] = { 7, 6, 4, 3 };
	for (uint8_t i = 0; i < ambiguity && i < 4; i++) {
		d[lon ? blank_lon[i] : blank_lat[i]] = ' ';
	}
	for (size_t i = 0; i < t.n; i++) {
		put_c(o, d[i]);
	}
	put_c(o, hemi);
}

static void put_altitude(out_t *o, int32_t alt_m)
{
	if (alt_m == APRS_ALT_NONE) {
		return;
	}
	int32_t ft = (int32_t)(((int64_t)alt_m * 3281 + (alt_m < 0 ? -500 : 500)) / 1000);
	put_s(o, "/A=", 3);
	if (ft < 0) {
		put_c(o, '-');
		put_u(o, (uint32_t)(ft < -99999 ? 99999 : -ft), 5);
	} else {
		put_u(o, (uint32_t)(ft > 999999 ? 999999 : ft), 6);
	}
}

size_t aprs_build_position(const aprs_report_t *r, char *out, size_t max)
{
	out_t o = { out, 0, max, false };
	const aprs_pos_t *p = &r->pos;
	bool cse = p->course != APRS_COURSE_NONE && p->speed_kn != APRS_SPEED_NONE;
	put_c(&o, r->messaging ? '=' : '!');
	if (r->compressed) {
		char b[4];
		// truncated, as the reference's worked example (chapter 9) does
		uint32_t y = (uint32_t)((380926LL * (900000000LL - p->lat)) / APRS_DEG);
		uint32_t x = (uint32_t)((190463LL * (1800000000LL + p->lon)) / APRS_DEG);
		put_c(&o, p->sym_table);
		aprs_base91(y, 4, b);
		for (int i = 0; i < 4; i++) {
			put_c(&o, b[i]);
		}
		aprs_base91(x, 4, b);
		for (int i = 0; i < 4; i++) {
			put_c(&o, b[i]);
		}
		put_c(&o, p->sym_code);
		if (cse) {
			uint16_t c = p->course % 360u;
			uint32_t si = 0;
			while (si < 89u && p->speed_kn >= speed_step[si]) {
				si++;
			}
			put_c(&o, (char)(33 + c / 4u));
			put_c(&o, (char)(33 + si));
		} else {
			put_c(&o, ' ');
			put_c(&o, ' ');
		}
		put_c(&o, (char)(33 + 0x22));	// T: current GPS fix, other NMEA source, software origin (chapter 9)
	} else {
		put_coord(&o, p->lat, false, r->ambiguity);
		put_c(&o, p->sym_table);
		put_coord(&o, p->lon, true, r->ambiguity);
		put_c(&o, p->sym_code);
		if (cse) {
			put_u(&o, p->course == 0 ? 360u : p->course, 3);
			put_c(&o, '/');
			put_u(&o, p->speed_kn > 999u ? 999u : p->speed_kn, 3);
		}
	}
	put_altitude(&o, p->alt_m);
	put_s(&o, r->comment, APRS_COMMENT_MAX);
	return finish(&o);
}

size_t aprs_build_status(const char *text, char *out, size_t max)
{
	out_t o = { out, 0, max, false };
	put_c(&o, '>');
	put_s(&o, text, 62);
	return finish(&o);
}

static void put_addressee(out_t *o, const char *a)
{
	size_t i = 0;
	put_c(o, ':');
	for (; a[i] && i < APRS_ADDRESSEE_LEN; i++) {
		put_c(o, a[i]);
	}
	for (; i < APRS_ADDRESSEE_LEN; i++) {
		put_c(o, ' ');
	}
	put_c(o, ':');
}

size_t aprs_build_message(const char *addressee, const char *text, const char *msgno, char *out, size_t max)
{
	out_t o = { out, 0, max, false };
	put_addressee(&o, addressee);
	for (size_t i = 0; text[i] && i < APRS_MSG_TEXT_MAX; i++) {
		char c = text[i];
		put_c(&o, (c == '|' || c == '~' || c == '{') ? ' ' : c);	// characters the spec reserves in messages
	}
	if (msgno != NULL && msgno[0]) {
		put_c(&o, '{');
		put_s(&o, msgno, APRS_MSGNO_MAX);
	}
	return finish(&o);
}

size_t aprs_build_ack(const char *addressee, const char *msgno, bool reject, char *out, size_t max)
{
	out_t o = { out, 0, max, false };
	put_addressee(&o, addressee);
	put_s(&o, reject ? "rej" : "ack", 3);
	put_s(&o, msgno, APRS_MSGNO_MAX);
	return finish(&o);
}

// --- Parsing ---

static bool digit(char c)
{
	return c >= '0' && c <= '9';
}

// "DDMM.hhN" / "DDDMM.hhW"; ambiguity spaces count as 0
static bool parse_coord(const char *s, bool lon, int32_t *v)
{
	uint8_t nd = lon ? 3 : 2;
	uint32_t val[3] = { 0, 0, 0 };	// degrees, minutes, hundredths
	const uint8_t start[3] = { 0, nd, (uint8_t)(nd + 3u) };
	const uint8_t len[3] = { nd, 2, 2 };
	if (s[nd + 2u] != '.') {
		return false;
	}
	for (int f = 0; f < 3; f++) {
		for (uint8_t i = 0; i < len[f]; i++) {
			char c = s[start[f] + i];
			if (c == ' ') {
				c = '0';
			} else if (!digit(c)) {
				return false;
			}
			val[f] = val[f] * 10u + (uint32_t)(c - '0');
		}
	}
	char h = s[nd + 5u];
	bool neg;
	if (lon) {
		if (h != 'E' && h != 'W') {
			return false;
		}
		neg = h == 'W';
	} else {
		if (h != 'N' && h != 'S') {
			return false;
		}
		neg = h == 'S';
	}
	if (val[1] >= 60u || val[0] > (lon ? 180u : 90u)) {
		return false;
	}
	int64_t e7 = (int64_t)val[0] * APRS_DEG + ((int64_t)val[1] * 100 + val[2]) * APRS_DEG / 6000;
	*v = (int32_t)(neg ? -e7 : e7);
	return true;
}

// "/A=001234" anywhere in the comment
static void find_altitude(const char *s, size_t n, int32_t *alt_m)
{
	for (size_t i = 0; i + 9u <= n; i++) {
		if (s[i] == '/' && s[i + 1u] == 'A' && s[i + 2u] == '=') {
			int32_t v = 0;
			bool neg = s[i + 3u] == '-';
			size_t k = i + (neg ? 4u : 3u);
			size_t end = i + 9u;
			bool ok = true;
			for (; k < end; k++) {
				if (!digit(s[k])) {
					ok = false;
					break;
				}
				v = v * 10 + (s[k] - '0');
			}
			if (ok) {
				int32_t ft = neg ? -v : v;
				*alt_m = (int32_t)(((int64_t)ft * 3048 + (ft < 0 ? -5000 : 5000)) / 10000);
				return;
			}
		}
	}
}

static bool table_char(char c)
{
	return c == '/' || c == '\\' || (c >= 'A' && c <= 'Z') || digit(c);
}

// A position at s (uncompressed or compressed), then its data extension and comment
static bool parse_position(const char *s, size_t n, aprs_parsed_t *p)
{
	aprs_pos_t *q = &p->pos;
	q->course = APRS_COURSE_NONE;
	q->speed_kn = APRS_SPEED_NONE;
	q->alt_m = APRS_ALT_NONE;
	size_t used;
	if (n >= 19 && (digit(s[0]) || s[0] == ' ')) {
		if (!parse_coord(s, false, &q->lat) || !parse_coord(s + 9, true, &q->lon) || !table_char(s[8])) {
			return false;
		}
		q->sym_table = s[8];
		q->sym_code = s[18];
		used = 19;
		if (n >= used + 7u && digit(s[19]) && digit(s[20]) && digit(s[21]) && s[22] == '/' && digit(s[23]) &&
		    digit(s[24]) && digit(s[25]) && q->sym_code != '_') {
			uint16_t c = (uint16_t)((s[19] - '0') * 100 + (s[20] - '0') * 10 + (s[21] - '0'));
			uint16_t v = (uint16_t)((s[23] - '0') * 100 + (s[24] - '0') * 10 + (s[25] - '0'));
			q->course = (c == 0 || c > 360) ? APRS_COURSE_NONE : c;
			q->speed_kn = v;
			used += 7u;
		}
	} else if (n >= 13 && (table_char(s[0]) || (s[0] >= 'a' && s[0] <= 'j'))) {
		for (int i = 1; i < 9; i++) {
			if ((uint8_t)s[i] < 33 || (uint8_t)s[i] > 123) {
				return false;
			}
		}
		uint32_t y = aprs_base91_value(s + 1, 4), x = aprs_base91_value(s + 5, 4);
		q->lat = (int32_t)(900000000LL - ((int64_t)y * APRS_DEG + 380926 / 2) / 380926);
		q->lon = (int32_t)(((int64_t)x * APRS_DEG + 190463 / 2) / 190463 - 1800000000LL);
		if (q->lat > 900000000 || q->lat < -900000000 || q->lon > 1800000000 || q->lon < -1800000000) {
			return false;
		}
		q->sym_table = (s[0] >= 'a' && s[0] <= 'j') ? (char)('0' + (s[0] - 'a')) : s[0];
		q->sym_code = s[9];
		// cs and T are base-91 ('!'..'{'): anything else carries no course, speed or altitude (unsigned: char is
		// signed on some compilers, and 0x80 and up must not become negative values)
		uint8_t c = (uint8_t)s[10], sp = (uint8_t)s[11], t = (uint8_t)s[12];
		if (c != ' ' && c >= 33 && c <= 123 && sp >= 33 && sp <= 123 && t >= 33 && t <= 123) {
			uint8_t tv = (uint8_t)(t - 33);
			if (((tv >> 3) & 3u) == 2u) {		// GGA source: cs is the altitude
				// 1.002^n feet, as meters rounded: 0.3048 m a foot, in Q24 (the float version is within a meter)
				uint64_t ft = pow_q24(alt_mant, alt_exp, (uint32_t)((c - 33) * 91 + (sp - 33)));
				q->alt_m = (int32_t)((ft * 3048u + ((uint64_t)5000u << 24)) / ((uint64_t)10000u << 24));
			} else if (c - 33 <= 89) {
				q->course = (uint16_t)((c - 33) * 4);
				if (q->course == 0) {
					q->course = APRS_COURSE_NONE;
				}
				q->speed_kn = (uint16_t)((pow_q24(spd_mant, spd_exp, (uint32_t)(sp - 33)) - (1u << 24) + (1u << 23)) >> 24);
			}
		}
		used = 13;
	} else {
		return false;
	}
	p->has_pos = true;
	p->text = s + used;
	p->text_len = (uint16_t)(n - used);
	find_altitude(p->text, p->text_len, &q->alt_m);
	if (q->sym_code == '_' && p->type == APRS_T_POSITION) {
		p->type = APRS_T_WEATHER;	// a weather station's report: the extension is wind, not course / speed
	}
	return true;
}

// Mic-E destination character: the digit (0..9, 10 = ambiguity space), the message bit, and custom
static bool mice_char(char c, uint8_t *d, bool *bit, bool *custom)
{
	*custom = false;
	if (c >= '0' && c <= '9') {
		*d = (uint8_t)(c - '0');
		*bit = false;
	} else if (c >= 'A' && c <= 'J') {
		*d = (uint8_t)(c - 'A');
		*bit = true;
		*custom = true;
	} else if (c == 'K') {
		*d = 0;
		*bit = true;
		*custom = true;
	} else if (c == 'L') {
		*d = 0;
		*bit = false;
	} else if (c >= 'P' && c <= 'Y') {
		*d = (uint8_t)(c - 'P');
		*bit = true;
	} else if (c == 'Z') {
		*d = 0;
		*bit = true;
	} else {
		return false;
	}
	return true;
}

static bool parse_mice(const ax25_addr_t *dst, const char *s, size_t n, aprs_parsed_t *p)
{
	uint8_t d[6];
	bool bit[6], custom[6], any_custom = false;
	if (dst == NULL || strlen(dst->call) != 6 || n < 9) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		if (!mice_char(dst->call[i], &d[i], &bit[i], &custom[i])) {
			return false;
		}
		if (i < 3 && custom[i]) {
			any_custom = true;
		}
	}
	aprs_pos_t *q = &p->pos;
	uint32_t lat_deg = d[0] * 10u + d[1], lat_min = d[2] * 10u + d[3], lat_hun = d[4] * 10u + d[5];
	if (lat_deg > 90u || lat_min >= 60u) {
		return false;
	}
	int64_t lat = (int64_t)lat_deg * APRS_DEG + ((int64_t)lat_min * 100 + lat_hun) * APRS_DEG / 6000;
	q->lat = (int32_t)(bit[3] ? lat : -lat);	// char 4: P-Z north

	int lon_deg = (uint8_t)s[1] - 28;
	if (bit[4]) {
		lon_deg += 100;				// char 5: P-Z longitude offset +100
	}
	if (lon_deg >= 180 && lon_deg <= 189) {
		lon_deg -= 80;
	} else if (lon_deg >= 190 && lon_deg <= 199) {
		lon_deg -= 190;
	}
	int lon_min = (uint8_t)s[2] - 28;
	if (lon_min >= 60) {
		lon_min -= 60;
	}
	int lon_hun = (uint8_t)s[3] - 28;
	if (lon_deg < 0 || lon_deg > 180 || lon_min < 0 || lon_min >= 60 || lon_hun < 0 || lon_hun > 99) {
		return false;
	}
	int64_t lon = (int64_t)lon_deg * APRS_DEG + ((int64_t)lon_min * 100 + lon_hun) * APRS_DEG / 6000;
	q->lon = (int32_t)(bit[5] ? -lon : lon);	// char 6: P-Z west

	int sp = (uint8_t)s[4] - 28, dc = (uint8_t)s[5] - 28, se = (uint8_t)s[6] - 28;
	int speed = sp * 10 + dc / 10, course = (dc % 10) * 100 + se;
	if (speed >= 800) {
		speed -= 800;
	}
	if (course >= 400) {
		course -= 400;
	}
	q->speed_kn = (uint16_t)(speed < 0 ? 0 : speed);
	q->course = (course <= 0 || course > 360) ? APRS_COURSE_NONE : (uint16_t)course;
	q->sym_code = s[7];
	q->sym_table = s[8];
	q->alt_m = APRS_ALT_NONE;

	uint8_t v = (uint8_t)((bit[0] ? 4u : 0u) | (bit[1] ? 2u : 0u) | (bit[2] ? 1u : 0u));
	p->mice_msg = (uint8_t)((7u - v) + (any_custom ? 8u : 0u));	// M0 (111) .. M6 (001), 7 = emergency (000)

	size_t k = 9;
	if (k < n && (s[k] == '`' || s[k] == '\'' || s[k] == '>' || s[k] == ']')) {
		k++;		// the radio's type character before the altitude
	}
	if (k + 4u <= n && s[k + 3u] == '}') {
		q->alt_m = (int32_t)aprs_base91_value(s + k, 3) - 10000;
		k += 4u;
	} else if (n >= 13 && s[12] == '}') {
		q->alt_m = (int32_t)aprs_base91_value(s + 9, 3) - 10000;
		k = 13;
	}
	p->has_pos = true;
	p->text = s + k;
	p->text_len = (uint16_t)(n - k);
	return true;
}

static void parse_message(const char *s, size_t n, aprs_parsed_t *p)
{
	p->type = APRS_T_MESSAGE;
	if (n < 11 || s[10] != ':') {
		p->type = APRS_T_OTHER;
		return;
	}
	size_t al = APRS_ADDRESSEE_LEN;
	while (al > 0 && s[al] == ' ') {
		al--;
	}
	memcpy(p->addressee, s + 1, al);
	p->addressee[al] = '\0';
	const char *t = s + 11;
	size_t tl = n - 11u;
	// ack / rej
	if (tl >= 4 && (memcmp(t, "ack", 3) == 0 || memcmp(t, "rej", 3) == 0)) {
		size_t k = 3;
		size_t m = 0;
		while (k < tl && m < APRS_MSGNO_MAX && t[k] != '}' && t[k] != ' ') {
			p->msgno[m++] = t[k++];
		}
		p->msgno[m] = '\0';
		if (m > 0) {
			p->ack = t[0] == 'a';
			p->rej = t[0] == 'r';
			p->text = t;
			p->text_len = 0;
			return;
		}
	}
	// the message number: after the last '{', up to 5 characters (1.1: "{MM}AA", the part before the '}')
	const char *brace = NULL;
	for (size_t i = 0; i < tl; i++) {
		if (t[i] == '{') {
			brace = t + i;
		}
	}
	p->text = t;
	p->text_len = (uint16_t)tl;
	if (brace != NULL) {
		size_t m = 0;
		const char *c = brace + 1;
		while (c < t + tl && m < APRS_MSGNO_MAX && *c != '}' && *c != '\r' && *c != '\n') {
			p->msgno[m++] = *c++;
		}
		p->msgno[m] = '\0';
		p->text_len = (uint16_t)(brace - t);
	}
}

bool aprs_parse(const ax25_addr_t *dst, const uint8_t *info, size_t n, aprs_parsed_t *p)
{
	memset(p, 0, sizeof(*p));
	const char *s = (const char *)info;
	if (n == 0) {
		return false;
	}
	p->type = APRS_T_OTHER;
	p->text = s;
	p->text_len = (uint16_t)n;
	switch (s[0]) {
	case '!':
	case '=':
		p->type = APRS_T_POSITION;
		p->messaging = s[0] == '=';
		if (!parse_position(s + 1, n - 1u, p)) {
			p->type = APRS_T_UNKNOWN;
		}
		break;
	case '/':
	case '@':
		p->type = APRS_T_POSITION;
		p->messaging = s[0] == '@';
		if (n < 8 || !parse_position(s + 8, n - 8u, p)) {
			p->type = APRS_T_UNKNOWN;
		}
		break;
	case ';':
		p->type = APRS_T_OBJECT;
		if (n < 18 || (s[10] != '*' && s[10] != '_')) {
			p->type = APRS_T_UNKNOWN;
			break;
		}
		memcpy(p->name, s + 1, APRS_NAME_MAX);
		for (int i = APRS_NAME_MAX; i > 0 && p->name[i - 1] == ' '; i--) {
			p->name[i - 1] = '\0';
		}
		p->killed = s[10] == '_';
		if (!parse_position(s + 18, n - 18u, p)) {
			p->type = APRS_T_UNKNOWN;
		}
		break;
	case ')': {
		p->type = APRS_T_ITEM;
		size_t k = 1;
		while (k < n && k <= APRS_NAME_MAX + 1u && s[k] != '!' && s[k] != '_') {
			k++;
		}
		if (k < 4 || k >= n || k > APRS_NAME_MAX + 1u || (s[k] != '!' && s[k] != '_')) {
			p->type = APRS_T_UNKNOWN;
			break;
		}
		memcpy(p->name, s + 1, k - 1u);
		p->killed = s[k] == '_';
		if (!parse_position(s + k + 1u, n - k - 1u, p)) {
			p->type = APRS_T_UNKNOWN;
		}
		break;
	}
	case '`':
	case '\'':
	case 0x1C:
	case 0x1D:
		p->type = APRS_T_MICE;
		if (!parse_mice(dst, s, n, p)) {
			p->type = APRS_T_UNKNOWN;
		}
		break;
	case '>':
		p->type = APRS_T_STATUS;
		p->text = s + 1;
		p->text_len = (uint16_t)(n - 1u);
		break;
	case ':':
		parse_message(s, n, p);
		break;
	case '}':
		p->type = APRS_T_THIRD_PARTY;
		p->text = s + 1;
		p->text_len = (uint16_t)(n - 1u);
		break;
	case '?':
		p->type = APRS_T_QUERY;
		break;
	case '_':
		p->type = APRS_T_WEATHER;
		break;
	case 'T':
		p->type = APRS_T_TELEMETRY;
		break;
	default:
		break;
	}
	return true;
}

// --- Geometry ---

// Single-precision polynomials instead of libm's sinf, cosf, atan2f and sqrtf (4 KB of flash with their range
// reduction, which angles of at most pi never need). Each is good to 1e-5 rad or better; the haversine and the
// bearing below are written without the cancellations the textbook forms have, so a station a few meters away
// still gets a true bearing

// sin x for |x| <= pi / 2: the Taylor series to x^13, error under 1e-9 (less than the float's own step)
static float sin_h(float x)
{
	float x2 = x * x;
	return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f *
		(1.0f - x2 / 156.0f))))));
}

// cos x for |x| <= pi / 2: to x^12, error under 7e-9
static float cos_h(float x)
{
	float x2 = x * x;
	return 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f * (1.0f - x2 / 56.0f * (1.0f - x2 / 90.0f *
		(1.0f - x2 / 132.0f)))));
}

// sqrt of v >= 0: a bit-pattern first guess (within 4 %) and three Newton steps, exact to the float's last bit or two
static float sqrt_f(float v)
{
	if (!(v > 0.0f)) {
		return 0.0f;
	}
	union { float f; uint32_t u; } g = { v };
	g.u = 0x1FBD1DF5u + (g.u >> 1);
	for (int i = 0; i < 3; i++) {
		g.f = 0.5f * (g.f + v / g.f);
	}
	return g.f;
}

// atan2(y, x) in radians, (-pi, pi], 0 for (0, 0): atan on the octant 0..1 by Abramowitz and Stegun 4.4.49, error
// under 1e-5 rad
static float atan2_f(float y, float x)
{
	float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y;
	if (ax == 0.0f && ay == 0.0f) {
		return 0.0f;
	}
	bool swap = ay > ax;
	float t = swap ? ax / ay : ay / ax, t2 = t * t;
	float a = t * (0.9998660f - t2 * (0.3302995f - t2 * (0.1801410f - t2 * (0.0851330f - t2 * 0.0208351f))));
	if (swap) {
		a = PI_F / 2.0f - a;
	}
	if (x < 0.0f) {
		a = PI_F - a;
	}
	return y < 0.0f ? -a : a;
}

// The longitude difference to the short way round, 1e-7 degree, at most 180 degrees
static int32_t dlon_e7(int32_t lon1, int32_t lon2)
{
	int64_t d = (int64_t)lon2 - lon1;
	if (d > 180LL * APRS_DEG) {
		d -= 360LL * APRS_DEG;
	} else if (d < -180LL * APRS_DEG) {
		d += 360LL * APRS_DEG;
	}
	return (int32_t)d;
}

static float rad(int32_t e7)
{
	return (float)e7 * (PI_F / 180.0f / (float)APRS_DEG);
}

// The haversine's central angle in radians
static float central(int32_t lat1, int32_t lat2, int32_t dlon)
{
	float sp = sin_h(rad(lat2 - lat1) / 2.0f), sl = sin_h(rad(dlon) / 2.0f);
	float a = sp * sp + cos_h(rad(lat1)) * cos_h(rad(lat2)) * sl * sl;
	if (a > 1.0f) {
		a = 1.0f;
	}
	return 2.0f * atan2_f(sqrt_f(a), sqrt_f(1.0f - a));
}

uint32_t aprs_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
	int32_t dl = dlon_e7(lon1, lon2);
	float c = central(lat1, lat2, dl);
	if (c > PI_F / 2.0f) {
		// Past a quarter of the globe the float cannot resolve 1 - a (a step of 1.2e-7 is km of arc near the
		// antipode): measure to the antipode instead, which is a short distance, and take it from pi
		c = PI_F - central(lat1, -lat2, dl > 0 ? dl - 180 * APRS_DEG : dl + 180 * APRS_DEG);
	}
	return (uint32_t)(EARTH_M * c + 0.5f);
}

uint16_t aprs_bearing_deg(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
	// y = sin(dl) cos(p2); x = cos(p1) sin(p2) - sin(p1) cos(p2) cos(dl) = sin(dp) + 2 sin(p1) cos(p2) sin(dl/2)^2
	float hp = rad(lat2 - lat1) / 2.0f, hl = rad(dlon_e7(lon1, lon2)) / 2.0f;
	float sl = sin_h(hl), c2 = cos_h(rad(lat2));
	float y = 2.0f * sl * cos_h(hl) * c2;
	float x = 2.0f * sin_h(hp) * cos_h(hp) + 2.0f * sin_h(rad(lat1)) * c2 * sl * sl;
	float b = atan2_f(y, x) * (180.0f / PI_F);
	if (b < 0.0f) {
		b += 360.0f;
	}
	uint16_t d = (uint16_t)(b + 0.5f);
	return d >= 360u ? 0u : d;
}
