// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 board revisions: the descriptor table, the stored record, the AK2365A read test's verdict and the choice
// between detection, record, probe and build default (rfplan_md9600.h; docs/spec/hardware-md9600.md 16)
#include <stddef.h>
#include "rfplan_md9600.h"
#include "config.h"

static const md9600_rev_desc_t revs[MD9600_REV_COUNT] = {
	[MD9600_REV_V1V3] = {
		.name = "V1-V3", .pll = MD9600_PLL_SKY72310, .pll_bits = 16, .if_chip = true,
		.vhf_vco_x2 = true, .tx_other_c5_off = false, .vhf_tune_at_power_on = false,
	},
	[MD9600_REV_V4] = {
		.name = "V4", .pll = MD9600_PLL_PLL1590, .pll_bits = 24, .if_chip = true,
		.vhf_vco_x2 = false, .tx_other_c5_off = true, .vhf_tune_at_power_on = true,
	},
	[MD9600_REV_V5] = {
		.name = "V4A/V5", .pll = MD9600_PLL_LMX2571, .pll_bits = 24, .if_chip = false,
		.vhf_vco_x2 = false, .tx_other_c5_off = true, .vhf_tune_at_power_on = false,
	},
};

const md9600_rev_desc_t *md9600_rev_desc(md9600_rev_t rev)
{
	if (rev <= MD9600_REV_UNKNOWN || rev >= MD9600_REV_COUNT) {
		return NULL;
	}
	return &revs[rev];
}

bool md9600_rev_built(md9600_rev_t rev)
{
	switch (rev) {
	case MD9600_REV_V1V3:
		return CONFIG_MD9600_V1V3 != 0;
	case MD9600_REV_V4:
		return CONFIG_MD9600_V4 != 0;
	case MD9600_REV_V5:
		return CONFIG_MD9600_V5 != 0;
	default:
		return false;
	}
}

// "M96R", a format byte, the revision and its complement, a spare byte
#define REC_FORMAT	1u

void md9600_rev_record_encode(md9600_rev_t rev, uint8_t out[MD9600_REV_RECORD_SIZE])
{
	out[0] = 'M';
	out[1] = '9';
	out[2] = '6';
	out[3] = 'R';
	out[4] = REC_FORMAT;
	out[5] = (uint8_t)rev;
	out[6] = (uint8_t)~(uint8_t)rev;
	out[7] = 0;
}

md9600_rev_t md9600_rev_record_decode(const uint8_t in[MD9600_REV_RECORD_SIZE])
{
	if (in[0] != 'M' || in[1] != '9' || in[2] != '6' || in[3] != 'R' || in[4] != REC_FORMAT) {
		return MD9600_REV_UNKNOWN;
	}
	if ((uint8_t)(in[5] ^ in[6]) != 0xFFu || md9600_rev_desc((md9600_rev_t)in[5]) == NULL) {
		return MD9600_REV_UNKNOWN;
	}
	return (md9600_rev_t)in[5];
}

// The first of a and b that is built
static md9600_rev_t first_built(md9600_rev_t a, md9600_rev_t b)
{
	return md9600_rev_built(a) ? a : md9600_rev_built(b) ? b : MD9600_REV_UNKNOWN;
}

static bool if_family(md9600_rev_t r)
{
	return r == MD9600_REV_V1V3 || r == MD9600_REV_V4;
}

md9600_rev_state_t md9600_rev_resolve(md9600_rev_t stored, uint8_t if_chip, md9600_rev_t probed,
				      md9600_rev_t build_default)
{
	md9600_rev_state_t s = { MD9600_REV_UNKNOWN, MD9600_REV_SRC_NONE, false, false, if_chip };
	if (if_chip == MD9600_IF_ABSENT) {
		// Only V4A / V5 have no AK2365A: the hardware says so, whatever a person stored
		s.rev = MD9600_REV_V5;
		s.source = MD9600_REV_SRC_DETECTED;
		s.usable = md9600_rev_built(MD9600_REV_V5);
		s.confirmed = true;
		return s;
	}
	bool has_if = if_chip == MD9600_IF_PRESENT;
	// A person said which board this is: that holds even when this image can't drive it (then the receiver stays
	// off rather than being programmed for a board it isn't). With an AK2365A only V1-V3 and V4 can be right
	if (md9600_rev_desc(stored) != NULL && (!has_if || if_family(stored))) {
		s.rev = stored;
		s.source = MD9600_REV_SRC_STORED;
		s.usable = md9600_rev_built(stored);
		s.confirmed = true;
		return s;
	}
	if (md9600_rev_desc(probed) != NULL && md9600_rev_built(probed) && (!has_if || if_family(probed))) {
		s.rev = probed;
		s.source = MD9600_REV_SRC_PROBE;
		s.usable = true;
		return s;
	}
	md9600_rev_t d = MD9600_REV_UNKNOWN;
	if (has_if) {
		d = first_built(MD9600_REV_V1V3, MD9600_REV_V4);	// V1-V3 boards are the common ones
	} else if (md9600_rev_desc(build_default) != NULL && md9600_rev_built(build_default)) {
		d = build_default;
	} else {
		// The default isn't in this image: the newest revision that is
		for (int r = MD9600_REV_COUNT - 1; r > MD9600_REV_UNKNOWN && d == MD9600_REV_UNKNOWN; r--) {
			if (md9600_rev_built((md9600_rev_t)r)) {
				d = (md9600_rev_t)r;
			}
		}
	}
	if (d != MD9600_REV_UNKNOWN) {
		s.rev = d;
		s.source = MD9600_REV_SRC_DEFAULT;
		s.usable = true;
	}
	return s;
}

