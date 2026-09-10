// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine: channel / zone / VFO selection, software squelch, PTT checks and the TX timer,
// and the everyday features around them (docs/spec/radio-features.md): scan, busy channel lockout, roger beep and
// 1750 Hz burst, VOX, beeps, the battery monitor and receiver battery saving.
// No RTOS here: radio_task.c serializes the calls, and the host tests drive it directly.
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/tones.h"
#include "settings/settings.h"
#include "config.h"
#if CONFIG_DMR
#include "dmr/call/dmr_call.h"
#include "dmr/ambe/ambeVoice.h"
#include "codeplug/dmr_iddb.h"
#endif
#if CONFIG_DMR_PRIVACY
#include "dmr/dmrPrivacy.h"
#endif
#include "radio/beep.h"	// beep_t (beep_play only with CONFIG_BEEP)
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"	// dmr_sms_format_t
#endif
#if CONFIG_DMR_EMERGENCY
#include "dmr/dmr_emergency.h"
#endif
#if CONFIG_HOTSPOT
#include "radio/radio_hotspot.h"	// the CW ID handover
#endif
#if CONFIG_DMR_IP_GATEWAY
#include "hostlink/hostlink_net_air.h"	// the IP gateway's air side (the data service's packet hook)
#endif
#if CONFIG_APRS
#include "aprs/afsk.h"			// AFSK_BAUD, the tones
#endif

// The radio's own DMR ID when the codeplug's general settings hold none (build value; 0 = refuse DMR transmit)
#ifndef RADIO_DMR_ID_DEFAULT
#define RADIO_DMR_ID_DEFAULT	0
#endif

// Simplex frequencies tried, in order, for the VFO when there is nothing better to start from
// The VFO is FM only for now; a DMR VFO (CONFIG_VFO_DMR) will join this when it exists
#define VFO_AVAILABLE	CONFIG_VFO_FM

static const uint32_t vfo_start_hz[] = { 146520000, 145500000, 446000000, 433500000 };

static void changed(radio_sm_t *sm)
{
	sm->serial++;
}

void radio_settings_default(radio_settings_t *s)
{
	memset(s, 0, sizeof(*s));
	s->tot_s = RADIO_TOT_DEFAULT_S;
	s->tot_warn_s = 10;
	s->bcl = RADIO_BCL_OFF;
	s->scan_resume = SCAN_RESUME_CARRIER;
	s->scan_hold_s = RADIO_SCAN_HOLD_S;
	s->scan_delay_ms = RADIO_SCAN_DELAY_MS;
	s->scan_dwell_ms = RADIO_SCAN_DWELL_MS;
	s->beep_level = 50;
	s->key_beep = true;
	s->vox = RADIO_VOX_OFF;
	s->vox_level = 5;
	s->vox_tail_ms = 1500;
	s->rx_save = 0;		// off until the board's receiver sleep is verified on the bench
	s->rx_save_idle_s = 10;
	s->battery_off_s = 0;
	s->backlight_dim_s = 30;
	s->backlight_off_s = 120;
	s->reply_s = RADIO_REPLY_CODEPLUG;
	s->roam_rssi_dbm = -105;	// ROAM_RSSI_DEFAULT
	s->roam_hyst_db = 6;		// ROAM_HYST_DEFAULT
	s->roam_probe = 0;		// passive: no automatic transmission (P7)
	s->roam_check_s = 0;
}

static void activity(radio_sm_t *sm, uint32_t now_ms)
{
	sm->activity_ms = now_ms;
}

// A beep at the settings' level: mixed into the DMR voice stream when it runs, else on its own stream
static void beep(radio_sm_t *sm, uint8_t b)
{
#if CONFIG_BEEP
	bool shared = false;
#if CONFIG_DMR
	shared = ambeVoiceStreaming();
#endif
	beep_play((beep_t)b, sm->set.beep_level, shared);
#else
	(void)sm;
	(void)b;
#endif
}

// The TX time-out in effect (FM and DMR): the channel's own from the codeplug, or the radio's (settings, 180 s by
// default) when the channel's is 0 (builtin channels and the VFO too)
static void apply_tot(radio_sm_t *sm)
{
	uint16_t t = sm->view.ch.tot_s;
	sm->view.tot_s = t != 0 ? t : sm->set.tot_s;
}

static bool in_band(uint32_t hz, bool need_tx)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		if (hz >= b->lo_hz && hz <= b->hi_hz && (b->tx || !need_tx)) {
			return true;
		}
	}
	return false;
}

static uint8_t master_power = SETTINGS_UNSET8;	// the settings' power for "default" channels

static uint8_t default_power(void)
{
	if (board.power_count == 0) {
		return 0;
	}
	if (master_power < board.power_count) {
		return master_power;
	}
	return board.power_default < board.power_count ? board.power_default : board.power_count - 1;
}

static uint8_t channel_power(const channel_t *ch)
{
	if (ch->power == CODEPLUG_POWER_DEFAULT || ch->power >= board.power_count) {
		return default_power();
	}
	return ch->power;
}

uint8_t radio_sm_squelch_open_below(uint8_t level)
{
	if (level == 0) {
		return 255;
	}
	if (level > RADIO_SQUELCH_MAX) {
		level = RADIO_SQUELCH_MAX;
	}
	return (uint8_t)(RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP);
}

static bool same_tone(const subtone_t *a, const subtone_t *b)
{
	return a->type == b->type && (a->type == TONE_NONE || a->value == b->value);
}

// Recompute the temporary flags. Bandwidth and tones count against the channel's own values; in the VFO they are
// the VFO's settings, never temporary
static void set_temporary(radio_sm_t *sm)
{
	uint8_t t = 0;
	if (sm->temp_power) {
		t |= RADIO_TEMP_POWER;
	}
	if (sm->temp_squelch) {
		t |= RADIO_TEMP_SQUELCH;
	}
	if (!sm->view.vfo) {
		if (sm->view.ch.bw != sm->base_bw) {
			t |= RADIO_TEMP_BW;
		}
		if (!same_tone(&sm->view.ch.tx_tone, &sm->base_tx_tone)) {
			t |= RADIO_TEMP_TX_TONE;
		}
		if (!same_tone(&sm->view.ch.rx_tone, &sm->base_rx_tone)) {
			t |= RADIO_TEMP_RX_TONE;
		}
		if (sm->view.ch.mode != sm->base_mode) {
			t |= RADIO_TEMP_MODE;
		}
		if (sm->view.ch.mode == RADIO_MODE_DMR) {
			if (sm->view.ch.color_code != sm->base_cc) {
				t |= RADIO_TEMP_CC;
			}
			if (sm->temp_ts != 0) {
				t |= RADIO_TEMP_TS;
			}
		}
	}
	sm->view.temp = t;
	sm->view.temporary = t != 0;
}

// Back to the channel's own settings. Returns whether the receiver needs retuning (bandwidth, RX tone, mode, color
// code or timeslot changed: tune() also reconfigures the DMR call stack)
static bool clear_temporary(radio_sm_t *sm)
{
	bool retune = (sm->view.temp & (RADIO_TEMP_BW | RADIO_TEMP_RX_TONE | RADIO_TEMP_MODE | RADIO_TEMP_CC |
					RADIO_TEMP_TS)) != 0;
	sm->temp_power = false;
	sm->temp_squelch = false;
	sm->temp_ts = 0;
	sm->view.power = channel_power(&sm->view.ch);
	sm->view.squelch = sm->base_squelch;
	if (!sm->view.vfo) {
		sm->view.ch.bw = sm->base_bw;
		sm->view.ch.rx_tone = sm->base_rx_tone;
		sm->view.ch.tx_tone = sm->base_tx_tone;
		sm->view.ch.mode = sm->base_mode;
		sm->view.ch.color_code = sm->base_cc;
		sm->view.ch.timeslot = sm->base_ts;
	}
	set_temporary(sm);
	return retune;
}

// RX tone squelch: whether the tone counts as heard, debounced. Always heard when the channel sets no RX tone
// (DMR channels never use one)
static bool tone_needed(const radio_sm_t *sm)
{
	return sm->view.ch.rx_tone.type != TONE_NONE && sm->view.ch.mode == RADIO_MODE_FM;
}

static void follow_tone(radio_sm_t *sm, bool raw)
{
	bool needed = tone_needed(sm);
	if (!needed) {
		raw = true;
	}
	if (raw == sm->view.tone_ok) {
		sm->tone_count = 0;
		return;
	}
	sm->tone_count++;
	if (!needed || sm->tone_count >= (raw ? RADIO_TONE_ON_POLLS : RADIO_TONE_OFF_POLLS)) {
		sm->view.tone_ok = raw;
		sm->tone_count = 0;
		changed(sm);
	}
}

// After a retune, or with a new RX tone: nothing heard yet (or no tone needed)
static void reset_tone(radio_sm_t *sm)
{
	sm->view.tone_ok = !tone_needed(sm);
	sm->tone_count = 0;
}

// Speaker and LEDs follow the state; only changes go to the board
// FM: the board's speaker path follows the squelch (or the monitor). DMR voice goes out as PCM from the vocoder
// (core/dmr/ambe routes the speaker itself); the green LED shows a call being played
static void outputs(radio_sm_t *sm)
{
	bool tx = sm->view.state == RS_TX;
	bool fm = sm->view.ch.mode == RADIO_MODE_FM && !sm->hotspot;
	bool open = !tx && fm && (sm->view.monitor || sm->view.state == RS_RX);
	bool green = open;
#if CONFIG_DMR
	if (!fm && !tx && sm->view.state == RS_RX && sm->view.dmr.state == RDMR_RX) {
		green = true;
	}
	if (sm->hotspot && board.dmr != NULL && board.dmr->is_transmitting()) {
		tx = true;
	}
#endif
#if CONFIG_HOTSPOT
	if (sm->cw_phase != 0) {
		tx = true;	// the hotspot's CW ID
	}
#endif

	if (sm->audio_out != (int8_t)open) {
		sm->audio_out = (int8_t)open;
		board.radio->audio(open);
	}
	if (sm->green_out != (int8_t)green) {
		sm->green_out = (int8_t)green;
		board.power->led(LED_GREEN, green);
	}
	if (sm->red_out != (int8_t)tx) {
		sm->red_out = (int8_t)tx;
		board.power->led(LED_RED, tx);
	}
}

static void end_reception(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->view.state == RS_RX) {
		sm->view.state = RS_IDLE;
		sm->view.last_rx_ms = now_ms != 0 ? now_ms : 1;
	}
	sm->close_count = 0;
}

// Receive on what view.ch says now
static void dmr_begin(radio_sm_t *sm);
static void dmr_end(radio_sm_t *sm);

static void tune(radio_sm_t *sm, uint32_t now_ms)
{
	end_reception(sm, now_ms);
	reset_tone(sm);
#if CONFIG_BATTERY_SAVE
	sm->save_on = sm->save_asleep = false;	// rx() below wakes the receiver
	sm->view.rx_sleeping = false;
#endif
	radio_mode_t mode = (CONFIG_DMR && sm->view.ch.mode == RADIO_MODE_DMR) ? RADIO_MODE_DMR : RADIO_MODE_FM;
	if (sm->board_mode == RADIO_MODE_DMR && mode != RADIO_MODE_DMR) {
		dmr_end(sm);		// the call stack lets go of the modem before the chip leaves DMR
	}
	if (mode != sm->board_mode) {
		sm->board_mode = mode;
		board.radio->set_mode(mode);
	}
	board.radio->rx(sm->view.ch.rx_hz, (radio_bw_t)sm->view.ch.bw, &sm->view.ch.rx_tone);
	if (mode == RADIO_MODE_DMR) {
		dmr_begin(sm);		// after set_mode(DMR): the modem only starts in DMR mode
	}
	outputs(sm);
	changed(sm);
}

// From position start in z, the first readable channel going in dir (wraps once round the zone)
static bool find_channel(const zone_t *z, int start, int dir, uint16_t *pos, channel_t *ch)
{
	int n = z->count;
	for (int i = 0; i < n; i++) {
		int p = ((start + dir * i) % n + n) % n;
		if (codeplug_channel(z->channels[p], ch)) {
			*pos = (uint16_t)p;
			return true;
		}
	}
	return false;
}

static void set_zone(radio_sm_t *sm, uint16_t index, const zone_t *z)
{
	if (z != &sm->zone) {
		sm->zone = *z;
	}
	sm->zone.name[CODEPLUG_NAME_LEN] = '\0';
	sm->view.zone_index = index;
	sm->view.zone_size = sm->zone.count;
	memcpy(sm->view.zone_name, sm->zone.name, sizeof(sm->view.zone_name));
}

// A channel change: the channel's own settings, temporary changes dropped (P7)
static void set_channel(radio_sm_t *sm, uint16_t pos, const channel_t *ch)
{
	sm->view.ch = *ch;
	sm->view.ch.name[CODEPLUG_NAME_LEN] = '\0';
#if !CONFIG_SUBTONE
	sm->view.ch.rx_tone = sm->view.ch.tx_tone = (subtone_t){ TONE_NONE, 0 };	// built without CTCSS / DCS
#endif
	sm->view.vfo = false;
	sm->view.zone_pos = pos;
	sm->view.channel_number = sm->zone.channels[pos];
	sm->base_bw = sm->view.ch.bw;
	sm->base_rx_tone = sm->view.ch.rx_tone;
	sm->base_tx_tone = sm->view.ch.tx_tone;
	sm->base_mode = sm->view.ch.mode;
	sm->base_cc = sm->view.ch.color_code;
	sm->base_ts = sm->view.ch.timeslot;
	clear_temporary(sm);
	apply_tot(sm);
}

static bool read_zone(uint16_t index, zone_t *z)
{
	if (!codeplug_zone(index, z)) {
		return false;
	}
	if (z->count > CODEPLUG_ZONE_MAX) {
		z->count = CODEPLUG_ZONE_MAX;
	}
	return z->count > 0;
}

static uint32_t vfo_default_hz(void)
{
	for (unsigned i = 0; i < sizeof(vfo_start_hz) / sizeof(vfo_start_hz[0]); i++) {
		if (in_band(vfo_start_hz[i], true)) {
			return vfo_start_hz[i];
		}
	}
	for (uint8_t i = 0; i < board.band_count; i++) {
		if (board.bands[i].tx) {
			uint32_t mid = board.bands[i].lo_hz + (board.bands[i].hi_hz - board.bands[i].lo_hz) / 2;
			return mid - mid % RADIO_VFO_STEP_HZ;
		}
	}
	return board.band_count > 0 ? board.bands[0].lo_hz : 0;
}

static void vfo_set_hz(radio_sm_t *sm, uint32_t hz)
{
	sm->vfo_ch.rx_hz = hz;
	sm->vfo_ch.tx_hz = hz;
	sm->vfo_seeded = true;
	if (sm->view.vfo) {
		sm->view.ch.rx_hz = hz;
		sm->view.ch.tx_hz = hz;
	}
}

static void enter_vfo(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->vfo_seeded) {
		uint32_t hz = (!sm->view.vfo && in_band(sm->view.ch.rx_hz, false)) ? sm->view.ch.rx_hz : vfo_default_hz();
		vfo_set_hz(sm, hz);
	}
	sm->view.ch = sm->vfo_ch;
	sm->view.vfo = true;
	sm->view.channel_number = 0;
	clear_temporary(sm);
	apply_tot(sm);
	tune(sm, now_ms);
}

static void leave_vfo(radio_sm_t *sm, uint32_t now_ms)
{
	uint16_t pos;
	channel_t ch;
	if (!sm->have_channels || !find_channel(&sm->zone, sm->view.zone_pos, 1, &pos, &ch)) {
		return;
	}
	set_channel(sm, pos, &ch);
	tune(sm, now_ms);
}

// Next band edge past hz in dir, wrapping round the board's bands
static uint32_t band_wrap(uint32_t hz, int dir)
{
	bool found = false;
	uint32_t best = 0;
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		if (dir > 0 && b->lo_hz > hz && (!found || b->lo_hz < best)) {
			best = b->lo_hz;
			found = true;
		} else if (dir < 0 && b->hi_hz < hz && (!found || b->hi_hz > best)) {
			best = b->hi_hz;
			found = true;
		}
	}
	if (found) {
		return best;
	}
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		uint32_t edge = dir > 0 ? b->lo_hz : b->hi_hz;
		if (!found || (dir > 0 ? edge < best : edge > best)) {
			best = edge;
			found = true;
		}
	}
	return best;
}

static void vfo_step(radio_sm_t *sm, int dir, uint32_t now_ms)
{
	int64_t hz = (int64_t)sm->view.ch.rx_hz + (int64_t)dir * sm->view.vfo_step_hz;
	uint32_t next;
	if (hz > 0 && hz <= (int64_t)UINT32_MAX && in_band((uint32_t)hz, false)) {
		next = (uint32_t)hz;
	} else {
		next = band_wrap(sm->view.ch.rx_hz, dir);
	}
	vfo_set_hz(sm, next);
	tune(sm, now_ms);
}

static void channel_step(radio_sm_t *sm, int dir, uint32_t now_ms)
{
	uint16_t pos;
	channel_t ch;
	if (!find_channel(&sm->zone, sm->view.zone_pos + dir, dir, &pos, &ch)) {
		return;
	}
	set_channel(sm, pos, &ch);
	tune(sm, now_ms);
}

// Something on the channel by the noise reading, at the squelch level (the default level when the squelch is open)
static bool carrier_heard(const radio_sm_t *sm, uint8_t noise)
{
	uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
	return noise < radio_sm_squelch_open_below(sq);
}

