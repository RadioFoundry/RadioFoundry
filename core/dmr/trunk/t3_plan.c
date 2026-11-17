// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III channel plans (see t3_plan.h).
#include <string.h>
#include "dmr/trunk/t3_plan.h"

uint32_t t3_sep_hz(uint8_t code)
{
	static const uint32_t hz[8] = { 5000, 6250, 10000, 12500, 15000, 20000, 25000, 30000 };
	return code < 8 ? hz[code] : 0;
}

bool t3_fixed_lookup(const t3_fixed_plan_t *f, uint16_t lcn, t3_chan_t *out)
{
	uint32_t sep = t3_sep_hz(f->sep);
	if (f->base_hz == 0 || sep == 0 || lcn < f->first || lcn == 0 || lcn > T3_LPCN_MAX) {
		return false;
	}
	uint32_t tx = f->base_hz + (uint32_t)(lcn - f->first) * sep;
	uint32_t rx = f->tx_low ? tx + f->split_hz : tx - f->split_hz;
	if (!f->tx_low && f->split_hz > tx) {
		return false;
	}
	out->lcn = lcn;
	out->tx_hz = tx;
	out->rx_hz = rx;
	out->cc = T3_CC_ANY;
	return true;
}

void t3_learn_clear(t3_learned_t *l)
{
	memset(l, 0, sizeof(*l));
}

bool t3_learn_add(t3_learned_t *l, const t3_cdef_t *c)
{
	if (c->cdeftype != 0 || c->chan == T3_LPCN_NULL || c->chan > T3_LPCN_MAX || c->rx_hz == 0 || c->tx_hz == 0) {
		return false;
	}
	t3_chan_t e = { .lcn = c->chan, .cc = T3_CC_ANY, .rx_hz = c->rx_hz, .tx_hz = c->tx_hz };
	for (uint8_t i = 0; i < l->n; i++) {
		if (l->e[i].lcn == c->chan) {
			bool changed = l->e[i].rx_hz != e.rx_hz || l->e[i].tx_hz != e.tx_hz;
			l->e[i] = e;
			return changed;
		}
	}
	if (l->n < T3_LEARN_MAX) {
		l->e[l->n++] = e;
	} else {
		l->e[l->next] = e;
		l->next = (uint8_t)((l->next + 1u) % T3_LEARN_MAX);
	}
	return true;
}

bool t3_plan_lookup(const t3_plan_t *plan, const t3_learned_t *l, uint16_t lcn, t3_chan_t *out)
{
	if (lcn == T3_LPCN_NULL || lcn > T3_LPCN_MAX) {
		return false;
	}
	if (plan->learn && l != NULL) {
		for (uint8_t i = 0; i < l->n; i++) {
			if (l->e[i].lcn == lcn) {
				*out = l->e[i];
				return true;
			}
		}
	}
	for (uint16_t i = 0; i < plan->ntable; i++) {
		if (plan->table[i].lcn == lcn) {
			*out = plan->table[i];
			return true;
		}
	}
	return t3_fixed_lookup(&plan->fixed, lcn, out);
}
