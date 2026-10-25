// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Standalone SFR (see radio_sfr.h)
#include "config.h"
#if CONFIG_SFR	// the whole file
#include <string.h>
#include "radio/radio_sfr.h"

#define run sfr_runner	// shared with the hotspot's SFR mode (sfrRunner.h)
static bool running;

// Modem events, copied by the driver's task (the callback) and taken by the radio task: single producer, single
// consumer, as in the call stack
typedef struct {
	uint8_t kind;
	bool has_burst;
	dmr_burst_t burst;
} sfr_ev_t;

static sfr_ev_t evq[RADIO_SFR_EVQ_DEPTH] RFOUNDRY_CPU_RAM;	// CPU only: the board's CPU RAM where it has one
static uint8_t evq_head, evq_tail;
static uint32_t evq_drops;

uint8_t radio_sfr_channel(const channel_t *ch, bool vfo, sfrConfig_t *cfg)
{
	sfrChannelSettings_t s;

	if (vfo || ch->mode != RADIO_MODE_DMR || !sfrChannelDecode(ch->sfr_flag1, ch->sfr_a, ch->sfr_b, &s)) {
		return SFR_ROLE_OFF;
	}
	// One frequency: a repeater needs it to send on; a user may have no TX frequency (receive only)
	bool simplex = ch->tx_hz == ch->rx_hz || (ch->tx_hz == 0 && sfrRoleIsUser(s.role));
	if (!simplex || (sfrRoleIsRepeater(s.role) && ch->rx_only)) {
		return SFR_ROLE_OFF;
	}
	uint32_t tot15 = (ch->tot_s + 14u) / 15u;
	sfrConfigBuild(&s, ch->color_code, (uint8_t)(tot15 > 255u ? 255u : tot15), cfg);
	return s.role;
}

void radio_sfr_user(const sfrConfig_t *cfg, uint8_t *ts, uint8_t *tx_sync, uint8_t *rx_sync, int8_t *cc2)
{
	*ts = cfg->inboundSlot ? 2 : 1;
	if (cfg->framing == SFR_FRAMING_TS) {
		*tx_sync = cfg->inboundSlot ? DMR_SYNC_TS2 : DMR_SYNC_TS1;
		*rx_sync = cfg->inboundSlot ? DMR_SYNC_TS1 : DMR_SYNC_TS2;
	} else {
		*tx_sync = DMR_SYNC_MS;
		*rx_sync = cfg->framing == SFR_FRAMING_BS ? DMR_SYNC_BS : DMR_SYNC_MS;
	}
	*cc2 = cfg->outboundCC != cfg->inboundCC ? (int8_t)cfg->outboundCC : -1;
}

static void on_event(const dmr_event_t *ev, void *ctx)
{
	(void)ctx;
	if (ev->kind == DMR_EV_SLOT_TICK) {
		return;		// the runner keeps no slot clock of its own
	}
	uint8_t head = evq_head;
	uint8_t tail = __atomic_load_n(&evq_tail, __ATOMIC_ACQUIRE);

	if ((uint8_t)(head - tail) >= RADIO_SFR_EVQ_DEPTH) {
		evq_drops++;
		return;
	}
	sfr_ev_t *e = &evq[head & (RADIO_SFR_EVQ_DEPTH - 1)];

	e->kind = ev->kind;
	e->has_burst = ev->kind == DMR_EV_BURST_RX && ev->burst != NULL;
	if (e->has_burst) {
		e->burst = *ev->burst;
	}
	__atomic_store_n(&evq_head, (uint8_t)(head + 1), __ATOMIC_RELEASE);
}

bool radio_sfr_start(const hal_dmr_modem_t *modem, const sfrConfig_t *cfg, uint16_t beacon_s, uint8_t beacon_len_s,
		     bool keep_stats, uint32_t now_ms)
{
	sfrRunnerStats_t stats = run.stats;	// (the runner's state survives its stop)

	radio_sfr_stop();
	if (modem == NULL) {
		return false;
	}
	dmr_config_t mc = {
		.own_id = 0,			// the repeater passes every call
		.cc = (int8_t)cfg->inboundCC,
		.access = DMR_ACCESS_IMPOLITE,	// the runner keys over its own hang and RF priority rules
		.repeater = false,		// DMO framing: SFR channels never switch to RMO on BS sync (sfr.md 3 item 6)
	};
	evq_head = evq_tail = 0;
	if (!modem->start(&mc, on_event, NULL)) {
		return false;
	}
	modem->rx_lock_slot(0);			// both slots: the inbound one, and ours to recognize our echo
	modem->set_cc_filter(mc.cc);
	sfrRunnerStart(&run, modem, cfg, NULL);
	if (keep_stats) {
		run.stats = stats;
	}
	sfrRunnerSetBeacon(&run, beacon_s, beacon_len_s, now_ms);
	running = true;
	return true;
}

void radio_sfr_stop(void)
{
	if (!running) {
		return;
	}
	const hal_dmr_modem_t *modem = run.modem;

	sfrRunnerStop(&run);
	modem->stop();
	running = false;
}

bool radio_sfr_running(void)
{
	return running;
}

void radio_sfr_poll(uint32_t now_ms)
{
	if (!running) {
		return;
	}
	while (evq_tail != __atomic_load_n(&evq_head, __ATOMIC_ACQUIRE)) {
		sfr_ev_t *e = &evq[evq_tail & (RADIO_SFR_EVQ_DEPTH - 1)];

		sfrRunnerEvent(&run, e->kind, e->has_burst ? &e->burst : NULL, now_ms);
		__atomic_store_n(&evq_tail, (uint8_t)(evq_tail + 1), __ATOMIC_RELEASE);
	}
	sfrRunnerPoll(&run, now_ms);
}

void radio_sfr_get_view(radio_sfr_view_t *v)
{
	memset(v, 0, sizeof(*v));
	v->evq_drops = evq_drops;
	v->stats = run.stats;	// the last role's counters also while stopped (its CW ID)
	if (!running) {
		return;
	}
	const sfrConfig_t *c = sfrRunnerConfig(&run);

	v->on = true;
	v->keyed = run.state == SFR_RUN_KEYED || run.state == SFR_RUN_ENDING;
	v->inbound_busy = sfrRunnerInboundBusy(&run);
	v->role = c->role;
	v->inbound_slot = c->inboundSlot;
	v->framing = c->framing;
}

#endif	// CONFIG_SFR