// Busy channel lockout (FM): refuse while the channel is in use (by anyone, or by a group with another tone)
static bool bcl_busy(const radio_sm_t *sm)
{
	if (sm->view.ch.mode != RADIO_MODE_FM || sm->set.bcl == RADIO_BCL_OFF) {
		return false;
	}
	bool carrier = sm->view.state == RS_RX || carrier_heard(sm, sm->view.noise);
	if (sm->set.bcl == RADIO_BCL_CARRIER) {
		return carrier;
	}
	return carrier && tone_needed(sm) && !sm->view.tone_ok;
}

static uint8_t tx_check(const radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	if (sm->set.tx_inhibit) {
		return TXDENY_INHIBIT;
	}
	if (ch->tx_hz == 0) {
		return TXDENY_NO_FREQ;
	}
	if (ch->rx_only) {
		return TXDENY_RX_ONLY;
	}
	if (ch->mode == RADIO_MODE_DMR ? !CONFIG_DMR : !CONFIG_FM) {
		return TXDENY_MODE;
	}
	if (!in_band(ch->tx_hz, true)) {
		return TXDENY_BAND;
	}
	if (sm->batt.level == BATT_CRITICAL) {
		return TXDENY_BATTERY;
	}
	if (bcl_busy(sm)) {
		return TXDENY_BUSY;
	}
	return TXDENY_NONE;
}

static void deny(radio_sm_t *sm, uint8_t reason)
{
	sm->view.state = RS_TX_DENIED;
	sm->view.tx_deny = reason;
	sm->view.tot_warning = false;
	outputs(sm);
	changed(sm);
	beep(sm, BEEP_TX_DENY);
}

// The TX timer in the view, and the warning tot_warn_s before the time-out (FM and DMR)
static void tx_timer(radio_sm_t *sm, uint32_t now_ms)
{
	uint32_t elapsed = now_ms - sm->tx_start_ms;
	if (elapsed / 1000 != sm->view.tx_elapsed_ms / 1000) {
		changed(sm);
	}
	sm->view.tx_elapsed_ms = elapsed;
	uint16_t tot = sm->view.tot_s, warn = sm->set.tot_warn_s;
	if (tot != 0 && warn != 0 && tot > warn && !sm->tot_warned && elapsed >= (uint32_t)(tot - warn) * 1000u) {
		sm->tot_warned = true;
		sm->view.tot_warning = true;
		changed(sm);
		beep(sm, BEEP_TOT_WARN);
	}
}

// --- DMR: the call stack on DMR channels (core/dmr/call), voice through core/dmr/ambe ---
#if CONFIG_DMR
static void add_name(radio_sm_t *sm, const contact_t *c)
{
	if (sm->name_count >= RADIO_NAMES_MAX || c->name[0] == '\0') {
		return;
	}
	radio_name_t *n = &sm->names[sm->name_count++];
	n->id = c->id;
	n->group = c->type != CONTACT_PRIVATE;
	memcpy(n->name, c->name, sizeof(n->name));
}

// A name for an ID: the channel's contacts (talk target, listen list), then the lookup cache, then every codeplug
// contact and, for a private ID, the DMR ID database (both read from flash once per ID, the answer cached, "" too)
static void find_name(radio_sm_t *sm, uint32_t id, bool group, char *out)
{
	out[0] = '\0';
	if (id == 0) {
		return;
	}
	for (uint8_t i = 0; i < sm->name_count; i++) {
		if (sm->names[i].id == id && sm->names[i].group == group) {
			memcpy(out, sm->names[i].name, CODEPLUG_NAME_LEN + 1);
			return;
		}
	}
	for (uint8_t i = 0; i < RADIO_LOOKUPS; i++) {
		if (sm->lookups[i].id == id && sm->lookups[i].group == group) {
			memcpy(out, sm->lookups[i].name, CODEPLUG_NAME_LEN + 1);
			return;
		}
	}
	radio_name_t *n = &sm->lookups[sm->lookup_next];
	sm->lookup_next = (uint8_t)((sm->lookup_next + 1u) % RADIO_LOOKUPS);
	n->id = id;
	n->group = group;
	n->name[0] = '\0';
	contact_t c;
	if (codeplug_contact_find(id, group, &c)) {
		memcpy(n->name, c.name, sizeof(n->name));
	} else if (!group) {
		char text[DMR_IDDB_TEXT_MAX + 1];
		if (dmr_iddb_lookup(id, text, sizeof(text))) {
			memcpy(n->name, text, CODEPLUG_NAME_LEN);
			n->name[CODEPLUG_NAME_LEN] = '\0';
		}
	}
	memcpy(out, n->name, CODEPLUG_NAME_LEN + 1);
}

// The channel's talk target and listen list from the codeplug contacts, with their names for the screens
static void dmr_load_channel(radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	contact_t c;
	sm->name_count = 0;
	sm->listen_count = 0;
	sm->talk_id = 9;		// no contact: local talkgroup 9, the usual default
	sm->talk_private = false;
	sm->talk_ts = 0;
	if (sm->manual && (sm->manual_vfo != sm->view.vfo || sm->manual_ch != sm->view.channel_number)) {
		sm->manual = false;	// a chosen target lasts as long as its channel (temporary state, docs/ux)
	}
	if (sm->hear_all_latch && (sm->latch_vfo != sm->view.vfo || sm->latch_ch != sm->view.channel_number)) {
		sm->hear_all_latch = false;	// so does the hear all latch
	}
	if (sm->one_shot && (sm->view.vfo || sm->one_shot_ch != sm->view.channel_number)) {
		sm->one_shot = false;		// and a one-shot target
	}
	if (sm->view.vfo) {
		return;
	}
	if (ch->contact != 0 && codeplug_contact(ch->contact, &c)) {
		sm->talk_id = c.id;
		sm->talk_private = c.type == CONTACT_PRIVATE;
		sm->talk_ts = c.timeslot;
		add_name(sm, &c);
	}
	static listen_list_t list;	// only the radio task uses it
	if (ch->listen_list != 0 && codeplug_listen_list(ch->listen_list, &list)) {
		for (uint8_t i = 0; i < list.count; i++) {
			if (!codeplug_contact(list.contacts[i], &c)) {
				continue;
			}
			if (c.type != CONTACT_PRIVATE && sm->listen_count < CODEPLUG_LIST_LEN) {
				sm->listen[sm->listen_count++] = c.id;
			}
			add_name(sm, &c);
		}
	}
}

#if CONFIG_DMR_PRIVACY
static dmrPrivacyKey_t privacy_key;	// the call stack's privacy hook context (radio task only)
#endif

// Hang time (ms) as the call stack's reply window (whole seconds, rounded up); 0 keeps the stack's default
static uint8_t hang_s(uint16_t ms, uint8_t dflt)
{
	if (ms == 0) {
		return dflt;
	}
	uint32_t s = (ms + 999u) / 1000u;
	return (uint8_t)(s > 30u ? 30u : s);
}

static void dmr_configure(radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	dmr_call_config_t cfg;
	dmr_call_config_default(&cfg);
	cfg.own_id = ch->dmr_id != 0 ? ch->dmr_id : sm->own_id;
	cfg.cc = ch->color_code & 0x0Fu;
	// The talk target: a one-shot target (the focused row PTT went to), else one chosen for now, else the channel's.
	// A timeslot chosen for now beats a contact's timeslot override
	uint8_t ts_override = sm->one_shot ? 0 : sm->manual ? sm->manual_ts : sm->talk_ts;
	if (sm->temp_ts != 0) {
		ts_override = sm->temp_ts;
	}
	cfg.ts = ts_override != 0 ? ts_override : (ch->timeslot == 2 ? 2 : 1);
	cfg.rmo = ch->tx_hz != 0 && ch->rx_hz != ch->tx_hz && !ch->dmo;
	cfg.talk_target.id = sm->one_shot ? sm->one_shot_id : sm->manual ? sm->manual_id : sm->talk_id;
	cfg.talk_target.private_call = sm->one_shot ? sm->one_shot_private : sm->manual ? sm->manual_private :
		sm->talk_private;
	cfg.listen.hear_all = sm->view.monitor || sm->hear_all_latch;
	cfg.listen.groups = sm->listen;
	cfg.listen.group_count = sm->listen_count;
	// The channel's TX time-out when it has one, else the radio's
	cfg.tot_s = sm->view.tot_s;	// apply_tot: the channel's, else the radio's
	if (sm->set.reply_s <= 30u) {
		cfg.reply_window_s = cfg.reply_window_private_s = sm->set.reply_s;	// the radio's setting
	} else {
		cfg.reply_window_s = hang_s(sm->group_hang_ms, cfg.reply_window_s);
		cfg.reply_window_private_s = hang_s(sm->private_hang_ms, cfg.reply_window_s);
	}
	sm->win_group_s = cfg.reply_window_s;
	sm->win_private_s = cfg.reply_window_private_s != 0 ? cfg.reply_window_private_s : cfg.reply_window_s;
	bool alias = false;
#if CONFIG_DMR_TALKER_ALIAS
	alias = sm->radio_name[0] != '\0' && !sm->view.vfo &&
		(ch->alias_tx & (cfg.ts == 2 ? CODEPLUG_ALIAS_TS2_TEXT : CODEPLUG_ALIAS_TS1_TEXT)) != 0;
	if (alias) {
		memcpy(cfg.tx_alias, sm->radio_name, sizeof(sm->radio_name));
		cfg.tx_alias_format = DMR_TA_ISO8;
	}
#endif
	bool privacy = false;
#if CONFIG_DMR_PRIVACY
	privacy = sm->privacy_type != DMR_PRIVACY_OFF;
	if (privacy) {
		privacy_key.type = (dmrPrivacyType_t)sm->privacy_type;
		privacy_key.key = sm->privacy_key;
		cfg.privacy = dmrPrivacyCallHook;
		cfg.privacy_ctx = &privacy_key;
		cfg.tx_fid = dmrPrivacyFID(privacy_key.type, cfg.tx_fid);
		cfg.tx_svc = dmrPrivacyServiceOptions(privacy_key.type, cfg.tx_svc);
	}
#endif
#if CONFIG_DMR_EMERGENCY
	// In emergency: every call carries the emergency service option, goes to the emergency target, and doesn't wait
	// for a busy channel (docs/spec/dmr-emergency.md 3)
	if (dmr_emergency_in_progress()) {
		uint32_t id;
		bool priv;
		dmr_emergency_target(&id, &priv);
		if (id != 0) {
			cfg.talk_target.id = id;
			cfg.talk_target.private_call = priv;
		}
		cfg.tx_svc |= DMR_SVC_EMERGENCY;
		cfg.access = DMR_ACCESS_IMPOLITE;
	}
	sm->em_cfg_serial = dmr_emergency_config_serial();
#endif
	dmr_call_configure(&cfg);
	sm->view.dmr.own_id = cfg.own_id;
	sm->view.dmr.hear_all = cfg.listen.hear_all;
	sm->view.dmr.hear_all_latch = sm->hear_all_latch;
	sm->view.dmr.one_shot = sm->one_shot;
	sm->view.dmr.target_manual = sm->manual;
	// The target with its flags in one update (dmr_publish copies the stack's own at the next tick): a reader never
	// sees the new manual / one-shot flag beside the old target
	sm->view.dmr.target_id = cfg.talk_target.id;
	sm->view.dmr.target_private = cfg.talk_target.private_call;
	find_name(sm, cfg.talk_target.id, !cfg.talk_target.private_call, sm->view.dmr.target_name);
	sm->view.dmr.alias_tx = alias;
	sm->view.dmr.privacy = privacy;
}

// A talk target for now (typed, a contact, a call back): reconfigures the stack, which ends the reply window
static void dmr_set_manual(radio_sm_t *sm, uint32_t id, bool private_call, uint8_t ts)
{
	if (id == 0 || id > DMR_ID_MAX || sm->view.ch.mode != RADIO_MODE_DMR) {
		return;
	}
	sm->one_shot = false;		// an explicit choice replaces a one-shot target
	sm->manual = true;
	sm->manual_id = id;
	sm->manual_private = private_call;
	sm->manual_ts = ts;
	sm->manual_ch = sm->view.channel_number;
	sm->manual_vfo = sm->view.vfo;
	if (sm->dmr_running) {
		dmr_configure(sm);
		sm->dmr_serial = dmr_call_view()->serial - 1u;	// publish the new target at the next tick
	}
	changed(sm);
}

// The stack follows a change of the channel's DMR settings (temporary color code, timeslot, hear all)
static void dmr_reconfigure(radio_sm_t *sm)
{
	if (sm->dmr_running) {
		dmr_configure(sm);
		sm->dmr_serial = dmr_call_view()->serial - 1u;	// publish at the next tick
	}
	changed(sm);
}

// A timeslot for now (RREQ_DMR_TS). The slot the radio would use without it (a contact's override, else the
// channel's) clears it again
static void dmr_set_timeslot(radio_sm_t *sm, int32_t ts, bool busy)
{
	if (busy || sm->view.vfo || sm->view.ch.mode != RADIO_MODE_DMR || (ts != 1 && ts != 2)) {
		return;
	}
	uint8_t over = sm->one_shot ? 0 : sm->manual ? sm->manual_ts : sm->talk_ts;
	uint8_t natural = over != 0 ? over : (sm->base_ts == 2 ? 2 : 1);
	uint8_t want = ts == natural ? 0 : (uint8_t)ts;
	if (want == sm->temp_ts) {
		return;
	}
	sm->temp_ts = want;
	sm->view.ch.timeslot = want != 0 ? want : sm->base_ts;
	set_temporary(sm);
	dmr_reconfigure(sm);
}

// On a DMR channel, after set_mode(DMR) and rx(): configure, and start the stack if it isn't running
static void dmr_begin(radio_sm_t *sm)
{
	dmr_load_channel(sm);
	dmr_configure(sm);
	if (!sm->dmr_running && board.dmr != NULL) {
		sm->dmr_running = dmr_call_start();
	}
	sm->view.dmr.active = sm->dmr_running;
	sm->dmr_serial = dmr_call_view()->serial - 1u;	// copy the stack's view at the next tick
}

// Leaving DMR (or into hotspot mode): drop any transmission, stop the stack and the PCM stream
static void dmr_end(radio_sm_t *sm)
{
	dmr_call_ptt(false);
	dmr_call_stop();
	ambeVoiceStop();
	if (sm->dmr_keyed) {
		sm->dmr_keyed = false;
		board.radio->tx_off();
	}
	sm->dmr_running = false;
	sm->manual = false;		// a chosen talk target ends with the channel
	sm->view.dmr.target_manual = false;
	sm->one_shot = false;
	sm->view.dmr.one_shot = false;
	sm->hear_all_latch = false;	// and so does the hear all latch
	sm->view.dmr.hear_all_latch = false;
	sm->view.dmr.hear_all = sm->view.monitor;
	memset(&sm->view.dmr.call, 0, sizeof(sm->view.dmr.call));
	sm->view.dmr.active = false;
	sm->view.dmr.state = RDMR_IDLE;
	sm->view.dmr.reply_active = false;
	sm->view.dmr.alias[0] = '\0';
}

static uint8_t dmr_deny_reason(uint8_t err)
{
	switch (err) {
	case DMR_TX_ERR_BUSY: return TXDENY_BUSY;
	case DMR_TX_ERR_NO_REPEATER: return TXDENY_NO_REPEATER;
	case DMR_TX_ERR_REJECTED: return TXDENY_REJECTED;
	case DMR_TX_ERR_TIMEOUT: return TXDENY_TIMEOUT;
	default: return TXDENY_NO_DMR;
	}
}

static bool dmr_tx_phase(uint8_t phase)
{
	return phase == DMR_CALL_PHASE_TX_ACCESS || phase == DMR_CALL_PHASE_TX_WAKEUP ||
		phase == DMR_CALL_PHASE_TX_VOICE || phase == DMR_CALL_PHASE_TX_ENDING || phase == DMR_CALL_PHASE_TX_DATA;
}

static void copy_call(radio_sm_t *sm, const dmr_caller_t *c, radio_dmr_call_t *out)
{
	out->src = c->src;
	out->dst = c->dst;
	out->group = c->group;
	out->to_us = c->to_us;
	out->encrypted = c->encrypted;
	out->emergency = c->emergency;
	out->pi_header = c->pi_header;
	find_name(sm, c->src, false, out->src_name);
	find_name(sm, c->dst, c->group, out->dst_name);
}

// The stack's view into radio_view_t.dmr
static void dmr_publish(radio_sm_t *sm)
{
	const dmr_call_view_t *cv = dmr_call_view();
	radio_dmr_view_t *d = &sm->view.dmr;
	if (cv->serial == sm->dmr_serial) {
		return;
	}
	sm->dmr_serial = cv->serial;
	switch (cv->phase) {
	case DMR_CALL_PHASE_RX_VOICE: d->state = RDMR_RX; break;
	case DMR_CALL_PHASE_RX_BUSY: d->state = RDMR_RX_OTHER; break;
	case DMR_CALL_PHASE_TX_ACCESS:
	case DMR_CALL_PHASE_TX_WAKEUP: d->state = RDMR_TX_WAIT; break;
	case DMR_CALL_PHASE_TX_VOICE: d->state = RDMR_TX; break;
	case DMR_CALL_PHASE_TX_ENDING: d->state = RDMR_TX_ENDING; break;
	case DMR_CALL_PHASE_TX_DATA: d->state = RDMR_TX_DATA; break;
	default: d->state = RDMR_IDLE; break;
	}
	d->codec = sm->codec;
	d->cc = cv->cc;
	d->ts = cv->ts;
	d->rmo = cv->rmo;
	d->repeater_up = cv->repeater_up;
	d->target_id = cv->tx_target.id;
	d->target_private = cv->tx_target.private_call;
	find_name(sm, d->target_id, !d->target_private, d->target_name);
	d->reply_active = cv->reply_active;
	d->reply_s = cv->reply_s;
	d->one_shot = sm->one_shot;
#if CONFIG_DMR_EMERGENCY
	if (d->state == RDMR_RX && cv->caller.emergency) {
		dmr_emergency_heard(DMR_EMERGENCY_ALERT_CALL, cv->caller.src, cv->caller.dst, cv->caller.group, cv->call_start_ms);
	}
#endif
	if (d->state != RDMR_IDLE) {
		copy_call(sm, &cv->caller, &d->call);
		d->late_entry = cv->caller.late_entry;
#if CONFIG_DMR_TALKER_ALIAS
		memcpy(d->alias, cv->caller.alias, sizeof(d->alias));
		d->alias[RADIO_ALIAS_MAX] = '\0';
#else
		d->alias[0] = '\0';
#endif
	}
	d->rssi_dbm = cv->rssi_dbm;
	d->call_ms = cv->call_ms;
	if (cv->last_heard_ms != 0) {
		copy_call(sm, &cv->last_heard, &d->last);
		d->last_ms = cv->last_heard_ms;
	}
	d->underruns = cv->tx_underruns;
	changed(sm);
}

