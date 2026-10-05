// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The single frequency repeater (SFR) on any DMR modem behind hal_dmr_modem_t: the repeater roles' runtime around
// sfrEngine_t (core/dmr/sfr.h). It takes the modem's received bursts, filters them (inbound slot, own echo, late
// entry, checksums while keyed), feeds the engine, and keys the modem for the outbound slot, asking the engine for one
// burst per outbound slot and keeping two queued in the modem. Optionally every burst heard on the inbound slot goes
// up to a host (the MMDVM hotspot's SFR mode, core/hotspot/hotspotSfr.c).
//
// What the driver must do for it (docs/spec/sfr.md 3; the HR_C6000's notes are docs/spec/gotchas.md 14):
//   - keep receiving (and reporting bursts) while it transmits: the inbound slot is heard between our bursts;
//   - tx_begin(DMR_TIMING_ALIGNED, ts): send in the slot opposite to the bursts being received, in step with them;
//     DMR_TIMING_OWN with a ts: its own slot clock, our bursts on every other slot;
//   - honor dmr_tx_burst_t.sync_override (the outbound sync: MS, BS, or the direct mode sync of the outbound slot);
//   - send DMR_BURST_IDLE bursts with the payload given (the ETSI idle message) and no CRC of its own;
//   - DMR_EV_TX_SLOT_STARTED once per burst taken from its queue, DMR_EV_TX_DONE after tx_end has drained it;
//   - tx_listen (optional, called after every tx_begin): receive the inbound slot between our bursts.
//
// No RTOS, no board headers: the host link task (hotspot) or the radio task (repeater role) runs it, and the host
// tests drive it against the simulated modem.
#ifndef ODR_SFR_RUNNER_H
#define ODR_SFR_RUNNER_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_dmr_modem.h"
#include "dmr/sfr.h"

#define SFR_RUNNER_IN_MODEM	2	// bursts kept queued in the modem while keyed (one on air, one waiting)
#define SFR_RUNNER_ECHO_MS	250U	// a burst heard that went on air less than this long ago is our own echo (gotchas 14.2)
#define SFR_RUNNER_ECHO_NUM	8

// Every burst heard on the inbound slot, for a host. header_lc: send a voice LC header made from this LC before it
// (a call heard without its header), else NULL. A voice burst's superframe LC is sfrRunnerCallLc()
typedef struct {
	void (*burst)(void *ctx, const sfrBurst_t *b, const uint8_t *header_lc, int16_t rssi_dbm);
	void (*lost)(void *ctx);	// the call stopped without its terminator (send DMR_LOST)
	void *ctx;
} sfrRunnerUplink_t;

typedef struct {
	uint32_t rx_bursts;		// bursts reported by the modem
	uint32_t rx_repeated;		// queued for the outbound slot
	uint32_t rx_ignored;		// not for the repeater (engine: colour code, sync, idle; or the outbound slot)
	uint32_t rx_ignored_slot;	// of those, reported on the outbound slot
	uint32_t rx_bad_crc;		// data bursts whose checksum failed
	uint32_t rx_duplicate, rx_tot, rx_full;
	uint32_t rx_echo;		// our own bursts heard back
	uint32_t rx_late_held;		// a voice A outside a call, waiting for its B
	uint32_t rx_lc_rebuilt;		// headers / terminators heard while keyed, rebuilt from the call's LC
	uint32_t tx_bursts, tx_idle, tx_starts, tx_rejected;
	uint32_t tx_beacons;		// idle beacons keyed (sfrRunnerSetBeacon)
	uint32_t up_bursts, up_lost;
	uint8_t last_rx;		// sync (bits 7-6, sfrSync_t), data type or 0x0F voice (3-0)
	uint8_t last_rx_cc;		// colour code, 0x80 checksum good
} sfrRunnerStats_t;

typedef enum { SFR_RUN_OFF, SFR_RUN_IDLE, SFR_RUN_KEYED, SFR_RUN_ENDING } sfrRunState_t;