const uint8_t md9600_if_patterns[MD9600_IF_PATTERNS] = { 0x5Au, 0xA5u };

// The pattern read back at one alignment of every frame of the band
static bool if_answers(const uint16_t r[2][MD9600_IF_PATTERNS])
{
	for (uint8_t a = 0; a + 8u <= MD9600_IF_READ_BITS; a++) {
		uint8_t shift = (uint8_t)(MD9600_IF_READ_BITS - 8u - a);
		bool all = true;
		for (uint8_t pull = 0; pull < 2u && all; pull++) {
			for (uint8_t k = 0; k < MD9600_IF_PATTERNS && all; k++) {
				all = ((r[pull][k] >> shift) & 0xFFu) == md9600_if_patterns[k];
			}
		}
		if (all) {
			return true;
		}
	}
	return false;
}

// Every sample of the band followed the pull: nothing drives the line
static bool if_floats(const uint16_t r[2][MD9600_IF_PATTERNS])
{
	const uint16_t ones = (uint16_t)((1u << MD9600_IF_READ_BITS) - 1u);
	for (uint8_t k = 0; k < MD9600_IF_PATTERNS; k++) {
		if (r[0][k] != 0u || r[1][k] != ones) {
			return false;
		}
	}
	return true;
}

md9600_if_t md9600_if_verdict(const uint16_t r[2][2][MD9600_IF_PATTERNS])
{
	if (if_answers(r[0]) || if_answers(r[1])) {
		return MD9600_IF_PRESENT;
	}
	return if_floats(r[0]) && if_floats(r[1]) ? MD9600_IF_ABSENT : MD9600_IF_UNKNOWN;
}

md9600_rev_t md9600_rev_probe_pick(uint16_t baseline, const uint16_t *readings, const md9600_rev_t *cand, uint8_t n,
				   uint16_t threshold)
{
	uint16_t best = 0, second = 0;
	md9600_rev_t pick = MD9600_REV_UNKNOWN;
	for (uint8_t i = 0; i < n; i++) {
		uint16_t d = readings[i] > baseline ? (uint16_t)(readings[i] - baseline) : (uint16_t)(baseline - readings[i]);
		if (d > best) {
			second = best;
			best = d;
			pick = cand[i];
		} else if (d > second) {
			second = d;
		}
	}
	if (best == 0 || best < threshold || (uint32_t)second * 2u > best) {
		return MD9600_REV_UNKNOWN;
	}
	return pick;
}

static void cat(char *buf, uint8_t len, uint8_t *n, const char *t)
{
	while (*t && *n + 1u < len) {
		buf[(*n)++] = *t++;
	}
	buf[*n] = '\0';
}

bool md9600_rev_text(uint8_t line, md9600_rev_state_t s, int16_t probe, char *buf, uint8_t len)
{
	static const char *const sources[] = { "none", "stored", "probe", "default", "detected" };
	static const char *const ak[] = { "?", "yes", "no" };
	uint8_t n = 0;
	if (len == 0 || line >= MD9600_REV_TEXT_LINES) {
		return false;
	}
	buf[0] = '\0';
	if (line == 0) {
		const md9600_rev_desc_t *d = md9600_rev_desc(s.rev);
		cat(buf, len, &n, "rev ");
		cat(buf, len, &n, d != NULL ? d->name : "?");
		cat(buf, len, &n, " ");
		// A revision this image doesn't drive (stored, in practice) keeps the receiver off: that, not the source
		cat(buf, len, &n, d != NULL && !s.usable ? "unbuilt" : s.source < 5 ? sources[s.source] : "?");
	} else if (line == 1) {
		// Only a stored or detected revision allows transmit: ask for one until there is one
		cat(buf, len, &n, s.confirmed ? "rev confirmed" : "Set rev: Adv>Hardware");
	} else {
		const md9600_rev_desc_t *p = probe >= 0 ? md9600_rev_desc((md9600_rev_t)probe) : NULL;
		cat(buf, len, &n, "ak ");
		cat(buf, len, &n, s.if_chip < 3 ? ak[s.if_chip] : "?");
		cat(buf, len, &n, " probe ");
		cat(buf, len, &n, probe < 0 ? "off" : p != NULL ? p->name : "none");
	}
	return true;
}
