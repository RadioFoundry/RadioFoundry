// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The ui_svc.h calls the radio core already answers: the talk target (docs/spec/dmr-voice.md), scan, key beeps and
// the radio's settings (docs/spec/radio-features.md). These are the strong definitions; ui_svc_stub.c keeps the weak
// ones for what other packages still have to provide.
#include "ui_svc.h"
#include "radio/radio.h"
#include "radio/beep.h"
#include <string.h>
#include "config.h"

uint32_t ui_svc_feat_voice(void)
{
	return CONFIG_DMR ? UI_FEAT_TALK_TARGET : 0;
}

uint32_t ui_svc_feat_radio(void)
{
	return CONFIG_SCAN ? UI_FEAT_SCAN : 0;
}

#if CONFIG_DMR
bool ui_svc_talk_to(uint32_t id, bool private_call, uint8_t life)
{
	switch (life) {
	case UI_TALK_TEMPORARY:
		radio_req_dmr_target(id, private_call);
		return true;
	case UI_TALK_DEFAULT:
		radio_req_dmr_target_clear();
		return true;
	case UI_TALK_PERMANENT:
		if (!ui_svc_save_talk_target(id, private_call))
			return false;
		radio_req_dmr_target_clear();	// the channel's own target is the new one once the radio re-reads it
		return true;
	default:
		return false;
	}
}
#endif

#if CONFIG_SCAN
// The UI only asks for "on" when its copy of view.scan says the radio isn't scanning, so TOGGLE starts the zone
// scan (or the VFO band scan in the VFO) the way the radio core chooses
bool ui_svc_scan(bool on)
{
	radio_req_scan(on ? RSCAN_TOGGLE : RSCAN_STOP);
	return true;
}

bool ui_svc_scan_skip(void)
{
	radio_req_scan(RSCAN_NUISANCE);
	return true;
}
#endif

#if CONFIG_BEEP
void ui_svc_beep(uint8_t beep)
{
	radio_req_beep(beep == UI_BEEP_ERROR ? BEEP_ERROR : BEEP_KEY);
}
#endif

// The UI's settings that live in radio_settings_t. The radio core saves them (settings store) and restores them at
// start-up, so ui_svc_setting_get reads them back from it (ui_svc_storage.c) and a value it already has is not sent
// again. A local copy, so a run of changes doesn't read back one the radio task hasn't taken in yet
void ui_svc_apply(uint8_t id, int32_t value)
{
	static radio_settings_t rs;
	static bool have;
	if (!have) {
		radio_get_settings(&rs);
		have = true;
	}
	radio_settings_t was = rs;
	uint16_t v16 = (uint16_t)(value < 0 ? 0 : value > 0xFFFF ? 0xFFFF : value);
	switch (id) {
	case UI_SET_KEY_TONES:
		rs.key_beep = value != 0;
		break;
	case UI_SET_TONE_VOLUME:		// 0..10
		rs.beep_level = (uint8_t)(v16 > 10 ? 100 : v16 * 10u);
		break;
	case UI_SET_TOT_S:
		rs.tot_s = v16;
		break;
	case UI_SET_BACKLIGHT_S:		// dims after this, goes off after four times as long (0 = always on)
		rs.backlight_dim_s = v16;
		rs.backlight_off_s = (uint16_t)(v16 > 0x3FFF ? 0xFFFF : v16 * 4u);
		break;
	case UI_SET_BATTERY_SAVER:
		rs.rx_save = value != 0 ? (rs.rx_save != 0 ? rs.rx_save : 1) : 0;
		break;
	default:
		return;
	}
	if (memcmp(&was, &rs, sizeof rs) != 0)
		radio_set_settings(&rs);
}
