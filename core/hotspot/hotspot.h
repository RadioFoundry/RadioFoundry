// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The simplex DMR hotspot engine: between the MMDVM host's 33-byte DMR frames and the DMR modem (hal_dmr_modem.h).
//
//   host -> RF   DMR_DATA2 frames are typed from the burst itself (docs/spec/gotchas.md 8.2), voice is buffered and
//                sent as one transmission per call, ended by the host's terminator (8.1); a new call waits behind the
//                old one and keys again with its own headers (8.3); data goes out as whole lists (hotspotData.c).
//   RF -> host   every header, late entry with a header made from the embedded LC, silence fill for missed bursts,
//                one terminator, DMR_LOST 500 ms after the last burst (8.5, hotspotUplink.c).
//
// While the hotspot runs it owns the modem: the radio core has stopped its DMR call stack and handed the modem over
// (hotspot_radio_ops_t.enter). No RTOS and no board globals here, so it runs in the host tests; the host link task
// calls everything except hotspot_modem_event(), which the modem driver calls from its own task.
#ifndef ODR_HOTSPOT_H
#define ODR_HOTSPOT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hal_dmr_modem.h"

// ---- What the radio core implements for the hotspot (registered with hotspot_set_radio) ----
//
// Every op is called from the host link task, between MMDVM frames: none may block. GET_STATUS must be answered
// within tens of milliseconds and MMDVMHost resets the modem after 2 s without a status reply
// (docs/spec/mmdvm-protocol.md 2.4, 3.4), so an op that has slow work to do posts it to the radio core and returns.
typedef struct {
	// Leave normal operation for the hotspot: stop FM and the DMR call stack, release the DMR modem (no events to
	// the call stack any more), put the RF side in DMR mode and receive on rx_hz (0 = stay on the current
	// frequency until set_freq). True only once the modem is handed over and may be started now.
	// MUST NOT BLOCK: if the handover takes time, post the request to the radio core and return false; false also
	// when the radio can't now (no DMR, transmitting, in a menu that owns the radio ...). The engine calls it again
	// at most once per HOTSPOT_ENTER_RETRY_MS while the host keeps talking, and answers the host meanwhile (the
	// status shows no space until the hotspot runs). Calling it again while a request is pending must be harmless
	bool (*enter)(uint32_t rx_hz);
	// Tune for the hotspot: receive on rx_hz, transmit on tx_hz (simplex: equal). Called with the modem idle.
	// Hotspot frequencies may lie outside the amateur bands (docs/spec/gotchas.md 8.7): only the board's band
	// limits apply. False if out of range (the host gets a NAK). A few register writes; must not block for long
	bool (*set_freq)(uint32_t rx_hz, uint32_t tx_hz);
	// Transmit power from the MMDVM RF level (0-255, 255 = full): map it to a board power step
	void (*set_power)(uint8_t rf_level);
	// Back to normal operation; the modem is stopped already and the call stack gets it back (may be posted)
	void (*leave)(void);
	// Optional: a carrier on the channel (receiver noise / RSSI as the squelch sees it), for the host's
	// listen-before-talk (GET_STATUS carrier detect, docs/spec/gotchas.md 8.6). Called only while not transmitting
	bool (*carrier)(void);
	// Optional: the hotspot state changed (for the UI's hotspot screen); called from the host link task
	void (*changed)(void);
	// Optional: the received signal strength now in dBm (0 = unknown), a cached reading. Used for the RSSI bytes
	// after an uplink burst when the modem's burst has no rssi_dbm of its own (docs/spec/mmdvm-protocol.md 2.8, 5.4)
	int16_t (*rssi_dbm)(void);
	// Optional: SEND_CWID. Start sending the callsign in Morse on FM (about 20 wpm), then restore the DMR setup fully
	// (docs/spec/gotchas.md 8.13). Non-blocking: start it and return; false if it can't. Without it (or when it
	// returns false) the hotspot still ACKs the request, so the host's CW ID schedule doesn't fail, and counts it
	// in hotspot_stats_t.cwid_unsent: no ID goes out (see SEND_CWID in mmdvm.h)
	bool (*cw_id)(const char *callsign, uint8_t len);
	// Optional, with cw_id: true while the CW ID is keyed (the GET_STATUS TX flag stays set meanwhile)
	bool (*cw_busy)(void);
} hotspot_radio_ops_t;

