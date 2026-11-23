// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ui_svc.h calls storage-cps answers (docs/spec/settings-store.md, core/codeplug editing):
//   - settings: each UI setting is read from where it is kept across power cycles. The radio core's own settings
//     (key tones, tone volume, TX timeout, backlight time, battery saver) from radio_get_settings, which the radio
//     core saves and restores itself (ui_svc_apply sends changes there); the preferences in settings_t (brightness,
//     default squelch, hotspot allowed) directly; the DMR data service's answers (radio check, call alert, LRRP) from
//     its settings, which the radio core saves; the UI's own (reply window, mic gain, auto lock, key timing, GPS
//     options) from their settings_t fields; the position settings from the position service, which saves them.
//     Settings nobody stores yet stay in the stub's RAM table.
//   - save to channel, the permanent talk target and the factory reset: the UI task only takes a copy of what to
//     write and hands a job to the housekeeping task (storage_job_post), so the UI never waits for a flash erase. The
//     channel is written with codeplug_set_channel; the radio core re-reads it when codeplug_serial() changes. A job
//     that fails tells the UI with a UI_EV_SAVE_FAILED event (a toast).
#include <string.h>
#include "board.h"
#include "ui_svc.h"
#include "i18n.h"
#include "radio/radio.h"
#include "settings/settings.h"
#include "radio/ref_trim.h"
#include "storage/storage.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "radio/radio_keys.h"
#include "dmr/dmr_emergency.h"	// the emergency tone (Settings > Sound); stubs without CONFIG_DMR_EMERGENCY
#include "hal_time.h"
#include "config.h"
#if CONFIG_HOSTLINK
#include "hostlink/hostlink.h"
#endif
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmr_data_api.h"
#endif
#if CONFIG_TFTP
#include "net/tftp_codeplug.h"
#endif
#if CONFIG_ZONE_SHARE
#include "dmr/zone_share_api.h"
#endif
#if CONFIG_DMR_AES
#include "codeplug/codeplug_records.h"
#include "dmr/dmrAes.h"
#endif
#if CONFIG_DMR_PRIVACY
#include "dmr/dmrPrivacy.h"
#endif
#if CONFIG_POSITION
#include "position/position.h"
#endif
#include "codeplug/codeplug_tag.h"
#if CONFIG_P25
#include "p25/p25_codeplug.h"
#include "radio/radio_p25.h"
#endif
#if CONFIG_NXDN
#include "nxdn/nxdn_codeplug.h"
#include "radio/radio_nxdn.h"
#endif
#if CONFIG_KEY_ASSIGN
#include "input/input.h"
#include "input/input_engine.h"
#include "input/keyassign.h"
#endif

// The crash report's target is a contact number in 11 spare bits (settings.h)
#if CONFIG_CRASH_REPORT
_Static_assert(CODEPLUG_CONTACTS_MAX <= 0x7FF, "the crash report's target doesn't fit settings_crash_contact");
#endif

uint32_t ui_svc_feat_storage(void)
{
	return UI_FEAT_SAVE_CHANNEL | UI_FEAT_FACTORY_RESET;
}

static uint16_t clamp16(int32_t v, int32_t lo, int32_t hi)
{
	return (uint16_t)(v < lo ? lo : v > hi ? hi : v);
}

// The UI's on / off settings kept as settings_t.ui_flags bits (0: not one of them)
static uint8_t ui_flag(uint8_t id)
{
	switch (id) {
	case UI_SET_LARGE_TEXT: return SETTINGS_UI_LARGE_TEXT;
	case UI_SET_HINTS: return SETTINGS_UI_HINTS;
	case UI_SET_MESSAGE_TONE: return SETTINGS_UI_MESSAGE_TONE;
	case UI_SET_ALIAS_SHOW: return SETTINGS_UI_ALIAS;
	case UI_SET_PTT_LOCKED: return SETTINGS_UI_PTT_LOCKED;
	case UI_SET_HEAR_ALL_DEFAULT: return SETTINGS_UI_HEAR_ALL;
#if CONFIG_DUAL_WATCH
	case UI_SET_DUAL_TALK_BACK: return SETTINGS_UI_DUAL_TALK_BACK;
#endif
	default: return 0;
	}
}

radio_view_t ui_svc_view;	// ui_svc.h

#if CONFIG_DMR_PRIVACY
// Privacy key slot 1 (settings-store.md 3.1), typed in on the radio. While privacy is on, the radio core holds the
// key in use and writes it back to slot 1 itself, so it gets the new one too (type off turns privacy off)
static void privacy_store(uint8_t type, uint32_t key)
{
	if (type >= DMR_PRIVACY_NUM_TYPES) {
		type = DMR_PRIVACY_OFF;
	}
	if (type == DMR_PRIVACY_MOTOROLA_BP && (key < 1 || key > 255)) {
		key = key == 0 ? 1 : 255;
	}
	key &= 0xFFFFu;
	settings_t *st = settings_begin();
	st->privacy_type[0] = type;
	st->privacy_key[0] = type != DMR_PRIVACY_OFF ? key : 0;
	if (type == DMR_PRIVACY_OFF && st->privacy_active == 1) {
		st->privacy_active = 0;
	}
	settings_end(hal_millis());
	radio_get_view(&ui_svc_view);
	if (ui_svc_view.dmr.privacy) {
		radio_req_dmr_privacy(type, (uint16_t)key);
	}
}
#endif

#if CONFIG_KEY_ASSIGN
static int32_t key_get(uint8_t id);
static bool key_set(uint8_t id, int32_t value);
#endif

#if CONFIG_DMR
static bool dmr_id_post(uint32_t id);	// a job: the codeplug write is the housekeeping task's
#endif
// The settings area's key of a Settings > Sound tone row (radio_keys.h radio_tone_rows)
static uint16_t tone_row_key(uint8_t id)
{
	return id == UI_SET_CALL_END_TONE ? CODEPLUG_KEY_CALL_END_TONE : id == UI_SET_TOT_WARN_TONE ? CODEPLUG_KEY_TOT_WARN_TONE :
		id == UI_SET_BATTERY_TONE ? CODEPLUG_KEY_BATTERY_TONE : CODEPLUG_KEY_TX_DENY_TONE;
}
static bool tone_post(uint16_t key, bool on);
#if CONFIG_SIG_TONES
static bool sig_post(uint8_t id, int32_t value);	// a job: the record's write is the housekeeping task's
#endif
#if CONFIG_DMR_DATA_CORE
// The menu row of a setting kept as a key of the settings area (radio_keys.h), else 0
static uint16_t data_row_key(uint8_t id)
{
	return id == UI_SET_ARS_SERVER ? CODEPLUG_KEY_ARS_SERVER : id == UI_SET_IP_GATEWAY ? CODEPLUG_KEY_IP_GATEWAY : 0;
}
static bool data_id_post(uint16_t key, uint32_t id);	// a job too: the key's write is the housekeeping task's
#endif

// The settings the radio core holds (radio_get_settings), apart from those read from flash below: two functions, so
// the radio_settings_t of this one is not on the stack under the latter's structs (the static stack check). INT32_MIN: not
// one of these
static __attribute__((noinline)) int32_t setting_get_radio(uint8_t id)
{
	radio_settings_t rs;
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
		if (id == UI_SET_BACKLIGHT_S)	// < 0: dims, never goes off
			return rs.backlight_off_s == 0 && rs.backlight_dim_s != 0 ? -(int32_t)rs.backlight_dim_s
										    : rs.backlight_dim_s;
		return rs.rx_save != 0;
	case UI_SET_REPLY_S:
		radio_get_settings(&rs);
		return ui_svc_reply_shown(&rs);
#if CONFIG_SFR
	case UI_SET_SFR_RX:
		radio_get_settings(&rs);
		return rs.sfr_rx;
#endif
#if CONFIG_SFR_CWID
	case UI_SET_SFR_CWID:
		radio_get_settings(&rs);
		return rs.sfr_cwid;
#endif
#if CONFIG_DUAL_WATCH
	case UI_SET_DUAL_DELAY_MS:
		radio_get_settings(&rs);
		return rs.dual_delay_ms;
#endif
#if CONFIG_BCAST_FM
	case UI_SET_BCAST_STEP:
		radio_get_settings(&rs);
		return rs.bcast_step == RADIO_BCAST_STEP_100 ? 100 : 200;
#endif
	case UI_SET_TALK_PERMIT:
	case UI_SET_SCAN_TONE:
		radio_get_settings(&rs);
		return id == UI_SET_TALK_PERMIT ? rs.talk_permit : rs.scan_beep;
#if CONFIG_SIG_TONES
	case UI_SET_SIG_DMR ... UI_SET_SIG_DMR_LAST:
		radio_get_settings(&rs);
		return rs.sig_dmr[id - UI_SET_SIG_DMR];
	case UI_SET_SIG_FM ... UI_SET_SIG_FM_LAST:
		radio_get_settings(&rs);
		return rs.sig_fm[id - UI_SET_SIG_FM];
	case UI_SET_SIG_HZ ... UI_SET_SIG_HZ_LAST:
		radio_get_settings(&rs);
		return rs.sig_hz[(id - UI_SET_SIG_HZ) / SIG_STEPS][(id - UI_SET_SIG_HZ) % SIG_STEPS];
	case UI_SET_SIG_MS ... UI_SET_SIG_MS_LAST:
		radio_get_settings(&rs);
		return (int32_t)rs.sig_ms[(id - UI_SET_SIG_MS) / SIG_STEPS][(id - UI_SET_SIG_MS) % SIG_STEPS] * 10;
#endif
	case UI_SET_CALL_END_TONE:
	case UI_SET_TOT_WARN_TONE:
	case UI_SET_BATTERY_TONE:
	case UI_SET_TX_DENY_TONE: {
		// The keys' bits come from the radio core (no flash read here, which sat on the radio and UI tasks' deepest
		// paths)
		radio_get_settings(&rs);
		int row = radio_tone_row(tone_row_key(id));
		return row < 0 || (rs.tones_off >> row & 1u) == 0;
	}
#if CONFIG_DMR_ROAMING
	case UI_SET_ROAM_SURVEY:
	case UI_SET_ROAM_BEACON_S:
	case UI_SET_ROAM_BEACON_LEN:
	case UI_SET_ROAM_PROBE:
		radio_get_settings(&rs);
		return id == UI_SET_ROAM_SURVEY ? rs.roam_survey : id == UI_SET_ROAM_BEACON_S ? rs.roam_beacon_s :
			id == UI_SET_ROAM_BEACON_LEN ? rs.roam_beacon_len_s : rs.roam_probe;
#endif
	default:
		return INT32_MIN;
	}
}

