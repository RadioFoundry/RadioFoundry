// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The DMR voice call stack, modem neutral: it talks only to a hal_dmr_modem_t (docs/spec/dmr-modem.md section 6) and
 * runs at task level.
 *
 *  RX: color code / timeslot / destination filters (talk target, listen list, private calls to us, all call,
 *      hear all), call start on a voice LC header or by late entry (consecutive voice bursts plus an embedded LC that
 *      is not the previous call's, or is confirmed by a second superframe), voice to a sink, end on a terminator or
 *      the fade timeout, caller info with talker alias, emergency and privacy (service options, PI header).
 *  TX: listen before talk per the access policy, BS activation in repeater mode (BS_Dwn_Act CSBK, wait, retry),
 *      three voice LC headers, superframes A-F with the embedded LC, end at the superframe boundary with a terminator,
 *      TX time-out, reply window (separate lengths for group and private calls), talker alias in alternate
 *      superframes (CONFIG_DMR_TALKER_ALIAS), voice privacy through a hook (CONFIG_DMR_PRIVACY).
 *  GPS Info (CONFIG_DMR_GPS): the caller's position from an embedded GPS Info LC (FLCO 8); ours in the alias
 *      superframes when dmr_call_set_tx_gps gave one.
 *
 * Threading: the modem's event callback only copies events into a small single-producer / single-consumer queue
 * (slot ticks are counted, not queued). Everything else, including the voice sink and source calls, happens in
 * dmr_call_poll(), from one task. The other functions must be called from that same task.
 */
#ifndef RFOUNDRY_DMR_CALL_H
#define RFOUNDRY_DMR_CALL_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_dmr_modem.h"
#include "dmr/call/dmr_lc.h"

#define DMR_AMBE_FRAME_BYTES		9	// one 72-bit AMBE+2 air frame (with its FEC), 20 ms
#define DMR_AMBE_FRAMES_PER_BURST	3

// Timing (milliseconds unless noted)
// Through a repeater, receive its outbound between our bursts where the modem can (its tx_listen_split)
#ifndef DMR_CALL_TX_LISTEN
#define DMR_CALL_TX_LISTEN		1
#endif
#define DMR_CALL_RC_CONFIRM_MS		1000	// a second reverse channel cease within this stops our over
#define DMR_CALL_TX_DROP_MS		240	// listening, no repeater burst this long during our over: it dropped
#define DMR_CALL_TX_START_DROP_MS	540	// the same before our first burst: the modem's search for the slot gets two tries
#define DMR_CALL_TX_HEADERS		3	// voice LC headers before burst A
#define DMR_CALL_TX_TERMINATORS		1
#define DMR_CALL_FADE_MS		390	// no voice burst on the call for this long ends it (13 slots)
#define DMR_CALL_RESUME_MS		1500	// the same LC this soon after a call faded is that call, not a new one
#define DMR_CALL_WAKEUP_ATTEMPTS	3
#define DMR_CALL_WAKEUP_LISTEN_MS	600	// after each BS_Dwn_Act
#define DMR_CALL_REPEATER_HOLD_MS	300	// the repeater counts as up this long after its outbound was last seen
#define DMR_CALL_LBT_DMO_MS		120	// machine traffic: quiet needed before sending (gotcha 3.6)
#define DMR_CALL_LBT_RMO_MS		180
#define DMR_CALL_RMO_HANG_MS		3000	// machine traffic waits this long after a call through a repeater
#define DMR_CALL_PERMIT_MAX_MS		400	// the talk permit hold keys after this long whatever permit_busy says
// SFR user: which copies of a call are heard (dmr_call_config_t.sfr_rx)
typedef enum {
	DMR_SFR_RX_REPEATER,	// the repeat only: the talker's own copy is dropped
	DMR_SFR_RX_FIRST,	// both: each voice burst played once, whichever copy comes first
	DMR_SFR_RX_DUALPATH,	// both: a frame whose FEC failed is taken from the other copy when it decodes (CONFIG_SFR_DUALPATH_RX)
	DMR_SFR_RX_NUM
} dmr_sfr_rx_t;
#define DMR_SFR_DUALPATH_MS		120	// a burst with a failed frame waits this long for the other copy (90 ms behind)

#define DMR_CALL_OWN_ECHO_MS		700	// a call from our own ID this soon after we stopped transmitting is the repeater's echo of it; later, a parrot's playback
#define DMR_CALL_ALIGN_MS		300	// DMO: reply aligned to a station heard this recently (within the modem's slot memory, 390 ms on the HR_C6000)
#define DMR_CALL_TX_DONE_GUARD_MS	1500	// tx_done overdue after the last burst: force receive
#define DMR_CALL_TX_MAX_INFLIGHT	4	// bursts queued ahead of the air at most
#define DMR_CALL_TX_STALL_MS		1500	// a voice transmission the modem takes no burst of for this long is cut
#define DMR_CALL_TX_ALIGN_GRACE_MS	300	// more for an aligned start through a repeater: the modem's own refusal comes first
#define DMR_CALL_REPLY_WINDOW_S		10
#define DMR_CALL_DATA_TIMEOUT_MS	10000	// a burst list on air takes at most this plus 60 ms per burst (gotcha 3.9)
#define DMR_CALL_DATA_ALIGN_MS		300	// DMO data: aligned to a station heard this recently (the modem's slot memory)
#ifndef DMR_CALL_EVQ_DEPTH
#define DMR_CALL_EVQ_DEPTH		8	// power of two
#endif

// AMBE+2 silence, one 72-bit frame (B9 E8 81 52 61 73 00 2A 6B)
extern const uint8_t dmr_ambe_silence[DMR_AMBE_FRAME_BYTES];

typedef struct {
	uint32_t id;		// talkgroup or DMR ID (24 bits)
	bool private_call;	// unit to unit
} dmr_target_t;

// Which group calls are played. A group call passes when hear_all is on, or its TG is the talk target's, all call
// (0xFFFFFF), the reply target's during the reply window, in groups[], or group_ok() says so.
typedef struct {
	bool hear_all;		// every talkgroup (and private calls between others) on our timeslot
	bool any_cc;		// monitor: any color code too
	bool both_ts;		// monitor: either timeslot, the first call heard until it ends
	bool accept_bad_crc;	// the channel's Accept invalid CRC: a header / terminator LC that fails, taken when it repeats
	const uint32_t *groups;	// the channel's listen list (RX group list), owned by the caller, may be NULL
	uint16_t group_count;
	bool (*group_ok)(uint32_t tg, void *ctx);	// optional lookup instead of / besides the table
	void *group_ctx;
} dmr_listen_filter_t;

// Voice privacy hook: transform the 27 raw AMBE octets of a burst in place (TX before queueing, RX before the sink).
// voice_index 0-5 = A-F (docs/spec/gotchas.md 6.1, 6.2)
typedef void (*dmr_privacy_fn)(uint8_t ambe27[DMR_VOICE_BYTES], uint8_t voice_index, bool tx, void *ctx);

// Where a burst of a call with a PI header sits in the MI-based schemes' keystream (docs/spec/dmr-voice.md 5.1)
struct dmr_crypt_pos {
	uint8_t alg;		// the PI header's algorithm ID
	uint8_t key_id;
	uint32_t mi;		// its 32-bit message indicator
	uint32_t superframe;	// 0 = the superframe right after the PI header
	uint8_t voice_index;	// 0-5 = A-F
};
// MI-based encryption hook: as dmr_privacy_fn, for calls with a PI header. False: no key for it (RX: not played; TX:
// the over is sent as silence)
typedef bool (*dmr_crypt_fn)(uint8_t ambe27[DMR_VOICE_BYTES], const struct dmr_crypt_pos *pos, bool tx, void *ctx);
// A new MI for an over of ours (now_ms: some entropy). Never the same twice for a key: OFB keystream must not repeat
typedef uint32_t (*dmr_crypt_mi_fn)(uint32_t now_ms, void *ctx);

typedef struct {
	uint32_t own_id;
	uint8_t cc;			// 0-15
	uint8_t ts;			// 1 or 2
	bool rmo;			// through a repeater (BS sync, BS activation, terminators not trusted for call end)
	dmr_target_t talk_target;
	dmr_listen_filter_t listen;
	dmr_access_t access;		// voice PTT policy
	uint16_t access_wait_ms;	// polite and busy: wait this long for the channel (0 = refuse at once)
	uint8_t reply_window_s;		// group calls: 0 = off, up to 30
	uint8_t reply_window_private_s;	// private calls: 0 = reply_window_s, up to 30
	uint16_t tot_s;			// TX time-out, 0 = none
	uint8_t wakeup_attempts;	// BS_Dwn_Act tries, 0 = DMR_CALL_WAKEUP_ATTEMPTS
	uint16_t fade_ms;		// 0 = DMR_CALL_FADE_MS
	uint8_t tx_fid;			// voice LC FID (privacy schemes: 0x10)
	uint8_t tx_svc;			// voice LC service options (privacy bit, emergency, priority)
	dmr_privacy_fn privacy;		// NULL = none: bursts with PI set are not played
	void *privacy_ctx;
	dmr_crypt_fn crypt;		// NULL = none: calls with a PI header are not played
	void *crypt_ctx;
	// Our overs through crypt (tx_crypt_alg != 0, crypt and crypt_mi set): a PI header after the voice LC headers
	// with this algorithm, key ID and a new MI, then every voice burst encrypted. tx_fid / tx_svc are the caller's
	uint8_t tx_crypt_alg;
	uint8_t tx_crypt_key_id;
	dmr_crypt_mi_fn crypt_mi;
	// Our overs must be encrypted (an AES channel, whether its key loaded or not): an over that can't be is refused
	// (DMR_TX_ERR_CRYPT), never sent in the clear
	bool tx_crypt_required;
	char tx_alias[DMR_TA_TEXT_MAX + 1];	// our talker alias, "" = none (CONFIG_DMR_TALKER_ALIAS)
	uint8_t tx_alias_format;	// DMR_TA_7BIT or DMR_TA_ISO8
	// A user of a single frequency repeater (CONFIG_SFR, docs/spec/sfr.md 4; never with rmo): ts is the inbound slot.
	// Each call is on air twice: the talker's copy on the inbound slot, the repeat on the other one. sfr_rx says which
	// are heard. PTT sends on the inbound slot with tx_sync, in step with the repeater's slots when they were heard
	// recently (DMR_CALL_ALIGN_MS), else on our own timing (the repeater follows us)
	bool sfr_user;
	uint8_t tx_sync;		// dmr_sync_family_t of our bursts: DMR_SYNC_TS1 / TS2 / MS (DMR_SYNC_NONE = MS)
	uint8_t rx_sync;		// the repeat's sync: DMR_SYNC_TS1 / TS2 (TS framing), BS, or MS (the copies look alike)
	uint8_t sfr_rx;			// dmr_sfr_rx_t
	int8_t cc2;			// a second color code heard (an ERDM repeater's outbound), -1 = none
	// Talk permit before keying (radios whose beeps can't sound on air): once a voice call is admitted (the channel
	// free, the repeater up) it waits, receiving, at least permit_hold_ms and while permit_busy() is true, at most
	// DMR_CALL_PERMIT_MAX_MS, then keys. Each hold counts in view.permit_seq: the tone's cue. 0 = keys at once
	uint16_t permit_hold_ms;
	bool (*permit_busy)(void);	// NULL = the minimum alone
	// Receive both slots through a repeater (rmo): a trunked control channel (CONFIG_DMR_TRUNK). The stack still
	// takes calls on ts only; the other slot reaches the RX tap (dmr_call_set_rx_tap)
	bool both_slots;
} dmr_call_config_t;

// Caller info for the UI
typedef struct {
	uint32_t src;
	uint32_t dst;
	bool group;		// group call (FLCO 0), else private (FLCO 3)
	bool to_us;		// private call addressed to our ID
	uint8_t fid;
	uint8_t svc;		// service options (emergency, privacy, priority)
	bool encrypted;		// PI or the privacy service option
	bool late_entry;	// acquired from the embedded LC, no header heard
	bool emergency;		// the emergency service option
	bool pi_header;		// a PI header (data type 0) was heard for this call
	uint8_t pi_alg;		// in the DMRA layout (alg, 0x10, key ID, MI, 3 octets): the algorithm ID
	uint8_t pi_key;		// ... the key ID
	uint32_t pi_mi;		// ... and the message indicator
	char alias[DMR_TA_TEXT_MAX + 1];	// talker alias so far, "" if none
	bool alias_complete;
	bool gps_valid;		// a GPS Info LC was heard from this talker (CONFIG_DMR_GPS)
	dmr_gps_t gps;		// ... its position and error code
} dmr_caller_t;

// Where audio goes: the audio / vocoder side (core/dmr/ambe) FEC-decodes each 72-bit frame to 49 bits
typedef struct {
	void (*begin)(const dmr_caller_t *caller, void *ctx);	// optional: a call starts (reset the vocoder)
	void (*frame)(const uint8_t ambe72[DMR_AMBE_FRAME_BYTES], void *ctx);
	void (*lost)(void *ctx);				// one 20 ms frame missing: conceal
	void (*end)(void *ctx);					// optional: the call ended
	void *ctx;
} dmr_voice_sink_t;

// Where TX audio comes from: next() gives one encoded 72-bit frame if one is ready (false: not yet). The stack pulls
// at the air rate (three frames per 60 ms) and fills with AMBE silence only when a burst is due and frames are missing
typedef struct {
	void (*begin)(void *ctx);	// optional: the transmission was granted, start encoding
	bool (*next)(uint8_t ambe72[DMR_AMBE_FRAME_BYTES], void *ctx);
	void (*end)(void *ctx);		// optional: stop encoding
	// Optional: voice still to send after PTT is let go (an end of transmission tone, ambeVoiceTxTone). While it
	// returns true the over goes on at each superframe boundary, up to DMR_CALL_TX_TAIL_HOLDS of them
	bool (*busy)(void *ctx);
	void *ctx;
} dmr_voice_source_t;
#define DMR_CALL_TX_TAIL_HOLDS	3	// superframes (360 ms each) the over is held open for the source's tail

typedef enum { DMR_CALL_STATE_IDLE, DMR_CALL_STATE_RX, DMR_CALL_STATE_TX } dmr_call_state_t;

typedef enum {
	DMR_CALL_PHASE_IDLE,
	DMR_CALL_PHASE_RX_VOICE,	// playing a call that passed the filters
	DMR_CALL_PHASE_RX_BUSY,		// a call on our slot that we don't play (other TG, own echo, encrypted)
	DMR_CALL_PHASE_TX_ACCESS,	// PTT held, waiting for the channel (polite)
	DMR_CALL_PHASE_TX_WAKEUP,	// waking the repeater
	DMR_CALL_PHASE_TX_VOICE,
	DMR_CALL_PHASE_TX_ENDING,	// terminator queued, waiting for the modem's tx_done
	DMR_CALL_PHASE_TX_DENIED,	// PTT still held after a refusal: release to clear
	DMR_CALL_PHASE_TX_DATA,		// sending a data / CSBK burst list (dmr_call_data_tx), its repeater wake-up included
	DMR_CALL_PHASE_TX_PERMIT,	// admitted, still receiving: the talk permit hold before keying (permit_hold_ms)
} dmr_call_phase_t;

typedef enum {
	DMR_TX_OK,
	DMR_TX_ERR_NO_MODEM,
	DMR_TX_ERR_BUSY,		// polite and the channel stayed busy
	DMR_TX_ERR_NO_REPEATER,		// BS activation failed
	DMR_TX_ERR_REJECTED,		// the modem refused (tx_begin false or tx_rejected)
	DMR_TX_ERR_TIMEOUT,		// TX time-out ended the transmission
	DMR_TX_ERR_STALLED,		// the modem took no burst for DMR_CALL_TX_STALL_MS (no slot timing)
	DMR_TX_ERR_CEASED,		// the system stopped the over with the reverse channel
	DMR_TX_ERR_CRYPT,		// tx_crypt_required and the over can't be encrypted (no hook, key or MI)
} dmr_tx_error_t;

typedef struct {
	uint32_t serial;		// bumped on every visible change
	uint8_t state;			// dmr_call_state_t
	uint8_t phase;			// dmr_call_phase_t
	uint8_t tx_error;		// dmr_tx_error_t of the last PTT
	uint8_t cc;			// the channel's
	uint8_t ts;
	uint8_t rx_cc;			// the current / last call's color code (any_cc: may differ from cc)
	uint8_t rx_ts;			// its timeslot, 0 = unknown (simplex without TDMA direct mode sync)
	bool rmo;
	bool repeater_up;		// RMO: outbound heard recently
	int16_t rssi_dbm;		// last burst of the current / last call, 0 = unknown
	dmr_caller_t caller;		// RX: the current call; TX: us -> target
	uint32_t call_start_ms;
	uint32_t call_ms;		// duration of the current call (RX or TX), 0 when idle
	dmr_target_t tx_target;		// where PTT goes now (reply target during the reply window, else the talk target)
	bool reply_active;
	uint8_t reply_s;		// reply window seconds left (rounded up)
	dmr_caller_t last_heard;
	uint32_t last_heard_ms;		// poll time of its last burst, 0 = none
	uint32_t evq_overflows;		// diagnostics
	uint32_t tx_underruns;		// voice frames replaced with silence
	uint8_t wake_admits;		// counts repeater wake-ups answered after PTT was let go: the call would have been admitted
	uint8_t permit_seq;		// counts talk permit holds (permit_hold_ms): play the talk permit now
	uint8_t wake_sent;		// counts BS_Dwn_Act bursts sent (repeater wake-ups): the red LED blinks once per burst
} dmr_call_view_t;

void dmr_call_config_default(dmr_call_config_t *cfg);

// Bind the modem. No modem traffic yet
void dmr_call_init(const hal_dmr_modem_t *modem);
// Sink and source may be NULL (then audio is dropped / silence is sent)
void dmr_call_set_voice(const dmr_voice_sink_t *sink, const dmr_voice_source_t *source);
// Apply a channel / talk target / filter. A change of CC, TS or RMO ends any call in progress
void dmr_call_configure(const dmr_call_config_t *cfg);
// The board's RF side is in DMR mode: start the modem (false if it can't)
bool dmr_call_start(void);
// Leaving DMR: abort any call and stop the modem
void dmr_call_stop(void);

void dmr_call_ptt(bool down);
// Through a repeater, receive its outbound between our bursts (the modem's tx_listen_split) and wake it again when it drops
// under our over (DMR_CALL_TX_DROP_MS). Default DMR_CALL_TX_LISTEN
void dmr_call_set_tx_listen(bool on);
// Our position for the next transmissions (NULL: none). Taken when a transmission starts: one GPS Info LC in the
// rotation of the alias superframes (docs/spec/dmr-voice.md). CONFIG_DMR_GPS builds; ignored without
void dmr_call_set_tx_gps(const dmr_gps_t *gps);
// Clear the reply window (BACK on Home)
void dmr_call_reply_cancel(void);
// The whole stack runs here: queued modem events, PTT, timeouts, TX queue filling. Call every 10-25 ms with the time
void dmr_call_poll(uint32_t now_ms);
// Called when a transmission ends (the modem's DMR_EV_TX_DONE or DMR_EV_TX_REJECTED, already queued): poll now rather
// than at the next tick, so the receiver is back before a reply 30 ms after the last burst (a confirmed data ACK). From
// the modem's event callback, at task level (hal_dmr_modem.h): post, never poll or block. NULL: none. Kept across
// dmr_call_init
void dmr_call_set_wake(void (*fn)(void));

const dmr_call_view_t *dmr_call_view(void);

// Diagnostics counters (the debug link's GET_DIAG / RESET_COUNTERS; wrap freely)
typedef struct {
	uint32_t rx_calls;		// calls heard on our slot (a new voice LC, header or late entry)
	uint32_t rx_calls_resumed;	// faded calls that came back by late entry within DMR_CALL_RESUME_MS: not in rx_calls
	uint32_t rx_calls_played;	// ... of those, passed the filters and played
	uint32_t rx_late_entries;	// ... of those, started from the embedded LC (no header seen)
	uint32_t rx_voice_bursts;	// voice bursts handed to the sink (3 frames each)
	uint32_t rx_lost_frames;	// frames reported lost to the sink (missing bursts, voice lost, no privacy key)
	uint32_t rx_dup_dropped;	// voice bursts dropped as a repeated report of the previous one
	uint32_t rx_enc_dropped;	// voice bursts not played: privacy without a key
	uint32_t tx_calls;		// transmissions started
	uint32_t tx_denied;		// PTT refused (any dmr_tx_error_t)
	uint32_t tx_underruns;		// voice frames sent as silence
	uint32_t rx_in_tx;		// bursts the modem reported while we transmitted (tx_listen)
	uint32_t rx_bs_in_tx;		// of them, the repeater's (BS sync, our color code)
	uint32_t tx_rpt_drops;		// overs the repeater dropped under, or before their first burst, woken again
	uint32_t tx_wake_lost;		// wake-up CSBKs whose transmission ended without going on air (tried again at once)
	uint32_t evq_overflows;		// modem events dropped
	uint32_t rx_cc_dropped;		// bursts dropped for another color code (never voice A: it has no CC on air)
	uint32_t rx_lc_taken;		// header / terminator LCs taken with a failed check (listen.accept_bad_crc)
	uint32_t rx_ts_dropped;		// bursts of another slot than our call's / the channel's
	uint32_t rx_term_dropped;	// late-entry starts refused: the call a terminator just ended (its late reports)
	uint32_t rx_private_calls;	// rx_calls that were private (FLCO 3), to us or not
	uint32_t rx_emergency_calls;	// rx_calls with the emergency service option
	uint32_t rx_encrypted_calls;	// rx_calls marked private by the service options, PI or a PI header
	uint32_t rx_pi_headers;		// PI headers with a good CRC on our call's slot
	uint32_t tx_pi_headers;		// PI headers sent (encrypted overs, tx_crypt_alg)
	uint32_t rx_alias_complete;	// talker aliases completed
	uint32_t tx_alias_lcs;		// superframes sent with a talker alias block instead of the voice LC
	uint32_t rx_gps;		// GPS Info LCs decoded on our call
	uint32_t tx_gps_lcs;		// superframes sent with our GPS Info instead of the voice LC
	uint32_t rx_sfr_dup;		// SFR user: voice bursts dropped as the other copy of one already played
	uint32_t rx_sfr_direct;		// SFR user, repeat only: bursts of the talker's own copy dropped
	uint32_t rx_sfr_held;		// SFR user, dual-path RX: bursts held for the other copy (a frame's FEC failed)
	uint32_t rx_sfr_fixed;		// ... frames taken from the other copy
	uint32_t rx_end_term;		// received calls ended by their terminator
	uint32_t rx_end_sync;		// ... by the modem losing the channel
	uint32_t rx_end_fade;		// ... by no burst on the call for the fade time
	uint32_t rc_good;		// reverse channel commands heard while we transmitted (DMR_EV_RC, CRC good)
	uint32_t rc_bad;		// ... CRC failed (or a null fragment)
	uint32_t rc_ceased;		// overs the reverse channel stopped (DMR_TX_ERR_CEASED)
	uint8_t rc_last;		// the last command heard (DMR_RC_*)
} dmr_call_stats_t;
void dmr_call_get_stats(dmr_call_stats_t *out);
// The talker alias our calls send now ("" = none: off for the channel, no callsign, or built without it)
void dmr_call_tx_alias(char out[DMR_TA_TEXT_MAX + 1]);
// Zero the counters only (the view's evq_overflows / tx_underruns keep counting). From any task; an increment racing
// the reset may survive it
void dmr_call_reset_stats(void);

// RX trace (CONFIG_DEBUGLINK builds, DMR_RX_TRACE): every burst event as the stack saw it and what it did with it,
// packed into dlog() lines "crx @<slot> <entries>" (docs: dmr_call.c, rx trace). On by default; off saves the lines
void dmr_call_set_rx_trace(bool on);

// Listen before talk for machine traffic (data, ACKs, CSBK answers; gotcha 3.6): DMO 120 ms without voice, data or
// a call on our slot; RMO 180 ms without bursts on our slot, no call, and the last call over DMR_CALL_RMO_HANG_MS ago
bool dmr_call_channel_clear(uint32_t now_ms);
// Activity on the channel for the scan: the poll time of the last burst (not idle) on our slot with our color code,
// a call or not (a late entry before its LC is known counts). False if none since the stack was initialized
bool dmr_call_last_activity(uint32_t *ms);

// --- Data and CSBK (core/dmr/dmrDataService.c sits on these) ---

// Every data / control burst heard on our slot with our color code: CSBK, MBC header / continuation, data header,
// rate 1/2, 3/4 and 1 blocks, unified single block data. After the color code and slot filters, never while we
// transmit (the HR_C6000's reports are unreliable then, gotcha 14.1), from dmr_call_poll(). CRC checks are the
// receiver's: headers and CSBKs come with crc_ok as the modem said, blocks have no CRC of their own (gotcha 5.4)
typedef void (*dmr_data_rx_fn)(const dmr_burst_t *b, uint32_t now_ms, void *ctx);
void dmr_call_set_data_rx(dmr_data_rx_fn fn, void *ctx);

// A burst list to send (dmr_call_data_tx): next() gives the bursts in order (false: the list is over), done() is
// called once from dmr_call_poll() with a dmr_tx_error_t when the last burst has left the antenna (DMR_TX_OK) or the
// list was dropped (the repeater didn't wake, the modem refused, the time-out, a channel change)
typedef struct {
	bool (*next)(dmr_tx_burst_t *out, void *ctx);
	void (*done)(uint8_t err, void *ctx);
	void *ctx;
	uint8_t count;		// bursts in the list, for the time-out
	const char *tag;	// what the list is for, in the TX trace ("ars", "sms", "roam"); NULL = "data"
} dmr_data_source_t;

// Send a burst list now: on our slot, one burst per frame, the repeater woken first through a repeater that isn't
// up (as for voice). The caller has done its listen before talk (dmr_call_channel_clear); the modem is asked for
// impolite access. False (nothing started) while the stack is busy: a voice call being sent, PTT held, another
// list, or the modem not running. A PTT press during a list waits for it to end. *src is copied
bool dmr_call_data_tx(const dmr_data_source_t *src);
// A list is being sent (or its repeater woken)
bool dmr_call_data_busy(void);

// The RX tap (a trunked system, CONFIG_DMR_TRUNK): every burst heard, either slot, after the color code filter and
// before the slot filter, never while we transmit, from dmr_call_poll(). True takes the burst: the stack doesn't look at
// it further (control channel CSBKs aren't calls or data for the data service). NULL = no tap
typedef bool (*dmr_rx_tap_fn)(const dmr_burst_t *b, uint32_t now_ms, void *ctx);
void dmr_call_set_rx_tap(dmr_rx_tap_fn fn, void *ctx);
// Slot boundaries the modem has reported since the stack started (DMR_EV_SLOT_TICK), and the slot of the last one
// (1, 2, 0 unknown). A trunked system paces its slot work by them
uint32_t dmr_call_ticks(int8_t *ts);
// Tick n's (a dmr_call_ticks count) slot (0: unknown) and DMR_TICK_* flags; 0 and ts 0 once it is out of the last few
uint8_t dmr_call_tick_at(uint32_t n, int8_t *ts);
// CACH short LCs with a good CRC-8 (B.3.7) since boot, which a count newer than the caller's last says has come; *slc
// the latest (28 bits), *bad those with a bad CRC. Only from modems that report them (DMR_EV_SHORT_LC)
uint32_t dmr_call_short_lc(uint32_t *slc, uint32_t *bad);

#endif
