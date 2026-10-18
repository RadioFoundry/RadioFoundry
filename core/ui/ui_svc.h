// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// What the UI needs from packages it doesn't own, as thin calls. ui_svc_stub.c defines every one weakly with a
// harmless "not in this build" behavior, so the UI builds and runs alone; the owning package defines the real one
// (a strong definition replaces the weak one at link time). docs/spec/ui.md lists who owns what.
//
// All calls come from the UI task and must not block for long (queue a request and return).
#ifndef UI_SVC_H
#define UI_SVC_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "radio/radio.h"

// --- What is available (screens gray out what isn't, and say why on SELECT) ---
#define UI_FEAT_TALK_TARGET	(1u << 0)	// ui_svc_talk_to / ui_svc_focus_target (dmr-voice)
#define UI_FEAT_TIMESLOT	(1u << 1)	// ui_svc_dmr_timeslot (dmr-voice / radio core)
#define UI_FEAT_COLOR_CODE	(1u << 2)	// ui_svc_dmr_color_code
#define UI_FEAT_MODE		(1u << 3)	// ui_svc_mode: FM <-> DMR on a channel
#define UI_FEAT_HEAR_ALL	(1u << 4)	// ui_svc_hear_all latch
#define UI_FEAT_PRIVACY		(1u << 5)	// ui_svc_privacy
#define UI_FEAT_SCAN		(1u << 6)	// ui_svc_scan (radio-features)
#define UI_FEAT_VOX		(1u << 7)
#define UI_FEAT_SMS		(1u << 8)	// ui_svc_msg_* (dmr-data)
#define UI_FEAT_SIGNALING	(1u << 9)	// ui_svc_radio_check / ui_svc_call_alert (dmr-data)
#define UI_FEAT_SAVE_CHANNEL	(1u << 10)	// ui_svc_save_channel (storage-cps)
#define UI_FEAT_GPS		(1u << 11)	// ui_svc_position
#define UI_FEAT_EMERGENCY	(1u << 12)	// ui_svc_emergency
#define UI_FEAT_MONITOR_LATCH	(1u << 13)	// ui_svc_monitor latch (FM)
#define UI_FEAT_FACTORY_RESET	(1u << 14)	// ui_svc_factory_reset (storage-cps)
#define UI_FEAT_ROAMING		(1u << 15)	// ui_svc_roam (DMR repeater roaming, the radio core)
#define UI_FEAT_VIBRATE		(1u << 16)	// ui_svc_vibrate: CONFIG_VIBRATE and hal_power_t.vibrate
#define UI_FEAT_VOLUME		(1u << 17)	// the volume is the radio's to set: no knob (radio_volume_soft), VOL key + Settings
uint32_t ui_svc_features(void);
// The parts of it each package reports (ui_svc_features ORs them): dmr-voice / radio core, dmr-data, storage-cps,
// radio-features
uint32_t ui_svc_feat_voice(void);
uint32_t ui_svc_feat_data(void);
uint32_t ui_svc_feat_storage(void);
uint32_t ui_svc_feat_radio(void);
// A package outside the four above that connects calls of its own (dmr-ext: UI_FEAT_EMERGENCY with its
// ui_svc_emergency); weak 0 until one does
uint32_t ui_svc_feat_ext(void);

// --- Talk target (dmr-voice; information-architecture.md 1, three lifetimes) ---
typedef enum {
	UI_TALK_TEMPORARY,	// until the channel changes or BACK on Home
	UI_TALK_PERMANENT,	// written to the channel (the codeplug)
	UI_TALK_DEFAULT,	// back to the channel's default (clears a temporary target)
} ui_talk_life_t;
bool ui_svc_talk_to(uint32_t id, bool private_call, uint8_t life);
// The target focused in a list now (0 = none): a PTT press while it is focused transmits to it once (one-shot) and
// keeps it for the reply window. The radio reads this at PTT down; the UI calls it on every focus change
void ui_svc_focus_target(uint32_t id, bool private_call);

