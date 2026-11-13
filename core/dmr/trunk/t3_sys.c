// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III system identity and access rules (see t3_sys.h).
#include <string.h>
#include "dmr/trunk/t3_sys.h"

// Table 6.5 / figure 6.15: 14 bits after MODEL split between NET and SITE, 2 bits PAR
static const uint8_t net_bits[4] = { 9, 7, 4, 2 };

uint8_t t3_net_bits(uint8_t model)
{
	return net_bits[model & 3u];
}

uint8_t t3_site_bits(uint8_t model)
{
	return (uint8_t)(12u - net_bits[model & 3u]);
}

void t3_syscode_split(uint16_t code, t3_syscode_t *out)
{
	out->model = (uint8_t)(code >> 14);
	uint8_t sb = t3_site_bits(out->model);
	uint16_t ns = (uint16_t)((code >> 2) & 0xFFFu);
	out->net = (uint16_t)(ns >> sb);
	out->site = (uint16_t)(ns & ((1u << sb) - 1u));
	out->par = (uint8_t)(code & 3u);
}

uint16_t t3_syscode_join(const t3_syscode_t *in)
{
	uint8_t sb = t3_site_bits(in->model);
	uint16_t ns = (uint16_t)(((uint32_t)in->net << sb) | (in->site & ((1u << sb) - 1u)));
	return (uint16_t)((uint32_t)(in->model & 3u) << 14 | (uint32_t)(ns & 0xFFFu) << 2 | (in->par & 3u));
}

uint16_t t3_sys_area(uint16_t code, uint8_t dmrla)
{
	t3_syscode_t s;
	t3_syscode_split(code, &s);
	uint8_t sb = t3_site_bits(s.model);
	if (dmrla > sb) {
		dmrla = sb;
	}
	return (uint16_t)(s.site >> (sb - dmrla));
}

uint8_t t3_response_check(uint16_t code)
{
	// Bits 8-3 of the 16-bit C_SYScode (bit 1 = least significant): the low SITE bits above PAR
	return (uint8_t)((code >> 2) & 0x3Fu);
}

t3_auth_result_t t3_authorize(const t3_auth_t *a, uint16_t code)
{
	t3_syscode_t s;
	t3_syscode_split(code, &s);
	if (s.model != a->model) {
		return T3_AUTH_MODEL;
	}
	if (s.net != a->net) {
		return T3_AUTH_NET;
	}
	if (a->nsites != 0) {
		bool found = false;
		for (uint8_t i = 0; i < a->nsites && !found; i++) {
			found = a->sites[i] == s.site;
		}
		if (!found) {
			return T3_AUTH_SITE;
		}
	}
	if ((s.par & a->category) == 0) {
		return T3_AUTH_PAR;
	}
	return T3_AUTH_OK;
}

bool t3_mask_match(uint8_t mask, uint32_t pdu_addr, uint32_t own)
{
	if (mask == 0) {
		return true;
	}
	if (mask > 24) {
		mask = 24;
	}
	uint32_t m = (mask == 24) ? 0xFFFFFFu : ((1u << mask) - 1u);
	return ((pdu_addr ^ own) & m) == 0;
}

uint8_t t3_nwait_frames(uint8_t nrand_wait)
{
	nrand_wait &= 0xFu;
	if (nrand_wait == 14) {
		return 15;
	}
	if (nrand_wait == 15) {
		return 24;
	}
	return nrand_wait;
}

uint8_t t3_backoff_frames(uint8_t backoff)
{
	static const uint8_t frames[16] = { 1, 1, 2, 3, 4, 5, 8, 11, 15, 20, 26, 33, 41, 50, 70, 100 };
	return frames[backoff & 0xFu];
}

uint32_t t3_auth_response(const uint8_t key[T3_AUTH_KEY_LEN], uint32_t challenge, uint32_t psn)
{
	uint8_t k[3 + T3_AUTH_KEY_LEN];
	k[0] = (uint8_t)(challenge >> 16);
	k[1] = (uint8_t)(challenge >> 8);
	k[2] = (uint8_t)challenge;
	memcpy(k + 3, key, T3_AUTH_KEY_LEN);
	uint8_t st[256];
	for (int i = 0; i < 256; i++) {
		st[i] = (uint8_t)i;
	}
	uint8_t j = 0;
	for (int i = 0; i < 256; i++) {
		j = (uint8_t)(j + st[i] + k[i % (int)sizeof(k)]);
		uint8_t t = st[i];
		st[i] = st[j];
		st[j] = t;
	}
	uint8_t i = 0;
	uint32_t r = 0;
	j = 0;
	for (int n = 0; n < 259; n++) {
		i++;
		j = (uint8_t)(j + st[i]);
		uint8_t t = st[i];
		st[i] = st[j];
		st[j] = t;
		if (n >= 256) {
			r = (r << 8) | st[(uint8_t)(st[i] + st[j])];
		}
	}
	return (r ^ psn) & 0xFFFFFFu;
}
