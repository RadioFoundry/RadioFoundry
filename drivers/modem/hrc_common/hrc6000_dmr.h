// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 in DMR: register sequences, burst status decode, and the slot engine behind hal_dmr_modem.h
// (docs/spec/modem-hrc6000.md 3.1, 4-7; docs/spec/dmr-modem.md 6; docs/spec/gotchas.md 1, 3, 5).
//
// The engine is board independent. The board wires the chip's three interrupt lines to hrc6000_dmr_isr_*(), runs a
// task that drains the engine's events (hrc6000_dmr_pop) and calls the DMR stack, and supplies the ops below: the
// per-burst RF switch, a microsecond clock, a lock that keeps the modem ISRs out (task-level callers only) and a
// "wake the task" call.
//
// Timing model (modem-hrc6000.md 4.1, 6): what the chip does in slot k+1 is written at the timeslot interrupt that
// starts slot k (0x41 and, to send, the payload and 0x50). A burst reported by the system interrupt 1-12 ms after a
// timeslot interrupt was heard in the slot that just ended, in step with the chip's timing. One of our bursts is on
// air from its RF TX interrupt until the PA is dropped at the first timeslot interrupt more than 20 ms later
// (no RF RX interrupt on the STM32 radios), and only after that is it finished (gotchas 3.2).
//
// Slot numbers: through a repeater from the CACH TC bit (5.3); in direct mode from the TS1 / TS2 sync family when
// used, else a label: 1 is the phase of the first burst heard since the timing was (re)acquired, 2 the other. Labels
// stay put while the timing holds, so the stack can lock to, reply on, or transmit opposite to (SFR) a slot heard.
#ifndef HRC6000_DMR_H
#define HRC6000_DMR_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc.h"
#include "hal_dmr_modem.h"

// --- Chip-level helpers (any context that owns the bus; each register write is one atomic transaction) ---

// Section 3.1: DMR modulation pair from calibration, 0x10 = 0x6E, codec, mic gain. Restart the receiver after
bool hrc6000_dmr_enter(hrc_t *dev, uint8_t mod2, uint8_t mod1, uint8_t mic_gain);
// Section 5.1: 0x40 = 0xC3, 0x41 = 0x20, 0x00, 0x20, 0x50 (rx_ctl replaces the last 0x50 when non-zero)
bool hrc6000_dmr_chip_rx_restart(hrc_t *dev, uint8_t rx_ctl);
// 0x1F = CC << 4 (PI clear)
bool hrc6000_dmr_set_cc(hrc_t *dev, uint8_t cc);
// Page 1 0x04: the data sync pattern the chip transmits for a family (dmr_sync_family_t; NONE = MS)
bool hrc6000_dmr_set_sync(hrc_t *dev, uint8_t family);
// One 27-byte voice frame on page 3 (the bus's voice op, else the config port)
bool hrc6000_dmr_voice(hrc_t *dev, bool write, uint8_t *buf);
// Reg 0x21 value for an access policy (bit 1 also clears the vocoder encode buffer)
uint8_t hrc6000_dmr_access_reg(dmr_access_t access);

// --- Pure decode (modem-hrc6000.md 2 and 5.2) ---

typedef struct {
	uint8_t sync_class;	// 0x51 bits 1-0: 0 none / EMB only, 1 voice, 2 data, 3 RC
	uint8_t kind;		// dmr_burst_kind_t, 0xFF = not a usable burst (RC, bad voice number)
	uint8_t voice_index;	// 0..5 when kind is voice; HRC6000_INDEX_UNKNOWN: a class 0 report without a number
	bool crc_ok;
	bool pi;
	uint8_t cc;		// 0x52 bits 7-4
	uint8_t tc;		// 0x52 bit 2 (CACH TC)
	uint8_t sync;		// dmr_sync_family_t from 0x5F bits 1-0
} hrc6000_rx_status_t;

#define HRC6000_KIND_NONE	0xFFu
#define HRC6000_INDEX_UNKNOWN	0xFFu

