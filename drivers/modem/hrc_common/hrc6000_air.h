// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C6000's physical layer mode as an air interface provider (hal_air.h; docs/spec/modem-hrc6000.md 10). While a
// session is open it owns the chip in place of the DMR slot engine (halted): the board routes the three interrupt lines
// here and runs hrc6000_air_poll from its modem task, which calls the session's callback.
//
// Receive (10.2): the chip runs on its own 30 ms record clock, 4:0x10 = 0x02, 0x40 = 0x40, 0x41 = 0x40. Its one usable
// correlator (aux 1:0x47-0x4C) holds the profile's first 24-symbol sync. Each record is 144 symbols of 6-bit soft
// values at page 2 0x30, announced by the system interrupt (0x82 bit 0); 0x51 bits 1-0 = 2 when the correlator fired
// in it, which also changes the order the chip stores the record in. Nothing comes before the first sync, and after a
// timing step the chip never finds the sync again by itself: K records in a row without one re-arm it (0x41 = 0x60,
// then 0x40 after 40 ms).
//
// Transmit (10.3): 0x10 = 0x66, own timing (0x40 = 0xE3), 0x41 = 0x80 every slot. The chip sends the two TX RAM halves
// (page 2 0x00 and 0x24, 36 bytes each) in alternate slots and reads a half while it sends it, so each timeslot
// interrupt writes only the half the next slot reads. The PA is keyed once, held, and dropped after the last chunk.
// Deviation: the two-point pair (0x45, 0x46) read at tx_begin is scaled to the profile and put back after.
#ifndef HRC6000_AIR_H
#define HRC6000_AIR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hrc.h"
#include "hrc6000_dmr.h"
#include "hal_air.h"

#define HRC6000_AIR_SYMS	144	// one record or one TX RAM half: 30 ms at 4800 symbols/s
#define HRC6000_AIR_RAW_BYTES	288	// a record: two bytes per symbol
#define HRC6000_AIR_HALF_BYTES	36
#define HRC6000_AIR_RXQ		4	// records ISR -> task (a power of two: free-running indexes)
#define HRC6000_AIR_EVQ		8	// events ISR -> task (power of two)
#define HRC6000_AIR_TXQ		4	// transmit chunks, 120 ms (power of two)
#define HRC6000_AIR_TX_LEAD_MS	60	// a chunk is taken a slot before it is on air; one more slot of margin
#define HRC6000_AIR_TIME_US	3000	// t_us accuracy: the receive delay through the chip is not measured
#define HRC6000_AIR_SYNC_AT	66	// where a detected sync sits in a record in air order

typedef struct {
	uint32_t t_us;
	int16_t sync_at;
	uint8_t flags;
	uint8_t nsyms;		// fewer than a record's when it repeated the end of the one before
	uint8_t fill;		// symbols lost before it, their number known: delivered as erasures
	uint8_t hard[HRC6000_AIR_SYMS / 4];
	int8_t soft[HRC6000_AIR_SYMS];
} hrc6000_air_rec_t;

// The large buffers. The board hands over memory the DMR slot engine leaves unused while a session holds the chip (its
// event queue), so a session costs no RAM of its own: hence may_alias
typedef struct {
	uint8_t raw[HRC6000_AIR_RAW_BYTES];
	hrc6000_air_rec_t rec[HRC6000_AIR_RXQ];
	air_tx_chunk_t txq[HRC6000_AIR_TXQ];
} __attribute__((may_alias)) hrc6000_air_buf_t;

typedef struct {
	uint8_t kind;		// air_event_kind_t
	uint32_t t_us;
	uint32_t arg;
} hrc6000_air_ev_t;

typedef struct {
	uint32_t records;	// records read
	uint32_t synced;	// ... with a sync detection
	uint32_t rearms;	// the correlator re-armed (K records without a sync)
	uint32_t gaps;		// chunks flagged AIR_CHUNK_GAP
	uint32_t trims;		// records that repeated symbols of the one before: those were dropped
	uint32_t fills;		// holes of a known size delivered as erasures
	uint32_t rx_overflow;	// records dropped: the task was behind
	uint32_t ev_overflow;	// events dropped
	uint32_t tx_slots;	// TX RAM halves written
	uint32_t tx_underruns;	// ... of those, idle for want of a chunk mid-transmission
	uint32_t tx_done;
	uint32_t rf_fail;	// PA switches the board could not do
	uint32_t stalls;	// no timeslot interrupt for 200 ms while transmitting: dropped
} hrc6000_air_stats_t;

// Receive is the rest state; the transmit states follow the slots: START (set up, the first slot not yet taken), RUN,
// LAST (the AIR_TX_LAST chunk is on air), TAIL (one idle slot after it), then the PA drop and TX_DONE
enum { HRC6000_AIR_RX, HRC6000_AIR_START, HRC6000_AIR_RUN, HRC6000_AIR_LAST, HRC6000_AIR_TAIL };

