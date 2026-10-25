// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR emergency (dmr_emergency.h, docs/spec/dmr-emergency.md)
#include <string.h>
#include "config.h"
#include "dmr/dmr_emergency.h"

#if CONFIG_DMR_EMERGENCY
#include "dmr/dmrData.h"
#include "dmr/dmrDataService.h"

static void (*lock_fn)(void);
static void (*unlock_fn)(void);

static void lock(void)
{
	if (lock_fn != NULL) {
		lock_fn();
	}
}

static void unlock(void)
{
	if (unlock_fn != NULL) {
		unlock_fn();
	}
}

// Radio task state
static dmr_emergency_settings_t set;
static uint32_t set_serial;
static uint8_t state;			// dmr_emergency_state_t
static uint32_t hot_mic_until;
static bool hot_mic;
static uint32_t target_id;
static bool target_private;
static uint32_t cfg_serial;
static bool tone_due;
static uint32_t last_call_src, last_call_at;	// the last emergency call reported (src, its start)
static dmr_emergency_stats_t stats;

// Shared with the API (under the lock)
static dmr_emergency_alert_t alert;
static volatile uint32_t serial;
static volatile uint8_t shown_state;

// Requests from other tasks
static struct {
	bool start, cancel, dismiss, settings;
	dmr_emergency_settings_t set;
} mbox;

static void clamp(dmr_emergency_settings_t *s)
{
	if (s->mode > DMR_EMERGENCY_MODE_CALL) {
		s->mode = DMR_EMERGENCY_MODE_ALARM_CALL;
	}
	if (s->dst > 0xFFFFFFu) {
		s->dst = 0;
	}
	if (s->alarm_tries == 0 || s->alarm_tries > DMR_EMERGENCY_TRIES_MAX) {
		s->alarm_tries = DMR_EMERGENCY_TRIES_DEFAULT;
	}
	if (s->hot_mic_s > DMR_EMERGENCY_HOT_MIC_MAX_S) {
		s->hot_mic_s = DMR_EMERGENCY_HOT_MIC_MAX_S;
	}
}

static void set_state(uint8_t st)
{
	if (st == state) {
		return;
	}
	bool was = dmr_emergency_in_progress();
	state = st;
	shown_state = st;
	if (dmr_emergency_in_progress() != was) {
		cfg_serial++;
	}
	serial++;
}

void dmr_emergency_init(const dmr_emergency_settings_t *s)
{
	if (s != NULL) {
		set = *s;
	} else {
		dmr_emergency_settings_default(&set);
	}
	clamp(&set);
	set_serial++;
	state = DMR_EMERGENCY_IDLE;
	shown_state = state;
	hot_mic = false;
	tone_due = false;
	cfg_serial++;
	lock();
	memset(&mbox, 0, sizeof(mbox));
	memset(&alert, 0, sizeof(alert));
	unlock();
	serial++;
}

void dmr_emergency_set_lock(void (*l)(void), void (*u)(void))
{
	lock_fn = l;
	unlock_fn = u;
}

bool dmr_emergency_in_progress(void)
{
	return state == DMR_EMERGENCY_ALARM || state == DMR_EMERGENCY_HOT_MIC || state == DMR_EMERGENCY_ACTIVE;
}

void dmr_emergency_target(uint32_t *id, bool *private_call)
{
	*id = target_id;
	*private_call = target_private;
}

uint32_t dmr_emergency_config_serial(void)
{
	return cfg_serial;
}

bool dmr_emergency_hot_mic(void)
{
	return hot_mic;
}

bool dmr_emergency_take_tone(void)
{
	bool t = tone_due;
	tone_due = false;
	return t;
}

static void stop(void)
{
	dmrDataServiceEmergencyAlarmCancel();
	hot_mic = false;
	set_state(DMR_EMERGENCY_IDLE);
}

// After the alarm (or straight away for a call): the hot mic, or just the emergency mode for the user's own PTT
static void begin_call(uint32_t now_ms)
{
	if (set.mode == DMR_EMERGENCY_MODE_ALARM) {
		set_state(DMR_EMERGENCY_ACTIVE);
		return;
	}
	if (set.hot_mic_s == 0) {
		set_state(DMR_EMERGENCY_ACTIVE);
		return;
	}
	hot_mic = true;
	hot_mic_until = now_ms + (uint32_t)set.hot_mic_s * 1000u;
	stats.hot_mic_keyed++;
	set_state(DMR_EMERGENCY_HOT_MIC);
}

// The emergency key: the target from the settings or the channel, then the alarm or the call
static void start(uint32_t now_ms, const dmr_emergency_channel_t *ch)
{
	stats.started++;
	uint32_t id = set.dst != 0 ? set.dst : ch->talk_id;
	bool priv = set.dst != 0 ? set.dst_private : ch->talk_private;
	if (!ch->can_transmit || id == 0) {
		stats.failed++;
		set_state(DMR_EMERGENCY_FAILED);
		return;
	}
	if (id != target_id || priv != target_private) {
		target_id = id;
		target_private = priv;
		cfg_serial++;
	}
	if (set.mode != DMR_EMERGENCY_MODE_CALL) {
		if (!dmrDataServiceEmergencyAlarm(id, !priv, set.etsi_fid ? DMR_FID_ETSI : DMR_FID_MOTOROLA, set.alarm_tries)) {
			stats.failed++;
			set_state(DMR_EMERGENCY_FAILED);
			return;
		}
		stats.alarms_started++;
		set_state(DMR_EMERGENCY_ALARM);
		return;
	}
	begin_call(now_ms);
}

