// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 as an air interface provider (hal_air.h) and a receive tap (hal_baseband.h): docs/spec/modem-hrc7000.md
// 10. While a tap or a session is open it owns the chip in place of the DMR slot engine (halted): the board routes the
// modem interrupts here and runs hrc7000_air_poll from its modem task, which calls the session's callback.
//
// Receive (10.1): the chip's FM receive copy (8.5) on the RF chip's DMR set-up. WORK_MODE 0x80 with the band-pass off
// (FM_BANDWIDTH b6) and FM playback off, so the copy carries the discriminator and nothing reaches the speaker. Each
// 256-sample half (32 ms at 8 kHz) raises system interrupt bit 14; the interrupt copies it into a ring. A session runs
// the soft modem (core/air/air_softmodem.h) on the ring at task level and hands its chunks out as AIR_EV_RX.
//
// Transmit (10.2): the physical layer in timeslot mode (WORK_MODE 0x22) with every slot sent, own TX timing
// (LAYER2_CONTROL 0x82): one carrier with the slots back to back. The chip sends
// the two TX RAM halves (0x000, 0x024, 36 bytes each) in alternate slots and reads a half while it sends it, so each slot
// boundary (the timeslot interrupts or PHY_RDY, whichever comes first) writes only the half the next slot reads. The PA
// is keyed at the first RF TX interrupt and held to the end. Deviation: the modulation bytes of RF_MOD_BIAS (b15:0),
// which the board writes at hal_radio_t.tx(), are scaled to the profile at tx_begin and put back after.
#ifndef HRC7000_AIR_H
#define HRC7000_AIR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hrc7000.h"
#include "hrc7000_dmr.h"
#include "hal_air.h"
#include "hal_baseband.h"

#if CONFIG_AIR
#include "air/air_softmodem.h"
#endif

#define HRC7000_AIR_SYMS	144	// one TX RAM half: 30 ms at 4800 symbols/s
#define HRC7000_AIR_HALF_BYTES	36
#define HRC7000_AIR_RING	512	// receive samples: two copy halves, 64 ms (a power of two)
#define HRC7000_AIR_TXQ		4	// transmit chunks, 120 ms (a power of two)
#define HRC7000_AIR_EVQ		8	// events ISR -> task (a power of two)
#define HRC7000_AIR_TX_LEAD_MS	60	// a chunk is taken about 5 ms before its slot is on air; one more slot of margin
#define HRC7000_AIR_TX_LEAD_SLOTS 2	// idle slots after the RF TX interrupt before the first chunk: the radio's RF chain
					// carries no modulation for about 45 ms after the carrier (modem-hrc7000.md 10.2)
#define HRC7000_AIR_TIME_US	2000	// t_us accuracy: interrupt latency; the receive delay through the chips is not measured
#define HRC7000_AIR_RATE_HZ	8000u
#define HRC7000_AIR_RX_LO_HZ	0u	// with the band-pass off (10.1: not measured)
#define HRC7000_AIR_RX_HI_HZ	3000u	// the copy's low-pass (10.1: not measured)
#define HRC7000_AIR_DMR_DEV_HZ	1820u	// outer deviation the board's DMR modulation bytes give (measured, 10.3), not
					// DMR's nominal 1944 Hz

// The large buffers. The board hands over memory the DMR slot engine leaves unused while the chip is held (its event
// queue), so a session costs no RAM of its own: hence may_alias
typedef struct {
	int16_t ring[HRC7000_AIR_RING];
	air_tx_chunk_t txq[HRC7000_AIR_TXQ];
} __attribute__((may_alias)) hrc7000_air_buf_t;

typedef struct {
	uint8_t kind;		// air_event_kind_t
	uint32_t t_us;
} hrc7000_air_ev_t;

typedef struct {
	uint32_t halves;	// receive halves copied
	uint32_t overruns;	// ... dropped: the ring was full (the task was behind)
	uint32_t no_half;	// copy interrupts with FM_ADDR_SW naming neither half
	uint32_t chunks;	// AIR_EV_RX handed out
	uint32_t ev_overflow;	// events dropped
	uint32_t tx_slots;	// TX RAM halves written
	uint32_t tx_underruns;	// ... of those, idle for want of a chunk mid-transmission
	uint32_t tx_done;
	uint32_t stalls;	// no slot boundary for 200 ms while transmitting: dropped
	uint32_t modem_us;	// time in the soft modem ...
	uint32_t modem_span_us;	// ... for this much signal
} hrc7000_air_stats_t;

// Receive is the rest state; the transmit states follow the slots: START (set up, the first slot not yet taken), RUN,
// LAST (the AIR_TX_LAST chunk is on air), TAIL (one idle slot after it), then the PA drop and TX_DONE
enum { HRC7000_AIR_RX, HRC7000_AIR_START, HRC7000_AIR_RUN, HRC7000_AIR_LAST, HRC7000_AIR_TAIL };

#if CONFIG_AIR

