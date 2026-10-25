// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The APRS engine (docs/spec/aprs.md): who we are, where we are, when to beacon, what to send, and what was heard.
// It runs in the radio task (aprs_tick from radio_sm_poll); the radio core does the RF part of a transmission
// (aprs_rf_*: CSMA on the APRS frequency, keying, back to the user's channel). The UI, the debug link and the host
// link reach it through the calls marked "any task", which take the lock set with aprs_set_lock.
//
// Nothing is transmitted until the user turns APRS transmit on (or presses "send now"), the callsign is set and the
// APRS frequency is in an amateur band the radio may transmit in (docs/spec/aprs.md 3).
#ifndef RFOUNDRY_APRS_H
#define RFOUNDRY_APRS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "aprs/ax25.h"

#define APRS_TOCALL		"APZRDF"	// destination: the experimental APZ block (docs/spec/aprs.md 4.2)
#define APRS_SSID_DEFAULT	7		// handheld
#define APRS_DMR_DST_DEFAULT	5057u		// BrandMeister's APRS location service, "person" icon
#define APRS_LEVEL_DEFAULT	50
#define APRS_TXDELAY_DEFAULT	30		// 300 ms of flags
#define APRS_INTERVAL_DEFAULT	600		// s, when automatic beacons are turned on without SmartBeaconing
#define APRS_STATUS_MAX		40
#define APRS_STATIONS		12
#define APRS_MESSAGES		4
#define APRS_MSG_TRIES		3

typedef enum {
	APRS_REGION_NA, APRS_REGION_EU, APRS_REGION_AU, APRS_REGION_NZ, APRS_REGION_JP, APRS_REGION_CN,
	APRS_REGION_CUSTOM, APRS_REGION_COUNT,
} aprs_region_t;

typedef enum {
	APRS_PATH_NONE, APRS_PATH_WIDE1, APRS_PATH_WIDE1_WIDE2, APRS_PATH_WIDE2, APRS_PATH_WIDE2_2, APRS_PATH_COUNT,
} aprs_path_t;

typedef enum { APRS_POS_AUTO, APRS_POS_GPS, APRS_POS_HOST, APRS_POS_FIXED, APRS_POS_COUNT } aprs_pos_src_t;
typedef enum { APRS_VIA_RF, APRS_VIA_DMR, APRS_VIA_BOTH, APRS_VIA_COUNT } aprs_via_t;

typedef struct {
	bool tx_on;		// automatic beacons (manual "send now" works without)
	uint8_t region;		// aprs_region_t
	uint32_t freq_hz;	// APRS_REGION_CUSTOM's frequency
	bool narrow;		// 12.5 kHz on the APRS frequency
	uint8_t ssid;
	char sym_table, sym_code;
	uint8_t path;		// aprs_path_t
	bool compressed;
	uint8_t ambiguity;	// 0-4
	bool messaging;		// we take messages
	uint8_t pos_src;	// aprs_pos_src_t
	int32_t fix_lat, fix_lon;	// 1e-7 degree, both 0 = none
	int16_t fix_alt_m;	// INT16_MIN = none
	uint16_t interval_s;	// 0 = none
	bool smart;
	uint16_t distance_m;	// 0 = off
	bool after_ptt;
	bool boot;
	uint8_t level;		// AFSK level 0-100
	uint8_t power;		// RF power: 0 = the radio's current level (as for PTT), n = the board's level n - 1
	uint8_t txdelay;	// 10 ms units
	uint8_t via;		// aprs_via_t
	bool dmr_udt;		// ETSI UDT location, else Motorola LRRP
	uint32_t dmr_dst;	// destination ID (private call)
	bool kiss;		// KISS TNC on the host port
	bool to_is;		// our beacons to APRS-IS through the host helper
	bool rx_on;		// receive on the APRS frequency (boards with a receive path)
	bool igate;		// RF -> APRS-IS
	bool igate_tx;		// APRS-IS -> RF
	uint8_t digi;		// aprs_digi_mode_t
	bool dmr_gps;		// our position in DMR voice calls (ETSI GPS Info LC; Settings > DMR, CONFIG_DMR_GPS)
} aprs_cfg_t;

void aprs_cfg_default(aprs_cfg_t *c);
// The APRS frequency c selects (0 = none)
uint32_t aprs_cfg_freq(const aprs_cfg_t *c);
// Frequency of a region (0 for APRS_REGION_CUSTOM)
uint32_t aprs_region_hz(uint8_t region);
const char *aprs_region_name(uint8_t region);
const char *aprs_path_text(uint8_t path);
// In a 2 m, 1.25 m or 70 cm amateur band
bool aprs_amateur_hz(uint32_t hz);
// A plausible amateur callsign (letters and digits, 3-6 characters, a digit after the first character, not N0CALL)
bool aprs_callsign_ok(const char *call);

// Why nothing can be sent now (aprs_view_t.block)
typedef enum {
	APRS_OK, APRS_NO_CALL, APRS_NO_FREQ, APRS_NOT_AMATEUR, APRS_NO_POSITION, APRS_NO_AFSK, APRS_BUSY,
} aprs_block_t;

typedef enum { APRS_SRC_NONE, APRS_SRC_GPS, APRS_SRC_HOST, APRS_SRC_FIXED } aprs_src_t;

// How an RF transmission ended (aprs_rf_done). The failures name the step that stopped it
typedef enum {
	APRS_RF_SENT, APRS_RF_BUSY, APRS_RF_ABORTED, APRS_RF_NO_KEY, APRS_RF_NO_AFSK, APRS_RF_TIMEOUT,
} aprs_rf_result_t;

typedef struct {
	char call[AX25_CALL_LEN + 4];	// "N0CALL-7", "" = none
	uint8_t block;		// aprs_block_t for RF
	uint32_t freq_hz;
	uint8_t pos_src;	// aprs_src_t in use
	bool pos_valid;
	int32_t lat, lon;
	uint16_t speed_kmh;	// 0xFFFF unknown
	uint16_t course;	// 0xFFFF unknown
	uint32_t pos_age_s;
	uint16_t next_s;	// to the next time based beacon, 0xFFFF none
	uint8_t last_why;	// aprs_why_t of the last beacon
	uint32_t last_ms;	// when (0 = none yet)
	uint32_t rf_sent, rf_failed, rf_busy;	// RF transmissions done, failed, given up on a busy channel
	uint8_t rf_fail;	// aprs_rf_result_t of the last failed one (APRS_RF_SENT: none yet)
	uint32_t dmr_sent, dmr_failed;
	uint32_t rx_frames, rx_bad;	// frames decoded, rejected (RX builds)
	uint32_t host_in, host_out, host_dropped;	// KISS frames from / to the host, host frames refused
	uint32_t igated, is_to_rf, digi_sent;
	uint8_t stations, messages, unread;
	bool rf_active;		// the radio is on the APRS frequency for us now
	bool msg_pending;	// our message waiting for its ack / its next try
	uint8_t msg_tries;
	bool msg_acked;
	char last_info[48];	// the information field of our last beacon (truncated)
	uint32_t serial;	// bumped on every change worth a redraw
} aprs_view_t;

typedef struct {
	char call[AX25_CALL_LEN + 4];	// with SSID
	uint32_t heard_ms;
	bool direct;		// heard without a digipeater
	bool has_pos;
	int32_t lat, lon;
	char sym_table, sym_code;
	uint16_t course, speed_kn;
	uint8_t type;		// aprs_type_t of the last packet
	uint16_t count;
	char text[24];		// comment / status (truncated)
} aprs_station_t;

typedef struct {
	char from[AX25_CALL_LEN + 4];
	char text[68];
	char msgno[6];
	uint32_t ms;
	bool read;
} aprs_msg_t;

// --- The radio task ---

typedef struct {
	bool can_rf;		// an FM APRS transmission could start now (nothing else on the air, no PTT, no scan ...)
	bool fm_rx;		// receiving FM (the receive path's audio is worth demodulating)
	uint32_t rx_hz;		// on this frequency: APRS receive runs only on the APRS frequency
	bool on_dmr;		// the channel is DMR with the call stack running (digital positions can go out)
	uint32_t own_dmr_id;
} aprs_radio_t;

// Start-up (radio_sm_init), after the settings are loaded
void aprs_init(uint32_t now_ms);
// Every radio poll
void aprs_tick(const aprs_radio_t *r, uint32_t now_ms);
// Our own transmission (PTT, not APRS) ended: the "after PTT" trigger
void aprs_ptt_end(uint32_t now_ms);
// A storage job (it writes flash; not the radio or UI task): our path into the codeplug's APRS record 1, so the CPS
// reads what the radio uses. False when there is no such record or the write failed
bool aprs_path_to_codeplug(void);
// The settings changed underneath (RREQ_RELOAD): read them again
void aprs_reload(void);

// The level an RF transmission keys at: APRS's own setting (aprs_cfg_t.power: n = the board's level n - 1, capped at
// its highest) or, with none (0), the radio's current level
uint8_t aprs_tx_power(uint8_t current);

// RF job for the radio core
typedef struct {
	uint32_t hz;
	bool narrow;
	uint8_t level;
	const uint8_t *tones;
	uint16_t nbits;
} aprs_rf_job_t;
// A transmission is waiting (the radio core then checks it can start)
bool aprs_rf_pending(void);
// The radio core starts it: fills job (tones stay valid until aprs_rf_done). False: nothing after all
bool aprs_rf_take(aprs_rf_job_t *job, uint32_t now_ms);
// It went out, or not: a busy channel, or a failure (aprs_rf_result_t)
void aprs_rf_done(aprs_rf_result_t r, uint32_t now_ms);
// Seconds a pending job may wait for a clear channel before it is dropped
#define APRS_CSMA_GIVE_UP_MS	10000u

// The radio can receive APRS: a build with CONFIG_APRS_RX on a board with an AFSK receive path (board.afsk->rx_*) or
// FM receive audio on its PCM stream (AUDIO_TAP_FM_RX)
bool aprs_can_receive(void);
// The receive path is running (the receiver must stay awake: no battery saving)
bool aprs_rx_listening(void);

#if CONFIG_APRS_RX
#if CONFIG_APRS_RX
// The receive path's diagnostics (debug link): the largest sample since it started (0: only silence, the squelch never
// opened or no audio arrives), tone decisions, and the HDLC decoder's good frames, FCS errors and aborts
typedef struct {
	uint16_t peak;
	uint32_t bits, frames, fcs_errors, aborts;
} aprs_rx_stats_t;
void aprs_rx_stats(aprs_rx_stats_t *out);
#endif
// Received audio (the radio task reads it from the board's receive path or the PCM stream while receiving FM)
void aprs_rx_audio(const int16_t *s, size_t n, uint32_t now_ms);
// A decoded AX.25 frame (without the FCS): from the demodulator, or the tests
void aprs_rx_frame(const uint8_t *frame, size_t n, uint32_t now_ms);
// A DMR caller's position (ETSI GPS Info) for the station list: named by its alias's callsign, else its DMR ID.
// Any task. Not gated or sent anywhere: the list only
void aprs_dmr_heard(uint32_t id, const char *alias, int32_t lat, int32_t lon, uint32_t now_ms);
#endif

// --- Any task ---

void aprs_set_lock(void (*lock)(void), void (*unlock)(void));
void aprs_get_view(aprs_view_t *v);
void aprs_get_cfg(aprs_cfg_t *c);
// Settings in effect now and saved (core/settings)
void aprs_set_cfg(const aprs_cfg_t *c, uint32_t now_ms);
// "Send now": a position beacon at once (on the configured paths)
bool aprs_send_now(void);
// A status packet (">text")
bool aprs_send_status(const char *text);
// A message with acks and retries; false when one is still waiting or the text is unusable
bool aprs_send_message(const char *to, const char *text);
// The position in use now (GPS, host or fixed, as the beacons use it) for DMR: false when there is none. err_m is the
// error estimate in meters (position ambiguity included), dmr_gps the "send position in DMR calls" setting
bool aprs_position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps);
// RAM-only override for bench tests (debug link): the callsign ("" = back to the settings / codeplug)
void aprs_set_test_call(const char *call);
#if CONFIG_APRS_RX
uint8_t aprs_station_count(void);
bool aprs_station(uint8_t i, aprs_station_t *out);	// 0 = most recent
uint8_t aprs_msg_count(void);
bool aprs_msg(uint8_t i, aprs_msg_t *out);		// 0 = newest; marks it read
#endif

// --- The host link (aprs_host.c) and a board GPS ---

// A KISS data frame (AX.25 without FCS) from the host for RF: checked (our callsign as the source, rate limit)
bool aprs_host_frame(const uint8_t *frame, size_t n, uint32_t now_ms);
// An APRS-IS line from the host helper (KISS SetHardware 'G')
void aprs_host_is_line(const char *line, size_t n, uint32_t now_ms);
// An NMEA sentence from the host, or (gps true) from a board GPS driver
void aprs_nmea(const char *s, size_t n, bool gps, uint32_t now_ms);
// Settings > Location > GPS: off drops the board GPS fix and ignores its sentences, so the position falls back at once
void aprs_gps_enable(bool on);
// KISS-encoded bytes for the host: copies up to max, returns the count (0 = nothing waiting)
size_t aprs_host_output(uint8_t *out, size_t max);
// The KISS setting is on (host output allowed in the host link's idle mode)
bool aprs_host_enabled(void);

#endif
