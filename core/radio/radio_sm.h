// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine, without the RTOS: it takes requests and the time, polls board.radio and keeps
// the view. radio_task.c runs it in a task; the host tests run it against a fake board.
#ifndef RADIO_SM_H
#define RADIO_SM_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio.h"

#define RADIO_POLL_MS		25	// status poll and squelch period
#define RADIO_TICK_MS		10	// DMR call stack poll (radio_sm_tick)
#define RADIO_TOT_DEFAULT_S	180	// TX time-out
#define RADIO_SQUELCH_DEFAULT	5
#define RADIO_SQUELCH_MAX	9
#define RADIO_VFO_STEP_HZ	12500

// Software squelch on the chip's noise reading (1 dB units, larger = more noise). Level 0 is always open.
// Level 1..9 opens when noise < RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP, so 1 = 64 (loose) .. 9 = 40 (tight),
// and closes once noise rises RADIO_SQL_HYST_DB above that for RADIO_SQL_CLOSE_POLLS polls in a row.
#define RADIO_SQL_OPEN_L1	64
#define RADIO_SQL_DB_PER_STEP	3
#define RADIO_SQL_HYST_DB	3
#define RADIO_SQL_CLOSE_POLLS	2

// RX tone debounce: the AT1846S tone flag can drop out for a poll or two on a weak or fluttering signal. The tone
// counts as heard after RADIO_TONE_ON_POLLS matching polls in a row and as gone after RADIO_TONE_OFF_POLLS missing
// polls in a row (the chip's own detector already takes some 100-250 ms to report a match)
#define RADIO_TONE_ON_POLLS	2
#define RADIO_TONE_OFF_POLLS	8

// RSSI moves smaller than this don't bump the view serial (the snapshot still carries the latest value)
#define RADIO_RSSI_DEADBAND_DB	2

typedef enum {
	RREQ_CH_STEP,		// arg: direction
	RREQ_ZONE_POS,		// arg: position in the zone
	RREQ_ZONE,		// arg: zone index
	RREQ_VFO,		// arg: 1 on, 0 off
	RREQ_VFO_STEP,		// arg: direction
	RREQ_VFO_SET,		// arg: Hz
	RREQ_PTT,		// arg: 1 down, 0 up
	RREQ_MONITOR,		// arg: 1 down, 0 up
	RREQ_POWER,		// arg: direction
	RREQ_SQUELCH,		// arg: direction
	RREQ_CLEAR_TEMP,
	RREQ_BANDWIDTH,		// arg: radio_bw_t
	RREQ_TONE,		// arg: radio_tone_arg()
	RREQ_REPLY_CANCEL,	// DMR: end the reply window
	RREQ_DMR_TARGET,	// DMR: arg = ID (bits 23:0) | private (bit 24)
	RREQ_DMR_CONTACT,	// DMR: arg = contact index
	RREQ_DMR_CALL_BACK,	// DMR: talk to the last call heard
	RREQ_DMR_TARGET_CLEAR,	// DMR: back to the channel's talk target
	RREQ_DMR_PRIVACY,	// DMR: arg = type << 16 | key
	RREQ_HS_ENTER,		// hotspot: arg = RX Hz (0 = stay)
	RREQ_HS_FREQ,		// hotspot: arg = RX Hz, arg2 = TX Hz
	RREQ_HS_POWER,		// hotspot: arg = MMDVM RF level 0-255
	RREQ_HS_LEAVE,		// hotspot: back to the channel
	RREQ_RELOAD,		// the settings' preferences changed: read them again (codeplug changes are noticed alone)
} radio_req_type_t;

// RREQ_TONE's argument: which (RADIO_TONE_*) in bits 25:24, the tone type in 23:16, the value in 15:0
static inline int32_t radio_tone_arg(uint8_t which, subtone_t t)
{
	return (int32_t)(((uint32_t)(which & 3u) << 24) | ((uint32_t)t.type << 16) | t.value);
}

typedef struct {
	uint8_t type;		// radio_req_type_t
	int32_t arg;
	uint32_t arg2;
} radio_req_t;

#if CONFIG_DMR
#define RADIO_NAMES_MAX		(1 + CODEPLUG_LIST_LEN)	// talk target and the listen list's contacts
#define RADIO_LOOKUPS		4	// IDs looked up in all contacts / the DMR ID database, most recent kept

// A contact name the screens show for a talkgroup or an ID
typedef struct {
	uint32_t id;
	bool group;
	char name[CODEPLUG_NAME_LEN + 1];
} radio_name_t;
#endif