// Follow the stack: release the board's TX arming once the stack is off the air (after the modem's TX done), PTT
// refusals, the RX state from the calls heard
static void dmr_follow(radio_sm_t *sm, uint32_t now_ms)
{
	const dmr_call_view_t *cv = dmr_call_view();
	bool tx = dmr_tx_phase(cv->phase);

	if (sm->dmr_keyed && !tx) {
		sm->dmr_keyed = false;
		board.radio->tx_off();
	}
	if (sm->ptt) {
		if (cv->phase == DMR_CALL_PHASE_TX_DENIED && sm->view.state != RS_TX_DENIED) {
			deny(sm, dmr_deny_reason(cv->tx_error));
#if CONFIG_DMR_ROAMING
			if (cv->tx_error == DMR_TX_ERR_NO_REPEATER) {
				roam_trigger(&sm->roam, ROAM_WHY_TX_FAIL);	// the site didn't wake: look for another
			}
#endif
		}
	} else if (sm->view.state == RS_TX && !tx) {
		sm->view.state = RS_IDLE;
		sm->view.tx_deny = TXDENY_NONE;
		changed(sm);
	}
	dmr_publish(sm);
	if (sm->view.state != RS_TX && sm->view.state != RS_TX_DENIED) {
		bool rx = cv->state == DMR_CALL_STATE_RX;
		if (rx && sm->view.state != RS_RX) {
			sm->view.state = RS_RX;
			changed(sm);
		} else if (!rx && sm->view.state == RS_RX) {
			end_reception(sm, now_ms);
			changed(sm);
		}
	}
	if (sm->view.state == RS_TX) {
		tx_timer(sm, now_ms);
	}
	outputs(sm);

	// Talk permit when the voice goes on air, call end when a call we played ends (not on no-beep channels)
	uint8_t st = sm->view.dmr.state;
	if (st != sm->dmr_prev_state) {
		if (!sm->view.ch.no_beep) {
			if (st == RDMR_TX) {
				beep(sm, BEEP_TX_PERMIT);
			} else if (sm->dmr_prev_state == RDMR_RX && (st == RDMR_IDLE || st == RDMR_RX_OTHER)) {
				beep(sm, BEEP_CALL_END);
			}
		}
		sm->dmr_prev_state = st;
	}
}

// --- One-shot talk target: PTT while a list row is focused talks to that row's target once, and keeps it as the
// talk target for the reply window after the call (information-architecture.md 1). The stack sees it as the talk
// target, so replies on that talkgroup are heard; radio_sm_tick ends it ---

static bool same_dmr_target(uint32_t id_a, bool priv_a, uint32_t id_b, bool priv_b)
{
	return id_a == id_b && priv_a == priv_b;
}

// At PTT down: the focused target, if it isn't where PTT goes already
static void one_shot_start(radio_sm_t *sm)
{
	if (sm->focus_id == 0 || !sm->dmr_running) {
		if (sm->one_shot) {
			sm->one_shot_until = 0;	// PTT again inside the window: it runs again after this call
		}
		return;
	}
	const dmr_call_view_t *cv = dmr_call_view();
	if (same_dmr_target(cv->tx_target.id, cv->tx_target.private_call, sm->focus_id, sm->focus_private)) {
		if (sm->one_shot) {
			sm->one_shot_until = 0;
		}
		return;
	}
	sm->one_shot = true;
	sm->one_shot_id = sm->focus_id;
	sm->one_shot_private = sm->focus_private;
	sm->one_shot_ch = sm->view.channel_number;
	sm->one_shot_until = 0;
	dmr_configure(sm);
	sm->dmr_serial = cv->serial - 1u;	// publish the new target at the next tick
}

static void one_shot_end(radio_sm_t *sm)
{
	if (!sm->one_shot) {
		return;
	}
	sm->one_shot = false;
	sm->view.dmr.one_shot = false;
	if (sm->dmr_running) {
		dmr_configure(sm);
		sm->dmr_serial = dmr_call_view()->serial - 1u;
	}
	changed(sm);
}

// Every tick: the window runs from the end of our call (or of the target's answer), then the target goes back
static void one_shot_follow(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->one_shot) {
		return;
	}
	radio_dmr_view_t *d = &sm->view.dmr;
	const radio_dmr_call_t *c = &d->call;
	bool theirs = sm->view.state == RS_RX && d->state == RDMR_RX &&
		(sm->one_shot_private ? c->src == sm->one_shot_id : (c->group && c->dst == sm->one_shot_id));
	if (sm->ptt || sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED || theirs) {
		sm->one_shot_until = 0;
		return;
	}
	const dmr_call_view_t *cv = dmr_call_view();
	if (sm->one_shot_until == 0) {
		uint8_t w = sm->one_shot_private ? sm->win_private_s : sm->win_group_s;
		sm->one_shot_until = now_ms + (uint32_t)w * 1000u;
		if (sm->one_shot_until == 0) {
			sm->one_shot_until = 1;
		}
	}
	int32_t left = (int32_t)(sm->one_shot_until - now_ms);
	if (left <= 0) {
		if (!cv->reply_active) {	// someone else's reply window runs: the target comes back after it
			one_shot_end(sm);
		}
		return;
	}
	if (!cv->reply_active) {
		uint8_t s = (uint8_t)((left + 999) / 1000);
		if (!d->reply_active || d->reply_s != s) {
			d->reply_active = true;
			d->reply_s = s;
			changed(sm);
		}
	}
}

// PTT on a DMR channel (band, RX-only and frequency checks passed): arm the board's per-burst keying, then the stack
// decides when the bursts go out
static void dmr_ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	const channel_t *ch = &sm->view.ch;
	if (!sm->codec) {
		deny(sm, TXDENY_NO_CODEC);
		return;
	}
	if (!sm->dmr_running) {
		deny(sm, TXDENY_NO_DMR);
		return;
	}
	if ((ch->dmr_id != 0 ? ch->dmr_id : sm->own_id) == 0) {
		deny(sm, TXDENY_NO_ID);
		return;
	}
	if (!sm->dmr_keyed) {
		static const subtone_t none = { TONE_NONE, 0 };
		if (!board.radio->tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &none)) {
			deny(sm, TXDENY_BAND);
			return;
		}
		sm->dmr_keyed = true;
	}
	one_shot_start(sm);
	sm->view.state = RS_TX;
	sm->view.tx_deny = TXDENY_NONE;
	sm->view.tx_elapsed_ms = 0;
	sm->tx_start_ms = now_ms;
	changed(sm);
	dmr_call_ptt(true);
	dmr_call_poll(now_ms);
	dmr_follow(sm, now_ms);
}

#if CONFIG_DMR_DATA_CORE
// --- DMR data: the data service (core/dmr/dmrDataService.c) on the call stack. The service decides what to send and
// when (listen before talk, retries); this side checks the channel allows it, arms the board's per-burst keying as
// PTT does, and lends the list to the stack. dmr_follow releases the keying once the stack is off the air ---

static struct {
	const dmrBurst_t *list;		// the service's buffer, valid until dmrDataServiceTxDone
	int count, next;
} data_tx;

static bool data_next(dmr_tx_burst_t *out, void *ctx)
{
	(void)ctx;
	if (data_tx.list == NULL || data_tx.next >= data_tx.count) {
		return false;
	}
	const dmrBurst_t *d = &data_tx.list[data_tx.next++];
	memset(out, 0, sizeof(*out));
	out->kind = d->dataType;
	out->host_crc = (d->flags & DMR_BURST_FLAG_MCU_CRC) != 0;	// CRC_MCU_Control (gotcha 5.1)
	out->sync_override = DMR_SYNC_NONE;
	out->len = d->length <= DMR_PAYLOAD_MAX ? d->length : DMR_PAYLOAD_MAX;
	memcpy(out->payload, d->payload, out->len);
	return true;
}

static void data_done(uint8_t err, void *ctx)
{
#if CONFIG_DMR_ROAMING
	radio_sm_t *sm = ctx;
	if (err == DMR_TX_ERR_NO_REPEATER && sm != NULL) {
		roam_trigger(&sm->roam, ROAM_WHY_TX_FAIL);	// the site didn't wake for our data either
	}
#else
	(void)ctx;
#endif
	data_tx.list = NULL;
	dmrDataServiceTxDone(err);
}

// Every data / control burst the stack heard on our slot
static void data_rx(const dmr_burst_t *b, uint32_t now_ms, void *ctx)
{
	(void)ctx;
	dmrBurst_t d;
	d.dataType = b->kind;
	d.length = b->len <= DMR_BURST_PAYLOAD_MAX ? b->len : DMR_BURST_PAYLOAD_MAX;
	d.flags = b->crc_ok ? 0 : DMR_BURST_FLAG_RX_CRC_ERROR;
	memcpy(d.payload, b->payload, d.length);
	dmrDataServiceRxBurst(&d, now_ms);
}

static bool data_can_send(const radio_sm_t *sm)
{
	bool roaming = false;
#if CONFIG_DMR_ROAMING
	roaming = roam_searching(&sm->roam);	// never on a site being looked at
#endif
	return sm->dmr_running && !sm->hotspot && sm->view.ch.mode == RADIO_MODE_DMR && !sm->view.vfo &&
		tx_check(sm) == TXDENY_NONE && sm->view.dmr.own_id != 0 && !roaming;
}

static bool data_tx_start(const dmrBurst_t *bursts, int count, void *ctx)
{
	radio_sm_t *sm = ctx;
	if (!data_can_send(sm) || sm->ptt || count <= 0) {
		return false;
	}
	if (!sm->dmr_keyed) {
		static const subtone_t none = { TONE_NONE, 0 };
		const channel_t *ch = &sm->view.ch;
		if (!board.radio->tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &none)) {
			return false;
		}
		sm->dmr_keyed = true;
	}
	data_tx.list = bursts;
	data_tx.count = count;
	data_tx.next = 0;
	dmr_data_source_t src = { .next = data_next, .done = data_done, .ctx = sm,
				  .count = (uint8_t)(count > 255 ? 255 : count) };
	if (!dmr_call_data_tx(&src)) {
		data_tx.list = NULL;
		return false;	// the keying is released by dmr_follow (the stack isn't transmitting)
	}
	return true;
}

static bool data_channel_clear(uint32_t now_ms, void *ctx)
{
	(void)ctx;
	return dmr_call_channel_clear(now_ms);
}

static bool data_in_rx_group(uint32_t tg, void *ctx)
{
	const radio_sm_t *sm = ctx;
	for (uint16_t i = 0; i < sm->listen_count; i++) {
		if (sm->listen[i] == tg) {
			return true;
		}
	}
	return false;
}

// The data service's settings as last saved: a change (dmr_data_set_settings, applied by the service's tick) is
// noted in the settings store from data_tick
static dmrDataSettings_t data_saved;

static void data_save_settings(uint32_t now_ms)
{
	dmrDataSettings_t cur;
	dmrDataServiceGetSettings(&cur);
	if (cur.arsServer == data_saved.arsServer && cur.radioCheckReply == data_saved.radioCheckReply &&
	    cur.callAlertAck == data_saved.callAlertAck && cur.lrrpReply == data_saved.lrrpReply &&
	    cur.smsFormat == data_saved.smsFormat && cur.radioCheckEtsi == data_saved.radioCheckEtsi) {
		return;
	}
	data_saved = cur;
	settings_t *st = settings_begin();
	st->ds_ars_server = cur.arsServer;
	st->ds_flags = (uint8_t)((cur.radioCheckReply ? SETTINGS_DS_RADIO_CHECK_REPLY : 0) |
				 (cur.callAlertAck ? SETTINGS_DS_CALL_ALERT_ACK : 0) |
				 (cur.lrrpReply ? SETTINGS_DS_LRRP_REPLY : 0) |
				 (cur.smsFormat == DMR_SMS_FORMAT_ETSI ? SETTINGS_DS_SMS_ETSI : 0) |
				 (cur.radioCheckEtsi ? SETTINGS_DS_RADIO_CHECK_ETSI : 0));
	st->ds_stored = 1;
	settings_end(now_ms);
}

#if CONFIG_DMR_EMERGENCY
// --- DMR emergency (core/dmr/dmr_emergency.c): the module decides; this keys the hot mic as the 1750 Hz burst keys
// the transmitter (PTT taken over by a real press), reconfigures the stack when the emergency starts or ends, sounds
// the alert tone and keeps the settings in the store (settings_t em_*) ---

static uint32_t em_saved_serial;
static void ptt_down(radio_sm_t *sm, uint32_t now_ms);
static void ptt_release(radio_sm_t *sm, bool roger, uint32_t now_ms);

static void emergency_init(const settings_t *st)
{
	dmr_emergency_settings_t es;
	dmr_emergency_settings_default(&es);
	if (st->em_stored == 1) {
		es.mode = st->em_mode;
		es.dst = st->em_dst;
		es.dst_private = (st->em_flags & SETTINGS_EM_DST_PRIVATE) != 0;
		es.etsi_fid = (st->em_flags & SETTINGS_EM_ETSI_FID) != 0;
		es.rx_tone = (st->em_flags & SETTINGS_EM_NO_TONE) == 0;
		es.alarm_tries = st->em_alarm_tries;
		es.hot_mic_s = st->em_hot_mic_s;
	}
	dmr_emergency_init(&es);
	em_saved_serial = dmr_emergency_settings_serial();
}

static void emergency_save(uint32_t now_ms)
{
	if (dmr_emergency_settings_serial() == em_saved_serial) {
		return;
	}
	em_saved_serial = dmr_emergency_settings_serial();
	dmr_emergency_settings_t es;
	dmr_emergency_get_settings_now(&es);
	settings_t *st = settings_begin();
	st->em_mode = es.mode;
	st->em_dst = es.dst;
	st->em_flags = (uint8_t)((es.dst_private ? SETTINGS_EM_DST_PRIVATE : 0) | (es.etsi_fid ? SETTINGS_EM_ETSI_FID : 0) |
				 (es.rx_tone ? 0 : SETTINGS_EM_NO_TONE));
	st->em_alarm_tries = es.alarm_tries;
	st->em_hot_mic_s = es.hot_mic_s;
	st->em_stored = 1;
	settings_end(now_ms);
}

static void emergency_tick(radio_sm_t *sm, uint32_t now_ms)
{
	dmr_emergency_channel_t c = {
		.can_transmit = data_can_send(sm),
		.talk_id = sm->manual ? sm->manual_id : sm->talk_id,
		.talk_private = sm->manual ? sm->manual_private : sm->talk_private,
	};
	dmr_emergency_tick(now_ms, &c);
	if (sm->dmr_running && dmr_emergency_config_serial() != sm->em_cfg_serial) {
		dmr_configure(sm);	// the emergency option and target on or off; takes effect at the next call
	}
	bool want = dmr_emergency_hot_mic();
	if (want && !sm->em_keyed && !sm->ptt) {
		ptt_down(sm, now_ms);
		sm->em_keyed = sm->ptt;		// held by the hot mic, even when refused (the deny shows until it ends)
	} else if (!want && sm->em_keyed) {
		sm->em_keyed = false;
		ptt_release(sm, false, now_ms);
	}
	if (dmr_emergency_take_tone()) {
		beep(sm, BEEP_EMERGENCY);
	}
	emergency_save(now_ms);
}
#endif

static void data_init(radio_sm_t *sm)
{
	dmrDataEnv_t env = { .txStart = data_tx_start, .channelClear = data_channel_clear,
			     .inRxGroup = data_in_rx_group, .ctx = sm };
#if CONFIG_DMR_IP_GATEWAY
	env.packet = hostlink_net_air_packet;
	hostlink_net_air_start();
#endif
	// No auto-transmit features until the user turns them on (docs/ux/principles.md): no ARS server, no radio
	// check auto-reply. Call alerts are acknowledged (that is what makes them work); LRRP answers "no fix" without
	// a position source
	dmrDataSettings_t set = { .arsServer = 0, .radioCheckReply = false, .callAlertAck = true, .lrrpReply = true,
				  .hasGps = false };
	settings_t st;
	settings_read(&st);
	if (st.ds_stored == 1) {	// as saved (docs/spec/settings-store.md 3)
		set.arsServer = st.ds_ars_server <= 0xFFFFFFu ? st.ds_ars_server : 0;
		set.radioCheckReply = (st.ds_flags & SETTINGS_DS_RADIO_CHECK_REPLY) != 0;
		set.callAlertAck = (st.ds_flags & SETTINGS_DS_CALL_ALERT_ACK) != 0;
		set.lrrpReply = (st.ds_flags & SETTINGS_DS_LRRP_REPLY) != 0;
		set.smsFormat = (st.ds_flags & SETTINGS_DS_SMS_ETSI) ? DMR_SMS_FORMAT_ETSI : DMR_SMS_FORMAT_TMS;
		set.radioCheckEtsi = (st.ds_flags & SETTINGS_DS_RADIO_CHECK_ETSI) != 0;
	}
	data_saved = set;
	data_tx.list = NULL;
	dmrDataServiceInit(&env);
	dmrDataServiceSetSettings(&set);
	dmr_call_set_data_rx(data_rx, NULL);
#if CONFIG_DMR_EMERGENCY
	emergency_init(&st);
#endif
}