// may_alias: it lives in the slot engine's event queue too
typedef struct hrc7000_air {
	hrc7000_t *dev;
	const hrc7000_dmr_ops_t *ops;
	hrc7000_air_buf_t *buf;
	air_softmodem_t *sm;

	// The receive copy (a tap or a session's receiver)
	bool tap;		// the hal_baseband_t tap is open (no session)
	volatile bool copy;	// the chip is in the copy set-up and the interrupt takes halves
	bool chip_saved;
	uint32_t saved_mask, saved_bw, saved_coef;	// SYS_INTERP_MASK, FM_BANDWIDTH, FM_DEV_COEF before the copy set-up
	volatile uint32_t head, tail;	// samples, free running: the interrupt moves head, the task tail
	uint32_t t_half[2];	// the time of each ring half's first sample
	bool gap;		// the next half taken does not follow the ring's last (lost or a new set-up)
	bool gap_half[2];	// ... per ring half as taken: the session's receiver starts over there

	// The session
	bool open;
	const air_profile_t *p;
	air_event_cb_t cb;
	void *cb_ctx;
	uint32_t fit;
	hrc7000_air_ev_t evq[HRC7000_AIR_EVQ];
	volatile uint8_t ev_head, ev_tail;
	bool suspended;		// the chip left DMR under the session: no receive until it is back

	// Transmit
	volatile uint8_t tx;	// HRC7000_AIR_RX / START / RUN / LAST / TAIL
	uint8_t half;		// the TX RAM half the next slot reads
	uint8_t lead;		// idle slots still to send once the PA is up, before the first chunk
	bool last_due;		// the last byte of half last_half waits for the next boundary (10.2: read late)
	uint8_t last_half;
	uint8_t last_byte;
	volatile uint8_t txq_head, txq_tail;
	bool end_req;		// tx_end with nothing queued: the transmission ends with what went
	bool underrun;		// idle sent since the last chunk
	bool pa;
	bool tick_any;
	uint32_t t_tick;	// the last slot boundary
	uint32_t t_begin;
	bool mod_saved;
	uint32_t mod_bias;	// RF_MOD_BIAS before tx_begin

	hrc7000_air_stats_t st;
} __attribute__((may_alias)) hrc7000_air_t;

// Pure: what the chip can do with a profile, transmit side (AIR_FIT_TX and its qualifiers); the receive side is the
// soft modem's on the tap (air_softmodem_rx_fit)
uint32_t hrc7000_air_tx_fit(const air_profile_t *p);
// Pure: RF_MOD_BIAS for a profile from the board's DMR value: both modulation bytes scaled by dev_outer_hz /
// HRC7000_AIR_DMR_DEV_HZ (deviation is linear in them, 10.2), the bias bits kept
uint32_t hrc7000_air_mod_bias(const air_profile_t *p, uint32_t dmr_mod_bias);

// buf and sm: memory the board lends while the chip is held (the slot engine's queues)
void hrc7000_air_init(hrc7000_air_t *a, hrc7000_t *dev, const hrc7000_dmr_ops_t *ops, hrc7000_air_buf_t *buf,
		      air_softmodem_t *sm);

// The tap (hal_baseband_t receive). The chip in DMR with the slot engine halted (task level)
bool hrc7000_air_tap_start(hrc7000_air_t *a);
uint16_t hrc7000_air_tap_read(hrc7000_air_t *a, int16_t *buf, uint16_t max);
uint32_t hrc7000_air_tap_time(const hrc7000_air_t *a);	// the next sample rx_read gives
// The copy set-up undone: the chip back in DMR receive (WORK_MODE through FM to DMR, the mask and band bits as they
// were). The board restarts the slot engine after it
void hrc7000_air_tap_stop(hrc7000_air_t *a);

// A session: the chip in DMR with the slot engine halted (task level). Receiving when the tap's band carries the profile
bool hrc7000_air_open(hrc7000_air_t *a, const air_profile_t *p, uint32_t fit, air_event_cb_t cb, void *ctx);
// PA off, the modulation bytes back, the chip back in DMR receive (when chip; else left as it is: the board has it in
// another mode). The board restarts the slot engine after it
void hrc7000_air_close(hrc7000_air_t *a, bool chip);
void hrc7000_air_rx_restart(hrc7000_air_t *a);
// The chip leaves DMR under the session (a mode change): any transmission ends now (PA off, TX_DONE), receive stops
void hrc7000_air_suspend(hrc7000_air_t *a);
// Back in DMR under the session (the board wrote the chip's DMR set-up): the copy set-up again
void hrc7000_air_resume(hrc7000_air_t *a);

void hrc7000_air_isr_sys(hrc7000_air_t *a);
void hrc7000_air_isr_timeslot(hrc7000_air_t *a);
void hrc7000_air_isr_rftx(hrc7000_air_t *a);
// The board's modem task: receive samples through the soft modem, events to the callback, the transmit watchdog
void hrc7000_air_poll(hrc7000_air_t *a);

bool hrc7000_air_tx_begin(hrc7000_air_t *a);
bool hrc7000_air_tx_queue(hrc7000_air_t *a, const air_tx_chunk_t *c);
uint8_t hrc7000_air_tx_space(const hrc7000_air_t *a);
void hrc7000_air_tx_end(hrc7000_air_t *a);
void hrc7000_air_tx_abort(hrc7000_air_t *a);
bool hrc7000_air_tx_active(const hrc7000_air_t *a);
static inline bool hrc7000_air_held(const hrc7000_air_t *a) { return a->open || a->tap; }

#endif

#endif