// A setting kept in settings_t. Each case reads just its field (SETTINGS_FIELD): a whole settings_t on this frame once
// made it one of the deepest on the radio, UI and host tasks' paths (docs/dev/stack-check.md)
static __attribute__((noinline)) int32_t setting_get_flash(uint8_t id)
{
	switch (id) {
	case UI_SET_VOLUME:
		return radio_volume_step(SETTINGS_FIELD(volume));
	case UI_SET_KNOB_WHEN_LOCKED:
		return (SETTINGS_FIELD(sound_flags) & SETTINGS_KNOB_LOCK) == 0;	// stored as the lock, shown as the knob working
	case UI_SET_BRIGHTNESS: {
		uint8_t brightness = SETTINGS_FIELD(brightness);
		return brightness >= 10 && brightness <= 100 ? brightness : 100;
	}
	case UI_SET_SQUELCH: {
		uint8_t squelch = SETTINGS_FIELD(squelch);
		return squelch <= 9 ? squelch : ui_svc_ram_get(id);
	}
#if CONFIG_RX_EXT
	case UI_SET_RX_EXT:
		return (SETTINGS_FIELD(cp_flags) & SETTINGS_CP_RX_EXT) != 0;
#endif
	case UI_SET_HOTSPOT_AUTO:
		return SETTINGS_FIELD(hotspot_allowed) != 0;
	case UI_SET_MIC_GAIN: {
		uint8_t mic_gain = SETTINGS_FIELD(mic_gain);
		return mic_gain <= 10 ? mic_gain : 5;
	}
	case UI_SET_REF_TRIM:
		return ref_trim_clamp(SETTINGS_FIELD(ref_trim_ppb));
	case UI_SET_VFO_STEP: {
		uint32_t step = SETTINGS_FIELD(vfo_step_hz);
		return step >= 1000u && step <= 1000000u ? (int32_t)(step / 10u) : 0;
	}
#if CONFIG_DUAL_WATCH
	case UI_SET_DUAL_B: {
		uint16_t dual_b = SETTINGS_FIELD(dual_b);
		uint8_t dual_state = SETTINGS_FIELD(dual_state);
		if (dual_b > UI_DUAL_B_VFO2)
			return 0;
		return dual_b | ((dual_state & SETTINGS_DUAL_ON) ? UI_DUAL_ON : 0) |
		       ((dual_state & SETTINGS_DUAL_B_SEL) ? UI_DUAL_B_SEL : 0);
	}
#endif
	case UI_SET_AUTO_LOCK_S:
		return SETTINGS_FIELD(auto_lock_s);
	case UI_SET_LONG_PRESS_MS: {
		uint16_t ms = SETTINGS_FIELD(long_press_ms);
		return ms != 0 ? ms : ui_svc_ram_get(id);
	}
	case UI_SET_REPEAT_MS: {
		uint16_t ms = SETTINGS_FIELD(repeat_ms);
		return ms != 0 ? ms : ui_svc_ram_get(id);
	}
	case UI_SET_GPS:
		return SETTINGS_FIELD(gps) != 0;
	case UI_SET_POS_FORMAT: {
		uint8_t format = SETTINGS_FIELD(pos_format);
		return format <= 2 ? format : 0;
	}
	case UI_SET_POS_SEND_S:
		return SETTINGS_FIELD(pos_send_s);
	case UI_SET_LANGUAGE:
		return (int32_t)((uint8_t)SETTINGS_FIELD(lang[0]) << 8 | (uint8_t)SETTINGS_FIELD(lang[1]));
	case UI_SET_LARGE_TEXT:
	case UI_SET_HINTS:
	case UI_SET_MESSAGE_TONE:
	case UI_SET_ALIAS_SHOW:
	case UI_SET_PTT_LOCKED:
	case UI_SET_HEAR_ALL_DEFAULT:
#if CONFIG_DUAL_WATCH
	case UI_SET_DUAL_TALK_BACK:
#endif
		return (SETTINGS_FIELD(ui_flags) & ui_flag(id)) != 0;
	case UI_SET_VIBRATE:
		return (SETTINGS_FIELD(ui_flags) & SETTINGS_UI_NO_VIBRATE) == 0;
	case UI_SET_THEME:
		return settings_theme_of(SETTINGS_FIELD(theme));
	case UI_SET_ACCENT:
		return SETTINGS_FIELD(accent) & SETTINGS_ACCENT_COLOR;
	case UI_SET_STRIP:
		return (SETTINGS_FIELD(accent) & SETTINGS_ACCENT_PLAIN) == 0;
	case UI_SET_UPDOWN_HOME:
		return settings_updown_home_of(SETTINGS_FIELD(updown_home));
	case UI_SET_LABEL_FIRST:
		return (SETTINGS_FIELD(updown_home) & SETTINGS_HOME_LABEL_FIRST) != 0;
	case UI_SET_LOW_BATTERY:
		return SETTINGS_FIELD(low_batt_pct);
	case UI_SET_AUTO_OFF_MIN:
		return SETTINGS_FIELD(auto_off_min);
	case UI_SET_EMERGENCY_TONE: {
		dmr_emergency_settings_t es;
		dmr_emergency_get_settings(&es);
		return es.rx_tone;
	}
	case UI_SET_TFTP_WRITE:
		return (SETTINGS_FIELD(net_flags) & SETTINGS_NET_TFTP_NO_WRITE) == 0;
#if CONFIG_ZONE_SHARE
	case UI_SET_ZONE_OFFERS:
		return settings_zs_accept_of(SETTINGS_FIELD(zs_offers));
#endif
#if CONFIG_DMR
	case UI_SET_DMR_ID:
		return (int32_t)codeplug_dmr_id();
#endif
	case UI_SET_NOISE_REDUCTION:
		return (SETTINGS_FIELD(accent) & SETTINGS_AUDIO_NO_NOISE_REDUCTION) == 0;
	case UI_SET_MIC_AGC:
		return (SETTINGS_FIELD(accent) & SETTINGS_AUDIO_AGC) != 0;
#if CONFIG_DMR_PRIVACY
	case UI_SET_PRIVACY_TYPE: {
		uint8_t type = SETTINGS_FIELD(privacy_type[0]);
		return type < DMR_PRIVACY_NUM_TYPES ? type : DMR_PRIVACY_OFF;
	}
	case UI_SET_PRIVACY_KEY:
		return (int32_t)(SETTINGS_FIELD(privacy_key[0]) & 0xFFFFu);
#endif
#if CONFIG_DMR_DATA_CORE
	case UI_SET_CHECK_REPLY:
	case UI_SET_ALERT_ANSWER:
	case UI_SET_POS_REPLY:
	case UI_SET_ARS_SERVER:
	case UI_SET_IP_GATEWAY: {
		dmrDataSettings_t ds;
		dmr_data_get_settings(&ds);
		if (data_row_key(id) != 0) {
			const codeplug_key_row_t *k = codeplug_keys_find(radio_keys, RADIO_KEYS_N, data_row_key(id));
			return k != NULL ? (int32_t)codeplug_keys_field(k, &ds) : 0;
		}
		return id == UI_SET_CHECK_REPLY ? ds.radioCheckReply : id == UI_SET_ALERT_ANSWER ? ds.callAlertAck :
			ds.lrrpReply;
	}
#endif
	case UI_SET_MSG_FORMAT:
#if CONFIG_DMR_SMS
		{
			dmrDataSettings_t ds;
			dmr_data_get_settings(&ds);
			return ds.smsFormat;
		}
#else
		return 2;
#endif
#if CONFIG_CRASH_REPORT
	case UI_SET_CRASH_MODE: {
		uint8_t mode = settings_crash_mode_of(SETTINGS_FIELD(accent));
		return mode <= UI_CRASH_SEND_AUTO ? mode : UI_CRASH_SEND_OFF;
	}
	case UI_SET_CRASH_CONTACT:
		return settings_crash_contact_of(SETTINGS_FIELD(theme), SETTINGS_FIELD(updown_home));
#endif
#if CONFIG_USB_NETWORK && CONFIG_HOSTLINK
	case UI_SET_USB_BOOT_NET:
		return SETTINGS_FIELD(usb_mode) == 1;
	case UI_SET_PC_ENDPOINT:
		return (SETTINGS_FIELD(net_flags) & SETTINGS_NET_PASSTHROUGH) != 0;
	case UI_SET_USB_FIXED_LINK:
		return (SETTINGS_FIELD(net_flags) & SETTINGS_NET_FIXED_LINK) != 0;
#endif
#if CONFIG_POSITION
	case UI_SET_POS_SOURCE:
	case UI_SET_POS_FIXED_LAT:
	case UI_SET_POS_FIXED_LON:
	case UI_SET_POS_DMR_CALLS: {
		position_cfg_t pc;
		position_get_cfg(&pc);
		return id == UI_SET_POS_SOURCE ? pc.pref : id == UI_SET_POS_FIXED_LAT ? pc.fix_lat :
		       id == UI_SET_POS_FIXED_LON ? pc.fix_lon : pc.dmr_gps;
	}
#endif
	default:
		return ui_svc_ram_get(id);
	}
}