static void data_tick(radio_sm_t *sm, uint32_t now_ms)
{
	const channel_t *ch = &sm->view.ch;
	dmrDataChannel_t c = {
		.ownId = sm->view.dmr.own_id,
		.talkGroup = sm->talk_private ? 0 : sm->talk_id,
		.rmo = sm->view.dmr.rmo,
		.canTransmit = data_can_send(sm),
	};
	uint32_t h = 2166136261u;	// FNV-1a of what a registration is for
	uint32_t vals[] = { ch->rx_hz, ch->tx_hz, sm->view.dmr.ts, sm->view.dmr.cc };
	for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
		h = (h ^ vals[i]) * 16777619u;
	}
	c.channelKey = h;
	dmrDataServiceTick(now_ms, &c);
	data_save_settings(now_ms);
	if (sm->power_off_req && sm->power_off_busy) {
		sm->power_off_busy = dmrDataServicePowerOff();	// ARS deregistration, until it has gone out
	}
#if CONFIG_DMR_IP_GATEWAY
	hostlink_net_air_tick(sm->view.dmr.own_id, now_ms);
#endif
}
#endif

#else
static void dmr_begin(radio_sm_t *sm) { (void)sm; }
static void dmr_end(radio_sm_t *sm) { (void)sm; }
#endif

// --- Scan (radio/scan.c decides; this tunes) ---
#if CONFIG_SCAN
typedef struct {
	radio_sm_t *sm;
	bool dual;		// dual watch: the user picked both channels, scan skip doesn't apply
} scan_ctx_t;

static bool scan_usable(uint16_t pos, void *ctx)
{
	const scan_ctx_t *c = ctx;
	channel_t ch;
	return pos < c->sm->zone.count && codeplug_channel(c->sm->zone.channels[pos], &ch) && (c->dual || !ch.scan_skip);
}

static void scan_publish(radio_sm_t *sm)
{
	const scan_t *sc = &sm->scan;
	if (sm->view.scan != sc->kind || sm->view.scan_phase != sc->phase || sm->view.scan_skipped != sc->skipped) {
		sm->view.scan = sc->kind;
		sm->view.scan_phase = sc->phase;
		sm->view.scan_skipped = sc->skipped;
		changed(sm);
	}
}

// End the scan: stay where it is, or (cancel) go back to where it started
static void scan_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (!scan_on(sc)) {
		return;
	}
	uint8_t kind = sc->kind;
	scan_end(sc);
	if (cancel) {
		if (kind == SCAN_VFO) {
			if (sm->view.ch.rx_hz != sc->origin_hz) {
				vfo_set_hz(sm, sc->origin_hz);
				tune(sm, now_ms);
			}
		} else if (sm->view.zone_pos != sc->origin_pos) {
			channel_t ch;
			if (codeplug_channel(sm->zone.channels[sc->origin_pos], &ch)) {
				set_channel(sm, sc->origin_pos, &ch);
				tune(sm, now_ms);
			}
		}
	}
	scan_publish(sm);
}

// Move to the next stop. False (and the scan ends) when there is none
static bool scan_step(radio_sm_t *sm, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (sc->kind == SCAN_VFO) {
		uint32_t hz = scan_next_hz(sc, sm->view.ch.rx_hz, sm->view.vfo_step_hz);
		if (hz == 0 || !in_band(hz, false)) {
			scan_stop(sm, false, now_ms);
			return false;
		}
		vfo_set_hz(sm, hz);
	} else {
		scan_ctx_t c = { sm, sc->kind == SCAN_DUAL };
		int pos = scan_next_pos(sc, sm->zone.count, scan_usable, &c);
		channel_t ch;
		if (pos < 0 || !codeplug_channel(sm->zone.channels[pos], &ch)) {
			scan_stop(sm, false, now_ms);
			return false;
		}
		set_channel(sm, (uint16_t)pos, &ch);
	}
	tune(sm, now_ms);
	scan_tuned(sc, now_ms);
	scan_publish(sm);
	return true;
}

static void scan_start(radio_sm_t *sm, uint8_t kind, uint32_t lo_hz, uint32_t hi_hz, uint16_t dual_pos, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (scan_on(sc)) {
		scan_stop(sm, false, now_ms);
	}
	if (kind == SCAN_VFO) {
		if (!sm->view.vfo) {
			return;
		}
		if (lo_hz == 0 && hi_hz == 0) {
			// The band the VFO is in
			for (uint8_t i = 0; i < board.band_count; i++) {
				if (sm->view.ch.rx_hz >= board.bands[i].lo_hz && sm->view.ch.rx_hz <= board.bands[i].hi_hz) {
					lo_hz = board.bands[i].lo_hz;
					hi_hz = board.bands[i].hi_hz;
				}
			}
		}
		// Both ends in one band, and room for two stops
		bool same_band = false;
		for (uint8_t i = 0; i < board.band_count; i++) {
			const band_t *b = &board.bands[i];
			if (lo_hz >= b->lo_hz && hi_hz <= b->hi_hz) {
				same_band = true;
			}
		}
		if (!same_band || hi_hz < lo_hz + sm->view.vfo_step_hz) {
			return;
		}
	} else if (sm->view.vfo || !sm->have_channels || (kind == SCAN_DUAL && (dual_pos >= sm->zone.count ||
			dual_pos == sm->view.zone_pos))) {
		return;
	}
	scan_cfg_t cfg = {
		.dwell_ms = sm->set.scan_dwell_ms < SCAN_SETTLE_MS ? SCAN_SETTLE_MS : sm->set.scan_dwell_ms,
		.hold_ms = (uint16_t)(sm->set.scan_hold_s * 1000u),
		.delay_ms = sm->set.scan_delay_ms,
		.resume = sm->set.scan_resume,
		.priority = sm->set.scan_priority,
	};
	sc->origin_vfo = sm->view.vfo;
	sc->origin_pos = sm->view.zone_pos;
	sc->origin_hz = sm->view.ch.rx_hz;
	sc->dual_pos = dual_pos;
	sc->lo_hz = lo_hz;
	sc->hi_hz = hi_hz;
	scan_begin(sc, kind, &cfg, now_ms);
	if (kind != SCAN_VFO) {
		// Nothing to scan when no other channel qualifies
		scan_t probe = *sc;
		scan_ctx_t c = { sm, kind == SCAN_DUAL };
		int next = scan_next_pos(&probe, sm->zone.count, scan_usable, &c);
		if (next < 0 || next == sm->view.zone_pos) {
			scan_end(sc);
			return;
		}
	}
	activity(sm, now_ms);
	scan_step(sm, now_ms);
}

// Every poll while scanning: pause on the monitor, what the receiver hears, move on
static void scan_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	scan_pause(sc, sm->view.monitor, now_ms);
	bool dmr = sm->view.ch.mode == RADIO_MODE_DMR;
	bool carrier = carrier_heard(sm, st->noise);
	bool active;
#if CONFIG_DMR
	if (dmr) {
		active = sm->view.dmr.state == RDMR_RX;
	} else
#endif
	{
		active = sm->view.state == RS_RX && (sm->view.squelch != 0 || carrier);
	}
	sc->cfg.probe_ms = dmr ? RADIO_SCAN_DMR_PROBE_MS : tone_needed(sm) ? RADIO_SCAN_FM_PROBE_MS : 0;
	uint8_t was = sc->phase;
	switch (scan_poll(sc, now_ms, active, carrier)) {
	case SCAN_ACT_NEXT:
		scan_step(sm, now_ms);
		return;
	case SCAN_ACT_STOP:
		scan_stop(sm, false, now_ms);
		break;
	default:
		break;
	}
	if (sc->phase == SCAN_PH_HOLD && was != SCAN_PH_HOLD && was != SCAN_PH_RESUME && sm->set.scan_beep) {
		beep(sm, BEEP_SCAN_HOLD);
	}
	scan_publish(sm);
}

static void scan_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (sm->view.state == RS_TX || sm->hotspot || sm->ptt) {
		return;
	}
	switch (req->arg) {
	case RSCAN_STOP:
		scan_stop(sm, false, now_ms);
		break;
	case RSCAN_CANCEL:
		scan_stop(sm, true, now_ms);
		break;
	case RSCAN_ZONE:
		scan_start(sm, SCAN_ZONE, 0, 0, 0, now_ms);
		break;
	case RSCAN_VFO:
		scan_start(sm, SCAN_VFO, 0, 0, 0, now_ms);
		break;
	case RSCAN_DUAL:
		scan_start(sm, SCAN_DUAL, 0, 0, (uint16_t)req->arg2, now_ms);
		break;
	case RSCAN_TOGGLE:
		if (scan_on(&sm->scan)) {
			scan_stop(sm, false, now_ms);
		} else {
			scan_start(sm, sm->view.vfo ? SCAN_VFO : SCAN_ZONE, 0, 0, 0, now_ms);
		}
		break;
	case RSCAN_NUISANCE:
		if (scan_on(&sm->scan)) {
			bool ok = sm->scan.kind == SCAN_VFO ? scan_nuisance_hz(&sm->scan, sm->view.ch.rx_hz)
				: scan_nuisance_pos(&sm->scan, sm->view.zone_pos);
			if (ok) {
				scan_step(sm, now_ms);
			}
		}
		break;
	default:
		break;
	}
}
#else
static void scan_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms) { (void)sm; (void)cancel; (void)now_ms; }
#endif

static void persist(radio_sm_t *sm, uint32_t now_ms);

// --- DMR repeater roaming (radio/roam.c decides; this builds the site list, measures, tunes and keeps the block
// list; docs/spec/dmr-roaming.md) ---
#if CONFIG_DMR_ROAMING
static bool roam_zone_flag(const radio_sm_t *sm, uint16_t zi)
{
	return zi < SETTINGS_ROAM_ZONE_BYTES * 8u && (sm->roam_zones[zi / 8u] & (1u << (zi % 8u))) != 0;
}

static void roam_cfg_of(const radio_settings_t *s, roam_cfg_t *c)
{
	c->rssi_dbm = s->roam_rssi_dbm;
	c->hyst_db = s->roam_hyst_db;
	c->probe = s->roam_probe;
	c->check_s = s->roam_check_s;
}

// Where a channel talks: its contact's ID and call type, else the default talkgroup 9 (as dmr_load_channel)
static void roam_talk_of(const channel_t *ch, uint32_t *id, bool *priv)
{
	contact_t c;
	*id = 9;
	*priv = false;
	if (ch->contact != 0 && codeplug_contact(ch->contact, &c)) {
		*id = c.id;
		*priv = c.type == CONTACT_PRIVATE;
	}
}

// A DMR repeater channel the radio could roam to (docs/spec/dmr-roaming.md 2)
static bool roam_repeater(const channel_t *ch)
{
	return ch->mode == RADIO_MODE_DMR && !ch->rx_only && ch->tx_hz != 0 && ch->tx_hz != ch->rx_hz && !ch->dmo &&
		in_band(ch->rx_hz, false);
}

static void roam_publish(radio_sm_t *sm, uint32_t now_ms)
{
	const roam_t *r = &sm->roam;
	radio_roam_view_t *v = &sm->view.roam;
	bool on = r->state != ROAM_OFF;
	int16_t avg = on ? roam_signal(r, now_ms) : 0;
	bool zone_on = !sm->view.vfo && sm->have_channels && roam_zone_flag(sm, sm->view.zone_index);
	bool weak = avg != 0 && avg < sm->set.roam_rssi_dbm;
	uint8_t sites = on ? r->count : 0;
	if (v->zone_on != zone_on || v->state != r->state || v->why != r->why || v->pending != r->pending ||
	    v->sites != sites || v->site != r->cur || v->home != r->home || v->weak != weak ||
	    v->searches != r->searches || v->roams != r->roams || v->aborts != r->aborts) {
		changed(sm);
	}
	v->zone_on = zone_on;
	v->state = r->state;
	v->why = r->why;
	v->pending = r->pending;
	v->sites = sites;
	v->site = r->cur;
	v->home = r->home;
	v->rssi_dbm = avg;
	v->weak = weak;
	v->searches = r->searches;
	v->roams = r->roams;
	v->stays = r->stays;
	v->aborts = r->aborts;
	v->probes = r->probes;
	v->triggers = r->triggers;
	for (uint8_t i = 0; i < RADIO_ROAM_SITES; i++) {
		v->site_number[i] = i < sites ? r->site[i].number : 0;
		v->site_rssi[i] = i < sites ? r->site[i].rssi_dbm : 0;
	}
}

// The site list from the current zone (docs/spec/dmr-roaming.md 2). user: the user picked this channel (it is home)
static void roam_rebuild(radio_sm_t *sm, uint32_t now_ms, bool user)
{
	roam_site_t sites[ROAM_SITES_MAX];
	bool flagged[ROAM_SITES_MAX];
	uint8_t n = 0;
	int cur = -1;
	channel_t here, ch;
	sm->roam_dirty = false;
	sm->roam_zone = sm->view.zone_index;
	sm->roam_channel = sm->view.vfo ? 0 : sm->view.channel_number;
	if (sm->view.vfo || !sm->have_channels || !roam_zone_flag(sm, sm->view.zone_index) ||
	    !codeplug_channel(sm->view.channel_number, &here) || !roam_repeater(&here)) {
		roam_set_sites(&sm->roam, sites, 0, -1, -1, now_ms);
		roam_publish(sm, now_ms);
		return;
	}
	uint32_t talk_id;
	bool talk_priv, any_flag = false;
	roam_talk_of(&here, &talk_id, &talk_priv);
	for (uint16_t p = 0; p < sm->zone.count; p++) {
		uint32_t id;
		bool priv;
		if (!codeplug_channel(sm->zone.channels[p], &ch)) {
			continue;
		}
		any_flag |= ch.roam;
		if (n >= ROAM_SITES_MAX || !roam_repeater(&ch)) {
			continue;
		}
		roam_talk_of(&ch, &id, &priv);
		if (id != talk_id || priv != talk_priv) {
			continue;
		}
		memset(&sites[n], 0, sizeof(sites[n]));
		sites[n].pos = p;
		sites[n].number = sm->zone.channels[p];
		flagged[n] = ch.roam;
		n++;
	}
	if (any_flag) {
		// The legacy Roaming flag narrows the zone to the flagged channels
		uint8_t k = 0;
		for (uint8_t i = 0; i < n; i++) {
			if (flagged[i]) {
				sites[k++] = sites[i];
			}
		}
		n = k;
	}
	for (uint8_t i = 0; i < n; i++) {
		if (sites[i].pos == sm->view.zone_pos) {
			cur = i;
		}
	}
	roam_set_sites(&sm->roam, sites, n, cur, user ? cur : -1, now_ms);
	roam_publish(sm, now_ms);
}

// Why a search can't start now (docs/spec/dmr-roaming.md 4.3)
static uint8_t roam_blocked(const radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->hotspot) {
		return RROAM_BLOCK_HOTSPOT;
	}
	if (sm->view.vfo || sm->view.ch.mode != RADIO_MODE_DMR || !sm->dmr_running) {
		return RROAM_BLOCK_MODE;
	}
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return RROAM_BLOCK_SCAN;
	}
#endif
	if (sm->ptt || sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED || sm->dmr_keyed || dmr_call_data_busy()) {
		return RROAM_BLOCK_TX;
	}
	if (sm->view.state == RS_RX || sm->view.dmr.state != RDMR_IDLE) {
		return RROAM_BLOCK_CALL;
	}
	if (sm->view.dmr.reply_active || sm->one_shot) {
		return RROAM_BLOCK_REPLY;
	}
	if (now_ms - sm->roam_quiet_ms < ROAM_QUIET_MS) {
		return RROAM_BLOCK_QUIET;
	}
	if (sm->view.temporary || sm->manual || sm->hear_all_latch || sm->view.monitor) {
		return RROAM_BLOCK_OVERRIDE;
	}
#if CONFIG_DMR_EMERGENCY
	if (dmr_emergency_in_progress()) {
		return RROAM_BLOCK_EMERGENCY;
	}
#endif
	return RROAM_BLOCK_NONE;
}

// Tune site idx (a roaming move, not the user's: the site list stays). force: retune even when already there
static bool roam_tune(radio_sm_t *sm, uint8_t idx, bool force, uint32_t now_ms)
{
	const roam_site_t *s = &sm->roam.site[idx];
	channel_t ch;
	if (idx >= sm->roam.count || s->pos >= sm->zone.count || sm->zone.channels[s->pos] != s->number ||
	    !codeplug_channel(s->number, &ch)) {
		return false;
	}
	sm->roam_channel = s->number;
	if (!force && !sm->view.vfo && sm->view.zone_pos == s->pos) {
		return true;
	}
	set_channel(sm, s->pos, &ch);
	tune(sm, now_ms);
	return true;
}

// The wake probe: one BS_Dwn_Act CSBK through the stack's data path, which wakes the repeater first (its own
// activation bursts and retries) when it isn't up (docs/spec/dmr-roaming.md 4.5)
static uint8_t roam_probe_left;

static bool roam_probe_next(dmr_tx_burst_t *out, void *ctx)
{
	const radio_sm_t *sm = ctx;
	if (roam_probe_left == 0) {
		return false;
	}
	roam_probe_left--;
	memset(out, 0, sizeof(*out));
	out->kind = DMR_BURST_CSBK;
	out->host_crc = true;
	out->sync_override = DMR_SYNC_NONE;
	out->len = 12;
	dmr_csbk_bs_activation(sm->view.dmr.own_id, out->payload);
	return true;
}

