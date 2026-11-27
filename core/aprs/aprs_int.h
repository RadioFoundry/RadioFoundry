// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Inside the APRS engine: what aprs.c (the engine: settings, beacon triggers, the view) and aprs_ax25.c (everything
// AX.25: frames, AFSK jobs, messages, KISS, receive; CONFIG_APRS_AFSK) share. One state block, aprs_s: the radio task
// changes it in aprs_tick / aprs_rf_* / aprs_rx_*, other tasks through the any-task calls; every access is under
// LOCK() (the scheduler held: short copies only). Not for other modules: they use aprs.h.
#ifndef RFOUNDRY_APRS_INT_H
#define RFOUNDRY_APRS_INT_H

#include "aprs/aprs.h"
#include "aprs/aprs_beacon.h"
#include "aprs/aprs_fmt.h"
#include "codeplug/codeplug.h"
#include "settings/settings.h"
#include "aprs/aprs_igate.h"	// the digipeater modes are a setting
#if CONFIG_APRS_AFSK
#include "aprs/afsk.h"

#define QUEUE_LEN	2		// frames waiting for the air
#define TONES_BYTES	512		// one transmission: preamble, frames, tail (4096 symbols, 3.4 s)
#if CONFIG_APRS_RX
#define OUT_RING	512		// KISS bytes for the host: received frames, iGate lines
#else
#define OUT_RING	256		// only our own lines to APRS-IS
#endif

typedef struct {
	uint16_t len;
	uint8_t kind;
	uint8_t tries;
#if CONFIG_APRS_PACKET
	uint32_t queued_ms;	// packet mode: a host frame not on the air PACKET_STALE_MS later is dropped
#endif
	uint8_t data[AX25_FRAME_MAX];
} qframe_t;

#if CONFIG_APRS_PACKET
#define PACKET_STALE_MS		10000u		// aprs.md 9.2: the host's retry timer sends it again
#define PACKET_DUTY_WINDOW_MS	60000u
#define PACKET_DUTY_MS		30000u		// keyed time in any PACKET_DUTY_WINDOW_MS (open: #1199, aprs.md 14)
#define PACKET_DUTY_SLOTS	16		// transmissions remembered for the duty cycle
#endif
#endif

typedef struct {
	aprs_cfg_t cfg;
	aprs_beacon_cfg_t bcfg;
	aprs_beacon_t bc;
	uint32_t now;
	bool dmr_due;		// a digital beacon to send (outside the lock)
	bool reload;
	aprs_view_t v;
#if CONFIG_APRS_AFSK
	// The AX.25 half (aprs_ax25.c)
	char comment[CODEPLUG_APRS_COMMENT + 1];	// from the codeplug's APRS configuration 1
	char test_call[AX25_CALL_LEN + 4];
	ax25_addr_t me;
	bool me_ok;
	uint8_t block;
	bool cp_writing;	// aprs_to_codeplug runs: the record and its markers in settings may differ for a moment
	qframe_t q[QUEUE_LEN];
	uint8_t qn;
	uint8_t taken;		// frames in the transmission under way
	uint32_t rf_next_ms;	// no transmission before
	uint8_t tones[TONES_BYTES];
	struct {
		bool active;
		char to[AX25_CALL_LEN + 4];
		char text[APRS_MSG_TEXT_MAX + 1];
		char msgno[APRS_MSGNO_MAX + 1];
		uint8_t tries;
		uint32_t next_ms;
		bool acked;
		bool want_ack;
	} out;
	uint16_t msg_seq;
	uint8_t ring[OUT_RING];
	uint16_t r_head, r_len;
	uint8_t host_tokens;
	uint32_t host_refill_ms;
	uint32_t ident_ms;
	uint32_t cp_serial;
	char info[AX25_INFO_MAX + 1];	// scratch for information fields (under the lock)
#if CONFIG_APRS_PACKET
	// Packet mode (aprs.md 9.2): on (cfg.kiss && cfg.packet), the selected channel as the radio task last gave it,
	// why the host's frames can't go, the host's KISS parameters (10 ms units, persistence 0-255), and the duty cycle:
	// each transmission's start and keyed ms, and the time before which nothing more fits
	bool pk_on;
	uint8_t pk_block;
	uint32_t ch_rx_hz, ch_tx_hz;
	bool ch_narrow;
	uint8_t kp_txdelay, kp_persist, kp_slot, kp_txtail;
	uint32_t duty_at[PACKET_DUTY_SLOTS];
	uint16_t duty_ms[PACKET_DUTY_SLOTS];
	uint8_t duty_next;
	uint32_t duty_wait_ms;
	bool duty_waiting;
	uint16_t job_ms;	// the keyed time of the job taken
#endif
#if CONFIG_APRS_RX
	aprs_station_t st[APRS_STATIONS];
	uint8_t nst;
	aprs_msg_t msgs[APRS_MESSAGES];
	uint8_t nmsg;
	aprs_dedup_t dedup_gate, dedup_digi, dedup_is;
	afsk_demod_t demod;
	bool rx_running;
	uint16_t rx_peak;
	uint32_t rx_start_fails, rx_retry_ms;	// starts the board refused; the next try (RX_RETRY_MS later)
	bool rx_retry_wait;			// the last start was refused: wait for rx_retry_ms
	int16_t rxbuf[160];
	char gate[AX25_TNC2_MAX + 20];
#endif
#endif
} aprs_state_t;

