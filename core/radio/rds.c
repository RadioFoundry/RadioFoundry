// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RDS / RBDS group decoder (rds.h; docs/spec/airband-and-broadcast.md 4.6)
#include "config.h"

#if CONFIG_BCAST_RDS	// the whole file
#include <string.h>
#include "radio/rds.h"

// Block B: group type b15:12, version b11 (1 = B), TP b10, PTY b9:5; then per group type
#define B_TYPE(b)	((unsigned)((b) >> 12))
#define B_VER_B(b)	(((b) >> 11) & 1u)
#define B_TP(b)		(((b) >> 10) & 1u)
#define B_PTY(b)	((uint8_t)(((b) >> 5) & 0x1Fu))
#define B_TA(b)		(((b) >> 4) & 1u)	// group 0
#define B_PS_SEG(b)	((unsigned)((b) & 3u))	// group 0: which 2 characters of the PS
#define B_RT_AB(b)	((int8_t)(((b) >> 4) & 1u))	// group 2: text A/B flag, toggles with a new text
#define B_RT_SEG(b)	((unsigned)((b) & 0xFu))	// group 2: which 4 (2A) or 2 (2B) characters

#define RT_END		0x0D	// RadioText ends here when shorter than its 64 (32) characters
#define RT_SEGS		16u
#define PS_SEGS		4u

void rds_reset(rds_t *r)
{
	memset(r, 0, sizeof(*r));
	r->rt_ab = -1;
}

// The standard's character table matches ASCII for the printable range bar a few symbols; the rest (accented
// letters, controls) has no glyph in the radio's fonts
static char shown(char c)
{
	unsigned u = (unsigned char)c;
	return u >= 0x20u && u < 0x7Fu ? c : u < 0x20u ? ' ' : '?';
}

// Trailing spaces off; true when out changed
static bool put_text(char *out, size_t cap, const char *in, size_t n)
{
	while (n > 0 && in[n - 1] == ' ') {
		n--;
	}
	if (n >= cap) {
		n = cap - 1;
	}
	if (strncmp(out, in, n) == 0 && out[n] == 0) {
		return false;
	}
	memcpy(out, in, n);
	out[n] = 0;
	return true;
}

// A segment of n characters heard: confirmed when the same text arrived the time before. True when the confirmed
// text changed
static bool take(char *heard, char *conf, uint16_t *heard_mask, uint16_t *conf_mask, unsigned seg, const char *c,
		 unsigned n)
{
	uint16_t bit = (uint16_t)(1u << seg);
	char *h = heard + seg * n;
	bool again = (*heard_mask & bit) != 0 && memcmp(h, c, n) == 0;
	memcpy(h, c, n);
	*heard_mask |= bit;
	if (!again || ((*conf_mask & bit) != 0 && memcmp(conf + seg * n, c, n) == 0)) {
		return false;
	}
	memcpy(conf + seg * n, c, n);
	*conf_mask |= bit;
	return true;
}

static bool ps_update(rds_t *r)
{
	if (r->ps_conf_mask != (1u << PS_SEGS) - 1u) {
		return false;	// not all of it yet
	}
	char s[RDS_PS_LEN];
	for (unsigned i = 0; i < RDS_PS_LEN; i++) {
		s[i] = shown(r->ps_conf[i]);
	}
	return put_text(r->info.ps, sizeof r->info.ps, s, RDS_PS_LEN);
}

// The text is shown once every segment up to its end is confirmed
static bool rt_update(rds_t *r)
{
	unsigned n = r->rt_2b ? 2u : 4u;
	char s[RDS_RT_LEN];
	size_t len = 0;
	for (unsigned seg = 0; seg < RT_SEGS; seg++) {
		if ((r->rt_conf_mask & (1u << seg)) == 0) {
			return false;
		}
		for (unsigned i = 0; i < n; i++) {
			char c = r->rt_conf[seg * n + i];
			if (c == RT_END) {
				return put_text(r->info.rt, sizeof r->info.rt, s, len);
			}
			s[len++] = shown(c);
		}
	}
	return put_text(r->info.rt, sizeof r->info.rt, s, len);
}

static void rt_clear(rds_t *r)
{
	r->rt_heard_mask = r->rt_conf_mask = 0;
	r->info.rt[0] = 0;
}

// The program identification in block A. False: the group is from another station than the one shown (a new
// station is taken when its PI arrives twice in a row)
static bool take_pi(rds_t *r, uint16_t pi, bool *changed)
{
	bool twice = pi == r->pi_cand;
	r->pi_cand = pi;
	if (pi == r->info.pi) {
		return true;
	}
	if (!twice) {
		return r->info.pi == 0;	// nothing shown yet: the text still builds while the PI confirms
	}
	if (r->info.pi != 0) {
		rds_reset(r);	// another station on the frequency: none of the old text is its
		r->pi_cand = pi;
	}
	r->info.pi = pi;
	*changed = true;
	return true;
}

bool rds_group(rds_t *r, const uint16_t blk[4], uint8_t err_a, uint8_t err_b)
{
	if (err_b > RDS_ERR_SMALL) {
		return false;	// the group type itself is in doubt
	}
	bool changed = false;
	if (err_a <= RDS_ERR_SMALL && !take_pi(r, blk[0], &changed)) {
		return changed;
	}
	r->groups++;
	uint16_t b = blk[1];
	bool tp = B_TP(b) != 0;
	if (tp != r->info.tp || B_PTY(b) != r->info.pty) {
		r->info.tp = tp;
		r->info.pty = B_PTY(b);
		changed = true;
	}
	switch (B_TYPE(b)) {
	case 0: {
		bool ta = B_TA(b) != 0;
		if (ta != r->info.ta) {
			r->info.ta = ta;
			changed = true;
		}
		const char c[2] = { (char)(blk[3] >> 8), (char)blk[3] };
		if (take(r->ps_heard, r->ps_conf, &r->ps_heard_mask, &r->ps_conf_mask, B_PS_SEG(b), c, 2)) {
			changed = ps_update(r) || changed;
		}
		break;
	}
	case 2: {
		bool v_b = B_VER_B(b) != 0;
		int8_t ab = B_RT_AB(b);
		if (ab != r->rt_ab || v_b != r->rt_2b) {
			// A new text: the old one no longer applies
			changed = changed || r->info.rt[0] != 0;
			rt_clear(r);
			r->rt_ab = ab;
			r->rt_2b = v_b;
		}
		char c[4];
		unsigned n;
		if (v_b) {
			c[0] = (char)(blk[3] >> 8);
			c[1] = (char)blk[3];
			n = 2;
		} else {
			c[0] = (char)(blk[2] >> 8);
			c[1] = (char)blk[2];
			c[2] = (char)(blk[3] >> 8);
			c[3] = (char)blk[3];
			n = 4;
		}
		if (take(r->rt_heard, r->rt_conf, &r->rt_heard_mask, &r->rt_conf_mask, B_RT_SEG(b), c, n)) {
			changed = rt_update(r) || changed;
		}
		break;
	}
	default:
		break;
	}
	return changed;
}

#endif	// CONFIG_BCAST_RDS
