// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Weak stand-ins for everything in ui_svc.h that another package owns. Each reports "not available", except the
// settings store, which keeps values in RAM so the Settings screens work before storage-cps lands. A package that
// owns one of these defines it without the weak attribute and the linker takes that one.
#include "ui_svc.h"

#define WEAK __attribute__((weak))

// What each package provides, ORed; a package defines its own ui_svc_feat_* when it connects its calls
WEAK uint32_t ui_svc_feat_voice(void) { return 0; }
WEAK void ui_svc_boot_text(char line1[17], char line2[17]) { line1[0] = line2[0] = '\0'; }
WEAK uint32_t ui_svc_feat_data(void) { return 0; }
WEAK uint32_t ui_svc_feat_storage(void) { return 0; }
WEAK uint32_t ui_svc_feat_radio(void) { return 0; }
WEAK uint32_t ui_svc_feat_ext(void) { return 0; }
WEAK uint32_t ui_svc_features(void)
{
	return ui_svc_feat_voice() | ui_svc_feat_data() | ui_svc_feat_storage() | ui_svc_feat_radio() |
		ui_svc_feat_ext();
}

WEAK bool ui_svc_talk_to(uint32_t id, bool private_call, uint8_t life) { (void)id; (void)private_call; (void)life; return false; }
WEAK void ui_svc_focus_target(uint32_t id, bool private_call) { (void)id; (void)private_call; }

WEAK bool ui_svc_dmr_timeslot(uint8_t ts) { (void)ts; return false; }
WEAK bool ui_svc_dmr_color_code(uint8_t cc) { (void)cc; return false; }
WEAK bool ui_svc_mode(uint8_t mode) { (void)mode; return false; }
WEAK uint8_t ui_svc_vfo_copy(const radio_view_t *v) { (void)v; return VFO_COPY_NO_VFO; }
WEAK bool ui_svc_hear_all(bool on) { (void)on; return false; }
WEAK bool ui_svc_privacy(bool on) { (void)on; return false; }
WEAK bool ui_svc_monitor(bool on) { (void)on; return false; }
WEAK bool ui_svc_vox(bool on) { (void)on; return false; }
WEAK bool ui_svc_save_channel(void) { return false; }
WEAK bool ui_svc_write_channel(uint16_t number, const channel_t *ch) { (void)number; (void)ch; return false; }
WEAK bool ui_svc_save_talk_target(uint32_t id, bool private_call) { (void)id; (void)private_call; return false; }

WEAK bool ui_svc_scan(bool on) { (void)on; return false; }
WEAK bool ui_svc_roam(uint8_t cmd) { (void)cmd; return false; }
WEAK bool ui_svc_scan_skip(void) { return false; }
WEAK bool ui_svc_dual(const ui_dual_ref_t ref[2], uint8_t sel) { (void)ref; (void)sel; return false; }
WEAK bool ui_svc_vfo_get(channel_t *out) { (void)out; return false; }
WEAK uint8_t ui_svc_receivers(void) { return 1; }
WEAK bool ui_svc_dual_rx(uint8_t pane, ui_dual_rx_t *out) { (void)pane; (void)out; return false; }
WEAK void ui_svc_beep(uint8_t beep) { (void)beep; }
WEAK void ui_svc_vibrate(uint8_t vibe) { (void)vibe; }
WEAK void ui_svc_ptt_block(bool blocked) { (void)blocked; }
WEAK void ui_svc_poll(void) { }
WEAK void ui_svc_keys_poll(void) { }
WEAK void ui_svc_apply(uint8_t id, int32_t value) { (void)id; (void)value; }

WEAK uint16_t ui_svc_msg_count(void) { return 0; }
WEAK uint16_t ui_svc_msg_unread(void) { return 0; }
WEAK bool ui_svc_msg_get(uint16_t i, ui_msg_t *out) { (void)i; (void)out; return false; }
WEAK void ui_svc_msg_read(uint16_t i) { (void)i; }
WEAK bool ui_svc_msg_delete(uint16_t i) { (void)i; return false; }
WEAK bool ui_svc_msg_send(uint32_t dst, bool group, const char *text) { (void)dst; (void)group; (void)text; return false; }
WEAK bool ui_svc_radio_check(uint32_t id) { (void)id; return false; }
WEAK bool ui_svc_call_alert(uint32_t id) { (void)id; return false; }

WEAK bool ui_svc_position(ui_position_t *out) { (void)out; return false; }
WEAK bool ui_svc_emergency(bool on) { (void)on; return false; }
WEAK void ui_svc_emergency_dismiss(void) { }
WEAK bool ui_svc_emergency_active(void) { return false; }
WEAK bool ui_svc_factory_reset(void) { return false; }

WEAK bool ui_svc_crash_get(ui_crash_t *out) { (void)out; return false; }
WEAK void ui_svc_crash_seen(void) { }
WEAK void ui_svc_crash_clear(void) { }
WEAK bool ui_svc_crash_send(void) { return false; }
WEAK uint8_t ui_svc_crash_tx_state(void) { return UI_CRASH_TX_IDLE; }

// Safe defaults (P7): no auto-transmit feature on, tones on, hints on
static int32_t settings[UI_SET_COUNT] = {
	[UI_SET_KEY_TONES] = 1, [UI_SET_TONE_VOLUME] = 5, [UI_SET_TALK_PERMIT] = 1, [UI_SET_MESSAGE_TONE] = 1,
	[UI_SET_BRIGHTNESS] = 100, [UI_SET_BACKLIGHT_S] = 30, [UI_SET_HINTS] = 1,
	[UI_SET_REPLY_S] = 10, [UI_SET_TOT_S] = 180, [UI_SET_SQUELCH] = 5, [UI_SET_MIC_GAIN] = 5, [UI_SET_ALIAS_SHOW] = 1,
	[UI_SET_PTT_LOCKED] = 1, [UI_SET_LOW_BATTERY] = 15, [UI_SET_LONG_PRESS_MS] = 600, [UI_SET_REPEAT_MS] = 120,
	[UI_SET_ROAM_SURVEY] = 1, [UI_SET_ROAM_BEACON_S] = 60, [UI_SET_ROAM_BEACON_LEN] = 4, [UI_SET_TFTP_WRITE] = 1,
	[UI_SET_MSG_FORMAT] = 2,	// ETSI IP text
	[UI_SET_VIBRATE] = 1, [UI_SET_VOLUME] = RADIO_VOLUME_DEFAULT,
};
// (ui_svc_storage.c keeps the ones settings_t has fields for: these are the defaults of the rest, and the UI's
// stand-ins in a build without the store)

int32_t ui_svc_ram_get(uint8_t id) { return id < UI_SET_COUNT ? settings[id] : 0; }

bool ui_svc_ram_set(uint8_t id, int32_t value)
{
	if (id >= UI_SET_COUNT)
		return false;
	settings[id] = value;
	return true;
}

WEAK int32_t ui_svc_setting_get(uint8_t id) { return ui_svc_ram_get(id); }
WEAK bool ui_svc_setting_set(uint8_t id, int32_t value) { return ui_svc_ram_set(id, value); }
