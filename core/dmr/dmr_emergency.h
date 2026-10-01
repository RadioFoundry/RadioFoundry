// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR emergency (CONFIG_DMR_EMERGENCY, docs/spec/dmr-emergency.md): what the emergency key starts, and what the radio
 * shows when it hears someone else's emergency.
 *
 *  Ours:  the emergency alarm (a CSBK, sent by the data service, repeated alarm_tries times), then, per the mode,
 *         an emergency call: the voice LC carries the emergency service option (TS 102 361-2 7.2.1) to the emergency
 *         target, with the microphone keyed by itself for hot_mic_s ("hot mic"). The radio stays in emergency until
 *         canceled: every call made meanwhile carries the emergency option and goes to the emergency target.
 *  Heard: an emergency alarm to us or a group we listen to, or a voice call with the emergency option, raises an
 *         alert: a banner (dmr_emergency_alert) and the emergency tone, once per radio: its emergency heard again
 *         within DMR_EMERGENCY_REALERT_MS of the last (repeated alarms, the call after the alarm, the next over) adds
 *         no alert, dismissed or not.
 *
 * Threading: the radio core (core/radio/radio_sm.c) calls the "radio task" functions; the user calls (start, cancel,
 * dismiss, reads, settings) may come from any task: requests go through a mailbox taken at the next tick, reads are
 * copies under the lock given to dmr_emergency_set_lock (the firmware: the data service's, a scheduler suspend).
 * Built without CONFIG_DMR_EMERGENCY every call is an inline no-op, so UI code needs no #if.
 */
#ifndef ODR_DMR_EMERGENCY_H
#define ODR_DMR_EMERGENCY_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define DMR_EMERGENCY_REALERT_MS	30000	// the same radio's emergency again after this alerts again
#define DMR_EMERGENCY_TRIES_DEFAULT	3
#define DMR_EMERGENCY_TRIES_MAX		10
#define DMR_EMERGENCY_HOT_MIC_DEFAULT_S	10
#define DMR_EMERGENCY_HOT_MIC_MAX_S	60

typedef enum {
	DMR_EMERGENCY_MODE_ALARM = 0,		// the alarm only
	DMR_EMERGENCY_MODE_ALARM_CALL = 1,	// the alarm, then an emergency call (hot mic when set)
	DMR_EMERGENCY_MODE_CALL = 2,		// an emergency call only
} dmr_emergency_mode_t;

typedef enum {
	DMR_EMERGENCY_IDLE = 0,
	DMR_EMERGENCY_ALARM,		// the alarm is going out
	DMR_EMERGENCY_HOT_MIC,		// the emergency call is keyed by itself
	DMR_EMERGENCY_ACTIVE,		// in emergency: calls carry the emergency option (until canceled)
	DMR_EMERGENCY_FAILED,		// couldn't start (not a DMR channel we may transmit on, no target); until the next start
} dmr_emergency_state_t;

typedef enum {
	DMR_EMERGENCY_ALERT_NONE = 0,
	DMR_EMERGENCY_ALERT_ALARM,	// an emergency alarm CSBK
	DMR_EMERGENCY_ALERT_CALL,	// a voice call with the emergency service option
} dmr_emergency_alert_kind_t;

// The settings (kept in the settings store by the radio core: settings_t em_*)
typedef struct {
	uint8_t mode;		// dmr_emergency_mode_t
	uint32_t dst;		// the emergency target; 0 = the channel's talk target
	bool dst_private;	// dst is a radio (individual alarm, private call), else a talkgroup
	uint8_t alarm_tries;	// 1..DMR_EMERGENCY_TRIES_MAX
	uint8_t hot_mic_s;	// 0 = no hot mic (the user keys the emergency call), up to DMR_EMERGENCY_HOT_MIC_MAX_S
	bool etsi_fid;		// the alarm with FID 0x00, else Motorola's 0x10 (docs/spec/dmr-emergency.md 2)
	bool rx_tone;		// sound the emergency tone for an alert heard
} dmr_emergency_settings_t;

// The newest alert heard (the banner)
typedef struct {
	uint32_t serial;	// bumped on every new alert (redraw / tone)
	uint8_t kind;		// dmr_emergency_alert_kind_t, NONE: nothing heard yet
	uint32_t src;		// the radio in emergency
	uint32_t dst;		// the group (or us) it was sent to
	bool group;
	uint32_t at_ms;		// last heard (the radio core's clock)
	bool active;		// not dismissed yet
	uint8_t pending;	// alerts not dismissed (other radios too), 0..255
} dmr_emergency_alert_t;

typedef struct {
	uint32_t started;		// emergencies started by the user
	uint32_t failed;		// ... that couldn't start
	uint32_t alarms_started;	// alarm sequences handed to the data service
	uint32_t hot_mic_keyed;		// hot mic transmissions
	uint32_t alerts;		// alerts raised (new radios, or again after DMR_EMERGENCY_REALERT_MS)
	uint32_t alerts_alarm;		// ... from alarms
	uint32_t alerts_call;		// ... from voice calls
	uint32_t tones;			// emergency tones asked for
} dmr_emergency_stats_t;

// What the radio core tells the module at each tick
typedef struct {
	bool can_transmit;	// a DMR channel we may transmit on now (as the data service's canTransmit)
	uint32_t talk_id;	// the channel's talk target, for a dst of 0
	bool talk_private;
} dmr_emergency_channel_t;

// The defaults: alarm then a call with 10 s hot mic, to the channel's talk target, Motorola FID, the tone on
static inline void dmr_emergency_settings_default(dmr_emergency_settings_t *s)
{
	*s = (dmr_emergency_settings_t){ .mode = DMR_EMERGENCY_MODE_ALARM_CALL, .dst = 0, .dst_private = false,
		.alarm_tries = DMR_EMERGENCY_TRIES_DEFAULT, .hot_mic_s = DMR_EMERGENCY_HOT_MIC_DEFAULT_S, .etsi_fid = false,
		.rx_tone = true };
}

#if CONFIG_DMR_EMERGENCY

// --- The radio core (radio task) ---

void dmr_emergency_init(const dmr_emergency_settings_t *s);
void dmr_emergency_set_lock(void (*lock)(void), void (*unlock)(void));
// Every RADIO_TICK_MS: the mailbox, the alarm, the hot mic timer
void dmr_emergency_tick(uint32_t now_ms, const dmr_emergency_channel_t *ch);
// In emergency (alarm, hot mic or active): calls carry the emergency service option and go to the target below
bool dmr_emergency_in_progress(void);
// The emergency target in effect (the setting, else the channel's talk target as given at the tick)
void dmr_emergency_target(uint32_t *id, bool *private_call);
// Bumped when in_progress or the target changes: the radio core reconfigures the call stack
uint32_t dmr_emergency_config_serial(void);
// The radio core keys the microphone (hot mic) while this is true
bool dmr_emergency_hot_mic(void);
// An emergency tone is due (true once per alert, with rx_tone set)
bool dmr_emergency_take_tone(void);
// Heard: from the data service (alarms) and the radio core (calls with the emergency service option)
void dmr_emergency_heard(uint8_t kind, uint32_t src, uint32_t dst, bool group, uint32_t now_ms);
// The settings in effect (the radio core saves them when settings_serial moves)
void dmr_emergency_get_settings_now(dmr_emergency_settings_t *out);
uint32_t dmr_emergency_settings_serial(void);

// --- Any task ---

// The emergency key: start (false when one is running already); cancel ends it, whatever the state
bool dmr_emergency_start(void);
void dmr_emergency_cancel(void);
// The user has seen the alert(s): the banner goes
void dmr_emergency_dismiss(void);
dmr_emergency_state_t dmr_emergency_state(void);
// A copy of the newest alert; false when none is active (out filled either way)
bool dmr_emergency_alert(dmr_emergency_alert_t *out);
// Bumped on every state change and alert
uint32_t dmr_emergency_serial(void);
void dmr_emergency_get_settings(dmr_emergency_settings_t *out);
void dmr_emergency_set_settings(const dmr_emergency_settings_t *in);
void dmr_emergency_get_stats(dmr_emergency_stats_t *out);
void dmr_emergency_reset_stats(void);

#else

static inline bool dmr_emergency_start(void) { return false; }
static inline void dmr_emergency_cancel(void) { }
static inline void dmr_emergency_dismiss(void) { }
static inline dmr_emergency_state_t dmr_emergency_state(void) { return DMR_EMERGENCY_IDLE; }
static inline bool dmr_emergency_alert(dmr_emergency_alert_t *out)
{ if (out) { *out = (dmr_emergency_alert_t){ 0 }; } return false; }
static inline uint32_t dmr_emergency_serial(void) { return 0; }
static inline void dmr_emergency_get_settings(dmr_emergency_settings_t *out) { dmr_emergency_settings_default(out); }
static inline void dmr_emergency_set_settings(const dmr_emergency_settings_t *in) { (void)in; }

#endif

#endif