// --- On-the-fly channel changes (temporary, P7) ---
bool ui_svc_dmr_timeslot(uint8_t ts);
bool ui_svc_dmr_color_code(uint8_t cc);
bool ui_svc_mode(uint8_t mode);			// radio_mode_t: the VFO's mode (a channel's goes through Copy to VFO)
// Copy to VFO: the channel on view into the VFO, which opens (CONFIG_VFO). Returns a radio_vfo_copy_t: VFO_COPY_OK
// when the request went to the radio core, else why nothing changed
uint8_t ui_svc_vfo_copy(const radio_view_t *v);
bool ui_svc_hear_all(bool on);
bool ui_svc_privacy(bool on);			// false also when there is no key: the UI says how to add one
bool ui_svc_monitor(bool on);			// FM monitor latch
bool ui_svc_vox(bool on);
// Make the temporary changes permanent (storage-cps): false if the codeplug can't be written
bool ui_svc_save_channel(void);
// Store a whole channel (the channel editor, ui.md 2.1) at number 1..CODEPLUG_CHANNELS_MAX, as a storage job like
// ui_svc_save_channel. false: a write is still going on, or no codeplug storage in this build
bool ui_svc_write_channel(uint16_t number, const channel_t *ch);
// Write the talk target into the channel (UI_TALK_PERMANENT): false if it isn't a stored contact or can't be written
bool ui_svc_save_talk_target(uint32_t id, bool private_call);

// --- Scan (radio-features). Whether a scan runs is radio_view_t.scan ---
bool ui_svc_scan(bool on);			// on: the zone (in the VFO: the band); off: stop where it is
bool ui_svc_scan_skip(void);			// nuisance delete of the channel that stopped the scan

// --- DMR repeater roaming (the radio core, docs/spec/dmr-roaming.md). Its state is radio_view_t.roam ---
bool ui_svc_roam(uint8_t cmd);			// radio_roam_cmd_t: zone on / off, roam now, lock / unlock

// --- Beeps (radio-features): the radio sounds key beeps only when its key_beep setting is on ---
typedef enum { UI_BEEP_KEY, UI_BEEP_ERROR, UI_BEEP_MESSAGE, UI_BEEP_BATTERY_LOW } ui_beep_t;
void ui_svc_beep(uint8_t beep);
// --- Vibration (radio-features 4.1): a pattern on the board's motor; the UI asks only when Settings > Sound >
// Vibrate is on. Weak no-op without CONFIG_VIBRATE ---
typedef enum { UI_VIBE_MESSAGE, UI_VIBE_CALL, UI_VIBE_EMERGENCY } ui_vibe_t;
void ui_svc_vibrate(uint8_t vibe);
// PTT while the keys are locked (Settings > Privacy > PTT when locked): blocked = the input layer doesn't hand PTT
// presses to the radio core (a PTT already held is still released). Weak no-op without the input layer
void ui_svc_ptt_block(bool blocked);

// --- Messages and signaling (dmr-data) ---
#define UI_MSG_TEXT_MAX	143
typedef struct {
	uint32_t src;		// sender (or, for sent ones, recipient)
	bool group;		// sent to a talkgroup
	bool unread;
	bool sent;		// one we sent
	uint32_t age_ms;	// how long ago
	char name[17];		// sender's name if known
	char text[UI_MSG_TEXT_MAX + 1];
} ui_msg_t;
uint16_t ui_svc_msg_count(void);		// inbox, newest first
uint16_t ui_svc_msg_unread(void);
bool ui_svc_msg_get(uint16_t i, ui_msg_t *out);
void ui_svc_msg_read(uint16_t i);		// mark read
bool ui_svc_msg_delete(uint16_t i);
// Queue a text message; the result comes back as a ui_notify() event
bool ui_svc_msg_send(uint32_t dst, bool group, const char *text);
bool ui_svc_radio_check(uint32_t id);
bool ui_svc_call_alert(uint32_t id);

// --- Location ---
typedef struct {
	bool fix;
	uint8_t sats;
	int32_t lat_e7, lon_e7;	// degrees x 1e7
	int16_t alt_m;
} ui_position_t;
bool ui_svc_position(ui_position_t *out);	// false: no GPS on this radio (or off)

