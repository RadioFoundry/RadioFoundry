// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR's UI services (ui_dstar.c): the station set on the radio, kept in the settings over the codeplug's D-STAR
// record, and UR through the radio core. The radio core takes a change at once (radio_req_dstar_reload)
#include <string.h>
#include "config.h"

#if CONFIG_DSTAR
#include "hal_time.h"
#include "radio/radio.h"
#include "settings/settings.h"
#include "dstar/dstar_codeplug.h"

// The on-air field without its trailing spaces
static void trimmed(char *out, uint8_t cap, const char *f, size_t n)
{
	while (n > 0 && (f[n - 1] == ' ' || f[n - 1] == '\0')) {
		n--;
	}
	if (n >= cap) {
		n = cap - 1u;
	}
	memcpy(out, f, n);
	out[n] = '\0';
}

bool ui_svc_dstar_station(uint8_t field, char *out, uint8_t cap)
{
	if (field > 2 || cap == 0) {
		return false;
	}
	settings_t *s = settings_begin();
	if (field == 0) {
		trimmed(out, cap, s->dstar_my, sizeof s->dstar_my);
	} else if (field == 1) {
		trimmed(out, cap, s->dstar_suffix, sizeof s->dstar_suffix);
	} else {
		trimmed(out, cap, s->dstar_msg, sizeof s->dstar_msg);
	}
	settings_end(0);
	return true;
}

bool ui_svc_dstar_station_set(uint8_t field, const char *text)
{
	if (field > 2) {
		return false;
	}
	settings_t *s = settings_begin();
	if (field == 0) {
		dstar_field_set(s->dstar_my, sizeof s->dstar_my, text);
	} else if (field == 1) {
		dstar_field_set(s->dstar_suffix, sizeof s->dstar_suffix, text);
	} else {
		dstar_msg_set(s->dstar_msg, text);
	}
	settings_end(hal_millis());
	radio_req_dstar_reload();
	return true;
}

void ui_svc_dstar_ur(uint8_t mode, const char *ref)
{
	radio_req_dstar_ur(mode, ref);
}
#endif