void hrc6000_dmr_decode(uint8_t r51, uint8_t r52, uint8_t r5f, hrc6000_rx_status_t *st);
// Payload bytes the chip holds in page 2 for a data / control burst kind (12, 18 for rate 3/4, 24 for rate 1)
uint8_t hrc6000_dmr_payload_len(uint8_t kind);
// Reg 0x50 value that frames a burst kind (voice: 0x08 | index << 4; data: DT << 4)
uint8_t hrc6000_dmr_reg50(uint8_t kind, uint8_t voice_index);

// --- Slot engine ---

#define HRC6000_DMR_TXQ_DEPTH	12	// tx_queue holds this many bursts (the contract asks for at least 8)
#define HRC6000_DMR_EVQ_DEPTH	24	// ISR -> task events (bursts and slot ticks)

typedef struct {
	// Per-burst RF switch (from the ISRs): tx = key the PA for our burst, else PA off and the RF chip back to receive.
	// False when it could not be done now (the RF chip's bus busy); the engine counts it, the board retries RX
	bool (*rf)(void *ctx, bool tx);
	uint32_t (*now_us)(void *ctx);	// free-running microseconds (wraps), any context
	void (*lock)(void *ctx);	// keep the modem ISRs out (task-level engine calls)
	void (*unlock)(void *ctx);
	void (*notify)(void *ctx);	// events queued: wake the board's task (from ISRs and from the lock)
	bool (*sys_line_low)(void *ctx);	// optional: system interrupt line still asserted after its flags were cleared
	// Optional (timeslot ISR): an aligned transmission has found its slot; the first burst follows two ticks later.
	// A board whose receiver can't stay on during the wait (one synthesizer for both) switches to TX here
	void (*tx_slot_found)(void *ctx);
	void *ctx;
} hrc6000_dmr_ops_t;

// Why the engine restarted the receiver (hrc6000_dmr_stats_t.rs_why)
enum {
	HRC6000_RS_IDLE,	// nothing heard for 20 ticks (once per quiet period)
	HRC6000_RS_LOST,	// a reception ended: 13 ticks without a burst
	HRC6000_RS_OOS,		// reports out of step with the chip's timing
	HRC6000_RS_CC,		// color code followed to a new one
	HRC6000_RS_CMD,		// asked for: run, configure, rx_restart, set_cc_filter
	HRC6000_RS_STALL,	// timing stopped while busy
	HRC6000_RS_TX_END,	// end of a direct mode transmission
	HRC6000_RS_COUNT
};

#define HRC6000_DT_BINS	6	// report delay after the timeslot interrupt: <1, <3, <6, <12, <22, >=22 ms

// The LC trace of the last reception (the sticky LC rule, modem-hrc6000.md 5.2 / 5.6): every look at page 2 for an
// embedded LC until the reception's LC was handed over, so one HIL run shows when this call's LC appeared there
#define HRC6000_LC_TRACE_N	8
typedef struct {
	char ev;	// F burst F's report, f F reported again (a repeat, dropped), A burst A while no LC yet, P post-access
			// interrupt, p post-access shortly before the reception started, H voice LC header
	char res;	// n differs from the capture (taken), s same as the capture (held back), v same but taken (more than
			// STALE_LC_VOICE voice bursts), c CRC error flagged, k kept for the next voice burst, - not used
	uint8_t tk;	// ticks since the capture (255: 255 or more)
	uint8_t vss;	// voice bursts taken in this reception at the time
	uint32_t src;	// page 2's source ID (LC octets 6-8)
} hrc6000_lc_trace_t;