typedef struct {
	uint8_t hash[4];
	uint8_t kind, type;
	uint32_t ms;
} sfrRunnerSent_t;

typedef struct {
	const hal_dmr_modem_t *modem;
	sfrEngine_t eng;
	bool has_up;
	sfrRunnerUplink_t up_ops;
	sfrUplink_t up;
	sfrLateEntry_t late;
	uint8_t state;			// sfrRunState_t
	uint8_t in_modem;		// bursts queued in the modem, not yet on air
	bool pending;			// a burst the modem refused to start with: tried again at the next poll
	sfrBurst_t pending_burst;
	uint8_t emb_lc[SFR_LC_LENGTH];	// the embedded LC the modem has
	bool voice_since_header;	// in the inbound call (sfrLcBurstGuess)
	sfrRunnerSent_t sent[SFR_RUNNER_ECHO_NUM];	// ms 0: queued, not on air yet
	uint8_t sent_next;
	uint8_t fifo[4];		// what the modem holds, in order: an index into sent[], 0xFF idle
	uint8_t fifo_head, fifo_count;
	// The idle beacon (docs/spec/dmr-roaming.md 7): idle bursts for beacon_len_ms every beacon_ms while nothing is
	// repeated, so roaming radios can measure this repeater when it carries no traffic. 0 = off
	uint32_t beacon_ms, beacon_len_ms;
	uint32_t beacon_next;		// the next one is due
	uint32_t beacon_until;		// the one on the air ends
	bool beaconing;
	sfrRunnerStats_t stats;
} sfrRunner_t;

// The modem is the caller's, started in DMR mode (DMO framing, every slot received, the inbound colour code). up: NULL
// for no host
void sfrRunnerStart(sfrRunner_t *r, const hal_dmr_modem_t *modem, const sfrConfig_t *cfg, const sfrRunnerUplink_t *up);
// Unkeys at once (tx_abort) and drops everything; the modem stays the caller's
void sfrRunnerStop(sfrRunner_t *r);
bool sfrRunnerRunning(const sfrRunner_t *r);
// A modem event (DMR_EV_BURST_RX with its burst, DMR_EV_TX_SLOT_STARTED, DMR_EV_TX_DONE, DMR_EV_TX_REJECTED), at task
// level, in the order the modem gave them
void sfrRunnerEvent(sfrRunner_t *r, uint8_t kind, const dmr_burst_t *b, uint32_t now_ms);
// Often (every few ms): keys up, keeps the modem fed, times out the uplink
void sfrRunnerPoll(sfrRunner_t *r, uint32_t now_ms);

// From the host (the hotspot) for the outbound slot: 0, or the MMDVM NAK reason (sfrEngineNetBurst)
uint8_t sfrRunnerNetBurst(sfrRunner_t *r, const sfrBurst_t *b, uint32_t now_ms);
void sfrRunnerNetAbort(sfrRunner_t *r);

uint8_t sfrRunnerSpace(const sfrRunner_t *r);
bool sfrRunnerTransmitting(const sfrRunner_t *r);	// keyed, ending, or about to key
bool sfrRunnerInboundBusy(const sfrRunner_t *r);	// an inbound call is being repeated (MMDVM status 0x80)
const uint8_t *sfrRunnerCallLc(const sfrRunner_t *r);	// the inbound call's LC (all 0 until known)
const sfrConfig_t *sfrRunnerConfig(const sfrRunner_t *r);

// The idle beacon: every interval_s (0 = off), len_s of idle bursts on the outbound slot when nothing is being
// repeated. The schedule starts now (now_ms 0: at the next poll); without network time it is not in step with other
// repeaters' beacons
void sfrRunnerSetBeacon(sfrRunner_t *r, uint16_t interval_s, uint8_t len_s, uint32_t now_ms);

// The hrcSfrDebug-style switches that apply to any modem: data calls repeated as heard (no hold)
void sfrRunnerSetNoDataHold(sfrRunner_t *r, bool on);

#endif
