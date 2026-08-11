// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core: owns the channel / VFO state, squelch, PTT and the TX timer, and drives board.radio.
// It runs in its own task. The UI reads a snapshot and posts requests; it never touches the hardware.
#ifndef RADIO_CORE_H
#define RADIO_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"
#include "config.h"

typedef enum {
	RS_IDLE,	// receiving, squelch closed
	RS_RX,		// receiving, squelch open (carrier and tone)
	RS_TX,
	RS_TX_DENIED,	// PTT held but transmit refused (RX-only, out of band, timed out); shown until PTT release
} radio_state_t;

// TXDENY_MODE: the channel's mode is not built in (a DMR channel in an FM-only build, or the reverse).
// DMR: NO_CODEC no vocoder merged (receive only), NO_DMR the modem didn't start, BUSY a call on our slot (polite
// access), NO_REPEATER the repeater didn't wake, REJECTED the modem refused, HOTSPOT the radio is a hotspot now,
// NO_ID no DMR ID in the codeplug
typedef enum {
	TXDENY_NONE, TXDENY_RX_ONLY, TXDENY_BAND, TXDENY_TIMEOUT, TXDENY_NO_FREQ, TXDENY_MODE,
	TXDENY_NO_CODEC, TXDENY_NO_DMR, TXDENY_BUSY, TXDENY_NO_REPEATER, TXDENY_REJECTED, TXDENY_HOTSPOT, TXDENY_NO_ID,
} tx_deny_t;

#if CONFIG_DMR
#define RADIO_ALIAS_MAX		31	// talker alias characters (DMR_TA_TEXT_MAX)

// What the DMR side is doing, for the screens
typedef enum {
	RDMR_IDLE,		// listening
	RDMR_RX,		// playing a call that passed the filters
	RDMR_RX_OTHER,		// a call on our slot we don't play (other talkgroup, encrypted)
	RDMR_TX_WAIT,		// PTT held: waiting for the channel or waking the repeater
	RDMR_TX,		// on air
	RDMR_TX_ENDING,		// PTT released: finishing the superframe
} radio_dmr_state_t;

typedef struct {
	uint32_t src, dst;
	bool group;		// group call, else private
	bool to_us;		// private call to our ID
	bool encrypted;		// privacy service option, PI or a PI header
	bool emergency;		// the emergency service option
	bool pi_header;		// a PI header was heard (an MI-based scheme: we can't decrypt it)
	char src_name[CODEPLUG_NAME_LEN + 1];	// codeplug contact, else the DMR ID database's text cut short, "" if unknown
	char dst_name[CODEPLUG_NAME_LEN + 1];	// talkgroup name, "" if unknown
} radio_dmr_call_t;

typedef struct {
	bool active;		// the call stack runs (a DMR channel, not hotspot mode)
	bool codec;		// the vocoder is merged: voice plays and PTT works
	uint8_t state;		// radio_dmr_state_t
	uint32_t own_id;
	uint8_t cc, ts;
	bool rmo;		// through a repeater
	bool repeater_up;
	// Where PTT goes now (the reply target during the reply window)
	uint32_t target_id;
	bool target_private;
	char target_name[CODEPLUG_NAME_LEN + 1];
	bool reply_active;
	uint8_t reply_s;	// reply window seconds left
	bool target_manual;	// a talk target chosen for now (contact, typed ID, call back), until the channel changes
	bool privacy;		// a privacy key is loaded: encrypted calls play, our calls go out encrypted
	bool alias_tx;		// our talker alias goes out on this channel
	bool hear_all;		// MONITOR held: every talkgroup on our slot
	// The current call (RX or TX); valid while state != RDMR_IDLE
	radio_dmr_call_t call;
	bool late_entry;
	char alias[RADIO_ALIAS_MAX + 1];	// talker alias so far (CONFIG_DMR_TALKER_ALIAS), "" if none
	int16_t rssi_dbm;	// last burst of the call, 0 = unknown
	uint32_t call_ms;	// duration of the current call
	// The last call heard
	radio_dmr_call_t last;
	uint32_t last_ms;	// hal_millis() of its last burst, 0 = none yet
	uint32_t underruns;	// TX voice frames sent as silence (diagnostics)
} radio_dmr_view_t;
#endif