static void roam_probe_done(uint8_t err, void *ctx)
{
	radio_sm_t *sm = ctx;
	sm->roam_probing = false;
	sm->roam_probe_err = err;
}

static bool roam_probe(radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	if (!sm->dmr_running || sm->ptt || sm->view.dmr.own_id == 0 || tx_check(sm) != TXDENY_NONE ||
	    dmr_call_data_busy()) {
		return false;
	}
	if (!sm->dmr_keyed) {
		static const subtone_t none = { TONE_NONE, 0 };
		if (!board.radio->tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &none)) {
			return false;
		}
		sm->dmr_keyed = true;
	}
	roam_probe_left = 1;
	dmr_data_source_t src = { .next = roam_probe_next, .done = roam_probe_done, .ctx = sm, .count = 1 };
	if (!dmr_call_data_tx(&src)) {
		return false;	// dmr_follow releases the keying
	}
	sm->roam_probing = true;
	sm->roam_probe_err = 0xFF;
	return true;
}

// End a search now and go back to where it started (the user did something)
static void roam_cancel(radio_sm_t *sm, uint32_t now_ms)
{
	if (!roam_searching(&sm->roam)) {
		return;
	}
	roam_abort(&sm->roam, now_ms);
	bool restart = sm->roam_probing || dmr_call_data_busy() || sm->dmr_keyed;
	if (restart) {
		dmr_end(sm);		// our probe is on the air: stop it with the stack (tune() starts it again)
		sm->roam_probing = false;
	}
	if (!roam_tune(sm, sm->roam.cur, restart, now_ms)) {
		sm->roam_dirty = true;
	}
	roam_publish(sm, now_ms);
}

// Every poll (DMR builds, not in hotspot mode): the site list, the measurement, the search
static void roam_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	roam_t *r = &sm->roam;
	if (sm->view.state != RS_IDLE || sm->view.dmr.state != RDMR_IDLE) {
		sm->roam_quiet_ms = now_ms;
	}
	if (!roam_searching(r)) {
		bool moved = sm->view.zone_index != sm->roam_zone ||
			     sm->view.channel_number != sm->roam_channel || (sm->view.vfo && sm->roam_channel != 0);
		bool scanning = false;
#if CONFIG_SCAN
		scanning = scan_on(&sm->scan);
#endif
		if ((moved || sm->roam_dirty) && !scanning) {
			if (moved) {
				roam_lock(r, false, now_ms);	// a lock ends with the user's channel change
			}
			roam_rebuild(sm, now_ms, moved);
		}
	}
	if (r->state == ROAM_OFF) {
		roam_publish(sm, now_ms);
		return;
	}
	bool heard = sm->dmr_running && dmr_call_view()->repeater_up;
	roam_sample(r, now_ms, heard, st->rssi_dbm);
	uint8_t blocked = roam_blocked(sm, now_ms);
	sm->view.roam.blocked = blocked;
	if (roam_searching(r) && (sm->roam_probing || sm->dmr_keyed)) {
		roam_publish(sm, now_ms);	// the probe is on the air: no retune until it is done
		return;
	}
	uint32_t roams = r->roams;
	switch (roam_poll(r, now_ms, blocked, sm->view.dmr.state == RDMR_RX)) {
	case ROAM_ACT_TUNE:
		if (roam_tune(sm, r->visit, false, now_ms)) {
			roam_tuned(r, now_ms);
		} else {
			roam_cancel(sm, now_ms);
			sm->roam_dirty = true;
		}
		break;
	case ROAM_ACT_PROBE:
		roam_probe_sent(r, roam_probe(sm));
		break;
	case ROAM_ACT_DONE:
		if (!roam_tune(sm, r->cur, false, now_ms)) {
			sm->roam_dirty = true;
		}
		if (r->roams != roams) {
			persist(sm, now_ms);	// powers up on this site
			if (sm->set.key_beep && !sm->view.ch.no_beep) {
				beep(sm, BEEP_SCAN_HOLD);
			}
		}
		break;
	default:
		break;
	}
	roam_publish(sm, now_ms);
}

static void roam_request(radio_sm_t *sm, uint8_t cmd, uint32_t now_ms)
{
	roam_t *r = &sm->roam;
	switch (cmd) {
	case RROAM_ZONE_OFF:
	case RROAM_ZONE_ON:
	case RROAM_ZONE_TOGGLE: {
		uint16_t zi = sm->view.zone_index;
		if (sm->view.vfo || !sm->have_channels || zi >= SETTINGS_ROAM_ZONE_BYTES * 8u) {
			break;
		}
		bool on = cmd == RROAM_ZONE_ON || (cmd == RROAM_ZONE_TOGGLE && !roam_zone_flag(sm, zi));
		if (!on) {
			roam_cancel(sm, now_ms);
		}
		uint8_t bit = (uint8_t)(1u << (zi % 8u));
		sm->roam_zones[zi / 8u] = (uint8_t)(on ? sm->roam_zones[zi / 8u] | bit : sm->roam_zones[zi / 8u] & ~bit);
		settings_t *s = settings_begin();
		memcpy(s->roam_zones, sm->roam_zones, sizeof(s->roam_zones));
		settings_end(now_ms);
		roam_rebuild(sm, now_ms, true);
		break;
	}
	case RROAM_NOW:
		if (sm->roam_dirty) {
			roam_rebuild(sm, now_ms, false);
		}
		roam_trigger(r, ROAM_WHY_MANUAL);
		break;
	case RROAM_LOCK:
	case RROAM_UNLOCK:
	case RROAM_LOCK_TOGGLE:
		roam_cancel(sm, now_ms);
		roam_lock(r, cmd == RROAM_LOCK || (cmd == RROAM_LOCK_TOGGLE && r->state != ROAM_LOCKED), now_ms);
		break;
	default:
		break;
	}
	roam_publish(sm, now_ms);
}
#else
static inline void roam_cancel(radio_sm_t *sm, uint32_t now_ms) { (void)sm; (void)now_ms; }
#endif

#if CONFIG_APRS
// --- APRS transmissions (core/aprs, docs/spec/aprs.md 6) ---
// The engine (aprs_tick) decides what to send; here the radio leaves the user's channel for the APRS frequency (FM,
// no subtone), waits for a clear channel (p-persistent CSMA on the noise reading), keys up, lets board.afsk send the
// tones, and tunes back. Only from idle: no PTT, no reception, no scan, no hotspot, no DMR call or data, TX allowed.
// Any user action (PTT, a request) cuts it short and the engine tries again later.

#define APRS_SETTLE_MS		50	// the receiver settles on the APRS frequency before the first carrier check
#define APRS_SLOT_MS		100	// CSMA slot time
#define APRS_PERSIST		63	// p = (63 + 1) / 256 per clear slot
#define APRS_KEYUP_MS		25	// carrier up before the tones (the preamble flags follow)
#define APRS_SEND_MAX_MS	6000u	// longer than any tone buffer (4096 symbols = 3.4 s)
enum { APRS_RF_OFF = 0, APRS_RF_LISTEN, APRS_RF_KEYUP, APRS_RF_SEND };

static bool aprs_can_start(const radio_sm_t *sm)
{
	if (sm->aprs_phase != APRS_RF_OFF || sm->ptt || sm->hotspot || sm->view.state != RS_IDLE || sm->view.monitor ||
	    sm->power_off_req || sm->set.tx_inhibit || sm->batt.level == BATT_CRITICAL || board.afsk == NULL) {
		return false;
	}
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return false;
	}
#endif
#if CONFIG_FM_TX_TONES
	if (sm->roger_ms != 0 || sm->burst_keyed) {
		return false;
	}
#endif
#if CONFIG_VOX
	if (sm->vox_keyed) {
		return false;
	}
#endif
#if CONFIG_DMR
	if ((sm->dmr_running && sm->view.dmr.state != RDMR_IDLE) || sm->dmr_keyed) {
		return false;
	}
#if CONFIG_HOTSPOT
	if (sm->cw_phase != 0) {
		return false;
	}
#endif
#endif
#if CONFIG_DMR_DATA_CORE
	if (dmrDataServiceIsBusy()) {
		return false;
	}
#endif
#if CONFIG_DMR_ROAMING
	if (sm->roam_probing) {
		return false;
	}
#endif
#if CONFIG_DMR_EMERGENCY
	if (sm->em_keyed) {
		return false;
	}
#endif
	return true;
}

// Back to the user's channel; sent / busy (gave up on a busy channel) for the engine
static void aprs_rf_end(radio_sm_t *sm, bool sent, bool busy, uint32_t now_ms)
{
	uint8_t phase = sm->aprs_phase;
	if (phase == APRS_RF_OFF) {
		return;
	}
	if (phase == APRS_RF_SEND) {
		board.afsk->tx_stop();
	}
	if (phase >= APRS_RF_KEYUP) {
		board.radio->tx_off();
		sm->tx_end_ms = now_ms;
	}
	sm->aprs_phase = APRS_RF_OFF;
	sm->view.state = RS_IDLE;
	aprs_rf_done(sent, busy, now_ms);
	tune(sm, now_ms);	// the channel again (DMR set up again if it was DMR)
}

static void aprs_rf_abort(radio_sm_t *sm, uint32_t now_ms)
{
	aprs_rf_end(sm, false, false, now_ms);
}

static void aprs_rf_start(radio_sm_t *sm, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (!aprs_rf_take(&sm->aprs_job, now_ms)) {
		return;
	}
	if (sm->board_mode == RADIO_MODE_DMR) {
		dmr_end(sm);		// the call stack lets go of the modem before the chip leaves DMR
	}
	if (sm->board_mode != RADIO_MODE_FM) {
		sm->board_mode = RADIO_MODE_FM;
		board.radio->set_mode(RADIO_MODE_FM);
	}
	sm->aprs_phase = APRS_RF_LISTEN;
	sm->aprs_ms = now_ms;
	sm->aprs_listen_ms = now_ms;
	sm->aprs_rand ^= now_ms;
	board.radio->rx(sm->aprs_job.hz, sm->aprs_job.narrow ? RADIO_BW_12K5 : RADIO_BW_25K, &none);
	outputs(sm);
	changed(sm);
}

// Every poll while on the APRS frequency
static void aprs_rf_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	switch (sm->aprs_phase) {
	case APRS_RF_LISTEN:
		if (now_ms - sm->aprs_listen_ms >= APRS_CSMA_GIVE_UP_MS) {
			aprs_rf_end(sm, false, true, now_ms);
			return;
		}
		if (now_ms - sm->aprs_ms < APRS_SETTLE_MS + APRS_SLOT_MS) {
			return;
		}
		sm->aprs_ms = now_ms - APRS_SETTLE_MS;	// the next check a slot later
		if (st->noise < radio_sm_squelch_open_below(RADIO_SQUELCH_DEFAULT)) {
			return;		// a carrier: wait
		}
		sm->aprs_rand = sm->aprs_rand * 1103515245u + 12345u;
		if ((sm->aprs_rand >> 24) > APRS_PERSIST) {
			return;		// clear, but not our turn this slot
		}
		sm->aprs_phase = APRS_RF_KEYUP;		// before tx(): aprs_rf_end unkeys from here on
		sm->aprs_ms = now_ms;
		sm->view.state = RS_TX;			// speaker off, red LED on, before keying
		sm->view.tx_deny = TXDENY_NONE;
		outputs(sm);
		changed(sm);
		if (!board.radio->tx(sm->aprs_job.hz, sm->aprs_job.narrow ? RADIO_BW_12K5 : RADIO_BW_25K, default_power(),
				     &none)) {
			aprs_rf_end(sm, false, false, now_ms);
		}
		break;
	case APRS_RF_KEYUP:
		if (now_ms - sm->aprs_ms < APRS_KEYUP_MS) {
			return;
		}
		if (!board.afsk->tx_start(sm->aprs_job.tones, sm->aprs_job.nbits, AFSK_BAUD, AFSK_MARK_HZ, AFSK_SPACE_HZ,
					  sm->aprs_job.level)) {
			aprs_rf_end(sm, false, false, now_ms);
			return;
		}
		sm->aprs_phase = APRS_RF_SEND;
		sm->aprs_ms = now_ms;
		break;
	case APRS_RF_SEND:
		if (!board.afsk->tx_busy()) {
			aprs_rf_end(sm, true, false, now_ms);
		} else if (now_ms - sm->aprs_ms > APRS_SEND_MAX_MS) {
			aprs_rf_end(sm, false, false, now_ms);
		}
		break;
	default:
		break;
	}
}

// Every poll, before anything else: the engine, the "after PTT" trigger, a transmission to start
static void aprs_poll(radio_sm_t *sm, uint32_t now_ms)
{
	bool tx_now = sm->view.state == RS_TX && sm->aprs_phase == APRS_RF_OFF;
	if (sm->aprs_was_tx && !tx_now) {
		aprs_ptt_end(now_ms);
	}
	sm->aprs_was_tx = tx_now;
	aprs_radio_t r;
	memset(&r, 0, sizeof(r));
	r.can_rf = aprs_can_start(sm);
	r.fm_rx = sm->aprs_phase == APRS_RF_OFF && sm->view.ch.mode == RADIO_MODE_FM && !sm->hotspot &&
		  sm->view.state != RS_TX;
#if CONFIG_DMR
	r.on_dmr = sm->dmr_running && !sm->hotspot;
	r.own_dmr_id = sm->own_id;
#endif
	aprs_tick(&r, now_ms);
	if (r.can_rf && aprs_rf_pending()) {
		aprs_rf_start(sm, now_ms);
	}
}
#else
static inline void aprs_rf_abort(radio_sm_t *sm, uint32_t now_ms) { (void)sm; (void)now_ms; }
#endif

// --- FM transmit tones and the end of an FM transmission ---

// Back to receive after an FM transmission (PTT released, roger beep done, burst released)
// keyed: DMR per-burst keying is still armed (dmr_follow releases it), so no tx_off here
static void fm_tx_end(radio_sm_t *sm, bool keyed, uint32_t now_ms)
{
	if (sm->view.state == RS_TX && !keyed) {
		board.radio->tx_off();
	}
	if (sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED) {
		sm->view.state = RS_IDLE;
		sm->view.tx_deny = TXDENY_NONE;
		sm->view.tot_warning = false;
		sm->view.tone_burst = false;
		sm->view.roger = false;
		sm->close_count = 0;
		reset_tone(sm);
		outputs(sm);
		changed(sm);
	}
	sm->tx_end_ms = now_ms;
	if (sm->retune_pending) {
		sm->retune_pending = false;
		tune(sm, now_ms);	// a temporary bandwidth or RX tone was undone during TX
	}
}

#if CONFIG_FM_TX_TONES
static bool tx_tone(uint16_t hz)
{
	return board.radio->tx_tone != NULL && board.radio->tx_tone(hz);
}

// The roger beep's second tone, then its end (every poll while it runs)
static void roger_poll(radio_sm_t *sm, uint32_t now_ms)
{
	uint32_t t = now_ms - sm->roger_ms;
	if (t >= 2u * RADIO_ROGER_STEP_MS) {
		tx_tone(0);
		sm->roger_ms = 0;
		fm_tx_end(sm, false, now_ms);
	} else if (t >= RADIO_ROGER_STEP_MS && t < RADIO_ROGER_STEP_MS + RADIO_POLL_MS) {
		tx_tone(RADIO_ROGER_HZ2);
	}
}

static void roger_cancel(radio_sm_t *sm)
{
	if (sm->roger_ms != 0) {
		tx_tone(0);
		sm->roger_ms = 0;
		sm->view.roger = false;
		changed(sm);
	}
}
#endif

static void ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->ptt) {
		return;
	}
	aprs_rf_abort(sm, now_ms);	// back on the channel first: PTT talks there
	activity(sm, now_ms);
	roam_cancel(sm, now_ms);	// PTT transmits on the site the user saw before a roaming search
#if CONFIG_FM_TX_TONES
	if (sm->roger_ms != 0 && sm->view.state == RS_TX) {
		roger_cancel(sm);	// pressed again during the roger beep: the transmission goes on
		sm->ptt = true;
		return;
	}
#endif
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		// On a signal: talk back on it. Still searching: back to the start channel
		bool on_signal = sm->scan.phase == SCAN_PH_HOLD || sm->scan.phase == SCAN_PH_RESUME;
		scan_stop(sm, !on_signal, now_ms);
	}
#endif
	sm->ptt = true;
	sm->tot_warned = false;
	sm->view.tot_warning = false;
	if (sm->hotspot) {
		deny(sm, TXDENY_HOTSPOT);	// the host link transmits for the hotspot; PTT does nothing until it ends
		return;
	}
	end_reception(sm, now_ms);

	uint8_t reason = tx_check(sm);
	if (reason != TXDENY_NONE) {
		deny(sm, reason);
		return;
	}
#if CONFIG_DMR
	if (sm->view.ch.mode == RADIO_MODE_DMR) {
		dmr_ptt_down(sm, now_ms);
		return;
	}
#endif
	sm->view.state = RS_TX;		// speaker off, red LED on, before keying
	sm->view.tx_deny = TXDENY_NONE;
	sm->view.tx_elapsed_ms = 0;
	sm->tx_start_ms = now_ms;
	outputs(sm);
	const channel_t *ch = &sm->view.ch;
	if (!board.radio->tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &ch->tx_tone)) {
		deny(sm, TXDENY_BAND);
		return;
	}
	changed(sm);
}