int32_t ui_svc_setting_get(uint8_t id)
{
#if CONFIG_KEY_ASSIGN
	if (id >= UI_SET_KEY_FIRST && id <= UI_SET_KEY_LAST) {	// not under setting_get_flash's frame: key_apply is deep
		return key_get(id);
	}
#endif
	int32_t val = setting_get_radio(id);
	return val != INT32_MIN ? val : setting_get_flash(id);
}

bool ui_svc_setting_set(uint8_t id, int32_t value)
{
	settings_t *st;
#if CONFIG_KEY_ASSIGN
	if (id >= UI_SET_KEY_FIRST && id <= UI_SET_KEY_LAST) {
		return key_set(id, value);
	}
#endif
	switch (id) {
	case UI_SET_KEY_TONES:
	case UI_SET_TONE_VOLUME:
	case UI_SET_TOT_S:
	case UI_SET_BACKLIGHT_S:
	case UI_SET_BATTERY_SAVER:
	case UI_SET_REPLY_S:
	case UI_SET_TALK_PERMIT:
	case UI_SET_SCAN_TONE:
#if !CONFIG_SIG_TONES
	case UI_SET_SIG_DMR ... UI_SET_SIG_STEP:	// nothing to set in this build (the menu has no row)
#endif
#if CONFIG_SFR
	case UI_SET_SFR_RX:
#endif
#if CONFIG_SFR_CWID
	case UI_SET_SFR_CWID:
#endif
#if CONFIG_DUAL_WATCH
	case UI_SET_DUAL_DELAY_MS:
#endif
#if CONFIG_BCAST_FM
	case UI_SET_BCAST_STEP:
#endif
#if CONFIG_DMR_ROAMING
	case UI_SET_ROAM_SURVEY:
	case UI_SET_ROAM_BEACON_S:
	case UI_SET_ROAM_BEACON_LEN:
	case UI_SET_ROAM_PROBE:
#endif
		return true;	// ui_svc_apply hands it to the radio core, which saves it
	case UI_SET_LARGE_TEXT:
	case UI_SET_HINTS:
	case UI_SET_MESSAGE_TONE:
	case UI_SET_ALIAS_SHOW:
	case UI_SET_PTT_LOCKED:
	case UI_SET_HEAR_ALL_DEFAULT:
#if CONFIG_DUAL_WATCH
	case UI_SET_DUAL_TALK_BACK:
#endif
		st = settings_begin();
		st->ui_flags = (uint8_t)(value != 0 ? st->ui_flags | ui_flag(id) : st->ui_flags & ~ui_flag(id));
		settings_end(hal_millis());
		return true;
	case UI_SET_LABEL_FIRST:
		st = settings_begin();
		st->updown_home = (uint8_t)(value != 0 ? st->updown_home | SETTINGS_HOME_LABEL_FIRST :
					    st->updown_home & ~SETTINGS_HOME_LABEL_FIRST);
		settings_end(hal_millis());
		return true;
	case UI_SET_VIBRATE:
		st = settings_begin();
		st->ui_flags = (uint8_t)(value != 0 ? st->ui_flags & ~SETTINGS_UI_NO_VIBRATE :
					 st->ui_flags | SETTINGS_UI_NO_VIBRATE);
		settings_end(hal_millis());
		return true;
	case UI_SET_THEME:
	case UI_SET_ACCENT:
	case UI_SET_STRIP:
	case UI_SET_UPDOWN_HOME:
	case UI_SET_LOW_BATTERY:
	case UI_SET_AUTO_OFF_MIN:
		st = settings_begin();
		if (id == UI_SET_THEME) {
			st->theme = (uint8_t)((st->theme & ~SETTINGS_SMALL_MASK) | clamp16(value, 0, 3));
		} else if (id == UI_SET_ACCENT) {
			st->accent = (uint8_t)((st->accent & ~SETTINGS_ACCENT_COLOR) |
					       clamp16(value, 0, SETTINGS_ACCENT_COLOR));
		} else if (id == UI_SET_STRIP) {
			st->accent = (uint8_t)(value ? st->accent & ~SETTINGS_ACCENT_PLAIN : st->accent | SETTINGS_ACCENT_PLAIN);
		} else if (id == UI_SET_UPDOWN_HOME) {
			st->updown_home = (uint8_t)((st->updown_home & ~SETTINGS_SMALL_MASK) | clamp16(value, 0, 2));
		} else if (id == UI_SET_LOW_BATTERY) {
			st->low_batt_pct = (uint8_t)clamp16(value, 0, 50);
		} else {
			st->auto_off_min = clamp16(value, 0, 24u * 60u);
		}
		settings_end(hal_millis());
		return true;
#if CONFIG_DMR_PRIVACY
	case UI_SET_PRIVACY_TYPE:
		privacy_store((uint8_t)clamp16(value, 0, DMR_PRIVACY_NUM_TYPES - 1),
			      (uint32_t)ui_svc_setting_get(UI_SET_PRIVACY_KEY));
		return true;
	case UI_SET_PRIVACY_KEY:
		privacy_store((uint8_t)ui_svc_setting_get(UI_SET_PRIVACY_TYPE), (uint32_t)clamp16(value, 0, 0xFFFF));
		return true;
#endif
#if CONFIG_CRASH_REPORT
	case UI_SET_CRASH_MODE:
	case UI_SET_CRASH_CONTACT:
		st = settings_begin();
		if (id == UI_SET_CRASH_MODE)
			settings_set_crash_mode(st, (uint8_t)clamp16(value, UI_CRASH_SEND_OFF, UI_CRASH_SEND_AUTO));
		else
			settings_set_crash_contact(st, clamp16(value, 0, CODEPLUG_CONTACTS_MAX));
		settings_end(hal_millis());
		return true;
#endif
	case UI_SET_REF_TRIM:
		st = settings_begin();
		st->ref_trim_ppb = ref_trim_clamp(value);
		settings_end(hal_millis());
		return true;
	case UI_SET_MIC_GAIN:
	case UI_SET_AUTO_LOCK_S:
	case UI_SET_LONG_PRESS_MS:
	case UI_SET_REPEAT_MS:
	case UI_SET_GPS:
	case UI_SET_POS_FORMAT:
	case UI_SET_POS_SEND_S:
		st = settings_begin();
		if (id == UI_SET_MIC_GAIN) {
			st->mic_gain = (uint8_t)clamp16(value, 0, 10);
		} else if (id == UI_SET_AUTO_LOCK_S) {
			st->auto_lock_s = clamp16(value, 0, 3600);
		} else if (id == UI_SET_LONG_PRESS_MS) {
			st->long_press_ms = clamp16(value, 200, 3000);
		} else if (id == UI_SET_REPEAT_MS) {
			st->repeat_ms = clamp16(value, 40, 1000);
		} else if (id == UI_SET_GPS) {
			st->gps = value != 0;
		} else if (id == UI_SET_POS_FORMAT) {
			st->pos_format = (uint8_t)clamp16(value, 0, 2);
		} else {
			st->pos_send_s = clamp16(value, 0, 0xFFFF);
		}
		settings_end(hal_millis());
		return true;
	case UI_SET_LANGUAGE:
		if (value < 0 || value > 0xFFFF)
			return false;
		st = settings_begin();
		st->lang[0] = (char)(value >> 8);
		st->lang[1] = (char)value;
		settings_end(hal_millis());
		return true;
	case UI_SET_KNOB_WHEN_LOCKED:
		st = settings_begin();
		st->sound_flags = (uint8_t)(value == 0 ? st->sound_flags | SETTINGS_KNOB_LOCK :
							st->sound_flags & ~SETTINGS_KNOB_LOCK);
		settings_end(hal_millis());
		return true;
	case UI_SET_VOLUME:
		st = settings_begin();
		st->volume = radio_volume_stored((uint8_t)clamp16(value, 0, RADIO_VOLUME_MAX));
		settings_end(hal_millis());
		radio_req_volume((uint8_t)clamp16(value, 0, RADIO_VOLUME_MAX));	// in force now, not at the next reload
		return true;
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
#if CONFIG_RX_EXT
	case UI_SET_RX_EXT:
		st = settings_begin();
		st->cp_flags = (uint8_t)(value != 0 ? (st->cp_flags | SETTINGS_CP_RX_EXT) : (st->cp_flags & ~SETTINGS_CP_RX_EXT));
		settings_end(hal_millis());
		board_rx_ext_set(value != 0);
		radio_req_reload();	// a VFO past the table goes back in band when this goes off
		return true;
#endif
	case UI_SET_HOTSPOT_AUTO:
		st = settings_begin();
		st->hotspot_allowed = value != 0;
		settings_end(hal_millis());
#if CONFIG_HOSTLINK
		hostlink_set_hotspot_allowed(value != 0);
#endif
		return true;
	case UI_SET_TFTP_WRITE:
		st = settings_begin();
		st->net_flags = (uint8_t)(value != 0 ? (st->net_flags & ~SETTINGS_NET_TFTP_NO_WRITE) :
					  (st->net_flags | SETTINGS_NET_TFTP_NO_WRITE));
		settings_end(hal_millis());
#if CONFIG_TFTP
		tftp_codeplug_allow_write(value != 0);
#endif
		return true;
#if CONFIG_ZONE_SHARE
	case UI_SET_ZONE_OFFERS:
		if (value != 0 && value != 1)
			return false;
		st = settings_begin();
		st->zs_offers = value ? 1u : SETTINGS_ZS_OFF;
		settings_end(hal_millis());
		zone_share_set_offers(value ? ZS_OFFERS_ASK : ZS_OFFERS_OFF);
		return true;
#endif
#if CONFIG_DMR
	case UI_SET_DMR_ID:
		return dmr_id_post((uint32_t)value);
#endif
	case UI_SET_NOISE_REDUCTION:	// ui_svc_apply hands them to the vocoder
	case UI_SET_MIC_AGC: {
		// The stored bit is set for "on" with AGC, for "off" with noise reduction (0 = the defaults)
		uint8_t bit = id == UI_SET_MIC_AGC ? SETTINGS_AUDIO_AGC : SETTINGS_AUDIO_NO_NOISE_REDUCTION;
		bool set = (value != 0) == (id == UI_SET_MIC_AGC);
		st = settings_begin();
		st->accent = (uint8_t)(set ? (st->accent | bit) : (st->accent & ~bit));
		settings_end(hal_millis());
		return true;
	}
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
	case UI_SET_ARS_SERVER:
	case UI_SET_IP_GATEWAY:
		return data_id_post(data_row_key(id), (uint32_t)value);
#endif
#if CONFIG_SIG_TONES
	case UI_SET_SIG_DMR ... UI_SET_SIG_STEP:
		return sig_post(id, value);
#endif
	case UI_SET_CALL_END_TONE:
	case UI_SET_TOT_WARN_TONE:
	case UI_SET_BATTERY_TONE:
	case UI_SET_TX_DENY_TONE:
		return tone_post(tone_row_key(id), value != 0);
	case UI_SET_EMERGENCY_TONE: {
		dmr_emergency_settings_t es;
		dmr_emergency_get_settings(&es);
		es.rx_tone = value != 0;
		dmr_emergency_set_settings(&es);	// the radio core saves them when they change (em_flags)
		return CONFIG_DMR_EMERGENCY;
	}
	case UI_SET_MSG_FORMAT:
#if CONFIG_DMR_SMS
		if ((value != DMR_SMS_FORMAT_DEFAULT) && (value != DMR_SMS_FORMAT_ETSI_IP) && (value != DMR_SMS_FORMAT_ETSI) &&
		    !(CONFIG_DMR_TMS && (value == DMR_SMS_FORMAT_TMS)))
			return false;
		{
			dmrDataSettings_t ds;
			dmr_data_get_settings(&ds);
			ds.smsFormat = (uint8_t)value;
			dmr_data_set_settings(&ds);
		}
		return true;
#else
		return false;
#endif
#if CONFIG_USB_NETWORK && CONFIG_HOSTLINK
	case UI_SET_USB_BOOT_NET:
	case UI_SET_PC_ENDPOINT:
	case UI_SET_USB_FIXED_LINK:
		st = settings_begin();
		if (id == UI_SET_USB_BOOT_NET)
			st->usb_mode = value != 0;
		else if (CONFIG_DMR_CONTROL_STATION) {
			uint8_t bit = id == UI_SET_PC_ENDPOINT ? SETTINGS_NET_PASSTHROUGH : SETTINGS_NET_FIXED_LINK;
			st->net_flags = (uint8_t)(value ? (st->net_flags | bit) : (st->net_flags & ~bit));
		}
		settings_end(hal_millis());
		hostlink_settings_changed();
		return true;
#endif
	case UI_SET_VFO_STEP:
		return true;	// ui_svc_apply hands it to the radio core, which saves it
#if CONFIG_DUAL_WATCH
	case UI_SET_DUAL_B:
		st = settings_begin();
		st->dual_b = (uint16_t)clamp16(value & UI_DUAL_CODE, 0, UI_DUAL_B_VFO2);
		st->dual_state = (uint8_t)(((value & UI_DUAL_ON) ? SETTINGS_DUAL_ON : 0) |
					   ((value & UI_DUAL_B_SEL) ? SETTINGS_DUAL_B_SEL : 0));
		settings_end(hal_millis());
		return true;
#endif
#if CONFIG_POSITION
	case UI_SET_POS_SOURCE:
	case UI_SET_POS_FIXED_LAT:
	case UI_SET_POS_FIXED_LON:
	case UI_SET_POS_DMR_CALLS: {
		position_cfg_t pc;
		position_get_cfg(&pc);
		if (id == UI_SET_POS_SOURCE) {
			if (value < 0 || value >= POSITION_PREF_COUNT)
				return false;
			pc.pref = (uint8_t)value;
		} else if (id == UI_SET_POS_FIXED_LAT) {
			if (value < -900000000 || value > 900000000)
				return false;
			pc.fix_lat = value;
		} else if (id == UI_SET_POS_FIXED_LON) {
			if (value < -1800000000 || value > 1800000000)
				return false;
			pc.fix_lon = value;
		} else {
			pc.dmr_gps = value != 0;
		}
		position_set_cfg(&pc, hal_millis());	// in use at once, and saved
		return true;
	}
#endif
	default:
		return ui_svc_ram_set(id, value);
	}
}

