// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// hal_dmr_modem on an SCT baseband in layer 2 mode (docs/spec/modem-sct.md 4). The chip does layer 1 and 2: FEC,
// bursts, sync, slot timing and the voice frames from its own vocoder. The stack gives each burst's information bits
// as one DIGC_DATA_FRAME, paced at one per 60 ms by the chip's answers, and hears decoded bursts the same way.
//
// What differs from a burst-level modem:
// - Voice bursts carry no AMBE bits here. The first voice burst of a call (and burst A after set_embedded_lc) sends
//   the LC for the embedded signaling; the chip voices the call from its microphone path. Later voice bursts only
//   pace the queue: they are taken one per 60 ms and reported as sent.
// - The chip reports a received voice call once (its LC header, or the LC from the first superframe for a late
//   entry); it doesn't report each voice burst. Both reach the stack as a voice LC header with lc_valid, and from
//   there the driver reports a voice burst without payload every 60 ms until the terminator or the chip's sync
//   lost, so the stack holds the call as it does on other modems. The voice itself goes from the chip to the codec,
//   not through the MCU (the board sets CAP_MODEM_VOICE).
// - CSBK, data header, MBC header and PI header are reported with the CRC the chip checked, rebuilt and masked, so
//   the stack sees 12 bytes as from other modems.
//
// No RTOS calls and no waits: the board's modem task feeds packets in (sct_dmr_packet) and calls sct_dmr_poll, so
// the host tests drive it with a fake chip.
#ifndef SCT_DMR_H
#define SCT_DMR_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_dmr_modem.h"
#include "sct_pkt.h"

typedef struct {
	// One built packet to the chip, at task level. False if the bus failed
	bool (*send)(const uint8_t *buf, uint16_t len, void *ctx);
	void *ctx;
	// DMR_CALL_OPTION (docs/spec/modem-sct.md 4): byte 0 how the board wires the chip (squelch by the chip, IQ or
	// baseband, RF control), byte 1 IQ transmit and the voice delay, byte 2 good CACH before a verified slot and
	// LC header repeats, byte 3 which repeated messages are reported, byte 4 reserved. The traffic mode (byte 0
	// bits 1-0) is the driver's
	uint8_t call_option[5];
	// REPORT_FIELD bits beyond the driver's own (NEW_EVENT, FLC, CSBK, slot type and EMB)
	uint8_t report_field;
	// Optional: more fields for the start packet (TYPE 0x05), sent in order after the driver's: settings the
	// board takes from its calibration, for example. A field whose length is wrong fails start
	const sct_field_t *start_fields;
	uint8_t start_count;
	// Optional: a codec write sent when a transmission begins (codec_tx) and one when the chip goes back to
	// receive (codec_rx), such as muting an output while transmitting. Answered by a codec packet, which the
	// engine ignores
	const sct_codec_write_t *codec_tx;
	const sct_codec_write_t *codec_rx;
	// Optional: codec writes sent at each start, after the start packet: a codec setting the chip's own set-up
	// doesn't make, such as a filter with less delay (modem-sct.md 5)
	const sct_codec_write_t *codec_start;
	uint8_t codec_start_count;
	// Optional: control fields (TYPE 0) sent as each transmission begins, before its first packet, as the board's
	// state has them then: a setting that differs by band, for example. Also sent once at start, so that a
	// transmission's are never the chip's first (modem-sct.md 5). Returns how many (0: none)
	uint8_t (*tx_fields)(const sct_field_t **fields, void *ctx);
	// Optional: true once the chip has booted (start fails before). NULL: always
	bool (*ready)(void *ctx);
	// Optional: a recursive lock around the engine, taken by every entry (the stack's task calls the hal functions,
	// the modem task feeds packets). Events are sent with it held. NULL: one task only
	void (*lock)(void *ctx);
	void (*unlock)(void *ctx);
} sct_dmr_port_t;

typedef struct {
	uint32_t tx_frames;	// DIGC_DATA_FRAMEs sent
	uint32_t tx_acks;	// ... answered by the chip within their slot
	uint32_t tx_timeouts;	// ... not answered within it (the next one went anyway)
	uint32_t tx_paced;	// voice bursts taken without a frame
	uint32_t rx_frames;	// data frames reported
	uint32_t rx_fec;	// ... with an FEC error event before them
	uint32_t rx_voice;	// voice bursts reported for calls the chip plays
	uint32_t rx_bad;	// packets or fields that didn't parse
	uint32_t naks;		// ACK_MESSAGE codes other than OK, ready and far-end done
	uint8_t last_nak;
	uint8_t last_event;	// the last NEW_EVENT code
	uint8_t last_slot_found;
	uint8_t carrier;	// the last CARRIER_READY sent (0 lost, 1 ready, 2 ready with the phase reversed)
	uint32_t carrier_sent;	// CARRIER_READY packets sent
	bool sending;		// a frame is out, its answer not in yet
} sct_dmr_stats_t;

// After the last frame, how long to wait for the chip's "far end done" before reporting TX_DONE anyway
#define SCT_DMR_DONE_MS		600
#define SCT_DMR_SLOT_MS		60
// How long the board's carrier must stay lost before CARRIER_READY 0 goes out: a voice superframe
#define SCT_DMR_CR_HOLD_MS	360

void sct_dmr_init(const sct_dmr_port_t *port);
extern const hal_dmr_modem_t sct_dmr_modem;

// One packet from the chip (as sct_rx_feed gives it), at task level. Events go to the stack from here
void sct_dmr_packet(const uint8_t *pkt, uint16_t len, uint32_t now_ms);
// Pacing and timeouts. Call it every few ms while the modem runs (every 10 ms at the least)
void sct_dmr_poll(uint32_t now_ms);
// The board's carrier detection, as CARRIER_READY's value: 0 no carrier, 1 a carrier, 2 a carrier whose
// demodulated phase is reversed. The chip looks for sync only after it hears that a carrier came up, so a board
// calls this at each change, or every status poll (repeats cost nothing). The engine sends a change while it
// receives (a loss only once it has lasted SCT_DMR_CR_HOLD_MS, since a direct-mode station is silent between its
// bursts), and the current value again at each return to receive; it never sends one while it transmits. A board
// that never calls it relies on the chip's own squelch (docs/spec/modem-sct.md 3)
void sct_dmr_carrier(uint8_t ready);

void sct_dmr_get_stats(sct_dmr_stats_t *out);
void sct_dmr_reset_stats(void);

#endif
