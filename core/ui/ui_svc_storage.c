// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The ui_svc.h calls storage-cps answers (docs/spec/settings-store.md, core/codeplug editing):
//   - settings: each UI setting is read from where it is kept across power cycles. The radio core's own settings
//     (key tones, tone volume, TX timeout, backlight time, battery saver) from radio_get_settings, which the radio
//     core saves and restores itself (ui_svc_apply sends changes there); the preferences in settings_t (brightness,
//     default squelch, hotspot allowed) directly; the DMR data service's answers (radio check, call alert, LRRP) from
//     its settings, which the radio core saves. Settings nobody stores yet stay in the stub's RAM table.
//   - save to channel and the permanent talk target: written into the channel with codeplug_set_channel; the radio
//     core re-reads its channel when codeplug_serial() changes.
#include <string.h>
#include "ui_svc.h"
#include "radio/radio.h"
#include "settings/settings.h"
#include "codeplug/codeplug.h"
#include "hal_time.h"
#include "config.h"
#if CONFIG_USB
#include "hostlink/hostlink.h"
#endif
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmr_data_api.h"
#endif

uint32_t ui_svc_feat_storage(void)
{
	return UI_FEAT_SAVE_CHANNEL;
}

int32_t ui_svc_setting_get(uint8_t id)
{
	radio_settings_t rs;
	settings_t st;
	switch (id) {
	case UI_SET_KEY_TONES:
	case UI_SET_TONE_VOLUME:
	case UI_SET_TOT_S:
	case UI_SET_BACKLIGHT_S:
	case UI_SET_BATTERY_SAVER:
		radio_get_settings(&rs);
		if (id == UI_SET_KEY_TONES)
			return rs.key_beep;
		if (id == UI_SET_TONE_VOLUME)
			return (rs.beep_level + 5) / 10;
		if (id == UI_SET_TOT_S)
			return rs.tot_s;
		if (id == UI_SET_BACKLIGHT_S)
			return rs.backlight_dim_s;
		return rs.rx_save != 0;
	case UI_SET_BRIGHTNESS:
		settings_read(&st);
		return st.brightness >= 10 && st.brightness <= 100 ? st.brightness : 100;
	case UI_SET_SQUELCH:
		settings_read(&st);
		return st.squelch <= 9 ? st.squelch : ui_svc_ram_get(id);
	case UI_SET_HOTSPOT_AUTO:
		settings_read(&st);
		return st.hotspot_allowed != 0;
#if CONFIG_DMR_DATA_CORE
	case UI_SET_CHECK_REPLY:
	case UI_SET_ALERT_ANSWER:
	case UI_SET_POS_REPLY: {
		dmrDataSettings_t ds;
		dmr_data_get_settings(&ds);
		return id == UI_SET_CHECK_REPLY ? ds.radioCheckReply : id == UI_SET_ALERT_ANSWER ? ds.callAlertAck :
			ds.lrrpReply;
	}
#endif
	default:
		return ui_svc_ram_get(id);
	}
}

bool ui_svc_setting_set(uint8_t id, int32_t value)
{
	settings_t *st;
	switch (id) {
	case UI_SET_KEY_TONES:
	case UI_SET_TONE_VOLUME:
	case UI_SET_TOT_S:
	case UI_SET_BACKLIGHT_S:
	case UI_SET_BATTERY_SAVER:
		return true;	// ui_svc_apply hands it to the radio core, which saves it
	case UI_SET_BRIGHTNESS:
		st = settings_begin();
		st->brightness = (uint8_t)(value < 10 ? 10 : value > 100 ? 100 : value);
		settings_end(hal_millis());
		return true;
	case UI_SET_SQUELCH:
		st = settings_begin();
		st->squelch = (uint8_t)(value < 0 ? 0 : value > 9 ? 9 : value);
		settings_end(hal_millis());
		radio_req_reload();	// the default squelch is a preference the radio core reads on reload
		return true;
	case UI_SET_HOTSPOT_AUTO:
		st = settings_begin();
		st->hotspot_allowed = value != 0;
		settings_end(hal_millis());
#if CONFIG_USB
		hostlink_set_hotspot_allowed(value != 0);
#endif
		return true;
#if CONFIG_DMR_DATA_CORE
	case UI_SET_CHECK_REPLY:
	case UI_SET_ALERT_ANSWER:
	case UI_SET_POS_REPLY: {
		dmrDataSettings_t ds;
		dmr_data_get_settings(&ds);
		if (id == UI_SET_CHECK_REPLY)
			ds.radioCheckReply = value != 0;
		else if (id == UI_SET_ALERT_ANSWER)
			ds.callAlertAck = value != 0;
		else
			ds.lrrpReply = value != 0;
		dmr_data_set_settings(&ds);	// the radio core saves the data service's settings when they change
		return true;
	}
#endif
	default:
		return ui_svc_ram_set(id, value);
	}
}

// The channel the radio is on, as stored, and the radio's view (static: both are too big for the UI task's stack)
static radio_view_t v;
static channel_t ch;

static bool current_channel(void)
{
	radio_get_view(&v);
	return !v.vfo && v.channel_number != 0 && codeplug_channel(v.channel_number, &ch);
}

// Power, bandwidth and tones go into the channel; the squelch is radio-wide (the default squelch preference). The
// radio core re-reads the channel (codeplug_serial), and the clear makes the remaining flags follow the new values
bool ui_svc_save_channel(void)
{
	if (!current_channel())
		return false;
	if (v.temp & RADIO_TEMP_POWER)
		ch.power = v.power;
	if (v.temp & RADIO_TEMP_BW)
		ch.bw = v.ch.bw;
	if (v.temp & RADIO_TEMP_TX_TONE)
		ch.tx_tone = v.ch.tx_tone;
	if (v.temp & RADIO_TEMP_RX_TONE)
		ch.rx_tone = v.ch.rx_tone;
	if (!codeplug_set_channel(v.channel_number, &ch))
		return false;
	if (v.temp & RADIO_TEMP_SQUELCH) {
		settings_t *st = settings_begin();
		st->squelch = v.squelch;
		settings_end(hal_millis());
	}
	radio_req_clear_temporary();
	return true;
}

#if CONFIG_DMR
bool ui_svc_save_talk_target(uint32_t id, bool private_call)
{
	if (!current_channel())
		return false;
	contact_t c;
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		if (!codeplug_contact(i, &c) || c.id != id)
			continue;
		if ((c.type == CONTACT_PRIVATE) != private_call)
			continue;
		ch.contact = i;
		return codeplug_set_channel(v.channel_number, &ch);
	}
	return false;	// not a stored contact: the channel can only point at one
}
#endif
