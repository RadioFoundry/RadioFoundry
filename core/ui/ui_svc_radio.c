// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The ui_svc.h calls the radio core already answers: the talk target (docs/spec/dmr-voice.md), scan, key beeps and
// the radio's settings (docs/spec/radio-features.md). These are the strong definitions; ui_svc_stub.c keeps the weak
// ones for what other packages still have to provide.
#include "ui_svc.h"
#include "radio/radio.h"
#include "radio/beep.h"
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
	default:
		return false;	// permanent: writing the channel's contact needs the codeplug writer (storage-cps)
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

// The UI's settings that live in radio_settings_t. A local copy, so a run of changes at start-up doesn't read back a
// copy the radio task hasn't taken in yet
void ui_svc_apply(uint8_t id, int32_t value)
{
	static radio_settings_t rs;
	static bool have;
	if (!have) {
		radio_get_settings(&rs);
		have = true;
	}
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
		rs.rx_save = value != 0 ? 1 : 0;
		break;
	default:
		return;
	}
	radio_set_settings(&rs);
}