// Counters for bench bring-up and the diagnostics page (wrap freely)
typedef struct {
	uint32_t ticks;		// timeslot interrupts
	uint32_t sys_ints;	// system interrupts
	uint32_t rftx_ints;	// RF TX interrupts
	uint32_t rx_voice;	// voice bursts delivered
	uint32_t tr_calls;	// receptions whose voice is read at the ticks (rx_voice_read chips)
	uint32_t tr_bursts;	// voice bursts read at the ticks
	uint32_t rx_lc_header;	// voice LC headers
	uint32_t rx_terminator;
	uint32_t rx_csbk;	// CSBK, MBC
	uint32_t rx_data;	// PI header, data headers and rate 1/2, 3/4, 1, USBD blocks
	uint32_t rx_idle;
	uint32_t rx_crc_err;	// delivered with a CRC error (data blocks and voice) or dropped for one (headers, CSBK)
	uint32_t rx_dup;	// second report for one burst (the EMB report, extra reports), dropped
	uint32_t rx_dup_slot;	// ... of those: a second report in the same tick
	uint32_t rx_dup_extra;	// ... the same voice number again in the next slot (beyond the 45 ms window)
	uint32_t rx_dup_repeat;	// ... the same burst again (same kind and number / payload within 45 ms)
	uint32_t rx_no_call;	// class 0 reports with no superframe in progress on their slot (empty slot, after a call)
	uint32_t rx_stale;	// class 0 reports repeating the number of the burst just taken on that slot
	uint32_t rx_seq_fill;	// voice taken by sequence (the report had no usable number)
	uint32_t rx_seq_jump;	// voice whose number skipped ahead of the sequence (a burst missed)
	uint32_t rx_lone;	// voice sync reports (class 1) with no header or superframe on their slot and no burst B
				// after them: noise, the other slot's echo of A. Dropped (never delivered)
	uint32_t rx_a_cc;	// voice reports whose 0x52 CC was replaced: by the CC filter's (the chip deframes only that
				// CC), or without a filter for burst A (no CC on air) by its superframe's
	uint32_t sys_at_tick;	// system interrupts still pending at the timeslot interrupt, serviced there
	uint32_t rx_rc;		// reverse channel / unusable status, dropped
	uint32_t rx_out_of_step;	// reports 22 ms or more after the timeslot interrupt
	uint32_t rx_restarts;	// receiver restarts by the engine (silence, out of step, CC change, stall)
	uint32_t late_entry;	// post-access interrupts
	uint32_t voice_lost;	// voice-abnormal interrupts
	uint32_t ev_overflow;	// events dropped, the task was behind
	uint32_t tx_bursts;	// bursts set up
	uint32_t tx_underrun;	// our slot came with an empty queue mid-transmission
	uint32_t tx_rejected;	// send request rejected (polite access)
	uint32_t tx_done;	// transmissions finished
	uint32_t tx_empty;	// slots left empty on request (DMR_BURST_EMPTY)
	uint32_t rf_fail;	// RF switches the board could not do
	uint32_t stalls;	// timing stopped while busy (200 ms without a timeslot interrupt)
	uint32_t rs_why[HRC6000_RS_COUNT];	// receiver restarts by reason
	uint32_t by_class[4];	// every received-data report by 0x51 bits 1-0, before any filtering
	uint32_t by_type[16];	// ... by 0x51 bits 7-4
	uint32_t by_r5f[4];	// ... by 0x5F bits 1-0
	uint32_t dt_hist[HRC6000_DT_BINS];	// ... by delay after the timeslot interrupt
	// The sticky LC rule (embedded LCs looked at before a reception's LC was handed over)
	uint32_t lc_f_new;	// at F: differs from the capture, taken
	uint32_t lc_f_stale;	// at F: same as the capture, held back
	uint32_t lc_f_count;	// at F: same as the capture, taken by the voice count
	uint32_t lc_f_crc;	// at F: CRC error flagged
	uint32_t lc_a_new;	// at the next A: differs from the capture, taken (written to page 2 after F's report)
	uint32_t lc_rep_new;	// at F's repeated report: differs, kept for the next voice burst
	uint32_t lc_pa_new;	// at a post-access interrupt: differs, kept for the next voice burst
	uint32_t lc_kept;	// kept LCs handed over with the next voice burst
	uint8_t last_r5f;	// raw 0x5F of the last report
	uint8_t last_data_r51;	// raw 0x51 of the last report classified as non-voice
	uint8_t last_cc;	// last burst heard
	uint8_t last_sync;	// dmr_sync_family_t
	uint8_t last_r51;	// raw 0x51 of the last report
	uint8_t last_r42;	// raw 0x42 at the last timeslot interrupt
	uint8_t last_report_ms;	// last report: ms after its timeslot interrupt
	uint8_t last_pa_ms;	// last burst of ours: ms the PA was on
	int8_t last_ts;		// slot number of the last burst heard (0 unknown)
	uint8_t tx_state;	// engine TX state (0 idle)
	// The LC trace of the last reception (not cleared by a counter reset)
	uint16_t lc_serial;	// bumped at every capture and trace entry
	uint16_t lc_cap_seq;	// bumped at every capture (a new trace)
	uint8_t lc_n;		// entries in lc_tr
	char lc_cap_how;	// the report the capture was made at: 0 a status report (class 0 before any burst), A a lone
				// burst A, T a burst taken
	uint32_t lc_cap_src;	// the capture's source ID (LC octets 6-8)
	hrc6000_lc_trace_t lc_tr[HRC6000_LC_TRACE_N];
} hrc6000_dmr_stats_t;

