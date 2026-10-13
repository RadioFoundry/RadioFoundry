// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The ui_svc.h calls storage-cps answers (docs/spec/settings-store.md, core/codeplug editing):
//   - settings: each UI setting is read from where it is kept across power cycles. The radio core's own settings
//     (key tones, tone volume, TX timeout, backlight time, battery saver) from radio_get_settings, which the radio
//     core saves and restores itself (ui_svc_apply sends changes there); the preferences in settings_t (brightness,
//     default squelch, hotspot allowed) directly; the DMR data service's answers (radio check, call alert, LRRP) from
//     its settings, which the radio core saves; the UI's own (reply window, mic gain, auto lock, key timing, GPS
//     options) from their settings_t fields. Settings nobody stores yet stay in the stub's RAM table.
//   - save to channel, the permanent talk target and the factory reset: the UI task only takes a copy of what to
//     write and hands a job to the housekeeping task (storage_job_post), so the UI never waits for a flash erase. The
//     channel is written with codeplug_set_channel; the radio core re-reads it when codeplug_serial() changes. A job
//     that fails tells the UI with a UI_EV_SAVE_FAILED event (a toast).
#include <string.h>
#include "board.h"
#include "ui_svc.h"
#include "radio/radio.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "codeplug/codeplug.h"
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
#if CONFIG_DMR_PRIVACY
#include "dmr/dmrPrivacy.h"
#endif
#if CONFIG_KEY_ASSIGN
#include "input/input.h"
#include "input/input_engine.h"
#include "input/keyassign.h"
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
	default: return 0;
	}
}

static radio_view_t v;		// the radio's view (static: too big for the UI task's stack)

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
	radio_get_view(&v);
	if (v.dmr.privacy) {
		radio_req_dmr_privacy(type, (uint16_t)key);
	}
}
#endif

#if CONFIG_KEY_ASSIGN
static int32_t key_get(uint8_t id);
static bool key_set(uint8_t id, int32_t value);
#endif

int32_t ui_svc_setting_get(uint8_t id)
{
	radio_settings_t rs;
	settings_t st;
	switch (id) {
#if CONFIG_KEY_ASSIGN
	case UI_SET_KEY_SIDE1:
	case UI_SET_KEY_SIDE1_HOLD:
	case UI_SET_KEY_SIDE2:
	case UI_SET_KEY_SIDE2_HOLD:
	case UI_SET_KEY_TOP:
	case UI_SET_KEY_TOP_HOLD:
		return key_get(id);
#endif
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
	case UI_SET_REPLY_S:
		radio_get_settings(&rs);
		return ui_svc_reply_shown(&rs);
	case UI_SET_MIC_GAIN:
		settings_read(&st);
		return st.mic_gain <= 10 ? st.mic_gain : 5;
	case UI_SET_AUTO_LOCK_S:
		settings_read(&st);
		return st.auto_lock_s;
	case UI_SET_LONG_PRESS_MS:
		settings_read(&st);
		return st.long_press_ms != 0 ? st.long_press_ms : ui_svc_ram_get(id);
	case UI_SET_REPEAT_MS:
		settings_read(&st);
		return st.repeat_ms != 0 ? st.repeat_ms : ui_svc_ram_get(id);
	case UI_SET_GPS:
		settings_read(&st);
		return st.gps != 0;
	case UI_SET_POS_FORMAT:
		settings_read(&st);
		return st.pos_format <= 2 ? st.pos_format : 0;
	case UI_SET_POS_SEND_S:
		settings_read(&st);
		return st.pos_send_s;
	case UI_SET_LARGE_TEXT:
	case UI_SET_HINTS:
	case UI_SET_MESSAGE_TONE:
	case UI_SET_ALIAS_SHOW:
	case UI_SET_PTT_LOCKED:
	case UI_SET_HEAR_ALL_DEFAULT:
		settings_read(&st);
		return (st.ui_flags & ui_flag(id)) != 0;
	case UI_SET_THEME:
		settings_read(&st);
		return st.theme <= 2 ? st.theme : 0;
	case UI_SET_UPDOWN_HOME:
		settings_read(&st);
		return st.updown_home <= 2 ? st.updown_home : 0;
	case UI_SET_LOW_BATTERY:
		settings_read(&st);
		return st.low_batt_pct;
	case UI_SET_AUTO_OFF_MIN:
		settings_read(&st);
		return st.auto_off_min;
	case UI_SET_TALK_PERMIT:
		radio_get_settings(&rs);
		return rs.talk_permit;
	case UI_SET_TFTP_WRITE:
		settings_read(&st);
		return (st.net_flags & SETTINGS_NET_TFTP_NO_WRITE) == 0;
#if CONFIG_DMR_ROAMING
	case UI_SET_ROAM_SURVEY:
	case UI_SET_ROAM_BEACON_S:
	case UI_SET_ROAM_BEACON_LEN:
	case UI_SET_ROAM_PROBE:
		radio_get_settings(&rs);
		return id == UI_SET_ROAM_SURVEY ? rs.roam_survey : id == UI_SET_ROAM_BEACON_S ? rs.roam_beacon_s :
			id == UI_SET_ROAM_BEACON_LEN ? rs.roam_beacon_len_s : rs.roam_probe;
#endif
#if CONFIG_DMR_PRIVACY
	case UI_SET_PRIVACY_TYPE:
		settings_read(&st);
		return st.privacy_type[0] < DMR_PRIVACY_NUM_TYPES ? st.privacy_type[0] : DMR_PRIVACY_OFF;
	case UI_SET_PRIVACY_KEY:
		settings_read(&st);
		return (int32_t)(st.privacy_key[0] & 0xFFFFu);
#endif
#if CONFIG_DMR_DATA_CORE
	case UI_SET_CHECK_REPLY:
	case UI_SET_ALERT_ANSWER:
	case UI_SET_POS_REPLY:
	case UI_SET_MSG_FORMAT: {
		dmrDataSettings_t ds;
		dmr_data_get_settings(&ds);
		if (id == UI_SET_MSG_FORMAT)
			return ds.smsFormat;
		return id == UI_SET_CHECK_REPLY ? ds.radioCheckReply : id == UI_SET_ALERT_ANSWER ? ds.callAlertAck :
			ds.lrrpReply;
	}
#endif
#if CONFIG_USB_NETWORK && CONFIG_HOSTLINK
	case UI_SET_USB_BOOT_NET:
		settings_read(&st);
		return st.usb_mode == 1;
	case UI_SET_PC_ENDPOINT:
		settings_read(&st);
		return (st.net_flags & SETTINGS_NET_PASSTHROUGH) != 0;
	case UI_SET_USB_FIXED_LINK:
		settings_read(&st);
		return (st.net_flags & SETTINGS_NET_FIXED_LINK) != 0;
#endif
	default:
		return ui_svc_ram_get(id);
	}
}

