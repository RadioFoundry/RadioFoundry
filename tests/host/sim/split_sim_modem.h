// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A simulated hal_dmr_modem_t with the split duplex operations (hal_dmr_modem.h, CONFIG_HOTSPOT_SPLIT) for host
// tests: a virtual microsecond clock the test advances; the TX half's continuous BS outbound (a slot every 30 ms from
// bs_start, two bursts queued per slot, each slot's burst and CACH recorded as "on air"); the RX half's slot clock,
// following the bursts the test injects (as the HR_C6000's passive timing follows the received signal).
#ifndef SPLIT_SIM_MODEM_H
#define SPLIT_SIM_MODEM_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_dmr_modem.h"

#define SSIM_AIR_MAX	2048
#define SSIM_Q		2	// per slot: the core keeps the driver two bursts ahead
#define SSIM_SLOT_US	30000u

typedef struct {
	uint32_t t_us;		// slot start, modem clock
	uint32_t counter;
	int8_t ts;
	dmr_tx_burst_t b;
	dmr_cach_t cach;
	bool emb_lc_valid;
	uint8_t emb_lc[9];
	bool underrun;		// the driver's queue was empty: it repeated an idle burst
} ssim_air_t;

typedef struct {
	// Clock: modem time = host time x (1 + ppm / 1e6) + offset
	uint32_t now_us;
	// Stack side
	bool started;
	dmr_config_t cfg;
	dmr_event_cb_t cb;
	void *ctx;
	int8_t locked_ts;
	unsigned rx_restarts;
	// BS outbound
	bool bs;
	bool keyed;
	bool on_air;			// keyed and the first TS1 slot has started
	dmr_bs_config_t bs_cfg;
	unsigned bs_starts, bs_stops, keyups;
	uint32_t next_tick_us;		// the next outbound slot boundary
	uint32_t counter;		// slot boundaries since bs_start
	uint32_t t_tick;
	int8_t ts_tick;			// the outbound slot that started at t_tick
	ssim_air_t q[2][SSIM_Q];
	uint8_t qn[2];
	unsigned underruns;
	ssim_air_t air[SSIM_AIR_MAX];
	unsigned air_n;
	// RX slot clock
	bool rx_clock;
	uint32_t rx_origin;		// the first injected burst's start: boundaries every 30 ms from it
	bool no_split_ops;		// the INFO "roles offered" check: a modem without the operations
} ssim_t;

extern ssim_t ssim;
extern const hal_dmr_modem_t ssim_modem;
extern const hal_dmr_modem_t ssim_modem_plain;	// the same without any split operation

void ssim_reset(void);
// Move the modem clock to t_us (wrapping arithmetic), running the outbound slots that start up to it
void ssim_advance_to(uint32_t t_us);
// A burst heard by the RX half in the slot that started at t_start_us (modem clock), reported now (call it once the
// clock is at least 30 ms past t_start_us): arg = the counter of the boundary after the burst's slot, as the
// HR_C6000 reports it. The slot clock runs every 30 ms from the first burst's start
void ssim_rx_burst(const dmr_burst_t *b, uint32_t t_start_us);

#endif