typedef struct {
	bool active;		// a superframe in progress on this slot phase
	uint8_t last;		// voice number taken last (HRC6000_INDEX_UNKNOWN after a header)
	uint8_t next;		// the one expected
	uint8_t cc;		// color code of the superframe (from its header / bursts B-F): burst A has none on air
	uint32_t tick;		// when
} hrc6000_dmr_vstate_t;

typedef struct {
	dmr_event_t ev;
	dmr_burst_t burst;
	uint8_t dms;		// DMR_EV_BURST_RX: ms after the timeslot interrupt the report came, 255 = unknown / at the tick
} hrc6000_dmr_evslot_t;

// A report taken but not yet delivered: a lone voice sync (burst A with nothing before it on its slot) waits for the
// burst B that proves it is a call (rx_report)
typedef struct {
	bool valid;
	hrc6000_rx_status_t s;
	dmr_burst_t nb;		// payload as read at the report
	uint32_t t;		// now_us of the report
	uint32_t tick;		// tick of the report
	bool in_step;
	bool on_time;
	uint8_t dms;
} hrc6000_dmr_held_t;

typedef struct {
	hrc_t *dev;
	const hrc6000_dmr_ops_t *ops;
	bool running;		// chip in DMR and the engine in charge of it
	dmr_config_t cfg;
	int8_t cc_filter;	// -1 any
	int8_t lock_ts;		// rx_lock_slot

	// Slot clock
	uint32_t tick;
	uint32_t t_tick;	// now_us at the last timeslot interrupt
	bool timing;		// timeslot interrupts are coming
	int8_t phase_ts[2];	// slot number of each phase (tick & 1), 0 unknown
	uint8_t ctl_next;	// 0x41 written for the slot after the current one
	uint8_t ctl_now;	// ... and the one for the slot now starting
	// CACH TC tracking (5.3)
	int8_t own_ts;
	uint8_t tc_agree, tc_disagree;
	uint32_t bs_tick;	// tick of the last BS-sync burst heard
	bool rmo_seen;

	// Receive
	uint32_t rx_tick;	// tick of the last burst taken
	bool rx_any;		// a burst taken since the last restart
	uint32_t t_voice;	// now_us of the last voice burst taken
	bool voice_valid;
	uint32_t report_tick;	// tick of the last report taken (one per slot)
	bool report_valid;
	uint8_t out_of_step_run;
	uint32_t heard_tick;	// tick of the last burst taken, kept across receiver restarts (stale LC rule)
	bool heard_any;
	uint8_t stale_lc[9];
	uint8_t voice_since_start;	// voice bursts since the reception started (stale LC rule)
	uint32_t cap_tick;	// tick of the capture
	bool lc_given;		// this reception's LC was handed over (a header, a valid embedded LC)
	bool lc_kept;		// lc_keep holds an LC seen outside a voice report, for the next voice burst taken
	uint8_t lc_keep[9];
	bool f_crc_bad;		// the last F taken flagged a CRC error (its LC isn't taken at the next A either)
	uint8_t scap_lc[9];	// page 2 at the first status report of a quiet period (late entry: before any burst taken)
	uint32_t scap_tick;
	bool scap_valid;
	uint32_t pa_tick;	// tick of the last post-access interrupt
	bool pa_any;
	uint32_t pa_src;	// page 2's source ID then
	// CC acquisition (5.4) when the filter is off
	uint8_t cc_last, cc_matches, cc_cur;
	uint32_t t_cc;
	uint32_t cc_hold_tick;
	bool cc_held;
	bool restart_pending;
	uint8_t restart_why;
	bool idle_restarted;	// the quiet-period restart has been done
	// The burst taken last (repeat check)
	uint32_t t_report;
	uint8_t report_dms;	// ms after the tick of the report being handled (255 unknown)
	uint8_t pop_dms;	// ... of the burst event last popped
	uint8_t last_rx_kind, last_rx_index, last_rx_sync;
	uint8_t last_rx_payload[DMR_PAYLOAD_MAX];
	hrc6000_dmr_vstate_t vs[2];	// per slot phase (tick & 1 of the slot)
	hrc6000_dmr_held_t held[2];	// per slot phase: a lone burst A waiting for its B

	// Transmit
	uint8_t tx_state;
	uint8_t tx_timing;	// dmr_timing_t
	int8_t tx_ts;
	uint8_t tx_access;
	bool tx_listen;		// receive in the slots between our bursts (hrc6000_dmr_tx_listen)
	uint8_t tx_phase;	// phase (tick & 1) of our slots
	bool tx_end_req;
	bool tx_abort_req;
	uint32_t tx_wait;	// ticks waited for alignment
	bool tx_found;		// tx_slot_found called: the start waits for tx_go_tick
	uint32_t tx_go_tick;
	uint32_t tx_last_setup;	// tick of the last burst set up
	bool tx_any_setup;
	uint8_t reg40;		// shadow of 0x40 while transmitting
	uint8_t sync_cur;	// sync family in page 1
	dmr_tx_burst_t txq[HRC6000_DMR_TXQ_DEPTH];
	volatile uint8_t txq_head, txq_tail;
	dmr_tx_burst_t last_hdr;	// repeated when the queue runs dry in the header phase
	uint8_t last_kind;
	uint8_t last_voice;
	uint8_t emb_lc[9];
	bool emb_set, emb_pending;
	// PA
	bool pa_on;
	uint32_t t_pa_on;
	uint32_t pa_on_tick;

	hrc6000_dmr_evslot_t evq[HRC6000_DMR_EVQ_DEPTH];
	volatile uint8_t evq_head, evq_tail;
	bool notify_req;	// events pushed since the task was last woken

	// Voice read at the ticks (chips with rx_voice_read): a call in progress, the tick parity whose read holds its
	// bursts (-1 until found), the next burst's voice index, reads in a row without a new burst, the last burst read
	bool tr_active;
	int8_t tr_ph;
	uint8_t tr_index;
	uint8_t tr_miss;
	uint8_t tr_wait;
	uint8_t tr_last[27];
	// The tick phase of the header that started this read (-1: none yet), and the parity flip that names the read
	// bursts with their call's slot: learned when a header-started read locks, kept for later calls (late entry too)
	int8_t tr_hdr_ph;
	uint8_t tr_ph_flip;

	hrc6000_dmr_stats_t st;
#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
	struct hrc6000_bs *bs;	// split duplex BS outbound in charge of the chip (hrc6000_bs.c), NULL otherwise
#endif
} hrc6000_dmr_t;

