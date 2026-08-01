// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine: channel / zone / VFO selection, software squelch, PTT checks and the TX timer.
// No RTOS here: radio_task.c serializes the calls, and the host tests drive it directly.
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"

// Simplex frequencies tried, in order, for the VFO when there is nothing better to start from
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

static void set_temporary(radio_sm_t *sm)
{
	sm->view.temporary = sm->temp_power || sm->temp_squelch;
}

static void clear_temporary(radio_sm_t *sm)
{
	sm->temp_power = false;
	sm->temp_squelch = false;
	sm->view.power = channel_power(&sm->view.ch);
	sm->view.squelch = sm->base_squelch;
	set_temporary(sm);
}

// Speaker and LEDs follow the state; only changes go to the board
static void outputs(radio_sm_t *sm)
{
	bool tx = sm->view.state == RS_TX;
	bool open = !tx && (sm->view.monitor || (sm->view.state == RS_RX && sm->view.ch.mode == RADIO_MODE_FM));

	if (sm->audio_out != (int8_t)open) {
		sm->audio_out = (int8_t)open;
		board.radio->audio(open);
	}
	if (sm->green_out != (int8_t)open) {
		sm->green_out = (int8_t)open;
		board.power->led(LED_GREEN, open);
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
static void tune(radio_sm_t *sm, uint32_t now_ms)
{
	end_reception(sm, now_ms);
	board.radio->rx(sm->view.ch.rx_hz, (radio_bw_t)sm->view.ch.bw, &sm->view.ch.rx_tone);
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
	sm->view.vfo = false;
	sm->view.zone_pos = pos;
	sm->view.channel_number = sm->zone.channels[pos];
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
	if (ch->mode != RADIO_MODE_FM) {
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

static void ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->ptt) {
		return;
	}
	sm->ptt = true;
	end_reception(sm, now_ms);

	uint8_t reason = tx_check(sm);
	if (reason != TXDENY_NONE) {
		deny(sm, reason);
		return;
	}
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

static void ptt_up(radio_sm_t *sm)
{
	if (!sm->ptt) {
		return;
	}
	sm->ptt = false;
	if (sm->view.state == RS_TX) {
		board.radio->tx_off();
	}
	if (sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED) {
		sm->view.state = RS_IDLE;
		sm->view.tx_deny = TXDENY_NONE;
		sm->close_count = 0;
		outputs(sm);
		changed(sm);
	}
}

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

	board.radio->set_mode(RADIO_MODE_FM);

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
		enter_vfo(sm, now_ms);
	} else {
		tune(sm, now_ms);
	}
	sm->rssi_published = sm->view.rssi_dbm;
}

void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	bool busy = sm->view.state == RS_TX;	// no retuning while transmitting
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
		if (busy || (req->arg != 0) == sm->view.vfo) {
			break;
		}
		if (req->arg != 0) {
			enter_vfo(sm, now_ms);
		} else {
			leave_vfo(sm, now_ms);
		}
		break;
	case RREQ_VFO_SET:
		if (busy || req->arg <= 0 || !in_band((uint32_t)req->arg, false)) {
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
			ptt_up(sm);
		}
		break;
	case RREQ_MONITOR:
		if (sm->view.monitor != (req->arg != 0)) {
			sm->view.monitor = req->arg != 0;
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
			clear_temporary(sm);
			changed(sm);
		}
		break;
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

	uint8_t sq = sm->view.squelch;
	uint8_t open_below = radio_sm_squelch_open_below(sq);
	bool opens = st.tone_ok && (sq == 0 || st.noise < open_below);
	bool holds = st.tone_ok && (sq == 0 || st.noise < open_below + RADIO_SQL_HYST_DB);

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