// --- Emergency, reset ---
// Emergency is dmr-ext's: it defines ui_svc_emergency and reports UI_FEAT_EMERGENCY from ui_svc_feat_ext. Until
// then the weak stub says "not set up"
bool ui_svc_emergency(bool on);
// Our own emergency is going (alarm, hot mic or in emergency until canceled): the key then cancels it
bool ui_svc_emergency_active(void);
// dmr-ext: a new emergency alert heard becomes UI_EV_EMERGENCY (ok: an alarm, else a call); run from ui_svc_poll
void ui_svc_emergency_poll(void);
// The emergency screen was acknowledged: the received alert is dismissed (weak no-op without dmr-ext)
void ui_svc_emergency_dismiss(void);
// The settings back to their defaults and the native codeplug area emptied (never the open-firmware codeplug or the
// calibration: those would need a second, explicit confirmation), then the radio restarts. Done by the housekeeping
// task, not the UI's; false if it can't be started (no flash, or a save still going on)
bool ui_svc_factory_reset(void);

// --- Settings persistence (storage-cps). The stub keeps them in RAM (lost at power off) ---
typedef enum {
	UI_SET_KEY_TONES, UI_SET_TONE_VOLUME, UI_SET_TALK_PERMIT, UI_SET_VOICE_PROMPTS, UI_SET_MESSAGE_TONE,
	UI_SET_BRIGHTNESS, UI_SET_BACKLIGHT_S, UI_SET_THEME, UI_SET_LARGE_TEXT, UI_SET_HINTS,
	UI_SET_REPLY_S, UI_SET_TOT_S, UI_SET_SQUELCH, UI_SET_MIC_GAIN, UI_SET_HEAR_ALL_DEFAULT, UI_SET_ALIAS_SHOW,
	UI_SET_AUTO_LOCK_S, UI_SET_PTT_LOCKED,
	UI_SET_GPS, UI_SET_POS_FORMAT, UI_SET_POS_SEND_S, UI_SET_POS_REPLY,
	UI_SET_HOTSPOT_AUTO, UI_SET_CHECK_REPLY, UI_SET_ALERT_ANSWER,
	UI_SET_BATTERY_SAVER, UI_SET_AUTO_OFF_MIN, UI_SET_LOW_BATTERY,
	UI_SET_LONG_PRESS_MS, UI_SET_REPEAT_MS, UI_SET_UPDOWN_HOME,
	UI_SET_PRIVACY_TYPE, UI_SET_PRIVACY_KEY,	// key slot 1 (settings-store.md 3.1): dmrPrivacyType_t, key value
	// DMR roaming (Tools > Roaming, dmr-roaming.md 6.2): survey 0 off / 1 synced / 2 per site, the repeaters' beacon
	// interval s (0 none) and duration s, the wake probe 0 / 1
	UI_SET_ROAM_SURVEY, UI_SET_ROAM_BEACON_S, UI_SET_ROAM_BEACON_LEN, UI_SET_ROAM_PROBE,
	UI_SET_TFTP_WRITE,	// codeplug writes over TFTP allowed (CONFIG_TFTP; settings_t.net_flags)
	// Text messages (dmr-data.md 5): the format ours go in, dmr_sms_format_t (2 ETSI IP text, 1 ETSI short data, 0
	// Motorola TMS)
	UI_SET_MSG_FORMAT,
	// USB network (usb-network.md 1, 5): the network personality at power-on, control station passthrough (the PC
	// answers the radio's data), the fixed link pair (192.168.10.x) instead of the address by DMR ID
	UI_SET_USB_BOOT_NET, UI_SET_PC_ENDPOINT, UI_SET_USB_FIXED_LINK,
	// Side keys (Settings > Keys, CONFIG_KEY_ASSIGN; core/input/keyassign.h): a KEYFN_* each, kept in the codeplug's
	// quick-key slots. Press and long press of side key 1, side key 2 and the top key, in this order
	UI_SET_KEY_SIDE1, UI_SET_KEY_SIDE1_HOLD, UI_SET_KEY_SIDE2, UI_SET_KEY_SIDE2_HOLD, UI_SET_KEY_TOP,
	UI_SET_KEY_TOP_HOLD,
	UI_SET_VIBRATE,		// vibrate for alerts (Settings > Sound, CONFIG_VIBRATE; settings_t.ui_flags)
	UI_SET_ACCENT,		// the highlight color, UI_ACCENT_* (Settings > Display; settings_t.accent bits 0-2)
	UI_SET_STRIP,		// the status strip filled with it (Settings > Display; settings_t.accent bit 3)
	UI_SET_VOLUME,		// speaker volume 0 (mute) .. RADIO_VOLUME_MAX of a radio without a knob (UI_FEAT_VOLUME; settings_t.volume)
	UI_SET_NOISE_REDUCTION,	// the vocoder's mic noise reduction on DMR TX (Settings > Talk & listen; audio_flags)
	UI_SET_MIC_AGC,		// the vocoder's mic AGC on DMR TX (audio_flags); while on, mic gain is unity and dimmed
	UI_SET_COUNT
} ui_setting_t;

