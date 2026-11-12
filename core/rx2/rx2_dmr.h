// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The second receiver's SoC as the main SoC's second DMR transmitter (docs/spec/rx2-link.md 2.4): the main SoC's modem
// goes on receiving while its transmissions go out on the other SoC (hal_dmr_modem_t.tx_split, an SFR's repeat,
// sfr.md 3 item 7). Portable: no RTOS, no board headers; the boards move the payloads over the link (RX2_DMR,
// RX2_DMR_EV) and the host tests run both ends back to back.
//
// Main side: a hal_dmr_modem_t made from the main SoC's own modem, with the transmit calls sent over the link while
// split. Receiver side: the requests applied to its own modem, and its transmit events sent back.
#ifndef RX2_DMR_H
#define RX2_DMR_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_dmr_modem.h"

// RX2_DMR requests (main to receiver): the op, then its fields (little endian)
enum {
	RX2_DMR_SPLIT	= 0x01,	// u8 on, i8 color code: the transmitter role on or off
	RX2_DMR_ARM	= 0x02,	// u32 Hz (0: disarm), u8 power level: receive there for the slot timing, key the PA there
	RX2_DMR_BEGIN	= 0x03,	// u8 dmr_timing_t, i8 ts, u8 dmr_access_t
	RX2_DMR_BURST	= 0x04,	// u8 kind, u8 voice index, u8 flags (bit 0 host_crc), u8 sync_override, u8 len, payload.
				// No tx_fill: its answer can't wait for the link, so the main side queues instead
	RX2_DMR_END	= 0x05,
	RX2_DMR_ABORT	= 0x06,
	RX2_DMR_EMB	= 0x07,	// the embedded LC, 9 bytes
	RX2_DMR_LISTEN	= 0x08,	// tx_listen
};
// RX2_DMR_EV (receiver to main): u8 kind (a dmr_event_kind_t, or RX2_DMR_EV_STATE), i8 ts, u8 TX queue space after
// the bursts it has taken, u8 state, u8 bursts taken (RX2_DMR_BURST, mod 256: the main side counts the ones
// still on the link against the space)
#define RX2_DMR_EV_STATE	0x80u	// no modem event: the state below (sent on a change and every RX2_DMR_BEAT_MS)
#define RX2_DMR_ST_READY	0x01u	// split and armed: tx_begin can start
#define RX2_DMR_ST_TX		0x02u	// a transmission is begun
#define RX2_DMR_EV_LEN		5u
#define RX2_DMR_BEAT_MS		200u
#define RX2_DMR_QUIET_MS	600u	// the main side ends a transmission it hears nothing of this long
#define RX2_DMR_READY_MS	1500u	// split this long and the receiver not ready: the main SoC transmits itself

// --- Main side ---
typedef struct {
	const hal_dmr_modem_t *local;		// receives always; transmits while not split
	bool (*send)(const uint8_t *p, uint8_t len);	// RX2_DMR to the receiver; false: down, or no transmitter
	// The split turned on or off: the board's tx() arms the receiver's transmitter instead of its own from here
	// (rx2_dmr_main_arm), and arms it now if it was keyed
	void (*split)(bool on);
	// Optional: where the main receiver is (hz) and the power level a transmission there takes. The split arms the
	// receiver's transmitter there at once, so its modem has the inbound slot timing before the first repeat is begun
	// (an aligned begin with nothing heard takes the modem's own timing). False: not known yet (the first tx() arms)
	bool (*listen)(uint32_t *hz, uint8_t *power);
} rx2_dmr_main_cfg_t;

typedef struct {
	bool split;		// tx_split(true) and not off since
	bool ready;		// the receiver's last state: armed and in the role
	bool remote;		// the transmission begun last went to the receiver
	bool remote_tx;		// ... and hasn't ended
	bool ending;		// tx_end sent
	uint8_t space;		// the receiver's last reported queue space
	uint8_t sent, taken;	// bursts sent, and taken by the receiver at its last report (mod 256)
	int8_t cc;		// the color code the receiver filters on (start, set_cc_filter)
	uint32_t arm_hz;	// where the receiver's transmitter was armed last (0: not), and at what power
	uint8_t arm_power;
	uint32_t split_ms;	// when the split went on
	uint32_t heard_ms;	// its last event
	uint32_t now_ms;	// the last poll
	uint32_t events, bursts, ends_forced, fallbacks;
} rx2_dmr_main_state_t;

// The modem to hand the core in place of cfg->local: out becomes a copy of it with the transmit calls taken over
void rx2_dmr_main_init(const rx2_dmr_main_cfg_t *cfg, hal_dmr_modem_t *out);
// Arm the receiver's transmitter; the board's tx() while split. hz 0 (the board's tx_off()) keeps it where it is: it
// keeps receiving there for the slot timing and keys nothing without a begin; the split's end disarms it. False: not sent
bool rx2_dmr_main_arm(uint32_t hz, uint8_t power);
// The split is on: the board's tx() arms the receiver's transmitter, not its own
bool rx2_dmr_main_split(void);
// An RX2_DMR_EV payload (the link task)
void rx2_dmr_main_event(const uint8_t *p, uint8_t len, uint32_t now_ms);
// Every 10-50 ms from the link task: a transmission the receiver went quiet on (or down) ends here, so the core
// never waits for an event that will not come. up: the link
void rx2_dmr_main_poll(bool up, uint32_t now_ms);
const rx2_dmr_main_state_t *rx2_dmr_main_state(void);

// --- Receiver side ---
typedef struct {
	const hal_dmr_modem_t *modem;	// its own, receiving on the armed frequency while armed (the board starts it)
	// The role on (the board gives the modem to it: its own receiving stops) or off (back to the receiver's settings)
	bool (*split)(bool on, int8_t cc);
	// Receive on hz with the modem started (both slots, cc) and key the PA there per burst at the power level; 0: off
	bool (*arm)(uint32_t hz, uint8_t power);
	void (*send)(const uint8_t *p, uint8_t len);	// RX2_DMR_EV, from rx2_dmr_recv_poll
} rx2_dmr_recv_cfg_t;

typedef struct {
	bool split, armed, tx;
	int8_t cc;
	uint8_t taken;		// bursts taken (mod 256)
	uint32_t hz;
	uint8_t power;
	uint32_t requests, bursts, refused, dropped;
	uint32_t arm_fails;	// arms the board's arm() refused: never ready, so the main SoC transmits itself
} rx2_dmr_recv_state_t;

void rx2_dmr_recv_init(const rx2_dmr_recv_cfg_t *cfg);
// An RX2_DMR payload; (NULL, 0): the main SoC restarted, everything off
void rx2_dmr_recv(const uint8_t *p, uint8_t len);
// Its modem's events (any task): the transmit ones are queued for the main SoC
void rx2_dmr_recv_event(const dmr_event_t *ev);
// From the link task: the queued events out, and the state on a change or every RX2_DMR_BEAT_MS while split
void rx2_dmr_recv_poll(uint32_t now_ms);
const rx2_dmr_recv_state_t *rx2_dmr_recv_state(void);

#endif
