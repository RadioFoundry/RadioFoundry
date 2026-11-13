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
}

#endif