// --- Codeplug writes, on the housekeeping task (storage_job_post) ---

// why: a key (TRN). The UI shows it in the language in use (tr), so this task needs no language
static void save_failed(tr_id_t why)
{
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.kind = UI_EV_SAVE_FAILED;
	ev.why = why;
	ui_notify(&ev);
}

// What the job writes: a copy of the radio's view taken when the UI asked (the job runs up to 100 ms later), and the
// talk target for a permanent one. Only the UI task fills it, and only while no job is pending
static struct {
	uint16_t channel;
	uint8_t temp;
	uint8_t power;
	uint8_t squelch;
	uint8_t held;		// a write held for the foreign codeplug question: cp_jobs[held - 1], 0 none (in padding)
	union {
		channel_t now;		// the channel as the radio has it now (temporary changes included)
#if CONFIG_CONTACT_EDIT
		contact_t contact;	// the contact editor's, at index channel (one job at a time: RAM is short)
#endif
		char name[CODEPLUG_NAME_LEN + 1];	// a name field's new text, UI_NAME_* in channel
#if CONFIG_DMR
		contact_t target;	// save_target_job's search: the stored contact found
#endif
	};
	uint32_t id;		// the talk target's, or the radio's own DMR ID
	bool private_call;
#if CONFIG_P25 || CONFIG_NXDN
	bool air_set;		// write_channel_job: air is the channel's block
	ui_air_conv_t air;
#endif
} job;