void hrc6000_dmr_init(hrc6000_dmr_t *e, hrc_t *dev, const hrc6000_dmr_ops_t *ops);

// The chip has just been put in DMR (hrc6000_dmr_enter): engine on, receiver restarted. Task level
void hrc6000_dmr_run(hrc6000_dmr_t *e);
// Leaving DMR: any transmission dropped at once (PA off), engine off. Task level
void hrc6000_dmr_halt(hrc6000_dmr_t *e);

// ISR entry points (the three interrupt lines, one priority, never nested in each other)
void hrc6000_dmr_isr_timeslot(hrc6000_dmr_t *e);
void hrc6000_dmr_isr_sys(hrc6000_dmr_t *e);
void hrc6000_dmr_isr_rftx(hrc6000_dmr_t *e);

// Task side. pop: the next event (ev->burst points at *burst for DMR_EV_BURST_RX); false when none
bool hrc6000_dmr_pop(hrc6000_dmr_t *e, dmr_event_t *ev, dmr_burst_t *burst);
// For the trace: ms after its timeslot interrupt the burst last popped was reported (255 unknown / serviced at a tick)
static inline uint8_t hrc6000_dmr_pop_ms(const hrc6000_dmr_t *e) { return e->pop_dms; }
// Watchdog from the board's task, every few ms: timing stall (200 ms), PA left on (no timeslot interrupt)
void hrc6000_dmr_poll(hrc6000_dmr_t *e);