// may_alias: it lives in the slot engine's event queue too (hrc6000_air_buf_t)
typedef struct hrc6000_air {
	hrc_t *dev;
	const hrc6000_dmr_ops_t *ops;
	hrc6000_air_buf_t *buf;
	bool open;
	const air_profile_t *p;
	air_event_cb_t cb;
	void *cb_ctx;
	uint32_t fit;

	// Receive
	bool rx_on;		// the profile can be received (AIR_FIT_RX)
	uint8_t sync_id;	// the pattern in the correlator
	uint8_t rearm_k;	// records without a sync detection that re-arm it
	uint8_t sync_per;	// records between two syncs on air (FREE_RUN past it)
	uint8_t unsynced;	// records in a row without a sync detection
	uint8_t since_sync;	// records since the last one with
	bool rearm_hold;	// 0x41 = 0x60 written at t_rearm, 0x40 due
	uint32_t t_rearm;
	bool gap;		// the next chunk follows lost symbols
	bool rec_any;
	uint32_t t_rec;		// the last record's interrupt
	bool tick_any;
	uint32_t t_tick;	// the last timeslot interrupt
	int16_t dc16;		// receive offset, soft counts x 16
	int16_t outer16;	// soft counts x 16 of an outer symbol: from the profile's deviation, then the syncs received
	int16_t outer_nom16;	// ... from the profile's deviation alone
	bool next_any;
	uint32_t t_next;	// when the symbol after the last record's is due
	volatile uint8_t rec_head, rec_tail;
	hrc6000_air_ev_t evq[HRC6000_AIR_EVQ];
	volatile uint8_t ev_head, ev_tail;

	// Transmit
	volatile uint8_t tx;	// HRC6000_AIR_RX / START / RUN / LAST / TAIL
	uint8_t tail;		// idle slots set up after the last chunk
	bool rx_paused;		// the chip left DMR under the session (suspend): no receive until rx_restart
	volatile uint8_t txq_head, txq_tail;
	bool end_req;		// tx_end with nothing queued: the transmission ends with what went
	bool underrun;		// idle sent since the last chunk
	bool pa;
	bool last_out;		// the PA drop ends the transmission (the board may put the receiver back at once)
	uint32_t tx_n;		// timeslot interrupts since own timing started
	uint32_t t_begin;
	bool mod_saved;
	uint8_t mod2, mod1;	// the pair before tx_begin
	uint8_t tx_mod2, tx_mod1;	// ... and the one written for the profile

	hrc6000_air_stats_t st;
} __attribute__((may_alias)) hrc6000_air_t;

#if CONFIG_AIR

// Pure: what the chip can do with a profile (hal_air_t.fit, without AIR_FIT_UNTESTED: the board adds that)
uint32_t hrc6000_air_fit(const air_profile_t *p);
// Pure: the two-point pair for a transmit deviation, from the pair calibrated for DMR (1944 Hz outer)
void hrc6000_air_mod_pair(const air_profile_t *p, uint8_t cal2, uint8_t cal1, uint8_t *mod2, uint8_t *mod1);
// Pure: one record (2 bytes per symbol, chip order) to air order without its first skip symbols, soft scaled to
// AIR_SOFT_OUTER and sliced again. Returns the whole record's mean level x 16 (the offset tracker's input)
int16_t hrc6000_air_unpack(const uint8_t *raw, bool synced, uint8_t skip, int16_t dc16, int16_t outer16,
			   uint8_t levels, uint8_t *hard, int8_t *soft);
// Pure: the outer level x 16 a record with a sync detection shows: half the distance between the means of the
// pattern's +3 and -3 symbols (syms: the correlator's pattern, outer symbols only). 0 when the pattern has one sign
int16_t hrc6000_air_sync_outer16(const uint8_t *raw, const uint8_t *syms);

void hrc6000_air_init(hrc6000_air_t *a, hrc_t *dev, const hrc6000_dmr_ops_t *ops, hrc6000_air_buf_t *buf);
// The chip in DMR and the slot engine halted (task level). Receiving when the profile fits for it
bool hrc6000_air_open(hrc6000_air_t *a, const air_profile_t *p, air_event_cb_t cb, void *ctx);
// PA off, the deviation pair back, the chip back in the DMR receive set-up (when chip, else left as it is: the board
// has it in another mode). The board restarts the slot engine after it. Task level
void hrc6000_air_close(hrc6000_air_t *a, bool chip);
// The chip was put in DMR again (a mode change, a retune): the receive set-up again, the next chunk a gap
void hrc6000_air_rx_restart(hrc6000_air_t *a);
// The chip leaves DMR under the session: any transmission ends now (PA off, TX_DONE); receive waits for rx_restart
void hrc6000_air_suspend(hrc6000_air_t *a);

void hrc6000_air_isr_timeslot(hrc6000_air_t *a);
void hrc6000_air_isr_sys(hrc6000_air_t *a);
void hrc6000_air_isr_rftx(hrc6000_air_t *a);
// The board's modem task: events to the callback, the re-arm's second write, the transmit watchdog
void hrc6000_air_poll(hrc6000_air_t *a);

bool hrc6000_air_tx_begin(hrc6000_air_t *a);
bool hrc6000_air_tx_queue(hrc6000_air_t *a, const air_tx_chunk_t *c);
uint8_t hrc6000_air_tx_space(const hrc6000_air_t *a);
void hrc6000_air_tx_end(hrc6000_air_t *a);
void hrc6000_air_tx_abort(hrc6000_air_t *a);
bool hrc6000_air_tx_active(const hrc6000_air_t *a);
// The last chunk and its idle slot are out: the PA drop now ends the transmission (interrupt context)
static inline bool hrc6000_air_tx_last_out(const hrc6000_air_t *a) { return a->last_out; }

#endif

#endif