// Another firmware's codeplug (codeplug_foreign, docs/spec/codeplug-native.md 2): the radio erases nothing of it on
// its own. The first codeplug write is held (job.held) and the UI asks (UI_EV_CODEPLUG_FOREIGN,
// ui_svc_codeplug_answer). While one is held no other write starts (job_busy), so the job struct keeps what the held
// write needs. An index into this table, not a pointer: no RAM of its own (some radios have none to spare)
static void save_channel_job(void *ctx);
#if CONFIG_DMR_AES && CONFIG_CHANNEL_EDIT
void ui_svc_next_aes_key(channel_t *c)
{
	// Step through system/PI key ID in numeric order. Read only the public record headers.
	uint32_t current = c->kind == CODEPLUG_KIND_DMR_AES ? (uint32_t)c->system * 256 + c->aes_ref : 0;
	uint32_t best = 0x10000u;
	codeplug_kv_cursor_t cursor;
	uint8_t hdr[CODEPLUG_TRAFFIC_HDR];
	uint16_t record;
	int len;
	codeplug_kv_begin(&cursor, CODEPLUG_KEY_TRAFFIC, CODEPLUG_KEY_TRAFFIC + 1023);
	while ((len = codeplug_kv_next(&cursor, &record, hdr, sizeof hdr)) >= 0) {
		if (len != CODEPLUG_TRAFFIC_HDR + DMR_AES_KEY_BYTES || hdr[0] == CODEPLUG_SYSTEM_NONE ||
		    hdr[1] != DMR_AES_ALG_AES256 || hdr[3] != 0)
			continue;
		uint32_t rank = (uint32_t)hdr[0] * 256 + hdr[2] + 1;
		if (rank > current && rank < best) {
			codeplug_system_t sys;
			uint8_t block[CODEPLUG_SYS_HDR];
			if (codeplug_system(hdr[0], &sys, block, 0) >= 0 && sys.type == CODEPLUG_SYSTYPE_DMR_AES)
				best = rank;
		}
	}
	c->kind = best == 0x10000u ? CODEPLUG_KIND_NONE : CODEPLUG_KIND_DMR_AES;
	c->system = best == 0x10000u ? CODEPLUG_SYSTEM_NONE : (uint8_t)((best - 1) >> 8);
	c->aes_ref = best == 0x10000u ? 0 : (uint16_t)(1u + ((best - 1) & 255));
}
#endif

static void write_channel_job(void *ctx);
static void name_job(void *ctx);
#if CONFIG_DMR
static void dmr_id_job(void *ctx);
static void save_target_job(void *ctx);
#endif
#if CONFIG_CONTACT_EDIT
static void write_contact_job(void *ctx);
#endif
#if CONFIG_ZONE_EDIT
static void zone_edit_job(void *ctx);
#endif
#if CONFIG_KEY_ASSIGN
static void key_write_job(void *ctx);
#endif
static void (*const cp_jobs[])(void *ctx) = {
	save_channel_job, write_channel_job, name_job,
#if CONFIG_DMR
	dmr_id_job, save_target_job,
#endif
#if CONFIG_CONTACT_EDIT
	write_contact_job,
#endif
#if CONFIG_ZONE_EDIT
	zone_edit_job,
#endif
#if CONFIG_KEY_ASSIGN
	key_write_job,
#endif
};

static bool job_busy(void)
{
	return storage_job_busy() || job.held != 0;
}

// A codeplug write as a storage job, or held for the question
static bool cp_post(void (*fn)(void *ctx))
{
	if (job.held != 0) {
		return false;
	}
	if (!codeplug_foreign()) {
		return storage_job_post(fn, NULL);
	}
	const uint8_t n = (uint8_t)(sizeof(cp_jobs) / sizeof(cp_jobs[0]));
	uint8_t i = 0;
	while (i < n && cp_jobs[i] != fn) {
		i++;
	}
	if (i == n) {
		return false;
	}
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.kind = UI_EV_CODEPLUG_FOREIGN;
	job.held = (uint8_t)(i + 1u);
	if (!ui_notify(&ev)) {
		job.held = 0;
		return false;
	}
	return true;
}

#if CONFIG_ZONE_EDIT
static void zone_drop(void);
#endif
#if CONFIG_KEY_ASSIGN
static void key_reload(void);
#endif

// What a held write changed on the UI task alone goes back to the codeplug's: the side keys, the zone edits queued
static void cp_drop(void)
{
	job.held = 0;
#if CONFIG_KEY_ASSIGN
	key_reload();
#endif
#if CONFIG_ZONE_EDIT
	zone_drop();
#endif
}

static void cp_answer_job(void *ctx)
{
	void (*fn)(void *ctx) = cp_jobs[job.held - 1u];
	if ((uintptr_t)ctx == UI_CP_USE) {
		codeplug_accept(true);		// app.c stores it
		codeplug_reload();		// the old codeplug's records shown; the write made on the built-in set dropped
		cp_drop();
	} else if (codeplug_start_new()) {
		job.held = 0;
		fn(NULL);
	} else {
		cp_drop();
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
	}
}

bool ui_svc_codeplug_answer(uint8_t answer)
{
	if (job.held == 0 || storage_job_busy()) {
		return false;
	}
	if (answer != UI_CP_NEW && answer != UI_CP_USE) {
		cp_drop();
		return true;
	}
	return storage_job_post(cp_answer_job, (void *)(uintptr_t)answer);	// the answer in ctx: no RAM of its own
}

#if CONFIG_DMR
// The radio's own DMR ID, written into the codeplug (Settings > My radio): the radio core takes it when
// codeplug_serial() changes, and a CPS read shows it
static void dmr_id_job(void *ctx)
{
	(void)ctx;
	if (!codeplug_set_dmr_id(job.id))
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
}

static bool dmr_id_post(uint32_t id)
{
	if (id == 0 || id > CODEPLUG_DMR_ID_MAX || job_busy())
		return false;
	job.id = id;
	return cp_post(dmr_id_job);
}
#endif

#if CONFIG_DMR_DATA_CORE
// The ARS server or the IP gateway (Settings > Connections > Data services), set as every keyed setting is
// (docs/spec/codeplug-settings.md 5): the key is written when the settings area can be, so a CPS read shows it, and
// the data service takes the setting, which the radio core saves as the radio's own. Not a codeplug write that asks
// about another firmware's codeplug: there the area can't be written and holds no key, so the setting is only the
// radio's own. Refused only when the codeplug has the key and it couldn't be updated (a CPS session holds the
// flash), since the key would put the old value back. The key in job.channel, the ID in job.id
static void data_id_job(void *ctx)
{
	(void)ctx;
	const codeplug_key_row_t *row = codeplug_keys_find(radio_keys, RADIO_KEYS_N, job.channel);
	dmrDataSettings_t was, set;
	dmr_data_get_settings(&set);
	was = set;
	codeplug_keys_set_field(row, &set, job.id);
	if (!codeplug_keys_store(radio_keys, RADIO_KEYS_N, &was, &set))
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
	else
		dmr_data_set_settings(&set);
}

static bool data_id_post(uint16_t key, uint32_t id)
{
	const codeplug_key_row_t *row = codeplug_keys_find(radio_keys, RADIO_KEYS_N, key);
	if (row == NULL || !codeplug_keys_valid(row, id) || job_busy())
		return false;
	job.channel = key;
	job.id = id;
	return storage_job_post(data_id_job, NULL);
}
#endif

// A Settings > Sound tone on or off, set as every keyed setting is (docs/spec/codeplug-settings.md 5.2, data_id_job):
// the key is written when the settings area can be, and the radio core takes the tone into its settings, which it
// saves as the radio's own. Refused only when the codeplug has the key and it couldn't be updated. The key in
// job.channel, on or off in job.temp
static void tone_job(void *ctx)
{
	(void)ctx;
	radio_settings_t rs;
	radio_get_settings(&rs);
	radio_tones_t was, now;
	radio_tones_unpack(rs.tones_off, &was);
	now = was;
	codeplug_keys_set_field(codeplug_keys_find(radio_tone_rows, RADIO_TONE_ROWS_N, job.channel), &now, job.temp);
	if (!codeplug_keys_store(radio_tone_rows, RADIO_TONE_ROWS_N, &was, &now)) {
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
		return;
	}
	// Read again after the write (a flash write can take a while): a setting the UI or the debug link changed
	// meanwhile stays, only this tone's bit changes
	uint8_t bit = (uint8_t)(1u << radio_tone_row(job.channel));
	radio_get_settings(&rs);
	rs.tones_off = (uint8_t)(job.temp != 0 ? rs.tones_off & ~bit : rs.tones_off | bit);
	radio_set_settings(&rs);
}

static bool tone_post(uint16_t key, bool on)
{
	if (codeplug_keys_find(radio_tone_rows, RADIO_TONE_ROWS_N, key) == NULL || job_busy())
		return false;
	job.channel = key;
	job.temp = on ? 1u : 0u;
	return storage_job_post(tone_job, NULL);
}

#if CONFIG_SIG_TONES
// A signaling tone setting (Settings > Signaling tones), set as every keyed setting is (docs/spec/codeplug-settings.md
// 5.2): the event's record is written when the settings area can be, and the radio core takes the change into its
// settings, which it saves as the radio's own. Refused only when the codeplug has the key and it couldn't be updated.
// The setting's id in job.channel, the value in job.id

