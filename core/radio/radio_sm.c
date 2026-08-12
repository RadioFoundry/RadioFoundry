// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine: channel / zone / VFO selection, software squelch, PTT checks and the TX timer.
// No RTOS here: radio_task.c serializes the calls, and the host tests drive it directly.
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/tones.h"
#include "config.h"
#if CONFIG_DMR
#include "dmr/call/dmr_call.h"
#include "dmr/ambe/ambeVoice.h"
#endif
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmrDataService.h"
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

static uint8_t default_power(void)
{
	if (board.power_count == 0) {
		return 0;
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
	}
	sm->view.temp = t;
	sm->view.temporary = t != 0;
}

// Back to the channel's own settings. Returns whether the receiver needs retuning (bandwidth or RX tone changed)
static bool clear_temporary(radio_sm_t *sm)
{
	bool retune = (sm->view.temp & (RADIO_TEMP_BW | RADIO_TEMP_RX_TONE)) != 0;
	sm->temp_power = false;
	sm->temp_squelch = false;
	sm->view.power = channel_power(&sm->view.ch);
	sm->view.squelch = sm->base_squelch;
	if (!sm->view.vfo) {
		sm->view.ch.bw = sm->base_bw;
		sm->view.ch.rx_tone = sm->base_rx_tone;
		sm->view.ch.tx_tone = sm->base_tx_tone;
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
	clear_temporary(sm);
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

static uint8_t tx_check(const radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
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
	return TXDENY_NONE;
}

static void deny(radio_sm_t *sm, uint8_t reason)
{
	sm->view.state = RS_TX_DENIED;
	sm->view.tx_deny = reason;
	outputs(sm);
	changed(sm);
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

static void find_name(const radio_sm_t *sm, uint32_t id, bool group, char *out)
{
	out[0] = '\0';
	for (uint8_t i = 0; i < sm->name_count; i++) {
		if (sm->names[i].id == id && sm->names[i].group == group) {
			memcpy(out, sm->names[i].name, CODEPLUG_NAME_LEN + 1);
			return;
		}
	}
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

static void dmr_configure(radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	dmr_call_config_t cfg;
	dmr_call_config_default(&cfg);
	cfg.own_id = ch->dmr_id != 0 ? ch->dmr_id : sm->own_id;
	cfg.cc = ch->color_code & 0x0Fu;
	cfg.ts = sm->talk_ts != 0 ? sm->talk_ts : (ch->timeslot == 2 ? 2 : 1);
	cfg.rmo = ch->tx_hz != 0 && ch->rx_hz != ch->tx_hz && !ch->dmo;
	cfg.talk_target.id = sm->talk_id;
	cfg.talk_target.private_call = sm->talk_private;
	cfg.listen.hear_all = sm->view.monitor;
	cfg.listen.groups = sm->listen;
	cfg.listen.group_count = sm->listen_count;
	cfg.tot_s = sm->view.tot_s;
	dmr_call_configure(&cfg);
	sm->view.dmr.own_id = cfg.own_id;
	sm->view.dmr.hear_all = cfg.listen.hear_all;
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

static void copy_call(const radio_sm_t *sm, const dmr_caller_t *c, radio_dmr_call_t *out)
{
	out->src = c->src;
	out->dst = c->dst;
	out->group = c->group;
	out->to_us = c->to_us;
	out->encrypted = c->encrypted;
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
		uint32_t elapsed = now_ms - sm->tx_start_ms;
		if (elapsed / 1000 != sm->view.tx_elapsed_ms / 1000) {
			changed(sm);
		}
		sm->view.tx_elapsed_ms = elapsed;
	}
	outputs(sm);
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
	(void)ctx;
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
	return sm->dmr_running && !sm->hotspot && sm->view.ch.mode == RADIO_MODE_DMR && !sm->view.vfo &&
		tx_check(sm) == TXDENY_NONE && sm->view.dmr.own_id != 0;
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
	dmr_data_source_t src = { .next = data_next, .done = data_done, .ctx = NULL,
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

static void data_init(radio_sm_t *sm)
{
	dmrDataEnv_t env = { .txStart = data_tx_start, .channelClear = data_channel_clear,
			     .inRxGroup = data_in_rx_group, .ctx = sm };
	// No auto-transmit features until the user turns them on (docs/ux/principles.md): no ARS server, no radio
	// check auto-reply. Call alerts are acknowledged (that is what makes them work); LRRP answers "no fix" without
	// a position source
	dmrDataSettings_t set = { .arsServer = 0, .radioCheckReply = false, .callAlertAck = true, .lrrpReply = true,
				  .hasGps = false };
	data_tx.list = NULL;
	dmrDataServiceInit(&env);
	dmrDataServiceSetSettings(&set);
	dmr_call_set_data_rx(data_rx, NULL);
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
}
#endif

#else
static void dmr_begin(radio_sm_t *sm) { (void)sm; }
static void dmr_end(radio_sm_t *sm) { (void)sm; }
#endif

static void ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->ptt) {
		return;
	}
	sm->ptt = true;
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

static void ptt_up(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->ptt) {
		return;
	}
	sm->ptt = false;
#if CONFIG_DMR
	if (sm->dmr_running) {
		dmr_call_ptt(false);
		dmr_call_poll(now_ms);
		if (sm->view.state == RS_TX) {
			dmr_follow(sm, now_ms);	// on air until the superframe ends and the modem reports TX done
			return;
		}
	}
	if (sm->view.state == RS_TX && !sm->dmr_keyed) {
		board.radio->tx_off();
	}
#else
	if (sm->view.state == RS_TX) {
		board.radio->tx_off();
	}
#endif
	if (sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED) {
		sm->view.state = RS_IDLE;
		sm->view.tx_deny = TXDENY_NONE;
		sm->close_count = 0;
		reset_tone(sm);
		outputs(sm);
		changed(sm);
	}
	if (sm->retune_pending) {
		sm->retune_pending = false;
		tune(sm, now_ms);	// a temporary bandwidth or RX tone was undone during TX
	}
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
	sm->dmr_keyed = board.radio->tx(sm->hs_tx_hz, RADIO_BW_12K5, sm->hs_power, &none);
}

static void hotspot_enter(radio_sm_t *sm, uint32_t rx_hz, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (sm->hotspot || board.dmr == NULL || sm->ptt || sm->view.state == RS_TX || sm->dmr_keyed) {
		return;		// refused for now: the hotspot asks again
	}
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

void radio_sm_init(radio_sm_t *sm, uint32_t now_ms)
{
	memset(sm, 0, sizeof(*sm));
	sm->audio_out = -1;
	sm->green_out = -1;
	sm->red_out = -1;
	sm->volume_sent = -1;
	sm->base_squelch = RADIO_SQUELCH_DEFAULT;
	sm->view.squelch = RADIO_SQUELCH_DEFAULT;
	sm->view.tot_s = RADIO_TOT_DEFAULT_S;
	sm->view.vfo_step_hz = RADIO_VFO_STEP_HZ;
	sm->view.source = codeplug_source();

	memset(&sm->vfo_ch, 0, sizeof(sm->vfo_ch));
	strcpy(sm->vfo_ch.name, "VFO");
	sm->vfo_ch.mode = RADIO_MODE_FM;
	sm->vfo_ch.bw = RADIO_BW_12K5;
	sm->vfo_ch.power = CODEPLUG_POWER_DEFAULT;
	sm->vfo_ch.timeslot = 1;

	sm->board_mode = RADIO_MODE_FM;
	board.radio->set_mode(RADIO_MODE_FM);

#if CONFIG_DMR
	sm->own_id = codeplug_dmr_id();
	if (sm->own_id == 0) {
		sm->own_id = RADIO_DMR_ID_DEFAULT;
	}
	sm->codec = board.vocoder != NULL && board.vocoder->present != NULL && board.vocoder->present();
	sm->view.dmr.codec = sm->codec;
	sm->view.dmr.own_id = sm->own_id;
	sm->hs_power = default_power();
	dmr_call_init(board.dmr);	// board.dmr NULL: DMR channels receive nothing and refuse PTT
	ambeVoiceInit(board.audio, board.vocoder);
	dmr_call_set_voice(ambeVoiceSink(), ambeVoiceSource());
#if CONFIG_DMR_DATA_CORE
	data_init(sm);
#endif
#endif

	// Zone 0 and its first channel; if zone 0 has none, the first zone that has one
	uint16_t zones = codeplug_zone_count();
	for (uint16_t zi = 0; zi < zones; zi++) {
		uint16_t pos;
		channel_t ch;
		if (read_zone(zi, &sm->zone) && find_channel(&sm->zone, 0, 1, &pos, &ch)) {
			set_zone(sm, zi, &sm->zone);
			set_channel(sm, pos, &ch);
			sm->have_channels = true;
			break;
		}
	}
	if (!sm->have_channels) {
		memset(&sm->zone, 0, sizeof(sm->zone));
		if (VFO_AVAILABLE) {
			enter_vfo(sm, now_ms);
		} else {
			strcpy(sm->view.ch.name, "No channels");	// built without a VFO and nothing programmed: nothing to tune
		}
	} else {
		tune(sm, now_ms);
	}
	sm->rssi_published = sm->view.rssi_dbm;
}

void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	bool busy = sm->view.state == RS_TX || sm->hotspot;	// no retuning while transmitting or a hotspot
	int dir = req->arg > 0 ? 1 : -1;
	int steps = req->arg > 0 ? req->arg : -req->arg;

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
		break;
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
#endif
	default:
		break;
	}
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

void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms)
{
	radio_status_t st;
	follow_volume(sm);
	board.radio->status(&st);
	sm->view.rssi_dbm = st.rssi_dbm;
	sm->view.noise = st.noise;
	int drift = sm->view.rssi_dbm - sm->rssi_published;
	if (drift >= RADIO_RSSI_DEADBAND_DB || drift <= -RADIO_RSSI_DEADBAND_DB) {
		sm->rssi_published = sm->view.rssi_dbm;
		changed(sm);
	}
#if CONFIG_DMR
	if (sm->hotspot) {
		// Carrier for the host's listen before talk: the noise reading against the squelch level's threshold
		uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
		sm->hs_carrier = st.noise < radio_sm_squelch_open_below(sq);
		outputs(sm);
		return;
	}
	if (sm->dmr_running) {
		return;		// the state follows the call stack (radio_sm_tick), and the stack keeps its own TX time-out
	}
#endif

	if (sm->view.state == RS_TX) {
		uint32_t elapsed = now_ms - sm->tx_start_ms;
		if (elapsed / 1000 != sm->view.tx_elapsed_ms / 1000) {
			changed(sm);
		}
		sm->view.tx_elapsed_ms = elapsed;
		if (sm->view.tot_s != 0 && elapsed >= (uint32_t)sm->view.tot_s * 1000) {
			board.radio->tx_off();
			deny(sm, TXDENY_TIMEOUT);	// until PTT is released
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
		sm->close_count = 0;
	} else if (++sm->close_count >= RADIO_SQL_CLOSE_POLLS) {
		end_reception(sm, now_ms);
		changed(sm);
	}
	outputs(sm);
}

void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms)
{
#if CONFIG_DMR
	if (sm->dmr_running) {
		dmr_call_poll(now_ms);
		dmr_follow(sm, now_ms);
	}
#if CONFIG_DMR_DATA_CORE
	data_tick(sm, now_ms);	// also off DMR: requests fail at once, ARS keeps its state. A list it starts is queued
				// to the modem at once (dmr_call_data_tx)
#endif
#else
	(void)sm;
	(void)now_ms;
#endif
}