bool ui_svc_setting_set(uint8_t id, int32_t value)
{
	settings_t *st;
	switch (id) {
#if CONFIG_KEY_ASSIGN
	case UI_SET_KEY_SIDE1:
	case UI_SET_KEY_SIDE1_HOLD:
	case UI_SET_KEY_SIDE2:
	case UI_SET_KEY_SIDE2_HOLD:
	case UI_SET_KEY_TOP:
	case UI_SET_KEY_TOP_HOLD:
		return key_set(id, value);
#endif
	case UI_SET_KEY_TONES:
	case UI_SET_TONE_VOLUME:
	case UI_SET_TOT_S:
	case UI_SET_BACKLIGHT_S:
	case UI_SET_BATTERY_SAVER:
	case UI_SET_REPLY_S:
	case UI_SET_TALK_PERMIT:
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
		st = settings_begin();
		st->ui_flags = (uint8_t)(value != 0 ? st->ui_flags | ui_flag(id) : st->ui_flags & ~ui_flag(id));
		settings_end(hal_millis());
		return true;
	case UI_SET_THEME:
	case UI_SET_UPDOWN_HOME:
	case UI_SET_LOW_BATTERY:
	case UI_SET_AUTO_OFF_MIN:
		st = settings_begin();
		if (id == UI_SET_THEME) {
			st->theme = (uint8_t)clamp16(value, 0, 2);
		} else if (id == UI_SET_UPDOWN_HOME) {
			st->updown_home = (uint8_t)clamp16(value, 0, 2);
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
	case UI_SET_MSG_FORMAT: {
		dmrDataSettings_t ds;
		if ((value != DMR_SMS_FORMAT_ETSI_IP) && (value != DMR_SMS_FORMAT_ETSI) &&
		    !(CONFIG_DMR_TMS && (value == DMR_SMS_FORMAT_TMS)))
			return false;
		dmr_data_get_settings(&ds);
		ds.smsFormat = (uint8_t)value;
		dmr_data_set_settings(&ds);
		return true;
	}
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
	default:
		return ui_svc_ram_set(id, value);
	}
}

// --- Codeplug writes, on the housekeeping task (storage_job_post) ---

static void save_failed(const char *text)
{
	ui_event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.kind = UI_EV_SAVE_FAILED;
	strncpy(ev.text, text, sizeof ev.text - 1);
	ui_notify(&ev);
}

// What the job writes: a copy of the radio's view taken when the UI asked (the job runs up to 100 ms later), and the
// talk target for a permanent one. Only the UI task fills it, and only while no job is pending
static struct {
	uint16_t channel;
	uint8_t temp;
	uint8_t power;
	uint8_t squelch;
	channel_t now;		// the channel as the radio has it now (temporary changes included)
	uint32_t id;
	bool private_call;
} job;
static channel_t job_ch;	// the job's working copy of the stored channel

// Power, bandwidth, tones, mode, color code and timeslot go into the channel; the squelch is radio-wide (the default
// squelch preference). The radio core re-reads the channel (codeplug_serial), and the clear makes the remaining
// flags follow the new values
static void save_channel_job(void *ctx)
{
	(void)ctx;
	if (!codeplug_channel(job.channel, &job_ch)) {
		save_failed("Couldn't read the channel: not saved");
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
	if (!codeplug_set_channel(job.channel, &job_ch)) {
		save_failed("Couldn't write the codeplug: not saved");
		return;
	}
	if (job.temp & RADIO_TEMP_SQUELCH) {
		settings_t *st = settings_begin();
		st->squelch = job.squelch;
		settings_end(hal_millis());
	}
	radio_req_clear_temporary();
}

// Whether the radio is on a stored channel (its view in v)
static bool take_channel(void)
{
	if (storage_job_busy()) {
		return false;	// the last write is still going on
	}
	radio_get_view(&v);
	if (v.vfo || v.channel_number == 0 || v.hotspot) {
		return false;
	}
	job.channel = v.channel_number;
	job.temp = v.temp;
	job.power = v.power;
	job.squelch = v.squelch;
	job.now = v.ch;
	return true;
}

bool ui_svc_save_channel(void)
{
	return take_channel() && storage_job_post(save_channel_job, NULL);
}

#if CONFIG_DMR
// The first stored contact with this ID and kind becomes the channel's talk target. Searching the contacts reads up
// to 1024 records, so it is part of the job too. Until it is done the target is a temporary one (ui_svc_talk_to)
static void save_target_job(void *ctx)
{
	(void)ctx;
	contact_t c;
	if (!codeplug_channel(job.channel, &job_ch)) {
		save_failed("Couldn't read the channel: for now only");
		return;
	}
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		if (!codeplug_contact(i, &c) || c.id != job.id || (c.type == CONTACT_PRIVATE) != job.private_call)
			continue;
		job_ch.contact = i;
		if (!codeplug_set_channel(job.channel, &job_ch)) {
			save_failed("Couldn't write the codeplug: for now only");
			return;
		}
		radio_req_dmr_target_clear();	// the channel's own target is this one now
		return;
	}
	save_failed("Not a stored contact: for now only");
}

bool ui_svc_save_talk_target(uint32_t id, bool private_call)
{
	if (id == 0 || !take_channel())
		return false;
	job.id = id;
	job.private_call = private_call;
	if (!storage_job_post(save_target_job, NULL))
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
	settings_defaults(st);
	settings_end(hal_millis());
	bool saved = settings_flush();
	storage_native_t n = storage_native_erase();
	if (!saved && storage_ready()) {
		save_failed("Reset: the settings couldn't be saved");
		return;
	}
	if (n == STORAGE_NATIVE_FAILED && storage_ready()) {
		save_failed("Reset: the native area couldn't be erased");
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

#if CONFIG_KEY_ASSIGN
// --- Side keys (core/input/keyassign.h): the codeplug's quick-key slots, a copy here (UI task only), the key map
// built from them handed to the input task. A codeplug a CPS wrote is read again; a change made in Settings is
// written by a storage job, and until it lands the copy here is the one that counts ---

static uint16_t key_slots[KEYASSIGN_SLOTS];
static uint16_t key_job_slots[KEYASSIGN_SLOTS];	// what the job writes (filled only while no job is pending)
static uint32_t key_serial;
static bool key_loaded, key_dirty;

static void key_apply(void)
{
	keymap_entry_t map[INPUT_KEYS_MAX];
	uint8_t n = board.keymap_len > INPUT_KEYS_MAX ? INPUT_KEYS_MAX : board.keymap_len;
	keyassign_build(board.keymap, n, board.side_keys, board.side_key_count, key_slots, map);
	input_set_keymap(map);
}

static void key_load(void)
{
	key_serial = codeplug_serial();
	(void)codeplug_quick_keys(key_slots);	// none stored: all 0, no marker, the board's key map
	key_loaded = true;
	key_apply();
}

static void key_write_job(void *ctx)
{
	(void)ctx;
	if (!codeplug_set_quick_keys(key_job_slots)) {
		save_failed("Couldn't save the keys: for now only");
	}
}

void ui_svc_keys_poll(void)
{
	if (!key_loaded) {
		key_load();
	}
	if (key_dirty) {
		if (storage_job_busy()) {
			return;
		}
		memcpy(key_job_slots, key_slots, sizeof(key_job_slots));
		if (storage_job_post(key_write_job, NULL)) {
			key_dirty = false;
		}
		return;
	}
	if (!storage_job_busy() && codeplug_serial() != key_serial) {
		key_load();	// a CPS or TFTP write, or our own job, landed
	}
}

static int32_t key_get(uint8_t id)
{
	if (!key_loaded) {
		key_load();
	}
	uint8_t k = (uint8_t)(id - UI_SET_KEY_SIDE1);
	return keyassign_get(key_slots, k / 2u, (k & 1u) != 0);
}

static bool key_set(uint8_t id, int32_t value)
{
	uint8_t k = (uint8_t)(id - UI_SET_KEY_SIDE1);
	if (k / 2u >= board.side_key_count || value < 0 || value >= KEYFN_COUNT) {
		return false;
	}
	if (!key_loaded) {
		key_load();
	}
	keyassign_set(key_slots, k / 2u, (k & 1u) != 0, (uint8_t)value);
	key_apply();
	key_dirty = true;
	return true;
}
#endif