// The event a setting belongs to, or SIG_EV_COUNT
static sig_event_t sig_event_of(uint8_t id, int32_t value)
{
	unsigned ev = SIG_EV_COUNT;
	if (id >= UI_SET_SIG_DMR && id <= UI_SET_SIG_DMR_LAST)
		ev = id - UI_SET_SIG_DMR;
	else if (id >= UI_SET_SIG_FM && id <= UI_SET_SIG_FM_LAST)
		ev = id - UI_SET_SIG_FM;
	else if (id >= UI_SET_SIG_HZ && id <= UI_SET_SIG_HZ_LAST)
		ev = (id - UI_SET_SIG_HZ) / SIG_STEPS;
	else if (id >= UI_SET_SIG_MS && id <= UI_SET_SIG_MS_LAST)
		ev = (id - UI_SET_SIG_MS) / SIG_STEPS;
	else if (id == UI_SET_SIG_STEP && ((uint32_t)value >> 19 & 15u) < SIG_STEPS)
		ev = (uint32_t)value >> 23 & 3u;
	return ev < SIG_EV_COUNT ? (sig_event_t)ev : SIG_EV_COUNT;
}

// The setting's change in its event's record
static void sig_change(sig_rec_t *r, uint8_t id, int32_t value)
{
	uint32_t x = (uint32_t)value;
	if (id >= UI_SET_SIG_DMR && id <= UI_SET_SIG_DMR_LAST) {
		r->dmr = (uint8_t)(x > 255u ? 255u : x);
	} else if (id >= UI_SET_SIG_FM && id <= UI_SET_SIG_FM_LAST) {
		r->fm = (uint8_t)(x > 255u ? 255u : x);
	} else if (id >= UI_SET_SIG_HZ && id <= UI_SET_SIG_HZ_LAST) {
		r->hz[(id - UI_SET_SIG_HZ) % SIG_STEPS] = (uint16_t)(x > 0xFFFFu ? 0xFFFFu : x);
	} else if (id >= UI_SET_SIG_MS && id <= UI_SET_SIG_MS_LAST) {
		r->ms10[(id - UI_SET_SIG_MS) % SIG_STEPS] = (uint8_t)(x / SIG_MS_UNIT > 255u ? 255u : x / SIG_MS_UNIT);
	} else if (id == UI_SET_SIG_STEP) {
		unsigned k = x >> 19 & 15u;
		r->hz[k] = (uint16_t)(x & 0xFFFu);
		r->ms10[k] = (uint8_t)(x >> 12 & 0x7Fu);
	}
}

static void sig_job(void *ctx)
{
	(void)ctx;
	radio_settings_t rs;
	sig_event_t ev = sig_event_of((uint8_t)job.channel, (int32_t)job.id);
	if (ev == SIG_EV_COUNT)
		return;
	radio_get_settings(&rs);
	sig_rec_t was, now;
	uint8_t a[SIG_REC_LEN], b[SIG_REC_LEN];
	radio_sig_get(&rs, ev, &was);
	now = was;
	sig_change(&now, (uint8_t)job.channel, (int32_t)job.id);
	sig_rec_encode(&was, a);
	sig_rec_encode(&now, b);
	if (!codeplug_keys_write_record((uint16_t)(CODEPLUG_KEY_SIG_BEGIN + ev), a, b, SIG_REC_LEN)) {
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
		return;
	}
	// Read again after the write (a flash write can take a while): only this event's record changes
	radio_get_settings(&rs);
	radio_sig_put(&rs, ev, &now);
	radio_set_settings(&rs);
}

static bool sig_post(uint8_t id, int32_t value)
{
	radio_settings_t rs;
	sig_rec_t r;
	uint8_t b[SIG_REC_LEN];
	sig_event_t ev = sig_event_of(id, value);
	if (ev == SIG_EV_COUNT || job_busy())
		return false;
	radio_get_settings(&rs);
	radio_sig_get(&rs, ev, &r);
	sig_change(&r, id, value);
	sig_rec_encode(&r, b);
	if (!sig_rec_decode(b, SIG_REC_LEN, ev, &r))
		return false;	// a value the setting can't take
	job.channel = id;
	job.id = (uint32_t)value;
	return storage_job_post(sig_job, NULL);
}
#endif

static channel_t job_ch;	// the job's working copy of the stored channel

#if CONFIG_P25
// A plain channel in P25 (the VFO, a mode changed for now) made a stored P25 channel from the radio's P25 settings
// (p25-codeplug.md 2.3): its NACs, the squelch, the talk target's stored contact (none: talk group 1), talkaround
// on or automatic; the NACs and squelch from conv instead when given (the channel editor's). Uses job_ch
static bool p25_save_now(uint16_t number, const channel_t *ch, const p25_conv_t *given)
{
	radio_p25_settings_t s;
	radio_p25_settings(&s);
	p25_conv_t conv = { .rx_nac = s.rx_nac, .tx_nac = s.tx_nac,
			    .squelch = s.rx_tgid != 0 ? P25_SQUELCH_SELECTIVE : P25_SQUELCH_NORMAL };
	if (given != NULL)
		conv = *given;
	if (ch != &job_ch)
		job_ch = *ch;
#if CONFIG_DMR
	job_ch.contact = codeplug_contact_find_index(s.tx_dst, !s.unit_call, NULL);
#else
	job_ch.contact = 0;
#endif
	job_ch.dmo = s.talkaround == RADIO_P25_TA_ON;
	return p25_conv_save(number, &job_ch, &conv);
}
#endif

#if CONFIG_NXDN
// A plain channel in NXDN (the VFO, a mode changed for now) made a stored NXDN channel from the radio's NXDN settings
// (nxdn-codeplug.md 2.2): the rate, the RANs, the squelch, the talk target's stored contact (none: group 1); the
// block from conv instead when given (the channel editor's). Uses job_ch
static bool nxdn_save_now(uint16_t number, const channel_t *ch, const nxdn_conv_t *given)
{
	radio_nxdn_settings_t s;
	radio_nxdn_settings(&s);
	nxdn_conv_t conv = { .rate9600 = s.rate9600, .rx_ran = s.rx_ran, .tx_ran = s.tx_ran,
			     .squelch = s.rx_gid != 0 ? NXDN_SQUELCH_SELECTIVE : NXDN_SQUELCH_RAN };
	if (given != NULL)
		conv = *given;
	if (ch != &job_ch)
		job_ch = *ch;
	job_ch.contact = codeplug_contact_find_index(s.tx_dst, !s.unit_call, NULL);	// CONFIG_NXDN needs CONFIG_DMR
	return nxdn_conv_save(number, &job_ch, &conv);
}
#endif

// A plain channel in an air mode written as a stored channel of its kind (the VFO, a mode changed for now); false in
// *ok's place when it isn't one: the caller writes it as a plain channel
static bool air_save_now(uint16_t number, const channel_t *ch, bool *ok)
{
	if (ch->kind != CODEPLUG_KIND_NONE)
		return false;
#if CONFIG_P25
	if (ch->mode == RADIO_MODE_P25) {
		*ok = p25_save_now(number, ch, NULL);
		return true;
	}
#endif
#if CONFIG_NXDN
	if (ch->mode == RADIO_MODE_NXDN) {
		*ok = nxdn_save_now(number, ch, NULL);
		return true;
	}
#endif
	(void)number;
	(void)ok;
	return false;
}

// Power, bandwidth, tones, mode, color code and timeslot go into the channel; the squelch is radio-wide (the default
// squelch preference). The radio core re-reads the channel (codeplug_serial), and the clear makes the remaining
// flags follow the new values
static void save_channel_job(void *ctx)
{
	(void)ctx;
	if (!codeplug_channel(job.channel, &job_ch)) {
		save_failed(TRN(STORE_ERR_CHANNEL_READ, "Couldn't read the channel: not saved"));
		return;
	}
	if (job.temp & RADIO_TEMP_POWER)
		job_ch.power = job.power;
	if (job.temp & RADIO_TEMP_BW)
		job_ch.bw = job.now.bw;
	if (job.temp & RADIO_TEMP_TX_TONE)
		job_ch.tx_tone = job.now.tx_tone;
	if (job.temp & RADIO_TEMP_RX_TONE)
		job_ch.rx_tone = job.now.rx_tone;
	if (job.temp & RADIO_TEMP_MODE)
		job_ch.mode = job.now.mode;
	if (job.temp & RADIO_TEMP_CC)
		job_ch.color_code = job.now.color_code;
	if (job.temp & RADIO_TEMP_TS)
		job_ch.timeslot = job.now.timeslot;
	bool ok;
	if (!air_save_now(job.channel, &job_ch, &ok))
		ok = codeplug_set_channel(job.channel, &job_ch);
	if (!ok) {
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
		return;
	}
	if (job.temp & RADIO_TEMP_SQUELCH) {
		settings_t *st = settings_begin();
		st->squelch = job.squelch;
		settings_end(hal_millis());
	}
	radio_req_clear_temporary();
}