extern aprs_state_t aprs_s;
extern void (*aprs_lock_fn)(void);
extern void (*aprs_unlock_fn)(void);
#define LOCK()		aprs_lock_fn()
#define UNLOCK()	aprs_unlock_fn()

// Lock held: something worth a redraw changed
static inline void aprs_changed(void)
{
	aprs_s.v.serial++;
}

static inline uint16_t aprs_kmh_to_kn(uint16_t kmh)
{
	return kmh == 0xFFFFu ? APRS_SPEED_NONE : (uint16_t)(((uint32_t)kmh * 1000u + 926u) / 1852u);
}

// A settings record being changed that holds no APRS settings yet gets the defaults, marked stored, so one field can
// be saved into it
void aprs_cfg_store_defaults(settings_t *s);

// --- What the engine asks of the AX.25 half; nothing when it isn't built ---
#if CONFIG_APRS_AFSK
// Start-up, after aprs_s was cleared (no lock)
void aprs_ax25_init(uint32_t now_ms);
// Who we are, from the settings and the codeplug (reads storage: outside the lock)
void aprs_ax25_identity(void);
// Every tick, outside the lock: the identity again when the codeplug changed, and now and then
void aprs_ax25_poll(uint32_t now_ms);
// Every tick, lock held, before the beacon triggers: the host's rate limit, old station positions, what blocks RF, packet
// mode's channel and stale frames (r: the radio task's state)
void aprs_ax25_tick(const aprs_radio_t *r, uint32_t now_ms);
// A beacon is due (lock held): the packet is built, queued for RF when rf says so and RF isn't blocked, and sent to
// APRS-IS when that is on. True when it went anywhere
bool aprs_ax25_beacon(const position_fix_t *f, bool rf);
// Every tick, lock held, after the beacon triggers: the message waiting for its ack, and this half's view fields
void aprs_ax25_service(void);
// "Send now" has somewhere to go on this half (lock held)
bool aprs_ax25_can_send(void);
// This half's part of a view copy (lock held)
void aprs_ax25_view(aprs_view_t *v);
#if CONFIG_APRS_RX
// The receive path: on while receiving FM with the setting on (radio task, outside the lock)
void aprs_ax25_rx_service(const aprs_radio_t *r, uint32_t now_ms);
#endif
#else
static inline void aprs_ax25_init(uint32_t now_ms) { (void)now_ms; }
static inline void aprs_ax25_identity(void) {}
static inline void aprs_ax25_poll(uint32_t now_ms) { (void)now_ms; }
static inline void aprs_ax25_tick(const aprs_radio_t *r, uint32_t now_ms) { (void)r; (void)now_ms; }
static inline bool aprs_ax25_beacon(const position_fix_t *f, bool rf) { (void)f; (void)rf; return false; }
static inline void aprs_ax25_service(void) {}
static inline bool aprs_ax25_can_send(void) { return false; }
static inline void aprs_ax25_view(aprs_view_t *v) { (void)v; }
#endif

#endif
