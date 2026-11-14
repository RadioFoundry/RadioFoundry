// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The staged Tier III system (radio_trunk.h)
#include <string.h>
#include "radio/radio_trunk.h"

#if CONFIG_DMR_TRUNK
static radio_t3_setup_t staged;

radio_t3_setup_t *radio_t3_staged(void)
{
	return &staged;
}

void radio_t3_reset(radio_t3_setup_t *s)
{
	memset(s, 0, sizeof(*s));
	s->cfg.opts = T3_OPT_PRIVATE;
	s->cfg.auth.category = T3_PAR_A;
	s->cfg.plan.learn = true;
	s->cc = 1;
	s->sys = RADIO_T3_STAGED;
	radio_t3_fix(s);
}

void radio_t3_fix(radio_t3_setup_t *s)
{
	s->cfg.auth.sites = s->cfg.sites;
	s->cfg.plan.table = s->chans;
	s->cfg.plan.ntable = s->nchans;
	s->cfg.poll_data = s->poll_reply;
#if CONFIG_DMR_TRUNK_SUPL
	s->cfg.supl_data = s->supl;
#endif
#if CONFIG_DMR_TRUNK_GW
	s->cfg.dial = s->dial;
#endif
}

#if CONFIG_DMR_TRUNK_GW
void radio_t3_dial_args(const char *digits, uint32_t w[2])
{
	w[0] = w[1] = 0xFFFFFFFFu;
	for (unsigned i = 0; i < 16u && digits != NULL; i++) {
		char c = digits[i];
		uint32_t n = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c == '*' ? 0xAu : c == '#' ? 0xBu : 0xFu;
		if (n == 0xFu) {
			break;
		}
		unsigned sh = 28u - 4u * (i % 8u);
		w[i / 8u] = (w[i / 8u] & ~(0xFu << sh)) | n << sh;
	}
}
#endif

#endif