typedef struct {
	radio_view_t view;
	uint32_t serial;		// bumped on every visible change

	zone_t zone;			// the current zone's channel list
	channel_t vfo_ch;		// the VFO's settings, kept while on a channel
	bool vfo_seeded;		// vfo_ch holds a frequency
	bool have_channels;		// some zone holds a readable channel

	uint8_t base_squelch;		// squelch without the temporary change
	bool temp_power;
	bool temp_squelch;
	uint8_t base_bw;		// the channel's own bandwidth and tones, for undoing temporary changes
	subtone_t base_rx_tone;
	subtone_t base_tx_tone;
	uint8_t board_mode;		// radio_mode_t the board was last put in
	int16_t volume_sent;		// last level given to board.radio->volume, -1 = none yet

	bool ptt;			// PTT held
	bool retune_pending;		// receive settings changed during TX: retune on PTT release
	uint32_t tx_start_ms;
	uint8_t close_count;		// polls in a row above the close threshold
	uint8_t tone_count;		// polls in a row disagreeing with view.tone_ok (debounce)
	int16_t rssi_published;
	int8_t audio_out;		// last speaker / LED settings sent to the board, -1 = not yet
	int8_t green_out;
	int8_t red_out;
	bool hotspot;			// hotspot mode: the host link owns the modem
	uint32_t codeplug_serial;	// codeplug_serial() when the zone and channel were last read
	bool reload_pending;		// RREQ_RELOAD came while busy
#if CONFIG_DMR
	bool codec;			// the vocoder is present
	bool dmr_running;		// the call stack has the modem
	bool dmr_keyed;			// board.radio->tx() armed the per-burst keying (until tx_off)
	uint32_t own_id;		// the radio's DMR ID (codeplug, else RADIO_DMR_ID_DEFAULT)
	uint32_t dmr_serial;		// call stack view serial last copied
	uint32_t talk_id;		// the channel's talk target
	bool talk_private;
	uint8_t talk_ts;		// the contact's timeslot override, 0 = none
	uint32_t listen[CODEPLUG_LIST_LEN];	// the channel's listen list (talkgroup IDs)
	uint16_t listen_count;
	radio_name_t names[RADIO_NAMES_MAX];
	uint8_t name_count;
	radio_name_t lookups[RADIO_LOOKUPS];	// cache of codeplug_contact_find / dmr_iddb_lookup answers ("" = none)
	uint8_t lookup_next;
	// A talk target chosen for now (RREQ_DMR_TARGET / _CONTACT / _CALL_BACK), on channel manual_ch
	bool manual;
	uint32_t manual_id;
	bool manual_private;
	uint8_t manual_ts;
	uint16_t manual_ch;
	bool manual_vfo;
	uint8_t privacy_type;		// dmrPrivacyType_t, 0 = off
	uint16_t privacy_key;
	char radio_name[CODEPLUG_RADIO_NAME_LEN + 1];	// our talker alias text
	uint16_t group_hang_ms, private_hang_ms;	// codeplug hang times, 0 = the stack's default reply window
	uint32_t hs_rx_hz, hs_tx_hz;	// hotspot frequencies
	uint8_t hs_power;		// hotspot board power level
	bool hs_carrier;		// hotspot: the receiver hears a carrier
#endif
} radio_sm_t;

// Start-up: the codeplug and the settings (core/settings): the saved zone, channel or VFO, else zone 0 and its
// first channel (or the VFO); FM receive. Every request notes the position and the VFO in the settings, and
// radio_sm_poll re-reads the codeplug when codeplug_serial() moves
void radio_sm_init(radio_sm_t *sm, uint32_t now_ms);
void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms);
// Every RADIO_POLL_MS: status, squelch, audio, LEDs, TX time-out
void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms);

// Every RADIO_TICK_MS: the DMR call stack (poll, PTT keying, TX done, the DMR view). Nothing without CONFIG_DMR
void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms);

// Hotspot frequencies the board can use: RX in a band, TX in a band that allows transmit (the amateur band plan does
// not apply, docs/spec/gotchas.md 8.7)
bool radio_sm_hotspot_freq_ok(uint32_t rx_hz, uint32_t tx_hz);
// MMDVM RF level 0-255 -> board power level
uint8_t radio_sm_hotspot_power(uint8_t rf_level);

// Squelch thresholds for a level (exposed for the tests)
uint8_t radio_sm_squelch_open_below(uint8_t level);

#endif