// Whether the radio is on a stored channel (its view in ui_svc_view)
static bool take_channel(void)
{
	if (job_busy()) {
		return false;	// the last write is still going on
	}
	radio_get_view(&ui_svc_view);
	if (ui_svc_view.vfo || ui_svc_view.channel_number == 0 || ui_svc_view.hotspot) {
		return false;
	}
	job.channel = ui_svc_view.channel_number;
	job.temp = ui_svc_view.temp;
	job.power = ui_svc_view.power;
	job.squelch = ui_svc_view.squelch;
	job.now = ui_svc_view.ch;
	return true;
}

bool ui_svc_save_channel(void)
{
	return take_channel() && cp_post(save_channel_job);
}

#if CONFIG_P25 || CONFIG_NXDN
// The channel editor's P25 or NXDN channel with its block: a plain one in that mode becomes a stored one as in
// air_save_now; one of the kind is written with the block over its tag's, the rest of the tag kept. A copy (from: the
// source, another number) takes the source's tag first, so it keeps the same; where that fails its tag is new. False
// in *ok's place when ch is neither: the caller writes it as usual
static bool air_conv_save(uint16_t number, uint16_t from, const channel_t *ch, const ui_air_conv_t *conv, bool *ok)
{
	bool plain = ch->kind == CODEPLUG_KIND_NONE;
	bool p25 = false, nxdn = false;
#if CONFIG_P25
	p25 = plain ? ch->mode == RADIO_MODE_P25 : ch->kind == CODEPLUG_KIND_P25;
#endif
#if CONFIG_NXDN
	nxdn = plain ? ch->mode == RADIO_MODE_NXDN : ch->kind == CODEPLUG_KIND_NXDN;
#endif
	if (!p25 && !nxdn)
		return false;
	if (!plain && from != 0 && from != number && codeplug_set_channel(number, ch))
		(void)codeplug_tag_copy(from, number, ch);
#if CONFIG_P25
	if (p25)
		*ok = plain ? p25_save_now(number, ch, &conv->p25) : p25_conv_save(number, ch, &conv->p25);
#endif
#if CONFIG_NXDN
	if (nxdn)
		*ok = plain ? nxdn_save_now(number, ch, &conv->nxdn) : nxdn_conv_save(number, ch, &conv->nxdn);
#endif
	return true;
}
#endif

// The channel editor's whole channel (ui.md 2.1). Writing the channel in use drops its temporary changes: the radio
// core re-reads it (codeplug_serial), and the edited values are what the user wants now. Whether it is the channel in
// use is decided here on the UI task: the job runs on the housekeeping task, which must not touch ui_svc_view. A copy
// of a channel of a kind takes its source's tag (job.id: the source); a plain channel in P25 or NXDN becomes a stored
// one of that kind
static void write_channel_job(void *ctx)
{
	(void)ctx;
	bool ok = false;
	uint16_t from = (uint16_t)job.id;
	bool done = false;
#if CONFIG_P25 || CONFIG_NXDN
	done = job.air_set && air_conv_save(job.channel, from, &job.now, &job.air, &ok);
#endif
	if (!done && !air_save_now(job.channel, &job.now, &ok))
		ok = codeplug_set_channel(job.channel, &job.now) &&
		     (job.now.kind == CODEPLUG_KIND_NONE ||
#if CONFIG_DMR_AES
		      (job.now.kind == CODEPLUG_KIND_DMR_AES && job.now.aes_ref) ||
#endif
		      from == 0 || from == job.channel ||
		      codeplug_tag_copy(from, job.channel, &job.now));
	if (!ok) {
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
		return;
	}
	if (job.temp != 0) {
		radio_req_clear_temporary();
	}
}

// conv: the block of a P25 or NXDN channel (the channel editor's), NULL: none given
static bool write_channel_post(uint16_t number, const channel_t *ch, uint16_t from, const void *conv)
{
	if (number == 0 || number > CODEPLUG_CHANNELS_MAX || job_busy()) {
		return false;
	}
	radio_get_view(&ui_svc_view);
	job.channel = number;
	job.temp = !ui_svc_view.vfo && ui_svc_view.channel_number == number && ui_svc_view.temporary;	// here: drop the temporary changes after it
	job.now = *ch;
	job.id = from;
#if CONFIG_P25 || CONFIG_NXDN
	job.air_set = conv != NULL;	// before the post: the job may run at once on its own task
	if (conv != NULL)
		job.air = *(const ui_air_conv_t *)conv;
#else
	(void)conv;
#endif
	return cp_post(write_channel_job);
}

bool ui_svc_write_channel(uint16_t number, const channel_t *ch, uint16_t from)
{
	return write_channel_post(number, ch, from, NULL);
}

#if CONFIG_P25 || CONFIG_NXDN
bool ui_svc_write_channel_air(uint16_t number, const channel_t *ch, uint16_t from, const ui_air_conv_t *conv)
{
	return write_channel_post(number, ch, from, conv);
}
#endif

#if CONFIG_CONTACT_EDIT
// The contact editor's contact (ui.md 2.4). The radio core's name cache follows codeplug_serial()

static void write_contact_job(void *ctx)
{
	(void)ctx;
	if (!codeplug_set_contact(job.channel, &job.contact)) {
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
	}
}

bool ui_svc_write_contact(uint16_t index, const contact_t *c)
{
	if (index == 0 || index > CODEPLUG_CONTACTS_MAX || job_busy()) {
		return false;
	}
	job.channel = index;
	job.contact = *c;
	return cp_post(write_contact_job);
}
#endif

#if CONFIG_ZONE_EDIT
// The zone editor's changes (ui.md 2.2), queued: Move up pressed again while the last move is still being written
// waits its turn instead of being refused. The UI task adds at zq_head, the job takes from zq_tail. A new zone ahead
// of others or a deleted zone moves zone indexes: the radio core and dual watch follow them through zone_edit's log
// when they see the codeplug change, so the job only writes
#define ZONE_QUEUE	4u
static zone_op_t zq[ZONE_QUEUE];
static volatile uint8_t zq_head, zq_tail;

static tr_id_t zone_edit_text(zone_edit_result_t r)
{
	switch (r) {
	case ZONE_EDIT_FULL: return TRN(STORE_ERR_ZONE_FULL, "Zone full: not saved");
	case ZONE_EDIT_ZONES_FULL: return TRN(ZONE_ERR_NO_ROOM, "No room for another zone");
	case ZONE_EDIT_IN_ZONE: return TRN(STORE_ERR_ALREADY_IN_ZONE, "Already in this zone");
	case ZONE_EDIT_NO_FREE: return TRN(TOAST_NO_FREE_CHANNEL, "No free channel");
	case ZONE_EDIT_NO_NAME: return TRN(TOAST_NAME_FIRST, "Give it a name first");
	case ZONE_EDIT_EDGE: return TRN(STORE_ERR_ALREADY_AT_END, "Already at the end");
	case ZONE_EDIT_NO_ZONE: return TRN(STORE_ERR_ZONE_GONE, "That zone is gone");
	case ZONE_EDIT_NO_CHANNEL: return TRN(STORE_ERR_CHANNEL_GONE, "That channel is gone");
	default: return TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved");
	}
}

// A refusal drops the changes queued after it: they were made on the zone as the UI saw it with this one done
static void zone_edit_job(void *ctx)
{
	(void)ctx;
	while (zq_tail != zq_head) {
		zone_edit_result_t r = zone_edit_apply(&zq[zq_tail % ZONE_QUEUE], NULL);
		if (r != ZONE_EDIT_OK) {
			save_failed(zone_edit_text(r));
			zq_tail = zq_head;
			break;
		}
		zq_tail = (uint8_t)(zq_tail + 1u);
	}
}

// The job, unless one (this or another write) is running: then ui_svc_zone_poll posts it later
static void zone_kick(void)
{
	if (zq_tail != zq_head && !job_busy()) {
		cp_post(zone_edit_job);
	}
}

// The edits queued, dropped with the held write (cp_drop)
static void zone_drop(void)
{
	zq_tail = zq_head;
}

bool ui_svc_zone_edit(const zone_op_t *op)
{
	if ((uint8_t)(zq_head - zq_tail) >= ZONE_QUEUE) {
		return false;
	}
	zq[zq_head % ZONE_QUEUE] = *op;
	zq_head = (uint8_t)(zq_head + 1u);
	zone_kick();
	return true;
}

void ui_svc_zone_poll(void)
{
	zone_kick();
}

bool ui_svc_zone_pending(void)
{
	return zq_tail != zq_head;
}
#endif