typedef struct {
	radio_state_t state;
	uint8_t tx_deny;	// tx_deny_t, while RS_TX_DENIED
	bool vfo;		// tuning a frequency by hand rather than a channel
	uint16_t zone_index;
	uint16_t zone_pos;	// position in the zone, 0-based
	uint16_t zone_size;
	uint16_t channel_number;	// codeplug channel number, 0 in VFO
	char zone_name[CODEPLUG_NAME_LEN + 1];
	channel_t ch;		// what the radio is set to now (VFO: the VFO settings, with ch.name "VFO")
	uint8_t power;		// effective board power level index
	bool temporary;		// a temporary change (P7) is in effect on this channel
	uint8_t temp;		// which settings are temporary changes: RADIO_TEMP_* bits (temporary == (temp != 0))
	uint8_t squelch;	// 0 = open .. 9 = tight
	bool monitor;		// squelch held open by the user
	bool tone_ok;		// the RX tone or code is heard (debounced; true when the channel has no RX tone)
	int16_t rssi_dbm;
	uint8_t noise;
	uint32_t tx_elapsed_ms;	// time in TX, while RS_TX
	uint16_t tot_s;		// TX time-out, 0 = none
	uint32_t last_rx_ms;	// hal_millis() at the end of the last reception, 0 = none yet
	uint32_t vfo_step_hz;
	codeplug_source_t source;
	bool hotspot;		// the radio is an MMDVM hotspot: the host link drives the modem, channel keys are off
#if CONFIG_DMR
	radio_dmr_view_t dmr;
#endif
} radio_view_t;

// view.temp bits
#define RADIO_TEMP_POWER	0x01u
#define RADIO_TEMP_SQUELCH	0x02u
#define RADIO_TEMP_BW		0x04u
#define RADIO_TEMP_TX_TONE	0x08u
#define RADIO_TEMP_RX_TONE	0x10u

// Which tone radio_req_tone sets
#define RADIO_TONE_TX		0x01u
#define RADIO_TONE_RX		0x02u
#define RADIO_TONE_BOTH		(RADIO_TONE_TX | RADIO_TONE_RX)

void radio_core_start(void);
// A consistent copy of the current state
void radio_get_view(radio_view_t *v);
// Bumped on every visible change, so the UI can redraw only when needed
uint32_t radio_view_serial(void);

// Requests (queued, handled by the radio task)
void radio_req_channel_step(int8_t dir);	// next / previous channel in the zone, wraps
void radio_req_zone_pos(uint16_t pos);		// jump to a position in the zone
void radio_req_zone(uint16_t index);
void radio_req_vfo(bool on);
void radio_req_vfo_step(int8_t dir);
void radio_req_vfo_set(uint32_t hz);		// rx and tx frequency (simplex)
void radio_req_ptt(bool down);
void radio_req_monitor(bool down);
void radio_req_power(int8_t dir);		// temporary change of power level, wraps
void radio_req_squelch(int8_t dir);
void radio_req_clear_temporary(void);		// BACK on Home: revert temporary changes
void radio_req_reload(void);			// the settings' preferences changed (squelch, master power, DMR ID)
// FM bandwidth and sub-audio. On a channel these are temporary changes (P7); in the VFO they change the VFO itself.
// Refused on DMR channels, while transmitting, and for tones tones_valid() rejects
void radio_req_bandwidth(radio_bw_t bw);
void radio_req_tone(uint8_t which, subtone_t tone);	// which: RADIO_TONE_TX, RADIO_TONE_RX or RADIO_TONE_BOTH
#if CONFIG_DMR
void radio_req_reply_cancel(void);		// BACK on Home: end the DMR reply window
// Talk to someone else for now (until the channel changes or radio_req_dmr_target_clear): a typed ID or talkgroup,
// a codeplug contact, or back to whoever was heard last (their ID for a private call, the talkgroup for a group call)
void radio_req_dmr_target(uint32_t id, bool private_call);
void radio_req_dmr_contact(uint16_t index);
void radio_req_dmr_call_back(void);
void radio_req_dmr_target_clear(void);
#if CONFIG_DMR_PRIVACY
// Load a basic privacy key (type dmrPrivacyType_t, 0 = off). Held in RAM until changed or the radio restarts
void radio_req_dmr_privacy(uint8_t type, uint16_t key);
#endif
#endif
#if CONFIG_HOTSPOT
// Bumped when the hotspot engine reports a change (hotspot_radio_ops_t.changed), for the UI's hotspot screen
uint32_t radio_hotspot_serial(void);
#endif

#endif