// roger: send the roger beep (if set) before unkeying
static void ptt_release(radio_sm_t *sm, bool roger, uint32_t now_ms)
{
	if (!sm->ptt) {
		return;
	}
	sm->ptt = false;
	activity(sm, now_ms);
#if CONFIG_DMR
	if (sm->dmr_running) {
		dmr_call_ptt(false);
		dmr_call_poll(now_ms);
		if (sm->view.state == RS_TX) {
			dmr_follow(sm, now_ms);	// on air until the superframe ends and the modem reports TX done
			return;
		}
	}
	bool keyed = sm->dmr_keyed;	// the DMR keying stays armed until the stack lets go (dmr_follow)
#else
	bool keyed = false;
#endif
#if CONFIG_FM_TX_TONES
	if (sm->view.tone_burst) {
		tx_tone(0);
		sm->view.tone_burst = false;
	}
	sm->burst_keyed = false;
	if (roger && !keyed && sm->set.roger_beep && sm->view.state == RS_TX && sm->view.ch.mode == RADIO_MODE_FM &&
			tx_tone(RADIO_ROGER_HZ1)) {
		sm->roger_ms = now_ms != 0 ? now_ms : 1;
		sm->view.roger = true;
		changed(sm);
		return;		// roger_poll unkeys
	}
#else
	(void)roger;
#endif
	fm_tx_end(sm, keyed, now_ms);
}

static void ptt_up(radio_sm_t *sm, uint32_t now_ms)
{
	ptt_release(sm, true, now_ms);
}

// --- Hotspot mode (core/hotspot drives the modem; hotspot_radio_ops_t in radio_task.c posts these) ---

bool radio_sm_hotspot_freq_ok(uint32_t rx_hz, uint32_t tx_hz)
{
	return rx_hz != 0 && tx_hz != 0 && in_band(rx_hz, false) && in_band(tx_hz, true);
}

uint8_t radio_sm_hotspot_power(uint8_t rf_level)
{
	if (board.power_count == 0) {
		return 0;
	}
	return (uint8_t)(((uint32_t)rf_level * (board.power_count - 1u) + 127u) / 255u);
}

#if CONFIG_DMR
// Per-burst keying stays armed on the hotspot's TX frequency: the modem keys each burst the host sends
static void hotspot_arm(radio_sm_t *sm)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (!sm->hotspot || sm->hs_tx_hz == 0) {
		return;
	}
#if CONFIG_HOTSPOT
	if (sm->cw_phase != 0) {
		return;		// armed again when the CW ID ends
	}
#endif
	sm->dmr_keyed = board.radio->tx(sm->hs_tx_hz, RADIO_BW_12K5, sm->hs_power, &none);
}

#if CONFIG_HOTSPOT
// --- The hotspot's CW ID (SEND_CWID, docs/spec/mmdvm-protocol.md 2.11): the hotspot is idle (mmdvm.c accepts it only
// then); the DMR keying is released, the board goes to FM and keys a carrier on the hotspot's TX frequency, the
// board's TX tone is keyed in Morse (core/radio/morse.c) at the settings' speed, then FM is left: set_mode(DMR) sets up
// the modem again (gotcha 8.13), the receiver goes back to the hotspot's RX frequency and the keying is armed again ---

#define CW_LEAD_MS		150	// carrier up before the first element
#define CW_TAIL_MS		150	// and after the last
#define CW_TONE_MIN_HZ		300
#define CW_TONE_MAX_HZ		3000
enum { CW_OFF = 0, CW_KEYING, CW_TAIL };

static void cw_key(radio_sm_t *sm, bool down)
{
	sm->cw_key = down;
	if (board.radio->tx_tone_key != NULL && board.radio->tx_tone_key(down)) {
		return;
	}
	board.radio->tx_tone(down ? sm->cw_tone_hz : 0);
}

// FM off again, the hotspot's DMR set-up back
static void cw_restore(radio_sm_t *sm)
{
	static const subtone_t none = { TONE_NONE, 0 };
	board.radio->tx_tone(0);
	board.radio->tx_off();
	sm->cw_phase = CW_OFF;
	sm->cw_key = false;
	sm->board_mode = RADIO_MODE_DMR;
	board.radio->set_mode(RADIO_MODE_DMR);
	if (sm->hs_rx_hz != 0) {
		board.radio->rx(sm->hs_rx_hz, RADIO_BW_12K5, &none);
	}
	hotspot_arm(sm);
	outputs(sm);
}

// Leaving hotspot mode or retuning in the middle: the ID is cut
static void cw_abort(radio_sm_t *sm)
{
	if (sm->cw_phase != CW_OFF) {
		cw_restore(sm);
		radio_hs_cw_ended(false, 0);
	}
}

static void cw_start(radio_sm_t *sm, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	char text[MORSE_TEXT_MAX];
	uint8_t n = radio_hs_cw_take(text, sizeof(text));
	if (n == 0) {
		return;
	}
	if (sm->cw_phase != CW_OFF || !sm->hotspot || sm->hs_tx_hz == 0 || board.radio->tx_tone == NULL ||
	    (board.dmr != NULL && board.dmr->is_transmitting())) {
		radio_hs_cw_ended(false, 0);
		return;
	}
	settings_t st;
	settings_read(&st);
	uint8_t wpm = st.cw_wpm != 0 ? st.cw_wpm : MORSE_WPM_DEFAULT;
	if (wpm < MORSE_WPM_MIN || wpm > MORSE_WPM_MAX) {
		wpm = MORSE_WPM_DEFAULT;
	}
	uint16_t hz = st.cw_tone_hz;
	sm->cw_tone_hz = (hz >= CW_TONE_MIN_HZ && hz <= CW_TONE_MAX_HZ) ? hz : MORSE_TONE_HZ_DEFAULT;

	if (sm->dmr_keyed) {
		sm->dmr_keyed = false;
		board.radio->tx_off();
	}
	sm->board_mode = RADIO_MODE_FM;
	board.radio->set_mode(RADIO_MODE_FM);
	sm->cw_phase = CW_KEYING;	// before tx(): cw_restore and outputs see it
	if (!board.radio->tx(sm->hs_tx_hz, RADIO_BW_12K5, sm->hs_power, &none) || !board.radio->tx_tone(sm->cw_tone_hz)) {
		cw_restore(sm);
		radio_hs_cw_ended(false, 0);
		return;
	}
	cw_key(sm, false);		// a silent carrier for the lead-in
	morse_start(&sm->cw, text, n, wpm, now_ms + CW_LEAD_MS);
	sm->cw_start_ms = now_ms;
	radio_hs_cw_started(wpm);
	outputs(sm);
	changed(sm);
}

// Every RADIO_TICK_MS
static void cw_service(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->cw_phase == CW_KEYING) {
		bool key = morse_poll(&sm->cw, now_ms);
		if (key != sm->cw_key) {
			cw_key(sm, key);
		}
		if (sm->cw.done) {
			sm->cw_phase = CW_TAIL;
			sm->cw_ms = now_ms;
		}
	} else if (sm->cw_phase == CW_TAIL && (now_ms - sm->cw_ms) >= CW_TAIL_MS) {
		uint32_t ms = now_ms - sm->cw_start_ms;
		cw_restore(sm);
		radio_hs_cw_ended(true, ms);
		changed(sm);
	}
}
#endif

static void hotspot_enter(radio_sm_t *sm, uint32_t rx_hz, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (sm->hotspot || board.dmr == NULL || sm->ptt || sm->view.state == RS_TX || sm->dmr_keyed) {
		return;		// refused for now: the hotspot asks again
	}
	scan_stop(sm, false, now_ms);
	end_reception(sm, now_ms);
	if (sm->board_mode == RADIO_MODE_DMR) {
		dmr_end(sm);	// the modem is free for the hotspot
	}
	if (sm->board_mode != RADIO_MODE_DMR) {
		sm->board_mode = RADIO_MODE_DMR;
		board.radio->set_mode(RADIO_MODE_DMR);
	}
	uint32_t hz = (rx_hz != 0 && in_band(rx_hz, false)) ? rx_hz : sm->view.ch.rx_hz;
	if (hz != 0 && in_band(hz, false)) {
		board.radio->rx(hz, RADIO_BW_12K5, &none);
	}
	sm->hs_rx_hz = hz;
	sm->hs_tx_hz = 0;
	sm->hs_carrier = false;
	sm->hotspot = true;
	sm->view.hotspot = true;
	sm->view.state = RS_IDLE;
	outputs(sm);
	changed(sm);
}

static void hotspot_freq(radio_sm_t *sm, uint32_t rx_hz, uint32_t tx_hz)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (!sm->hotspot || !radio_sm_hotspot_freq_ok(rx_hz, tx_hz)) {
		return;
	}
#if CONFIG_HOTSPOT
	cw_abort(sm);
#endif
	board.radio->rx(rx_hz, RADIO_BW_12K5, &none);	// disarms; armed again below
	sm->dmr_keyed = false;
	sm->hs_rx_hz = rx_hz;
	sm->hs_tx_hz = tx_hz;
	hotspot_arm(sm);
	changed(sm);
}

static void hotspot_leave(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->hotspot) {
		return;
	}
#if CONFIG_HOTSPOT
	cw_abort(sm);
#endif
	if (sm->dmr_keyed) {
		sm->dmr_keyed = false;
		board.radio->tx_off();
	}
	sm->hotspot = false;
	sm->view.hotspot = false;
	sm->hs_carrier = false;
	tune(sm, now_ms);	// the channel again; on a DMR channel the call stack takes the modem back
}
#endif

// --- Settings: what the radio was set to, back at power-on (core/settings), and saved when it changes ---

// The preferences the radio core uses: base squelch, master power, VFO step, the own DMR ID override
static void apply_prefs(radio_sm_t *sm, const settings_t *st)
{
	master_power = st->master_power;
	if (st->squelch <= RADIO_SQUELCH_MAX && sm->base_squelch != st->squelch) {
		sm->base_squelch = st->squelch;
		if (!sm->temp_squelch) {
			sm->view.squelch = st->squelch;
		}
	}
	if (!sm->temp_power) {
		sm->view.power = channel_power(&sm->view.ch);
	}
	if (st->vfo_step_hz >= 1000u && st->vfo_step_hz <= 1000000u) {
		sm->view.vfo_step_hz = st->vfo_step_hz;
	}
#if CONFIG_DMR
	sm->own_id = st->dmr_id != 0 && st->dmr_id <= 0xFFFFFFu ? st->dmr_id : codeplug_dmr_id();
	if (sm->own_id == 0) {
		sm->own_id = RADIO_DMR_ID_DEFAULT;
	}
	sm->view.dmr.own_id = sm->own_id;
	// The radio name (talker alias text): the settings' override, else the codeplug's; hang times from the codeplug
	codeplug_dmr_settings_t gs;
	codeplug_dmr_settings(&gs);
	if (st->radio_name[0] != '\0') {
		memset(gs.radio_name, 0, sizeof(gs.radio_name));
		memcpy(gs.radio_name, st->radio_name, CODEPLUG_RADIO_NAME_LEN);
	}
	memcpy(sm->radio_name, gs.radio_name, sizeof(sm->radio_name));
	sm->group_hang_ms = gs.group_hang_ms;
	sm->private_hang_ms = gs.private_hang_ms;
#if CONFIG_DMR_PRIVACY
	// The privacy key in use, kept in the settings (slot 1 for now; per-channel choice: codeplug-native.md)
	uint8_t slot = st->privacy_active;
	if (slot >= 1 && slot <= SETTINGS_PRIVACY_KEYS && st->privacy_type[slot - 1] < DMR_PRIVACY_NUM_TYPES &&
	    st->privacy_type[slot - 1] != DMR_PRIVACY_OFF &&
	    dmrPrivacyKeyIsValid(st->privacy_type[slot - 1], (uint16_t)st->privacy_key[slot - 1])) {
		sm->privacy_type = st->privacy_type[slot - 1];
		sm->privacy_key = (uint16_t)st->privacy_key[slot - 1];
	} else {
		sm->privacy_type = DMR_PRIVACY_OFF;
		sm->privacy_key = 0;
	}
#endif
#endif
	set_temporary(sm);
}

// The VFO as saved, if its frequency still fits the board
static void restore_vfo(radio_sm_t *sm, const settings_t *st)
{
	if (st->vfo_rx_hz == 0 || !in_band(st->vfo_rx_hz, false)) {
		return;
	}
	sm->vfo_ch.rx_hz = st->vfo_rx_hz;
	sm->vfo_ch.tx_hz = st->vfo_tx_hz != 0 ? st->vfo_tx_hz : st->vfo_rx_hz;
	sm->vfo_ch.mode = RADIO_MODE_FM;	// the VFO is FM only for now
	sm->vfo_ch.bw = st->vfo_bw == RADIO_BW_25K ? RADIO_BW_25K : RADIO_BW_12K5;
	sm->vfo_ch.power = st->vfo_power < board.power_count ? st->vfo_power : CODEPLUG_POWER_DEFAULT;
	sm->vfo_ch.color_code = st->vfo_cc & 0x0Fu;
	sm->vfo_ch.timeslot = st->vfo_ts == 2 ? 2 : 1;
	subtone_t rx = { st->vfo_rx_tone_type, st->vfo_rx_tone }, tx = { st->vfo_tx_tone_type, st->vfo_tx_tone };
	sm->vfo_ch.rx_tone = tones_valid(&rx) ? rx : (subtone_t){ TONE_NONE, 0 };
	sm->vfo_ch.tx_tone = tones_valid(&tx) ? tx : (subtone_t){ TONE_NONE, 0 };
	sm->vfo_seeded = true;
}

// The position of channel number in z, or -1
static int zone_find(const zone_t *z, uint16_t number)
{
	for (uint16_t i = 0; number != 0 && i < z->count; i++) {
		if (z->channels[i] == number) {
			return i;
		}
	}
	return -1;
}

// Zone zi, on channel number (or else near pos); false if the zone has no readable channel
static bool select_in_zone(radio_sm_t *sm, uint16_t zi, uint16_t number, uint16_t pos)
{
	zone_t z;
	uint16_t p;
	channel_t ch;
	if (!read_zone(zi, &z)) {
		return false;
	}
	int at = zone_find(&z, number);
	if (at >= 0 && codeplug_channel(number, &ch)) {
		p = (uint16_t)at;
	} else if (!find_channel(&z, pos < z.count ? pos : 0, 1, &p, &ch)) {
		return false;
	}
	set_zone(sm, zi, &z);
	set_channel(sm, p, &ch);
	sm->have_channels = true;
	return true;
}

// Zone and channel: the saved ones, else zone 0's first channel, else the first zone that has one
static void select_position(radio_sm_t *sm, uint16_t zi, uint16_t number, uint16_t pos)
{
	uint16_t zones = codeplug_zone_count();
	sm->have_channels = false;
	if (zi < zones && select_in_zone(sm, zi, number, pos)) {
		return;
	}
	for (uint16_t i = 0; i < zones; i++) {
		if (select_in_zone(sm, i, 0, 0)) {
			return;
		}
	}
	memset(&sm->zone, 0, sizeof(sm->zone));
}

// Note the position and the VFO in the settings (saved later, when they rest; settings_end only marks changes)
static void persist(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->hotspot) {
		return;		// the hotspot's frequencies are the host's, not the radio's
	}
	settings_t *st = settings_begin();
	st->vfo = sm->view.vfo;
	if (sm->have_channels) {
		st->zone_index = sm->view.zone_index;
		st->zone_pos = sm->view.zone_pos;
		st->channel = sm->view.zone_pos < sm->zone.count ? sm->zone.channels[sm->view.zone_pos] : 0;
	}
	if (sm->vfo_seeded) {
		const channel_t *v = &sm->vfo_ch;
		st->vfo_rx_hz = v->rx_hz;
		st->vfo_tx_hz = v->tx_hz;
		st->vfo_mode = v->mode;
		st->vfo_bw = v->bw;
		st->vfo_power = v->power == CODEPLUG_POWER_DEFAULT ? SETTINGS_UNSET8 : v->power;
		st->vfo_cc = v->color_code;
		st->vfo_ts = v->timeslot;
		st->vfo_rx_tone_type = v->rx_tone.type;
		st->vfo_rx_tone = v->rx_tone.value;
		st->vfo_tx_tone_type = v->tx_tone.type;
		st->vfo_tx_tone = v->tx_tone.value;
	}
#if CONFIG_DMR_PRIVACY
	if (sm->privacy_type != DMR_PRIVACY_OFF) {
		st->privacy_active = 1;
		st->privacy_type[0] = sm->privacy_type;
		st->privacy_key[0] = sm->privacy_key;
	} else {
		st->privacy_active = 0;		// the key stays in its slot
	}
#endif
	settings_end(now_ms);
}

static void clamp_settings(radio_settings_t *s)
{
	if (s->beep_level > 100) {
		s->beep_level = 100;
	}
	if (s->vox_level > 10) {
		s->vox_level = 10;
	}
	if (s->rx_save > 3) {
		s->rx_save = 3;
	}
	if (s->reply_s > 30 && s->reply_s != RADIO_REPLY_CODEPLUG) {
		s->reply_s = 30;
	}
	if (s->roam_rssi_dbm < -125) {
		s->roam_rssi_dbm = -125;
	} else if (s->roam_rssi_dbm > -60) {
		s->roam_rssi_dbm = -60;
	}
	if (s->roam_hyst_db < 1) {
		s->roam_hyst_db = 1;
	} else if (s->roam_hyst_db > 30) {
		s->roam_hyst_db = 30;
	}
	if (s->roam_probe > 1) {
		s->roam_probe = 1;
	}
	if (s->roam_check_s != 0 && s->roam_check_s < 30) {
		s->roam_check_s = 30;
	} else if (s->roam_check_s > 3600) {
		s->roam_check_s = 3600;
	}
}