#if CONFIG_DMR
// The first stored contact with this ID and kind becomes the channel's talk target. Searching the contacts reads up
// to 1024 records, so it is part of the job too. Until it is done the target is a temporary one (ui_svc_talk_to)
static void save_target_job(void *ctx)
{
	(void)ctx;
	if (!codeplug_channel(job.channel, &job_ch)) {
		save_failed(TRN(STORE_ERR_TARGET_CHANNEL_READ, "Couldn't read the channel: for now only"));
		return;
	}
	// A contact record that couldn't be read counts as not stored: nothing is written then
	uint16_t found = codeplug_contact_find_index(job.id, !job.private_call, &job.target);
	if (found == 0) {
		save_failed(TRN(STORE_ERR_TARGET_NOT_STORED, "Not a stored contact: for now only"));
		return;
	}
	job_ch.contact = found;
	if (!codeplug_set_channel(job.channel, &job_ch)) {
		save_failed(TRN(STORE_ERR_TARGET_NOT_WRITTEN, "Codeplug not written: for now only"));
		return;
	}
	radio_req_dmr_target_clear();	// the channel's own target is this one now
}

bool ui_svc_save_talk_target(uint32_t id, bool private_call)
{
	if (id == 0)
		return false;
	radio_get_view(&ui_svc_view);
	if (ui_svc_view.vfo) {
		if (!CONFIG_VFO_DMR || ui_svc_view.hotspot)
			return false;
		radio_req_dmr_target(id, private_call);	// the VFO's own target: kept with its settings, nothing to write
		return true;
	}
	if (!take_channel())
		return false;
	job.id = id;
	job.private_call = private_call;
	if (!cp_post(save_target_job))
		return false;
	radio_req_dmr_target(id, private_call);	// in effect at once; the job makes it the channel's
	return true;
}
#endif

// Factory reset: the settings back to the defaults (saved at once), the native area emptied when it holds our data
// or nothing (storage_native_erase leaves another firmware's data alone), then a restart so every task starts from
// the defaults. Never the open-firmware codeplug, the calibration, or the settings ring and journal as raw flash (the
// defaults go in as a new record, through the foreign region guard)
static void factory_reset_job(void *ctx)
{
	(void)ctx;
	settings_t *st = settings_begin();
	settings_defaults(st);	// cp_flags too: "Use anyway" doesn't outlive a reset
	board_rx_ext_set(false);
	settings_end(hal_millis());
	codeplug_accept(false);
	bool saved = settings_flush();
	storage_native_t n = storage_native_erase();
	if (!saved && storage_ready()) {
		save_failed(TRN(STORE_ERR_RESET_SETTINGS, "Reset: the settings couldn't be saved"));
		return;
	}
	if (n == STORAGE_NATIVE_FAILED && storage_ready()) {
		save_failed(TRN(STORE_ERR_RESET_ERASE, "Reset: native area couldn't be erased"));
		return;
	}
	if (board.system != NULL && board.system->reboot != NULL) {
		board.system->reboot(HAL_REBOOT_NORMAL);	// does not return when it works
	}
	radio_req_reload();	// no restart: the radio core takes the defaults now
}

bool ui_svc_factory_reset(void)
{
	return storage_job_post(factory_reset_job, NULL);
}

void ui_svc_boot_text(char line1[17], char line2[17])
{
	codeplug_boot_text(line1, line2);
}

void ui_svc_name_get(uint8_t which, char out[17])
{
	char other[CODEPLUG_NAME_LEN + 1];
	if (which == UI_NAME_RADIO)
		codeplug_radio_name(out);
	else if (which == UI_NAME_BOOT1)
		codeplug_boot_text(out, other);
	else if (which == UI_NAME_BOOT2)
		codeplug_boot_text(other, out);
	else
		out[0] = '\0';
}

// The boot lines are one record: the line not changed is read again here, so a CPS write since stays
static void name_job(void *ctx)
{
	(void)ctx;
	char l1[CODEPLUG_NAME_LEN + 1], l2[CODEPLUG_NAME_LEN + 1];
	bool ok;
	if (job.channel == UI_NAME_RADIO) {
		ok = codeplug_set_radio_name(job.name);
	} else {
		codeplug_boot_text(l1, l2);
		ok = codeplug_set_boot_text(job.channel == UI_NAME_BOOT1 ? job.name : l1,
					    job.channel == UI_NAME_BOOT2 ? job.name : l2);
	}
	if (!ok)
		save_failed(TRN(STORE_ERR_CODEPLUG_WRITE, "Couldn't write the codeplug: not saved"));
}

bool ui_svc_name_set(uint8_t which, const char *text)
{
	if (which >= UI_NAME_COUNT || job_busy())
		return false;
	job.channel = which;
	strncpy(job.name, text, CODEPLUG_NAME_LEN);
	job.name[CODEPLUG_NAME_LEN] = '\0';
	return cp_post(name_job);
}

#if CONFIG_KEY_ASSIGN
// --- Side keys (core/input/keyassign.h): the codeplug's quick-key slots, a copy here (UI task only), the key map
// built from them handed to the input task. A codeplug a CPS wrote is read again; a change made in Settings is
// written by a storage job, and until it lands the copy here is the one that counts ---

_Static_assert(UI_KEY_ROLES == KEYASSIGN_ROLES, "a setting pair per role");
static uint16_t key_slots[KEYASSIGN_SLOTS];
static uint16_t key_job_slots[KEYASSIGN_SLOTS];	// what the job writes (filled only while no job is pending)
static uint32_t key_serial;
static bool key_loaded, key_dirty;

static void key_apply(void)
{
	keymap_entry_t map[INPUT_KEYS_MAX];
	uint8_t n = board.keymap_len > INPUT_KEYS_MAX ? INPUT_KEYS_MAX : board.keymap_len;
	keyassign_build(board.keymap, n, board.side_keys, board.side_key_count, key_slots, map);
	keyassign_build_home(map, n, board.home_keys, board.home_key_count, board.side_key_count, key_slots);
	input_set_keymap(map);
}

static void key_load(void)
{
	key_serial = codeplug_serial();
	(void)codeplug_quick_keys(key_slots);	// none stored: all 0, no marker, the board's key map
	key_loaded = true;
	key_apply();
}

// The slots read again at the next poll (the UI task's): a held write of them was dropped
static void key_reload(void)
{
	key_loaded = false;
}

static void key_write_job(void *ctx)
{
	(void)ctx;
	if (!codeplug_set_quick_keys(key_job_slots)) {
		save_failed(TRN(STORE_ERR_KEYS_NOT_SAVED, "Couldn't save the keys: for now only"));
	}
}

void ui_svc_keys_poll(void)
{
	if (!key_loaded) {
		key_load();
	}
	if (key_dirty) {
		if (job_busy()) {
			return;
		}
		memcpy(key_job_slots, key_slots, sizeof(key_job_slots));
		if (cp_post(key_write_job)) {
			key_dirty = false;
		}
		return;
	}
	if (!job_busy() && codeplug_serial() != key_serial) {
		key_load();	// a CPS or TFTP write, or our own job, landed
	}
}

static int32_t key_get(uint8_t id)
{
	if (!key_loaded) {
		key_load();
	}
	uint8_t k = (uint8_t)(id - UI_SET_KEY_FIRST);
	uint8_t fn = keyassign_get(key_slots, k / 2u, (k & 1u) != 0);
	// A home key's slot holding what it can't do (Monitor, Tone burst from a CPS) reads as Default, as it acts
	if (k / 2u >= board.side_key_count && !keyassign_home_allowed(fn, (k & 1u) != 0)) {
		return KEYFN_DEFAULT;
	}
	return fn;
}

uint8_t ui_svc_home_key(uint8_t action, uint8_t gesture, bool home)
{
	if (action < ACT_HOME_KEY_1) {
		return action;
	}
	if (!key_loaded) {
		key_load();
	}
	uint8_t n = board.keymap_len > INPUT_KEYS_MAX ? INPUT_KEYS_MAX : board.keymap_len;
	return keyassign_home_event(action, gesture, home, board.keymap, n, board.home_keys, board.home_key_count,
				    board.side_key_count, key_slots);
}

bool ui_svc_key_offered(uint8_t id, int32_t value)
{
	uint8_t k = (uint8_t)(id - UI_SET_KEY_FIRST);
	if (id < UI_SET_KEY_FIRST || id > UI_SET_KEY_LAST || value < 0 || value >= KEYFN_COUNT) {
		return false;
	}
	if (k / 2u < board.side_key_count) {
		return keyassign_allowed((uint8_t)value, (k & 1u) != 0);
	}
	return k / 2u < board.side_key_count + board.home_key_count &&
	       keyassign_home_allowed((uint8_t)value, (k & 1u) != 0);
}

static bool key_set(uint8_t id, int32_t value)
{
	if (!ui_svc_key_offered(id, value)) {
		return false;
	}
	uint8_t k = (uint8_t)(id - UI_SET_KEY_FIRST);
	if (!key_loaded) {
		key_load();
	}
	keyassign_set(key_slots, k / 2u, (k & 1u) != 0, (uint8_t)value);
	key_apply();
	key_dirty = true;
	return true;
}
#endif