void dmr_emergency_tick(uint32_t now_ms, const dmr_emergency_channel_t *ch)
{
	bool do_start, do_cancel, do_dismiss, do_set;
	dmr_emergency_settings_t ns;

	lock();
	do_start = mbox.start;
	do_cancel = mbox.cancel;
	do_dismiss = mbox.dismiss;
	do_set = mbox.settings;
	ns = mbox.set;
	mbox.start = mbox.cancel = mbox.dismiss = mbox.settings = false;
	if (do_dismiss && alert.active) {
		alert.active = false;
		alert.pending = 0;
		serial++;
	}
	unlock();

	if (do_set) {
		clamp(&ns);
		set = ns;
		set_serial++;
	}
	if (do_cancel) {
		stop();
	}
	if (do_start && !dmr_emergency_in_progress()) {
		start(now_ms, ch);
	}

	switch (state) {
	case DMR_EMERGENCY_ALARM:
		if (!dmrDataServiceEmergencyAlarmBusy()) {
			begin_call(now_ms);	// all tries out (or the channel went: the call then fails like PTT would)
		}
		break;
	case DMR_EMERGENCY_HOT_MIC:
		if ((int32_t)(now_ms - hot_mic_until) >= 0 || !ch->can_transmit) {
			hot_mic = false;
			set_state(DMR_EMERGENCY_ACTIVE);
		}
		break;
	default:
		break;
	}
	// A dst of 0 follows the channel while in emergency (the user changed channel)
	if (dmr_emergency_in_progress() && set.dst == 0 &&
	    (ch->talk_id != target_id || ch->talk_private != target_private) && ch->talk_id != 0) {
		target_id = ch->talk_id;
		target_private = ch->talk_private;
		cfg_serial++;
	}
}

void dmr_emergency_heard(uint8_t kind, uint32_t src, uint32_t dst, bool group, uint32_t now_ms)
{
	if (kind != DMR_EMERGENCY_ALERT_ALARM && kind != DMR_EMERGENCY_ALERT_CALL) {
		return;
	}
	// The same call reported again (the radio core publishes every change of a call)
	if (kind == DMR_EMERGENCY_ALERT_CALL && src == last_call_src && now_ms == last_call_at) {
		return;
	}
	if (kind == DMR_EMERGENCY_ALERT_CALL) {
		last_call_src = src;
		last_call_at = now_ms;
	}
	lock();
	// The same radio's emergency again within DMR_EMERGENCY_REALERT_MS of the last (an alarm repeated, the call that
	// follows it, the next over): no new alert, dismissed or not; its activity keeps the window open
	bool same = alert.kind != DMR_EMERGENCY_ALERT_NONE && alert.src == src && (now_ms - alert.at_ms) < DMR_EMERGENCY_REALERT_MS;
	if (same) {
		alert.at_ms = now_ms;
		if (kind == DMR_EMERGENCY_ALERT_CALL) {
			alert.kind = kind;	// the voice is what the user hears now
		}
		unlock();
		return;
	}
	alert.serial++;
	alert.kind = kind;
	alert.src = src;
	alert.dst = dst;
	alert.group = group;
	alert.at_ms = now_ms;
	alert.active = true;
	if (alert.pending < 255) {
		alert.pending++;
	}
	unlock();
	serial++;
	stats.alerts++;
	if (kind == DMR_EMERGENCY_ALERT_ALARM) {
		stats.alerts_alarm++;
	} else {
		stats.alerts_call++;
	}
	if (set.rx_tone) {
		tone_due = true;
		stats.tones++;
	}
}

void dmr_emergency_get_settings_now(dmr_emergency_settings_t *out)
{
	*out = set;
}

uint32_t dmr_emergency_settings_serial(void)
{
	return set_serial;
}

// --- Any task ---

bool dmr_emergency_start(void)
{
	bool ok;
	lock();
	ok = !mbox.start && !dmr_emergency_in_progress();
	if (ok) {
		mbox.start = true;
		mbox.cancel = false;
	}
	unlock();
	return ok;
}

void dmr_emergency_cancel(void)
{
	lock();
	mbox.cancel = true;
	mbox.start = false;
	unlock();
}

void dmr_emergency_dismiss(void)
{
	lock();
	mbox.dismiss = true;
	unlock();
}

dmr_emergency_state_t dmr_emergency_state(void)
{
	return (dmr_emergency_state_t)shown_state;
}

bool dmr_emergency_alert(dmr_emergency_alert_t *out)
{
	lock();
	*out = alert;
	unlock();
	return out->active;
}

uint32_t dmr_emergency_serial(void)
{
	return serial;
}

void dmr_emergency_get_settings(dmr_emergency_settings_t *out)
{
	lock();
	*out = mbox.settings ? mbox.set : set;
	unlock();
}

void dmr_emergency_set_settings(const dmr_emergency_settings_t *in)
{
	lock();
	mbox.set = *in;
	mbox.settings = true;
	unlock();
}

void dmr_emergency_get_stats(dmr_emergency_stats_t *out)
{
	*out = stats;
}

void dmr_emergency_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif
