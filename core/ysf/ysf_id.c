// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The YSF radio ID (ysf_id.h, docs/spec/ysf.md 6)
#include "ysf/ysf_id.h"

// One 32-bit word folded into the hash with a full avalanche: every input bit moves about half the output bits, so
// IDs that differ in one bit (neighboring parts of one wafer) give unrelated RIDs
static uint32_t mix(uint32_t h, uint32_t w)
{
	h ^= w;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

static void to_rid(uint32_t h, char out[YSF_RID_LEN + 1])
{
	static const char digit[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

	h %= YSF_RID_SPACE;
	for (int8_t i = YSF_RID_LEN - 1; i >= 0; i--) {
		out[i] = digit[h % 36u];
		h /= 36u;
	}
	out[YSF_RID_LEN] = '\0';
}

void ysf_rid_from_uid(const uint32_t uid[3], char out[YSF_RID_LEN + 1])
{
	uint32_t h = 0x9E3779B9u;	// a fixed start, so a zero word still moves the hash

	for (uint8_t i = 0; i < 3; i++)
		h = mix(h, uid[i]);
	to_rid(mix(h, 3u), out);	// the length last, as a hash of a fixed-size key
}

bool ysf_uid_usable(const uint32_t uid[3])
{
	return !(uid[0] == 0 && uid[1] == 0 && uid[2] == 0) &&
	       !(uid[0] == 0xFFFFFFFFu && uid[1] == 0xFFFFFFFFu && uid[2] == 0xFFFFFFFFu);
}

void ysf_rid_fallback(uint32_t dmr_id, const char *callsign, char out[YSF_RID_LEN + 1])
{
	uint32_t h = mix(0x7F4A7C15u, dmr_id);

	for (; callsign && *callsign; callsign++)
		h = mix(h, (uint8_t)*callsign);
	to_rid(h, out);
}