// UI_SET_ACCENT values: the theme's own highlight (blue), or one of seven. Color displays only
enum {
	UI_ACCENT_DEFAULT, UI_ACCENT_RED, UI_ACCENT_ORANGE, UI_ACCENT_YELLOW, UI_ACCENT_TEAL, UI_ACCENT_BLUE,
	UI_ACCENT_PURPLE, UI_ACCENT_PINK, UI_ACCENT_COUNT
};
int32_t ui_svc_setting_get(uint8_t id);
bool ui_svc_setting_set(uint8_t id, int32_t value);
// The RAM table behind the stub (defaults until set): for the settings no package stores
int32_t ui_svc_ram_get(uint8_t id);
bool ui_svc_ram_set(uint8_t id, int32_t value);
// A setting has its value (at start-up for every one, then on each change): hand it to the package that acts on it
// (the radio core's radio_settings_t for time-out, key beeps, backlight, battery saver)
void ui_svc_apply(uint8_t id, int32_t value);
// The reply window value shown for the radio core's settings (ui_svc_radio.c; the codeplug's hang time when unset)
int32_t ui_svc_reply_shown(const radio_settings_t *s);

// --- Inbound: events other packages hand to the UI (any task; the UI picks them up on its next pass) ---
typedef enum {
	UI_EV_MESSAGE,		// a message arrived: index (inbox), id, name, text
	UI_EV_MESSAGE_SENT,	// ok = delivered (confirmed; index 1: sent, the format or a group has no confirmation) /
				// failed after retries
	UI_EV_CALL_ALERT,	// call alert received from id
	UI_EV_RADIO_CHECK,	// answer to our radio check: ok, ms
	UI_EV_PRIVATE_CALL,	// a private call to us from id started
	UI_EV_EMERGENCY,	// emergency received from id
	UI_EV_HOTSPOT_STARTED,	// the host started hotspot mode
	UI_EV_SCAN_STOPPED,	// the scan stopped on a channel
	UI_EV_CALL_ALERT_RESULT,	// our call alert: ok = the radio answered, else no answer after the retries
	UI_EV_SAVE_FAILED,	// a save to the codeplug the UI asked for failed later (text: what failed)
	UI_EV_POWER_OFF_FAILED,	// auto power off dropped the power latch but the radio is still on (the knob holds it)
} ui_event_kind_t;

typedef struct {
	uint8_t kind;		// ui_event_kind_t
	bool ok;
	uint16_t index;
	uint32_t id;
	uint32_t ms;
	char name[17];
	char text[40];
} ui_event_t;
// The owner's two boot screen lines from the codeplug (storage-cps); empty strings when there are none
void ui_svc_boot_text(char line1[17], char line2[17]);

// Queue an event for the UI (copies it). False when the queue is full (the event is dropped)
bool ui_notify(const ui_event_t *ev);
// Every UI pass: a package whose state the UI polls (rather than one that calls ui_notify) turns changes into events
void ui_svc_poll(void);
// Every UI pass: the side key assignment follows a codeplug a CPS wrote, and a change made in Settings goes to the
// codeplug (CONFIG_KEY_ASSIGN)
void ui_svc_keys_poll(void);

#endif