// The radio core's settings as kept in the settings store (docs/spec/settings-store.md 3); the defaults if never set
static void settings_to_radio(const settings_t *st, radio_settings_t *s)
{
	radio_settings_default(s);
	if (st->roam_stored == 1) {
		s->roam_rssi_dbm = st->roam_rssi_dbm;
		s->roam_hyst_db = st->roam_hyst_db;
		s->roam_probe = st->roam_probe;
		s->roam_check_s = st->roam_check_s;
		clamp_settings(s);
	}
	if (st->rs_stored != 1) {
		return;
	}
	s->tot_s = st->rs_tot_s;
	s->tot_warn_s = st->rs_tot_warn_s;
	s->bcl = st->rs_bcl;
	s->roger_beep = (st->rs_flags & SETTINGS_RS_ROGER_BEEP) != 0;
	s->tx_inhibit = (st->rs_flags & SETTINGS_RS_TX_INHIBIT) != 0;
	s->scan_priority = (st->rs_flags & SETTINGS_RS_SCAN_PRIORITY) != 0;
	s->scan_beep = (st->rs_flags & SETTINGS_RS_SCAN_BEEP) != 0;
	s->key_beep = (st->rs_flags & SETTINGS_RS_KEY_BEEP) != 0;
	s->scan_resume = st->rs_scan_resume;
	s->scan_hold_s = st->rs_scan_hold_s;
	s->scan_delay_ms = st->rs_scan_delay_ms;
	s->scan_dwell_ms = st->rs_scan_dwell_ms;
	s->beep_level = st->rs_beep_level;
	s->vox = st->rs_vox;
	s->vox_level = st->rs_vox_level;
	s->vox_tail_ms = st->rs_vox_tail_ms;
	s->rx_save = st->rs_rx_save;
	s->rx_save_idle_s = st->rs_rx_save_idle_s;
	s->battery_off_s = st->rs_battery_off_s;
	s->backlight_dim_s = st->rs_backlight_dim_s;
	s->backlight_off_s = st->rs_backlight_off_s;
	s->reply_s = st->rs_reply_s;
	clamp_settings(s);
}

static void radio_to_settings(const radio_settings_t *s, uint32_t now_ms)
{
	settings_t *st = settings_begin();
	st->rs_tot_s = s->tot_s;
	st->rs_tot_warn_s = s->tot_warn_s;
	st->rs_bcl = s->bcl;
	st->rs_flags = (uint8_t)((s->roger_beep ? SETTINGS_RS_ROGER_BEEP : 0) | (s->tx_inhibit ? SETTINGS_RS_TX_INHIBIT : 0) |
				 (s->scan_priority ? SETTINGS_RS_SCAN_PRIORITY : 0) |
				 (s->scan_beep ? SETTINGS_RS_SCAN_BEEP : 0) | (s->key_beep ? SETTINGS_RS_KEY_BEEP : 0));
	st->rs_scan_resume = s->scan_resume;
	st->rs_scan_hold_s = s->scan_hold_s;
	st->rs_scan_delay_ms = s->scan_delay_ms;
	st->rs_scan_dwell_ms = s->scan_dwell_ms;
	st->rs_beep_level = s->beep_level;
	st->rs_vox = s->vox;
	st->rs_vox_level = s->vox_level;
	st->rs_vox_tail_ms = s->vox_tail_ms;
	st->rs_rx_save = s->rx_save;
	st->rs_rx_save_idle_s = s->rx_save_idle_s;
	st->rs_battery_off_s = s->battery_off_s;
	st->rs_backlight_dim_s = s->backlight_dim_s;
	st->rs_backlight_off_s = s->backlight_off_s;
	st->rs_reply_s = s->reply_s;
	st->rs_stored = 1;
	st->roam_rssi_dbm = s->roam_rssi_dbm;
	st->roam_hyst_db = s->roam_hyst_db;
	st->roam_probe = s->roam_probe;
	st->roam_check_s = s->roam_check_s;
	st->roam_stored = 1;
	settings_end(now_ms);
}

static void use_settings(radio_sm_t *sm, const radio_settings_t *s, uint32_t now_ms)
{
#if CONFIG_DMR
	bool reply_changed = s->reply_s != sm->set.reply_s;
#endif
	sm->set = *s;
	clamp_settings(&sm->set);
	apply_tot(sm);
#if CONFIG_DMR_ROAMING
	roam_cfg_t rc;
	roam_cfg_of(&sm->set, &rc);
	roam_set_cfg(&sm->roam, &rc);
#endif
	activity(sm, now_ms);
#if CONFIG_DMR
	if (reply_changed && sm->dmr_running) {
		dmr_configure(sm);	// the reply window
	}
#endif
#if CONFIG_VOX
	if (sm->vox_keyed && sm->set.vox == RADIO_VOX_OFF) {
		sm->vox_keyed = false;
		sm->view.vox_keyed = false;
		ptt_release(sm, false, now_ms);
	}
#endif
	changed(sm);
}

// From radio_set_settings (the settings screen, the debug link): in effect at once, saved by the settings store
void radio_sm_set_settings(radio_sm_t *sm, const radio_settings_t *s, uint32_t now_ms)
{
	use_settings(sm, s, now_ms);
	radio_to_settings(&sm->set, now_ms);
}

// The codeplug changed underneath (an edit, a CPS write) or the preferences did: read them again, keep the zone and
// the channel if they still exist
static void reload(radio_sm_t *sm, uint32_t now_ms)
{
	settings_t st;
	roam_cancel(sm, now_ms);	// back to the site the search started from first
	settings_read(&st);
#if CONFIG_DMR_ROAMING
	memcpy(sm->roam_zones, st.roam_zones, sizeof(sm->roam_zones));
	sm->roam_dirty = true;
#endif
	sm->reload_pending = false;
	sm->codeplug_serial = codeplug_serial();
	sm->view.source = codeplug_source();
	uint16_t number = sm->have_channels && sm->view.zone_pos < sm->zone.count ? sm->zone.channels[sm->view.zone_pos] : 0;
	bool vfo = sm->view.vfo;
	select_position(sm, sm->view.zone_index, number, sm->view.zone_pos);
	apply_prefs(sm, &st);
	radio_settings_t rs;
	settings_to_radio(&st, &rs);
	use_settings(sm, &rs, now_ms);
	if (vfo || !sm->have_channels) {
		if (VFO_AVAILABLE) {
			enter_vfo(sm, now_ms);
		} else {
			strcpy(sm->view.ch.name, "No channels");
			changed(sm);
		}
	} else {
		tune(sm, now_ms);
	}
}

void radio_sm_init(radio_sm_t *sm, uint32_t now_ms)
{
	settings_t st;
	settings_read(&st);

	memset(sm, 0, sizeof(*sm));
	sm->audio_out = -1;
	sm->green_out = -1;
	sm->red_out = -1;
	sm->volume_sent = -1;
	sm->base_squelch = RADIO_SQUELCH_DEFAULT;
	sm->view.squelch = RADIO_SQUELCH_DEFAULT;
	settings_to_radio(&st, &sm->set);	// as saved (docs/spec/settings-store.md 3)
#if CONFIG_DMR_ROAMING
	roam_cfg_t rc;
	roam_cfg_of(&sm->set, &rc);
	roam_init(&sm->roam, &rc);
	memcpy(sm->roam_zones, st.roam_zones, sizeof(sm->roam_zones));
	sm->roam_dirty = true;
#endif
	sm->view.tot_s = sm->set.tot_s;
	sm->activity_ms = now_ms;
	batt_init(&sm->batt);
#if CONFIG_BEEP
	beep_init(board.audio);
#endif
	sm->view.vfo_step_hz = RADIO_VFO_STEP_HZ;
	sm->view.source = codeplug_source();
	sm->codeplug_serial = codeplug_serial();

	memset(&sm->vfo_ch, 0, sizeof(sm->vfo_ch));
	strcpy(sm->vfo_ch.name, "VFO");
	sm->vfo_ch.mode = RADIO_MODE_FM;
	sm->vfo_ch.bw = RADIO_BW_12K5;
	sm->vfo_ch.power = CODEPLUG_POWER_DEFAULT;
	sm->vfo_ch.timeslot = 1;
	restore_vfo(sm, &st);

	sm->board_mode = RADIO_MODE_FM;
	board.radio->set_mode(RADIO_MODE_FM);

#if CONFIG_DMR
	sm->codec = board.vocoder != NULL && board.vocoder->present != NULL && board.vocoder->present();
	sm->view.dmr.codec = sm->codec;
	sm->hs_power = default_power();
	dmr_call_init(board.dmr);	// board.dmr NULL: DMR channels receive nothing and refuse PTT
	ambeVoiceInit(board.audio, board.vocoder);
	dmr_call_set_voice(ambeVoiceSink(), ambeVoiceSource());
#if CONFIG_BEEP
	ambeVoiceSetMixer(beep_mix);	// beeps while the voice stream owns the PCM audio
#endif
#if CONFIG_DMR_DATA_CORE
	data_init(sm);
#endif
#endif

	// The saved zone and channel; else zone 0 and its first channel; if zone 0 has none, the first zone that has one
	select_position(sm, st.zone_index, st.channel, st.zone_pos);
	apply_prefs(sm, &st);
#if CONFIG_DMR
	sm->hs_power = default_power();
#endif
	if (!sm->have_channels || (st.vfo && VFO_AVAILABLE)) {
		if (VFO_AVAILABLE) {
			enter_vfo(sm, now_ms);
		} else {
			strcpy(sm->view.ch.name, "No channels");	// built without a VFO and nothing programmed: nothing to tune
		}
	} else {
		tune(sm, now_ms);
	}
	sm->rssi_published = sm->view.rssi_dbm;
#if CONFIG_APRS
	aprs_init(now_ms);
#endif
}


#if CONFIG_FM_TX_TONES
// The 1750 Hz burst: on keys up (FM channel, the usual TX checks) if not transmitting yet
static void tone_burst(radio_sm_t *sm, bool on, uint32_t now_ms)
{
	if (on) {
		if (sm->view.tone_burst || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||
				board.radio->tx_tone == NULL) {
			return;
		}
		if (sm->view.state != RS_TX) {
			ptt_down(sm, now_ms);
			sm->burst_keyed = sm->ptt;	// held by the burst, even when refused (the deny shows until release)
			if (sm->view.state != RS_TX) {
				return;
			}
		}
		roger_cancel(sm);
		if (tx_tone(RADIO_BURST_HZ)) {
			sm->view.tone_burst = true;
			changed(sm);
		}
		return;
	}
	if (sm->view.tone_burst) {
		tx_tone(0);
		sm->view.tone_burst = false;
		changed(sm);
	}
	if (sm->burst_keyed) {
		sm->burst_keyed = false;
		ptt_release(sm, false, now_ms);
	}
}
#endif

// A request that changes what the receiver is tuned to ends a scan first (it stays where it is)
static bool retunes(uint8_t type)
{
	switch (type) {
	case RREQ_CH_STEP: case RREQ_ZONE_POS: case RREQ_ZONE: case RREQ_VFO: case RREQ_VFO_STEP: case RREQ_VFO_SET:
	case RREQ_BANDWIDTH: case RREQ_TONE: case RREQ_MODE: case RREQ_DMR_TS: case RREQ_DMR_CC:
		return true;
	default:
		return false;
	}
}

void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (req->type != RREQ_BEEP && req->type != RREQ_DMR_FOCUS && req->type != RREQ_SETTINGS) {
		aprs_rf_abort(sm, now_ms);	// the user does something: an APRS transmission gives way
	}
	bool busy = sm->view.state == RS_TX || sm->hotspot;	// no retuning while transmitting or a hotspot
	int dir = req->arg > 0 ? 1 : -1;
	int steps = req->arg > 0 ? req->arg : -req->arg;

	if (req->type != RREQ_BEEP) {
		activity(sm, now_ms);
	}
	if (req->type != RREQ_BEEP && req->type != RREQ_DMR_FOCUS && req->type != RREQ_SETTINGS && req->type != RREQ_ROAM) {
		roam_cancel(sm, now_ms);	// the user does something: back to the site a roaming search started from
	}
	if (retunes(req->type)) {
		scan_stop(sm, false, now_ms);
	}
	switch (req->type) {
	case RREQ_CH_STEP:
		if (busy || steps == 0) {
			break;
		}
		for (int i = 0; i < steps; i++) {
			if (sm->view.vfo) {
				vfo_step(sm, dir, now_ms);
			} else {
				channel_step(sm, dir, now_ms);
			}
		}
		break;
	case RREQ_VFO_STEP:
		if (busy || !sm->view.vfo) {
			break;
		}
		for (int i = 0; i < steps; i++) {
			vfo_step(sm, dir, now_ms);
		}
		break;
	case RREQ_ZONE_POS: {
		uint16_t pos;
		channel_t ch;
		if (busy || !sm->have_channels || req->arg < 0 || req->arg >= sm->zone.count) {
			break;
		}
		pos = (uint16_t)req->arg;
		if (codeplug_channel(sm->zone.channels[pos], &ch)) {
			set_channel(sm, pos, &ch);
			tune(sm, now_ms);
		}
		break;
	}
	case RREQ_ZONE: {
		zone_t z;
		uint16_t pos;
		channel_t ch;
		if (busy || req->arg < 0 || req->arg >= codeplug_zone_count()) {
			break;
		}
		if (read_zone((uint16_t)req->arg, &z) && find_channel(&z, 0, 1, &pos, &ch)) {
			set_zone(sm, (uint16_t)req->arg, &z);
			set_channel(sm, pos, &ch);
			sm->have_channels = true;
			tune(sm, now_ms);
		}
		break;
	}
	case RREQ_VFO:
		if (busy || !VFO_AVAILABLE || (req->arg != 0) == sm->view.vfo) {
			break;
		}
		if (req->arg != 0) {
			enter_vfo(sm, now_ms);
		} else {
			leave_vfo(sm, now_ms);
		}
		break;
	case RREQ_VFO_SET:
		if (busy || !VFO_AVAILABLE || req->arg <= 0 || !in_band((uint32_t)req->arg, false)) {
			break;
		}
		vfo_set_hz(sm, (uint32_t)req->arg);
		if (sm->view.vfo) {
			tune(sm, now_ms);
		} else {
			enter_vfo(sm, now_ms);
		}
		break;
	case RREQ_PTT:
#if CONFIG_FM_TX_TONES
		if (sm->burst_keyed) {
			sm->burst_keyed = req->arg == 0;	// PTT pressed during a burst takes the transmission over
			break;
		}
#endif
#if CONFIG_DMR_EMERGENCY
		if (sm->em_keyed) {
			sm->em_keyed = req->arg == 0;	// PTT pressed during the hot mic takes the transmission over
			break;
		}
#endif
#if CONFIG_VOX
		if (sm->vox_keyed) {
			if (req->arg != 0) {
				sm->vox_keyed = false;		// PTT takes a VOX transmission over
				sm->view.vox_keyed = false;
				changed(sm);
			}
			break;
		}
#endif
		if (req->arg != 0) {
			ptt_down(sm, now_ms);
		} else {
			ptt_up(sm, now_ms);
		}
		break;
	case RREQ_MONITOR:
		if (sm->view.monitor != (req->arg != 0)) {
			sm->view.monitor = req->arg != 0;
#if CONFIG_DMR
			if (sm->dmr_running) {
				dmr_configure(sm);	// DMR: hear all while held
			}
#endif
			outputs(sm);
			changed(sm);
		}
		break;
	case RREQ_POWER: {
		int n = board.power_count;
		if (n == 0 || steps == 0) {
			break;
		}
		int p = ((sm->view.power + req->arg) % n + n) % n;
		sm->view.power = (uint8_t)p;
		sm->temp_power = sm->view.power != channel_power(&sm->view.ch);
		set_temporary(sm);
		changed(sm);
		break;
	}
	case RREQ_SQUELCH: {
		int s = sm->view.squelch + req->arg;
		if (s < 0) {
			s = 0;
		} else if (s > RADIO_SQUELCH_MAX) {
			s = RADIO_SQUELCH_MAX;
		}
		if (s != sm->view.squelch) {
			sm->view.squelch = (uint8_t)s;
			sm->temp_squelch = sm->view.squelch != sm->base_squelch;
			set_temporary(sm);
			changed(sm);
		}
		break;
	}
	case RREQ_CLEAR_TEMP:
		if (sm->view.temporary) {
			if (clear_temporary(sm)) {
				if (busy) {
					sm->retune_pending = true;
				} else {
					tune(sm, now_ms);
				}
			}
			changed(sm);
		}
		break;
	case RREQ_BANDWIDTH:
		if (busy || sm->view.ch.mode != RADIO_MODE_FM || (req->arg != RADIO_BW_12K5 && req->arg != RADIO_BW_25K) ||
				req->arg == sm->view.ch.bw) {
			break;
		}
		sm->view.ch.bw = (uint8_t)req->arg;
		if (sm->view.vfo) {
			sm->vfo_ch.bw = sm->view.ch.bw;
		}
		set_temporary(sm);
		tune(sm, now_ms);
		break;
	case RREQ_TONE: {
		uint8_t which = (uint8_t)(((uint32_t)req->arg >> 24) & 3u);
		subtone_t t = { (uint8_t)(((uint32_t)req->arg >> 16) & 0xFFu), (uint16_t)req->arg };
		if (t.type == TONE_NONE) {
			t.value = 0;
		}
		if (busy || sm->view.ch.mode != RADIO_MODE_FM || which == 0 || !tones_valid(&t)) {
			break;
		}
		bool rx_changed = (which & RADIO_TONE_RX) && !same_tone(&t, &sm->view.ch.rx_tone);
		bool tx_changed = (which & RADIO_TONE_TX) && !same_tone(&t, &sm->view.ch.tx_tone);
		if (which & RADIO_TONE_TX) {
			sm->view.ch.tx_tone = t;
		}
		if (which & RADIO_TONE_RX) {
			sm->view.ch.rx_tone = t;
		}
		if (sm->view.vfo) {
			sm->vfo_ch.tx_tone = sm->view.ch.tx_tone;
			sm->vfo_ch.rx_tone = sm->view.ch.rx_tone;
		}
		set_temporary(sm);
		if (rx_changed) {
			tune(sm, now_ms);	// the receiver's tone detector
		} else if (tx_changed) {
			changed(sm);		// used on the next PTT
		}
		break;
	}
#if CONFIG_DMR
	case RREQ_REPLY_CANCEL:
		dmr_call_reply_cancel();
		one_shot_end(sm);
		break;
	case RREQ_DMR_TARGET:
		dmr_set_manual(sm, (uint32_t)req->arg & DMR_ID_MAX, ((uint32_t)req->arg >> 24) & 1u, 0);
		break;
	case RREQ_DMR_CONTACT: {
		contact_t c;
		if (req->arg > 0 && req->arg <= 0xFFFF && codeplug_contact((uint16_t)req->arg, &c)) {
			dmr_set_manual(sm, c.id, c.type == CONTACT_PRIVATE, c.timeslot);
		}
		break;
	}
	case RREQ_DMR_CALL_BACK: {
		const dmr_call_view_t *cv = dmr_call_view();
		if (sm->dmr_running && cv->last_heard_ms != 0) {
			const dmr_caller_t *l = &cv->last_heard;
			dmr_set_manual(sm, l->group ? l->dst : l->src, !l->group, 0);
		}
		break;
	}
	case RREQ_DMR_TARGET_CLEAR:
		if (sm->manual || sm->one_shot) {
			sm->manual = false;
			sm->one_shot = false;
			if (sm->dmr_running) {
				dmr_configure(sm);
				sm->dmr_serial = dmr_call_view()->serial - 1u;
			}
			changed(sm);
		}
		break;
	case RREQ_DMR_TS:
		dmr_set_timeslot(sm, req->arg, busy);
		break;
	case RREQ_DMR_CC:
		if (busy || sm->view.vfo || sm->view.ch.mode != RADIO_MODE_DMR || req->arg < 0 || req->arg > 15 ||
				req->arg == sm->view.ch.color_code) {
			break;
		}
		sm->view.ch.color_code = (uint8_t)req->arg;
		set_temporary(sm);
		dmr_reconfigure(sm);
		break;
	case RREQ_MODE:
		if (busy || sm->view.vfo || (req->arg != RADIO_MODE_FM && req->arg != RADIO_MODE_DMR) ||
				req->arg == sm->view.ch.mode || (req->arg == RADIO_MODE_FM && !CONFIG_FM)) {
			break;
		}
		sm->view.ch.mode = (uint8_t)req->arg;
		if (sm->view.ch.mode == RADIO_MODE_DMR && sm->view.ch.timeslot != 2) {
			sm->view.ch.timeslot = 1;
		}
		set_temporary(sm);
		tune(sm, now_ms);	// the call stack starts or stops with the mode
		break;
	case RREQ_DMR_HEAR_ALL:
		if ((req->arg != 0) == sm->hear_all_latch) {
			break;
		}
		sm->hear_all_latch = req->arg != 0;
		sm->latch_ch = sm->view.channel_number;
		sm->latch_vfo = sm->view.vfo;
		sm->view.dmr.hear_all_latch = sm->hear_all_latch;
		dmr_reconfigure(sm);
		break;
	case RREQ_DMR_FOCUS: {
		uint32_t id = (uint32_t)req->arg & DMR_ID_MAX;
		bool priv = (((uint32_t)req->arg >> 24) & 1u) != 0;
		if (id != sm->focus_id || priv != sm->focus_private) {
			sm->focus_id = id;
			sm->focus_private = id != 0 && priv;
			sm->view.dmr.focus_id = id;
			sm->view.dmr.focus_private = sm->focus_private;
			changed(sm);
		}
		break;
	}
#if CONFIG_DMR_PRIVACY
	case RREQ_DMR_PRIVACY: {
		uint8_t type = (uint8_t)(((uint32_t)req->arg >> 16) & 0xFFu);
		uint16_t key = (uint16_t)req->arg;
		if (type >= DMR_PRIVACY_NUM_TYPES || (type != DMR_PRIVACY_OFF && !dmrPrivacyKeyIsValid(type, key))) {
			break;
		}
		sm->privacy_type = type;
		sm->privacy_key = type != DMR_PRIVACY_OFF ? key : 0;
		if (sm->dmr_running) {
			dmr_configure(sm);
		}
		changed(sm);
		break;
	}
#endif
	case RREQ_HS_ENTER:
		hotspot_enter(sm, (uint32_t)req->arg, now_ms);
		break;
	case RREQ_HS_FREQ:
		hotspot_freq(sm, (uint32_t)req->arg, req->arg2);
		break;
	case RREQ_HS_POWER:
		sm->hs_power = radio_sm_hotspot_power((uint8_t)req->arg);
		hotspot_arm(sm);
		break;
	case RREQ_HS_LEAVE:
		hotspot_leave(sm, now_ms);
		break;
#if CONFIG_HOTSPOT
	case RREQ_HS_CWID:
		cw_start(sm, now_ms);
		break;
#endif
#endif
#if CONFIG_DMR_ROAMING
	case RREQ_ROAM:
		roam_request(sm, (uint8_t)req->arg, now_ms);
		break;
#endif
	case RREQ_POWER_OFF:
		sm->power_off_req = true;
		sm->power_off_busy = CONFIG_DMR_DATA_CORE;	// data_tick clears it once the deregistration is out
		break;
	case RREQ_RELOAD:
#if CONFIG_APRS
		aprs_reload();
#endif
		if (busy || sm->ptt) {
			sm->reload_pending = true;	// after the transmission or the hotspot (radio_sm_poll)
		} else {
			reload(sm, now_ms);
		}
		break;
#if CONFIG_SCAN
	case RREQ_SCAN:
		scan_request(sm, req, now_ms);
		break;
	case RREQ_SCAN_VFO:
		if (!busy && !sm->ptt) {
			scan_start(sm, SCAN_VFO, (uint32_t)req->arg, req->arg2, 0, now_ms);
		}
		break;
#endif
	case RREQ_BEEP:
		if (req->arg != BEEP_KEY || sm->set.key_beep) {
			beep(sm, (uint8_t)req->arg);
		}
		break;
#if CONFIG_FM_TX_TONES
	case RREQ_TONE_BURST:
		tone_burst(sm, req->arg != 0, now_ms);
		break;
#endif
	default:
		break;
	}
	persist(sm, now_ms);
}