// The hal_dmr_modem.h commands (task level; each takes the lock)
void hrc6000_dmr_configure(hrc6000_dmr_t *e, const dmr_config_t *cfg);
void hrc6000_dmr_rx_restart(hrc6000_dmr_t *e);
void hrc6000_dmr_rx_lock_slot(hrc6000_dmr_t *e, int8_t ts);
// The transmission begun last receives in the slots between its bursts (the SFR's inbound slot); ignored when idle
void hrc6000_dmr_tx_listen(hrc6000_dmr_t *e);
// b in place of the oldest queued DMR_BURST_EMPTY; the number of entries queued after it, -1 when there is none
int8_t hrc6000_dmr_tx_fill(hrc6000_dmr_t *e, const dmr_tx_burst_t *b);
bool hrc6000_dmr_tx_begin(hrc6000_dmr_t *e, dmr_timing_t timing, int8_t ts, dmr_access_t access);
bool hrc6000_dmr_tx_queue(hrc6000_dmr_t *e, const dmr_tx_burst_t *b);
uint8_t hrc6000_dmr_tx_queue_space(const hrc6000_dmr_t *e);
void hrc6000_dmr_tx_end(hrc6000_dmr_t *e);
void hrc6000_dmr_tx_abort(hrc6000_dmr_t *e);
void hrc6000_dmr_set_embedded_lc(hrc6000_dmr_t *e, const uint8_t lc[9]);
void hrc6000_dmr_set_cc_filter(hrc6000_dmr_t *e, int8_t cc);
bool hrc6000_dmr_is_transmitting(const hrc6000_dmr_t *e);

void hrc6000_dmr_stats(const hrc6000_dmr_t *e, hrc6000_dmr_stats_t *out);

#ifdef HRC5000_INT_LOG
#define HRC_INT_LOG_N	40
typedef struct {
	uint8_t pass, flags, r51, r52, r42;
	bool at_tick;
	uint32_t dt;
	uint8_t lc[9];	// page 2 from 0x00 (the LC / info data RAM) at the interrupt
} hrc_int_rec_t;
extern hrc_int_rec_t hrc_int_log[HRC_INT_LOG_N];
extern volatile uint8_t hrc_int_n;
#endif

#ifdef HRC5000_TICK_LOG
// Bench knob (issue 3): one call's timeline, timeslot ticks ('T') and system interrupts ('S') in order. A system
// interrupt with flags arms it; it records until full, the board's probe task prints it, then it re-arms. A tick
// record reads the voice port: all 27 bytes, and vh (a 16-bit hash of them), show whether and when it changes
#define HRC_TICK_LOG_N	120
typedef struct {
	char kind;
	uint8_t flags, r42, r51, r52, r90;
	uint8_t ph;	// the engine's tick parity: which slot the tick starts
	uint16_t vh;
	uint8_t v[27];	// the voice port as read (DMR_VOICE_BYTES)
	uint32_t dt;	// 'T': microseconds since the previous tick; 'S': since the tick
} hrc_tick_rec_t;
extern hrc_tick_rec_t hrc_tick_log[HRC_TICK_LOG_N];
extern volatile uint8_t hrc_tick_n;
extern volatile bool hrc_tick_armed;
// Two consecutive ticks' snapshots of the voice RAM from offset 0, taken at tick records HRC_SNAP_AT and + 1
#define HRC_SNAP_AT	40
#define HRC_SNAP_LEN	240
extern uint8_t hrc_tick_snap[2][HRC_SNAP_LEN];
#endif

#endif
