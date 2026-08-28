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

uint32_t ui_svc_feat_storage(void)
{
	return UI_FEAT_SAVE_CHANNEL | UI_FEAT_FACTORY_RESET;
}

static uint16_t clamp16(int32_t v, int32_t lo, int32_t hi)
{
	return (uint16_t)(v < lo ? lo : v > hi ? hi : v);
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
	case UI_SET_REPLY_S:
		return true;	// ui_svc_apply hands it to the radio core, which saves it
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

// The radio's view, and whether it is on a stored channel (static: too big for the UI task's stack)
static radio_view_t v;

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
// the defaults. Never the legacy codeplug, the calibration, or the settings ring and journal as raw flash (the
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