// Follow a volume knob the MCU reads (soft volume): pass changes of 2 % or more, and the ends exactly
static void follow_volume(radio_sm_t *sm)
{
	if (board.radio->volume == NULL || board.power == NULL || board.power->volume == NULL) {
		return;
	}
	int v = board.power->volume();
	if (v < 0) {
		return;
	}
	int d = v - sm->volume_sent;
	if (sm->volume_sent < 0 || d >= 2 || d <= -2 || ((v == 0 || v == 100) && v != sm->volume_sent)) {
		sm->volume_sent = (int16_t)v;
		board.radio->volume((uint8_t)v);
	}
}

// Battery monitor: a reading a second, none during or just after a transmission
static void battery_poll(radio_sm_t *sm, uint32_t now_ms)
{
	bool tx = sm->view.state == RS_TX;
#if CONFIG_DMR
	tx = tx || sm->dmr_keyed;
#endif
	if (tx) {
		sm->tx_end_ms = now_ms;
		return;
	}
	if (board.power == NULL || board.power->battery_mv == NULL || now_ms - sm->batt_ms < RADIO_BATT_PERIOD_MS ||
			(sm->tx_end_ms != 0 && now_ms - sm->tx_end_ms < RADIO_BATT_TX_QUIET_MS)) {
		return;
	}
	sm->batt_ms = now_ms;
	uint16_t mv = board.power->battery_mv();
	uint8_t pct = board.power->battery_pct != NULL ? board.power->battery_pct() : 0;
	batt_event_t ev = batt_update(&sm->batt, mv, board.battery_low_mv, board.battery_critical_mv, now_ms);
	sm->view.battery_mv = mv;
	if (sm->view.battery != sm->batt.level || sm->view.battery_pct != pct) {
		sm->view.battery = sm->batt.level;
		sm->view.battery_pct = pct;
		changed(sm);
	}
	if (ev == BATT_EV_LOW) {
		beep(sm, BEEP_BATTERY_LOW);
	} else if (ev == BATT_EV_CRITICAL) {
		beep(sm, BEEP_BATTERY_CRIT);
	}
	bool off = sm->set.battery_off_s != 0 &&
		batt_critical_for(&sm->batt, now_ms) >= (uint32_t)sm->set.battery_off_s * 1000u;
	if (off != sm->view.power_off) {
		sm->view.power_off = off;
		changed(sm);
	}
}

#if CONFIG_BATTERY_SAVE
static bool save_allowed(const radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->set.rx_save == 0 || board.radio->rx_sleep == NULL || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||
			sm->view.state != RS_IDLE || sm->view.monitor || sm->ptt || sm->view.ch.no_eco) {
		return false;
	}
#if CONFIG_APRS
	if (sm->aprs_phase != APRS_RF_OFF || aprs_rf_pending()) {
		return false;
	}
#endif
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return false;
	}
#endif
#if CONFIG_VOX
	if (sm->set.vox != RADIO_VOX_OFF) {
		return false;	// VOX listens to the microphone; keep the radio fully awake
	}
#endif
	return now_ms - sm->activity_ms >= (uint32_t)sm->set.rx_save_idle_s * 1000u;
}

// Receiver duty cycling while idle on an FM channel. True: skip this poll's status and squelch (asleep, or the
// readings right after waking)
static bool save_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (!save_allowed(sm, now_ms)) {
		if (sm->save_on) {
			if (sm->save_asleep) {
				board.radio->rx_sleep(false);
			}
			sm->save_on = sm->save_asleep = false;
			sm->view.rx_sleeping = false;
			changed(sm);
		}
		return false;
	}
	if (!sm->save_on) {
		sm->save_on = true;
		sm->save_asleep = false;
		sm->save_ms = now_ms;
	}
	uint32_t t = now_ms - sm->save_ms;
	if (sm->save_asleep) {
		uint32_t sleep_ms = (uint32_t)RADIO_SAVE_SLEEP_MS << (sm->set.rx_save - 1u);	// x1, x2, x4
		if (t >= sleep_ms) {
			board.radio->rx_sleep(false);
			sm->save_asleep = false;
			sm->save_ms = now_ms;
			sm->view.rx_sleeping = false;
			changed(sm);
		}
		return true;
	}
	if (t < RADIO_SAVE_SETTLE_MS) {
		return true;
	}
	if (t >= RADIO_SAVE_WAKE_MS) {
		board.radio->rx_sleep(true);
		sm->save_asleep = true;
		sm->save_ms = now_ms;
		sm->view.rx_sleeping = true;
		changed(sm);
		return true;
	}
	return false;
}
#endif

#if CONFIG_VOX
static bool vox_enabled(const radio_sm_t *sm)
{
	if (sm->set.vox == RADIO_VOX_OFF || sm->set.vox_level == 0 || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||
			board.power == NULL || board.power->mic_level == NULL) {
		return false;
	}
	return sm->set.vox == RADIO_VOX_ALL || sm->view.ch.vox;
}

// Key on voice, unkey vox_tail_ms after it stops. Never keyed by received audio (squelch open, monitor, just after)
static void vox_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->vox_keyed) {
		if (sm->ptt || !vox_enabled(sm)) {
			sm->vox_count = 0;
			return;
		}
		bool rx_audio = sm->view.state == RS_RX || sm->view.monitor ||
			(sm->view.last_rx_ms != 0 && now_ms - sm->view.last_rx_ms < RADIO_VOX_RX_GUARD_MS);
		uint8_t level = board.power->mic_level();
		if (rx_audio || level < 100u - 8u * sm->set.vox_level) {
			sm->vox_count = 0;
			return;
		}
		if (++sm->vox_count < RADIO_VOX_ATTACK_POLLS) {
			return;
		}
		sm->vox_count = 0;
		sm->vox_quiet_ms = 0;
		ptt_down(sm, now_ms);
		sm->vox_keyed = sm->ptt;
		sm->view.vox_keyed = sm->vox_keyed;
		changed(sm);
		return;
	}
	uint8_t level = vox_enabled(sm) ? board.power->mic_level() : 0;
	if (level >= 100u - 8u * sm->set.vox_level) {
		sm->vox_quiet_ms = 0;
		return;
	}
	if (sm->vox_quiet_ms == 0) {
		sm->vox_quiet_ms = now_ms != 0 ? now_ms : 1;
	} else if (now_ms - sm->vox_quiet_ms >= sm->set.vox_tail_ms) {
		sm->vox_keyed = false;
		sm->view.vox_keyed = false;
		ptt_release(sm, true, now_ms);
		changed(sm);
	}
}
#endif

void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms)
{
	radio_status_t st;
	if ((sm->reload_pending || codeplug_serial() != sm->codeplug_serial) && !sm->hotspot && !sm->ptt &&
	    sm->view.state != RS_TX) {
		reload(sm, now_ms);
		persist(sm, now_ms);
	}
	follow_volume(sm);
	battery_poll(sm, now_ms);
#if CONFIG_BEEP
	bool shared = false;
#if CONFIG_DMR
	shared = ambeVoiceStreaming();
#endif
	beep_service(shared);
#endif
#if CONFIG_APRS
	aprs_poll(sm, now_ms);	// before battery saving: beacons are due while the receiver sleeps too
#endif
#if CONFIG_BATTERY_SAVE
	if (save_poll(sm, now_ms)) {
		outputs(sm);
		return;
	}
#endif
	board.radio->status(&st);
	sm->view.rssi_dbm = st.rssi_dbm;
	sm->view.noise = st.noise;
	int drift = sm->view.rssi_dbm - sm->rssi_published;
	if (drift >= RADIO_RSSI_DEADBAND_DB || drift <= -RADIO_RSSI_DEADBAND_DB) {
		sm->rssi_published = sm->view.rssi_dbm;
		changed(sm);
	}
#if CONFIG_APRS
	if (sm->aprs_phase != APRS_RF_OFF) {
		aprs_rf_service(sm, &st, now_ms);	// on the APRS frequency: no squelch, scan or time-out here
		outputs(sm);
		return;
	}
#endif
#if CONFIG_SCAN
	if (scan_on(&sm->scan) && !sm->ptt && sm->view.state != RS_TX && sm->view.state != RS_TX_DENIED && !sm->hotspot) {
		uint32_t steps = sm->scan.steps;
		scan_service(sm, &st, now_ms);
		if (sm->scan.steps != steps) {
			outputs(sm);
			return;		// retuned: this poll's readings belong to the last stop
		}
	}
#endif
#if CONFIG_VOX
	vox_poll(sm, now_ms);
	if (vox_enabled(sm) != sm->view.vox_on) {
		sm->view.vox_on = !sm->view.vox_on;
		changed(sm);
	}
#endif
#if CONFIG_DMR
	if (sm->hotspot) {
		// Carrier for the host's listen before talk: the noise reading against the squelch level's threshold
		uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
		sm->hs_carrier = st.noise < radio_sm_squelch_open_below(sq);
		outputs(sm);
		return;
	}
#if CONFIG_DMR_ROAMING
	roam_service(sm, &st, now_ms);
#endif
	if (sm->dmr_running) {
		return;		// the state follows the call stack (radio_sm_tick), and the stack keeps its own TX time-out
	}
#endif

	if (sm->view.state == RS_TX) {
#if CONFIG_FM_TX_TONES
		if (sm->roger_ms != 0) {
			roger_poll(sm, now_ms);
			if (sm->view.state != RS_TX) {
				return;
			}
		}
#endif
		tx_timer(sm, now_ms);
		if (sm->view.tot_s != 0 && sm->view.tx_elapsed_ms >= (uint32_t)sm->view.tot_s * 1000) {
#if CONFIG_FM_TX_TONES
			if (sm->roger_ms != 0 || sm->view.tone_burst) {
				tx_tone(0);
				sm->roger_ms = 0;
				sm->view.roger = false;
				sm->view.tone_burst = false;
			}
#endif
			board.radio->tx_off();
			deny(sm, TXDENY_TIMEOUT);	// until PTT is released
			if (!sm->ptt) {
				fm_tx_end(sm, true, now_ms);	// the roger beep ran into the time-out: nothing holds the deny
			}
		}
		return;
	}
	if (sm->view.state == RS_TX_DENIED) {
		return;
	}

	// Open on carrier (noise below the level's threshold) and the RX tone, both; the monitor overrides (outputs())
	follow_tone(sm, st.tone_ok);
	uint8_t sq = sm->view.squelch;
	uint8_t open_below = radio_sm_squelch_open_below(sq);
	bool opens = sm->view.tone_ok && (sq == 0 || st.noise < open_below);
	bool holds = sm->view.tone_ok && (sq == 0 || st.noise < open_below + RADIO_SQL_HYST_DB);

	if (sm->view.state == RS_IDLE) {
		if (opens) {
			sm->view.state = RS_RX;
			sm->close_count = 0;
			changed(sm);
		}
	} else if (holds) {
		activity(sm, now_ms);
		sm->close_count = 0;
	} else if (++sm->close_count >= RADIO_SQL_CLOSE_POLLS) {
		end_reception(sm, now_ms);
		changed(sm);
	}
	outputs(sm);
}

void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms)
{
#if CONFIG_HOTSPOT
	cw_service(sm, now_ms);
#endif
#if CONFIG_DMR
	if (sm->dmr_running) {
		dmr_call_poll(now_ms);
		dmr_follow(sm, now_ms);
		one_shot_follow(sm, now_ms);
	}
#if CONFIG_DMR_DATA_CORE
	data_tick(sm, now_ms);	// also off DMR: requests fail at once, ARS keeps its state. A list it starts is queued
				// to the modem at once (dmr_call_data_tx)
#endif
#if CONFIG_DMR_EMERGENCY
	emergency_tick(sm, now_ms);
#endif
#else
	(void)sm;
	(void)now_ms;
#endif
}