#define HOTSPOT_ENTER_RETRY_MS	1000	// enter() refused: tried again at most this often

void hotspot_set_radio(const hotspot_radio_ops_t *ops);

// ---- Engine ----

// Frame output to the host: MMDVM command (DMR_DATA2, DMR_LOST2) and payload; mmdvm.c frames and sends it
typedef void (*hotspot_send_fn)(uint8_t command, const uint8_t *payload, uint8_t len);

void hotspot_init(const hal_dmr_modem_t *modem, hotspot_send_fn send);

// Take the modem (radio ops enter + modem start). False if the radio or the modem refused, or if the last refused
// attempt was less than HOTSPOT_ENTER_RETRY_MS ago (then nothing is called)
bool hotspot_start(uint32_t now_ms);
// Give the modem back to the radio core
void hotspot_stop(void);
bool hotspot_running(void);

// SET_CONFIG: color code, simplex flag (MMDVMHost Duplex=0) and the TX delay (ms)
void hotspot_config(uint8_t cc, bool simplex, uint16_t tx_delay_ms);
// SET_FREQ: drops anything in flight (the host should wait for idle first, docs/spec/gotchas.md 8.6); an uplink call
// that was open gets its DMR_LOST2 first, and nothing heard before the retune goes up after it. False: refused
bool hotspot_set_freq(uint32_t rx_hz, uint32_t tx_hz, uint8_t rf_level);
// SET_MODE idle: end the call on air with its terminator, drop everything queued behind it
void hotspot_net_idle(void);
// Pass what is heard up to the host (true), or keep receiving without passing it up (false: POCSAG state)
void hotspot_set_uplink(bool on);
// SEND_CWID accepted (the caller checked the state and ACKed): hand it to the radio's cw_id. True if it is sent
bool hotspot_cwid(const uint8_t *callsign, uint8_t len);

// A DMR_DATA2 frame from the host (control byte, 33 bytes). 0 if taken, else the MMDVM NAK reason
uint8_t hotspot_net_frame(uint8_t control, const uint8_t *frame, uint32_t now_ms);
// DMR_ABORT: drop what the host sent that is not on air yet, and stop transmitting
void hotspot_net_abort(void);

// The modem driver's event callback (dmr_event_cb_t); copies the event into a small queue for hotspot_poll()
void hotspot_modem_event(const dmr_event_t *ev, void *ctx);

// Run often (every few ms) from the host link task: modem events, the TX feed, data lists, uplink timers
void hotspot_poll(uint32_t now_ms);

// ---- Status for GET_STATUS ----
uint8_t hotspot_space(void);		// DMR frames the host may still send (the smaller of voice buffer and data list,
					// less a margin for frames already on their way, docs/spec/mmdvm-protocol.md 2.4)
bool hotspot_transmitting(void);	// from buffering until the last burst is on air, data queued and CW ID included
bool hotspot_carrier(void);		// a call or data is being heard
bool hotspot_rx_overflow(void);		// sticky until read: an RF burst was lost (event queue full)
bool hotspot_tx_overflow(void);		// sticky until read: a host frame was refused for lack of space

// ---- For the UI's hotspot screen (a snapshot; fields may be a poll apart) ----
typedef struct {
	bool running;
	bool transmitting;	// sending what the host sent
	bool receiving;		// a call or data heard on RF is going up
	uint32_t rx_hz, tx_hz;
	uint8_t cc;
	uint32_t src, dst;	// the last call's IDs (either direction), 0 = none yet
	bool group;		// that call was a group call
	bool from_net;		// that call came from the host
} hotspot_view_t;
void hotspot_get_view(hotspot_view_t *v);

// ---- Counters, for tests and a diagnostics page ----
typedef struct {
	uint32_t net_frames, net_dropped, net_calls, rf_bursts, rf_frames_up, rf_lost, events_dropped;
	uint32_t tx_rejected;
	uint32_t cwid_requests, cwid_unsent;	// SEND_CWID accepted; of those, not keyed (no cw_id op, or it refused)
	uint32_t enter_attempts;		// hotspot_start() calls that asked the radio (all sessions since boot)
	uint32_t starts, stops;			// the modem taken / given back (sessions since boot)
} hotspot_stats_t;
void hotspot_get_stats(hotspot_stats_t *s);

#endif
