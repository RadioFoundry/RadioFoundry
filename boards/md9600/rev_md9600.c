// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 board revisions: the descriptor table, the stored record and the choice between record, probe and build
// default (rfplan_md9600.h; docs/spec/hardware-md9600.md 16)
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

md9600_rev_state_t md9600_rev_resolve(md9600_rev_t stored, md9600_rev_t probed, md9600_rev_t build_default)
{
	md9600_rev_state_t s = { MD9600_REV_UNKNOWN, MD9600_REV_SRC_NONE, false, false };
	// A person said which board this is: that holds even when this image can't drive it (then the receiver stays
	// off rather than being programmed for a board it isn't)
	if (md9600_rev_desc(stored) != NULL) {
		s.rev = stored;
		s.source = MD9600_REV_SRC_STORED;
		s.usable = md9600_rev_built(stored);
		s.confirmed = true;
		return s;
	}
	if (md9600_rev_desc(probed) != NULL && md9600_rev_built(probed)) {
		s.rev = probed;
		s.source = MD9600_REV_SRC_PROBE;
		s.usable = true;
		return s;
	}
	if (md9600_rev_desc(build_default) != NULL && md9600_rev_built(build_default)) {
		s.rev = build_default;
		s.source = MD9600_REV_SRC_DEFAULT;
		s.usable = true;
		return s;
	}
	// The default isn't in this image: the newest revision that is
	for (int r = MD9600_REV_COUNT - 1; r > MD9600_REV_UNKNOWN; r--) {
		if (md9600_rev_built((md9600_rev_t)r)) {
			s.rev = (md9600_rev_t)r;
			s.source = MD9600_REV_SRC_DEFAULT;
			s.usable = true;
			break;
		}
	}
	return s;
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
