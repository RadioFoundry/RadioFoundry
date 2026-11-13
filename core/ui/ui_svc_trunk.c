// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI's Tier III systems (docs/spec/dmr-trunking.md 12): the codeplug's systems listed and selected, the selection
// kept in the settings store and started again at power on
#include <string.h>
#include "config.h"
#include "ui/ui_svc.h"
#include "radio/radio.h"
#include "radio/radio_trunk.h"
#include "settings/settings.h"
#include "hal_time.h"

#if CONFIG_DMR_TRUNK
static bool was_on;	// the radio was on a system: its leaving drops the selection
static bool started;	// the selection stored at power on was started

static uint8_t stored(void)
{
	uint8_t v = settings_begin()->t3_sys;
	settings_end(hal_millis());	// nothing changed: no save
	return v;
}

static void store(uint8_t v)
{
	settings_begin()->t3_sys = v;
	settings_end(hal_millis());
}

uint8_t ui_svc_t3_count(void)
{
	char name[CODEPLUG_NAME_LEN + 1];
	uint8_t c = 0;
	for (uint8_t n = 0; n < RADIO_T3_SYSTEMS; n++) {
		c += radio_t3_name(n, name) ? 1u : 0u;
	}
	return c;
}

bool ui_svc_t3_system(uint8_t row, uint8_t *n, char *name)
{
	for (uint8_t i = 0; i < RADIO_T3_SYSTEMS; i++) {
		if (radio_t3_name(i, name) && row-- == 0) {
			*n = i;
			return true;
		}
	}
	return false;
}

int ui_svc_t3_selected(void)
{
	return (int)stored() - 1;
}

// Load system n into the staged system and start it
static bool start(uint8_t n)
{
	if (!radio_t3_load(n, radio_t3_staged())) {
		return false;
	}
	radio_req_t3(RADIO_T3_START);
	return true;
}

bool ui_svc_t3_select(uint8_t n)
{
	if (ui_svc_t3_selected() == (int)n) {
		store(0);		// first: the stop seen by the poll then finds nothing to drop
		radio_req_t3(RADIO_T3_STOP);
		return true;
	}
	if (!start(n)) {
		return false;
	}
	was_on = false;		// the old system's stop, if one ran, isn't this one's leaving
	store((uint8_t)(n + 1u));
	return true;
}

void ui_svc_t3_poll(const radio_view_t *v)
{
	if (!started) {
		started = true;
		uint8_t s = stored();
		if (s != 0 && !start((uint8_t)(s - 1u))) {
			store(0);	// the system is gone from the codeplug
		}
	}
	if (v->t3.on) {
		was_on = true;
	} else if (was_on) {
		was_on = false;		// left: a channel or VFO change, or the debug link
		if (stored() != 0) {
			store(0);
		}
	}
}
#endif
