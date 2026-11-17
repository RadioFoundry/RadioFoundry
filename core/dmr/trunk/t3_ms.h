// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR Tier III mobile station engine (docs/spec/dmr-trunking.md): hunting for the selected system's control
// channel (TSCC), camping on it, following the grants for the user's talkgroups and private calls to the radio, and
// returning to the control channel when the call ends. ETSI TS 102 361-4 6.3 (acquisition and retention), 6.4
// (registration), 6.6 (voice calls), Annex D (hunting).
//
// No hardware, no RTOS, no codeplug: the radio core feeds it what the modem hears and acts on what t3_ms_poll()
// returns, as with the roaming engine.
//   t3_ms_start()     the user selected a trunked system
//   t3_ms_rx()        every data burst heard (CSBK, MBC header, MBC continuation) with its slot and color code
//   t3_ms_slc()       every short LC the CACH carried (CRC-8 good)
//   t3_ms_activity()  any sync on the slot being followed (voice or data), for the inactivity timer
//   t3_ms_call_burst() a call's burst (voice, its header or terminator) on a slot, with its color code
//   t3_ms_level()     the signal level of the channel tuned, for leaving a weak TSCC and sampling an adjacent one
//   t3_ms_poll()      every 10-30 ms: T3_ACT_TUNE means retune to act->chan and listen to act->slot (0: both)
//   t3_ms_stop()      another zone or channel selected
//   t3_ms_slot()      once per TDMA frame for each slot heard, at its CACH: the AT bit for the inbound slot after it
// Access (registration, calls, answers) is built with CONFIG_DMR_TRUNK_TX. T3_ACT_SEND: send a CSBK (or a UDT header
// and its blocks) on act->slot. A radio in listen-only mode never transmits.
#ifndef RFOUNDRY_T3_MS_H
#define RFOUNDRY_T3_MS_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "dmr/trunk/t3_pdu.h"
#include "dmr/trunk/t3_sys.h"
#include "dmr/trunk/t3_plan.h"

// Data types of the slot type (TS 102 361-1 9.3.6) the engine looks at
#define T3_DT_CSBK		3
#define T3_DT_MBC_HDR		4
#define T3_DT_MBC_CONT		5
#define T3_DT_DATA_HDR		6	// a UDT header
#define T3_DT_R12		7	// a UDT block

#define T3_CTRL_MAX		4	// programmed control channel candidates
#define T3_SITES_MAX		8
#define T3_TG_MAX		32
#define T3_DGNA_MAX		15	// DGNA_Address mode talkgroups (6.6.8.1.1)
#define T3_DGNA_ALIAS_MAX	21	// DGNA_Alias mode characters

// Talkgroup flags
#define T3_TG_LISTEN		0x01	// follow calls to it
#define T3_TG_TALK		0x02	// the default talk group (CONFIG_DMR_TRUNK_TX)
#define T3_TG_BROADCAST		0x04	// the radio's calls to it are broadcasts: one-way (6.6.2.1)
#define T3_REG_TG_MAX		7	// talkgroups in a subscription list (6.4.4.1.13.2, table 6.10)

// System options
#define T3_OPT_LISTEN_ONLY	0x0001	// never transmit (and so never register)
#define T3_OPT_ALLCALL		0x0002	// follow all-calls (ALLMSID, ALLMSIDL, ALLMSIDZ)
#define T3_OPT_EMERGENCY_ANY	0x0004	// follow emergency grants on any talkgroup
#define T3_OPT_MONITOR		0x0008	// follow every talkgroup grant (a scanner's view; listen-only)
#define T3_OPT_LEARN_SYSCODE	0x0010	// take the first system heard as ours (model/net unset)
#define T3_OPT_PRIVATE		0x0020	// follow private calls to the radio's ID (on by default)
#define T3_OPT_REG_TG		0x0040	// register with the talk group (subscription) rather than the system code
#define T3_OPT_ALS		0x0080	// accept Ambient Listening (6.6.2.6): the radio transmits by itself when called so
#define T3_OPT_KILL		0x0100	// accept an authenticated kill (6.4.10); the radio core keeps it in the settings (t3_kill)
#define T3_OPT_NO_ROAM		0x0200	// stay on a TSCC whatever its signal: no Vote Now sampling, no leaving a weak one
#define T3_OPT_STUN		0x0400	// accept a stun or revive (6.4.9): authenticated first with K, else as sent

// Timers and limits (TS 102 361-4 Annex A ranges in the comments). Fixed, but for the few t3_cfg_t overrides
// (T_Awake, T_ALS, T_ALS_E, the Vote Now sample)
#define T3_AWAKE_MS		2000	// T_Awake, 0.1-60 s: power save, awake this long after traffic for the radio
#define T3_NOSIG_MS		5000	// T_Nosig, 1-15 s: no valid TSCC PDU this long, hunt again
#define T3_DWELL_MS		700	// hunting: time on a candidate (12 TSCC slots and two short LCs)
#define T3_SETTLE_MS		60	// after a retune: what is heard before this belongs to the old channel
#define T3_INACTIVE_MS		2500	// TV_Inactive, 0-20 s: nothing on the followed slot this long, return
#define T3_CALL_MAX_MS		300000	// a followed call ends by this at the latest
#define T3_REGRANT_MS		2000	// a call just left isn't followed again this soon
#define T3_DATA_LINGER_MS	3000	// the radio's data item went through: a reply may come this long before it leaves
#define T3_PROTECT_ONE_MS	1000	// EN_PTT_ONE_MS for another radio holds PTT off this long after the last one
#define T3_TRAND_TC_MS		10000	// TRand_TC, 2-60 s: the longest a random access may take
#define T3_TP_MS		20000	// TP_Timer: after a progress acknowledgement to a call request
#define T3_TNP_MS		10000	// TNP_Timer: after one to a request without a payload channel
#define T3_ALS_S		30	// T_ALS (A.1: 10-300 s)
#define T3_ALS_E_S		60	// T_ALS_E (10-14 400 s)
#define T3_ALS_REQUEST_MS	5000	// T_ALS_REQUEST_LIFE_SPAN (1-10 s)
#define T3_LENT_MS		360	// a composite TSCC lent to a call: no access for 3 short LCs after the last such one
#define T3_NRAND_NR		6	// random access tries, a request
#define T3_NRAND_NE		10	// ... an emergency request
#define T3_N_MAINT		4	// P_MAINT bursts at the end of a call we end
#define T3_REG_RETRY_MS		30000	// a registration nobody answered is tried again after this
#define T3_REG_TG_DELAY_MS	1000	// talkgroups changed: register with them once they stay this long
#define T3_CLI_CALL_MS		10000	// a gateway's caller number belongs to its call granted this soon after
#define T3_REPEAT_MS		4000	// a status or short data the same as the last heard this soon is the TSCC's repeat
#define T3_SUPL_HOLD_MS		10000	// supplementary data waits this long for the call that names its sender
#define T3_DIAL_MAX		44	// dialed digits through a gateway (the Proxy Flag's most, 6.6.2.1.2)
#define T3_DENIED_MAX		4	// SYS_AREAs that denied registration (cleared by t3_ms_start)
#define T3_ADJ_MAX		4	// adjacent sites' TSCCs (Adjacent_Site, Vote_Now), hunted first
#define T3_VOTE_MS		360	// sampling an adjacent TSCC after a Vote Now (VOTE_BLK: 2-10 frames)
#define T3_VOTE_MARGIN_DB	6	// an adjacent TSCC this much stronger is better (no radiated preference)
#define T3_WEAK_MS		5000	// camped with the level below leave_dbm this long: hunt (6.3.3.1 a)
#define T3_VOTE_AGAIN_MS	30000	// an adjacent TSCC sampled isn't sampled again this soon, unless ours is weak

typedef struct {
	uint32_t id;
	uint8_t flags;
} t3_tg_t;

// An adjacent site's TSCC (6.7.1.3, 6.7.1.7): its channel, the 14 MSBs of its system code, and the priorities a
// Vote Now gave (table 6.81: 1 highest .. 7 lowest, 0 none radiated)
typedef struct {
	t3_chan_t chan;
	uint16_t sys14;
	uint8_t conf_prio;
	uint8_t adj_prio;
	int16_t dbm;		// its level when last sampled, 0 = never
	uint32_t sampled_ms;	// when (0: never)
} t3_adj_t;

typedef struct {
	uint32_t own_id;
	t3_auth_t auth;			// model, net, category; auth.sites points at sites[] below
	uint16_t sites[T3_SITES_MAX];
	uint8_t dmrla;
	t3_plan_t plan;
	t3_chan_t ctrl[T3_CTRL_MAX];	// control channel candidates, in hunt order
	uint8_t nctrl;
	t3_tg_t tg[T3_TG_MAX];
	uint8_t ntg;
	uint16_t opts;			// T3_OPT_*
	int16_t leave_dbm;		// signal quality (6.3.2.3): a TSCC below this is left for another; 0 = never
	uint16_t vote_ms;		// 0 = default
	uint32_t reg_id;		// the ID registered and called from, 0 = own_id
	uint32_t seed;			// random access backoff; 0 = from the ID and the time
	bool auth_key_set;		// authentication (6.4.8): K and the PSN; without K a challenge is not supported
	uint8_t auth_key[T3_AUTH_KEY_LEN];
	uint32_t psn;
	uint8_t ps_rq;			// power save asked for at registration (6.4.7, table 6.13): 0 off, 1-7 = 1:2-1:128
	uint32_t ip_addr;		// the radio's IPv4 address, advised at registration (6.4.11); 0: none
	uint16_t awake_ms;		// T_Awake, 0 = default
	uint16_t als_s, als_e_s;	// T_ALS, T_ALS_E (A.1), 0 = default
	// The answer to a short data poll (6.6.5.7): poll_len octets in UDT_Format poll_format; 0 octets: refused
	const uint8_t *poll_data;
	uint8_t poll_len;
	uint8_t poll_format;
	// Supplementary user data (6.4.13), in a format and size prearranged with the system: offered with every voice,
	// packet data, short data and status request while supl_len > 0 (up to T3_UDT_MAX octets)
	const uint8_t *supl_data;
	uint8_t supl_len;
	uint8_t supl_format;
	// The digits a call through a dialing gateway uploads (6.6.2.2.3; t3_dial_pack): dial_n BCD digits, the first in
	// the high nibble (CONFIG_DMR_TRUNK_GW)
	const uint8_t *dial;
	uint8_t dial_n;
} t3_cfg_t;

typedef enum {
	T3_OFF,
	T3_HUNT,	// looking for the TSCC
	T3_CAMPED,	// on the TSCC, idle
	T3_FOLLOW,	// on a payload channel, in a call
} t3_state_t;

typedef enum {
	T3_REG_UNKNOWN,		// no TSCC confirmed yet
	T3_REG_NOT_NEEDED,	// Reg = 0
	T3_REG_LISTEN,		// Reg = 1 and the system is listen-only here: following only
	T3_REG_NEEDED,		// Reg = 1, no record for this SYS_AREA (CONFIG_DMR_TRUNK_TX registers)
	T3_REG_OK,
	T3_REG_DENIED,
} t3_reg_t;

typedef enum { T3_ACT_NONE, T3_ACT_TUNE, T3_ACT_SEND } t3_act_type_t;

typedef struct {
	t3_act_type_t type;
	t3_chan_t chan;		// TUNE
	uint8_t slot;		// TUNE: 0 both (a control channel), 1 / 2 the payload slot. SEND: the slot to send on
	bool payload;		// TUNE: a payload channel. SEND: on the payload channel (else the TSCC)
	uint8_t burst[T3_CSBK_BYTES];	// SEND: the CSBK, CRC included (udt: a UDT header)
	bool udt;		// SEND: burst is a UDT header, followed by nblocks rate 1/2 blocks in the next frames
	uint8_t nblocks;
	const uint8_t (*blocks)[T3_UDT_BLOCK];	// the engine's, valid until the next t3_ms_* call
} t3_act_t;

#define T3_ACT_QUEUE	3

// Services the radio asks for (CONFIG_DMR_TRUNK_TX)
typedef enum {
	T3_SVC_NONE,
	T3_SVC_REGISTER,
	T3_SVC_DEREGISTER,
	T3_SVC_GROUP_VOICE,
	T3_SVC_IND_VOICE,
	T3_SVC_CANCEL,
	T3_SVC_STATUS,		// a status (or a poll, T3_STATUS_POLL) to an MS or talkgroup (6.6.6)
	T3_SVC_SDATA,		// UDT short data to an MS or talkgroup (6.6.4)
	T3_SVC_PDATA,		// a packet data channel to an MS or talkgroup (6.6.3): the data service sends on it
	T3_SVC_DIVERT,		// set or clear a call diversion (6.6.7)
	T3_SVC_SDPOLL,		// ask an MS for its short data (6.6.5)
} t3_svc_t;

#define T3_STATUS_POLL		127	// the status that asks the called MS for its own (6.6.6.2)
#define T3_STATUS_EMERGENCY	126	// 6.6.6.3.1
#define T3_STATUS_EMERGENCY_CANCEL	124	// 6.6.6.3.2
#define T3_STATUS_ALS_END	123	// ends an ALS call: the TS stops the talker (6.4.14.3.4)
#define T3_STATUS_INTERRUPT	125	// asks the TS to stop the talker (6.4.14.3.3)

typedef enum {
	T3_RES_NONE,
	T3_RES_PENDING,		// trying, or acknowledged and waiting (ax.progress says which)
	T3_RES_GRANTED,		// the call has its channel: the engine follows it
	T3_RES_DONE,		// registered, de-registered, canceled, call-back accepted
	T3_RES_REFUSED,		// a NACK: ax.reason
	T3_RES_NO_ANSWER,	// every try went unanswered
	T3_RES_TIMEOUT,		// TRand_TC, or TP / TNP after a progress acknowledgement
	T3_RES_NOT_ALLOWED,	// not now: listen only, not registered, no TSCC, PTT disabled
	T3_RES_LOST,		// the TSCC was lost meanwhile
	T3_RES_CANCELED,	// withdrawn before anything was sent
	T3_RES_DIVERTED,	// the called party's calls are diverted: divert_to (6.6.1.3 a 2)
} t3_res_t;

typedef enum { T3_AX_IDLE, T3_AX_WAIT_SLOT, T3_AX_WAIT_RESP, T3_AX_ACKED } t3_ax_state_t;

// The call being followed (or last followed), for the UI and the debug view
typedef struct {
	uint8_t op;		// the grant's opcode
	uint32_t dst;
	uint32_t src;
	bool group;
	bool emergency;
	t3_chan_t chan;
	uint8_t slot;
	uint32_t start_ms;
	bool ours;		// the radio asked for it (it may talk at once, and ends it with P_MAINT)
	bool data;		// a packet data call (PD_GRANT, TD_GRANT): the data service has the slot, PTT doesn't
	bool als;		// an Ambient Listening call to the radio: it transmits until als_end (6.6.2.6.3)
	bool als_ours;		// an Ambient Listening call the radio asked for: it listens
	uint32_t als_end;
	bool cli;		// a gateway's call whose caller's number (t3_ms_t.cli) came just before it (figure 6.45)
} t3_call_t;

#define T3_RA_LOG	4
#define T3_RA_NONE	0xFFu
typedef struct {
	uint32_t csbk;		// CSBKs with a good CRC
	uint32_t crc_bad;	// CSBKs / MBC headers with a bad CRC
	uint32_t grants;	// grants heard while camped
	uint32_t followed;
	uint32_t unmapped;	// grants to a channel no plan knows
	uint32_t returns;
	uint32_t hunts;
	uint32_t foreign;	// Alohas and broadcasts from another system (wrong syscode)
	uint32_t foreign_slc;	// C_SYS_Parms from another system (an 8-bit CRC: some are corrupt words)
	uint32_t lent;		// P_SYS_Parms "composite" on the TSCC's carrier while camped: its slot lent to a call
	uint32_t lent_calls;	// a call's bursts on the TSCC's slot, no short LC heard there: taken as lent
	uint32_t learned;	// Chan_Freq / CG_AP entries learned or changed
	uint32_t ahoys;		// C_AHOYs to the radio on the TSCC; the last one's source, time and Service_Kind
	uint32_t ahoy_src, ahoy_ms;
	uint8_t ahoy_kind;
	uint16_t last_unmapped;
	uint32_t sent;		// CSBKs handed to the radio to send
	uint32_t ra_tries;	// random access bursts among them
	uint32_t votes;		// adjacent TSCCs sampled after a Vote Now
	uint32_t vote_moves;	// ... and moved to
	uint32_t weak_leaves;	// TSCCs left for a weak signal
	uint32_t adj_refused;	// adjacent sites announced with a system code the radio may not use
	// The last T3_RA_LOG random access bursts (the debug link's t3.ra*): when each went, and what answered it
	struct {
		uint32_t tx_ms;		// when it was handed to the radio
		uint16_t rx_ms;		// the answer, this many ms after (0: none)
		uint16_t air_ms;	// the radio reported it sent this many ms after (t3_ms_sent; 0: not reported)
		uint8_t tx_slot;
		uint8_t frames;		// the answer came this many frames after it (T3_RA_NONE: none)
		uint8_t rx_slot;
		uint8_t op;		// the answer's opcode
	} ra_log[T3_RA_LOG];	// burst n (ra_tries counts from 1) at (n - 1) % T3_RA_LOG
} t3_stats_t;

typedef struct {
	const t3_cfg_t *cfg;
	t3_auth_t auth;		// cfg->auth, with model and net filled in by T3_OPT_LEARN_SYSCODE
	bool auth_learned;
	t3_state_t state;
	t3_reg_t reg;
	t3_learned_t learned;

	// the confirmed TSCC
	bool confirmed;
	t3_chan_t tscc;
	uint16_t syscode;
	t3_aloha_t aloha;
	bool csc_valid;
	uint16_t csc;
	uint8_t tscc_slots;	// bit 0: TS1 carries the TSCC, bit 1: TS2

	// where the radio is
	t3_chan_t tuned;
	uint32_t tuned_ms;
	t3_act_t q[T3_ACT_QUEUE];	// actions for t3_ms_poll, oldest first
	uint8_t nq;
	bool aloha_here;	// an Aloha since the radio came to this TSCC (random access waits for one)
	bool slc_here;		// a short LC from our system since the radio came to this TSCC

	// hunting
	uint32_t slc_foreign;	// the last foreign C_SYS_Parms code + 1 (0: none): two alike leave a candidate
	bool retry_tscc;	// the lost TSCC is tried before the list
	uint8_t hunt_idx;	// next candidate: the adjacent sites' TSCCs, then ctrl[]
	uint32_t hunt_deadline;

	// timers
	uint32_t last_valid_ms;	// last PDU from our system on the TSCC
	uint32_t lent_ms;	// last P_SYS_Parms "composite" while camped (0: none since the TSCC was heard)
	uint32_t last_act_ms;	// last activity on the followed slot
	uint32_t data_end_ms;	// not 0: the radio's single data item is over, the call ends then (t3_ms_data_done)

	// MBC assembly, per slot
	struct {
		bool open;
		t3_csbk_t hdr;
		uint32_t ms;
	} mbc[2];

	t3_call_t call;
	bool protect_dis;	// P_PROTECT DIS_PTT in force on the payload channel
	uint8_t data_nack;	// set by the radio: 0 when packet data to it can be taken, else the C_NACKU reason
	uint32_t protect_other_ms;	// last EN_PTT_ONE_MS for another radio: PTT off while they come (0: none)
	bool stunned;		// stunned by the TSCC (6.4.9): no user services until revived
	uint32_t stuns, revives;	// stun and revive commands taken
	uint32_t auths;		// authentication challenges answered
	// The radio challenging the TSCC before a stun, revive (6.4.9.2) or kill (6.4.10): what it was asked, its
	// challenge, until when an answer counts
	uint8_t acvit;		// T3_ACVIT_*
#define T3_ACVIT_NONE	0
#define T3_ACVIT_STUN	1
#define T3_ACVIT_REVIVE	2
#define T3_ACVIT_KILL	3
	uint32_t acvit_challenge, acvit_until;
	bool killed;		// killed (6.4.10): no DMR service; the radio core keeps it until a factory reset
	uint32_t kills, tscc_auth_fails;	// kills taken; TSCC answers that failed the check
	// Mass registration (6.4.5.1): a registration due at massreg_at; the window ends at massreg_end
	bool massreg;
	uint32_t massreg_at, massreg_end;
	uint32_t massregs;	// applicable MassReg broadcasts taken
	uint32_t moves;		// C_MOVE taken
	// Power save (6.4.7): the cycle and wake frame the TSCC gave at registration (ps_offset 0: none)
	uint8_t ps_rq_sent, ps_rq, ps_offset;
	bool ip_advised;		// the system took the radio's IP address at the last registration (6.4.11)
	bool asleep;
	uint32_t awake_until, csc_ms, ps_tick_ms;
	uint32_t woke_ms;		// the last wake from a power save sleep: the level is taken again T3_SETTLE_MS after it
	uint32_t sleeps;	// times the radio went to sleep
	// DGNA (6.6.8): talkgroups the system assigned. Heard like the configured ones, never attached (6.4.4.1.13.0).
	// The alias mode's talkgroup (the 16th, or one of the list named) keeps the latest alias, UTF-16
	uint32_t dgna[T3_DGNA_MAX];
	uint8_t ndgna;
	uint32_t dgna_onekey;	// the One-key talkgroup: PTT talks to it; 0 none
	uint32_t dgna_alias_tg;	// the 16th talkgroup (alias mode, not one of the list), 0 none
	uint32_t dgna_named;	// the talkgroup dgna_alias names
	uint16_t dgna_alias[T3_DGNA_ALIAS_MAX];
	uint8_t dgna_alias_len;
	uint32_t dgnas;		// DGNA downloads taken
	// Ambient Listening (6.6.2.6): the radio check accepted, waiting for the private grant until als_until
	uint32_t als_src, als_until;
	uint32_t als_calls;	// ALS calls the radio transmitted on
	uint32_t cleared_ms;	// when the last call was cleared (T3_REGRANT_MS)
	t3_call_t cleared;

	// Access (CONFIG_DMR_TRUNK_TX)
	struct {
		t3_ax_state_t state;
		t3_svc_t svc;
		uint32_t dst;
		bool emergency;
		uint8_t slot;		// the TSCC slot it uses
		uint16_t frames;	// WAIT_SLOT: frames to the chosen one; WAIT_RESP: frames left to wait
		bool to_air;		// the last try is with the radio, not yet reported on air (t3_ms_sent)
		uint8_t tries;
		uint32_t tc_deadline;	// TRand_TC
		uint32_t ack_deadline;	// TP_Timer / TNP_Timer
		t3_res_t result;
		uint8_t reason;		// of the last acknowledgement
		uint8_t progress;	// T3_WACK / T3_QACK of the last progress acknowledgement
		uint32_t retry_ms;	// registration: try again from here (0: no)
		uint8_t status;		// T3_SVC_STATUS: the status sent
		bool group;		// T3_SVC_STATUS, SDATA, PDATA: to a talkgroup
		uint8_t info;		// Response_Info of the final acknowledgement (a poll's answer: the polled status)
		uint8_t div;		// T3_SVC_DIVERT: T3_SO_DIV_SET and the T3_DIV_* services
		uint32_t div_to;	// T3_SVC_DIVERT set: where to (an MS, or a talkgroup when group)
		uint32_t ip;		// T3_SVC_PDATA to T3_ID_IPI: the IPv4 destination, uploaded when the TSCC asks
		bool ip_inform;		// T3_SVC_REGISTER: IP_Inform set, the address goes up when IPI asks (6.4.11)
		uint32_t divert_to;	// T3_RES_DIVERTED: the address the TSCC gave
		bool divert_group;
		bool als;		// T3_SVC_IND_VOICE: Ambient Listening asked for
		bool supl;		// supplementary user data offered (SUPED_SV), uploaded when SUPLI asks
		bool resume;		// T3_SVC_STATUS: back to the call left for it once answered (a payload interrupt)
	} ax;
	// The last status heard for the radio, from a C_AHOY to it or to a talkgroup it listens to (6.6.6)
	struct {
		uint32_t count;
		uint32_t src;
		uint32_t dst;
		bool group;
		uint8_t value;		// 0-126; T3_STATUS_POLL: a poll, answered with own_status
		uint32_t ms;
	} status_rx;
	uint8_t own_status;		// the radio's status, sent in answer to a poll: the last one it sent
	// UDT short data (6.6.4): what T3_SVC_SDATA sends, its blocks once packed, and the last message heard
	struct {
		uint8_t format;
		uint8_t len;
		uint8_t spare;		// fill bits (1s) inside the last data octet: ISO 7 bit, a BCD DigitNULL
		uint8_t data[T3_UDT_MAX];
	} sd_tx;
	uint8_t up[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK];
	struct {
		bool open;
		uint8_t slot;
		uint8_t got;
		uint32_t ms;
		t3_udt_t h;
		uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK];
	} udt_rx;			// a UDT being received
	struct {
		uint32_t count;
		uint32_t src;
		uint32_t dst;
		bool group;
		uint8_t format;
		uint8_t len;		// data octets, pad left out (Pad Nibble)
		uint8_t data[T3_UDT_MAX];
		uint32_t ms;
	} sd_rx;
#if CONFIG_DMR_TRUNK_GW
	// The last caller's number a gateway sent (CLI, table 6.33): n BCD digits as on air, from gateway gw
	struct {
		uint32_t count;
		uint32_t gw;
		uint32_t dst;
		bool group;
		uint8_t n;
		uint8_t bcd[T3_DIAL_MAX / 2];
		uint32_t ms;
	} cli;
#endif
#if CONFIG_DMR_TRUNK_SUPL
	// The last supplementary user data heard (6.4.13.2). The download doesn't name the sender: src is 0 until the
	// call's grant or user data that follows does
	struct {
		uint32_t count;
		uint32_t src;
		uint32_t dst;
		bool group;
		uint8_t format;
		uint8_t len;
		uint8_t data[T3_UDT_MAX];
		uint32_t ms;
	} supl_rx;
#endif
	// A burst to send on a slot without random access: an answer (once) or P_MAINT (count times)
	struct {
		uint8_t count;
		uint8_t slot;
		bool payload;
		bool then_return;	// back to the TSCC once sent
		bool udt;		// burst is a UDT header with up[] as its nblocks blocks (the short data upload)
		uint8_t nblocks;
		uint8_t burst[T3_CSBK_BYTES];
	} tx;
	uint32_t talk_id;		// set by t3_ms_request: the last target, for a user's PTT in the hang time
	uint16_t reg_area;		// the registration record: MODEL, NET and the SYS_AREA bits of SITE (reg T3_REG_OK)
	// The talkgroup subscription list the last registration sent (T3_OPT_REG_TG with two or more listened to), and
	// which the TSCC accepted: bit 6 the first (table 6.10)
	uint8_t reg_tgs;
	bool reg_single;	// the registration carried one talkgroup (6.4.4.1.13.1): its answer says whether it was taken
	uint8_t reg_tg_ok;
	t3_adj_t adj[T3_ADJ_MAX];
	uint8_t nadj;
	uint8_t adj_next;		// the entry a new site replaces when full
	int16_t level_dbm;		// the tuned channel's averaged signal level (t3_ms_level), 0 = none yet
	uint32_t weak_since;		// camped and below leave_dbm since (0: not)
	bool hunt_strict;		// hunting after a weak TSCC: confirm only one above leave_dbm, for one pass
	struct {
		bool on;		// sampling adj[idx] until `until`
		uint8_t idx;
		uint32_t start, until;
		bool heard;		// an Aloha or broadcast with its system code
		uint8_t cc;
		int16_t home_dbm;	// the TSCC's level before
	} vote;
	uint16_t denied[T3_DENIED_MAX];	// areas, as reg_area, that denied registration, + 1 (0: empty)
	bool deregistered;		// de-registered: no registration until t3_ms_start
	bool reg_stale;			// the talkgroups changed during a registration: register again after it
	uint32_t left_dst;		// a talkgroup call the user left: its late-entry grants don't pull us back (6.6.2.4.1)
	uint32_t rnd;

	t3_stats_t st;
} t3_ms_t;

void t3_ms_start(t3_ms_t *ms, const t3_cfg_t *cfg, uint32_t now);
void t3_ms_stop(t3_ms_t *ms);
void t3_ms_rx(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t dt, uint8_t cc, const uint8_t d[T3_CSBK_BYTES]);
void t3_ms_slc(t3_ms_t *ms, uint32_t now, uint32_t slc);
void t3_ms_activity(t3_ms_t *ms, uint32_t now, uint8_t slot);
void t3_ms_call_burst(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t cc);
// The radio's report on the last burst list the engine handed it (T3_ACT_SEND): sent (ok), or dropped. Optional; a
// random access burst's answer window (NRand_Wait) then runs from the burst on air, not from the hand-over
void t3_ms_sent(t3_ms_t *ms, uint32_t now, bool ok);
// The tuned channel's signal level, in dBm, as often as the radio measures it (0: none). Not while following a call
void t3_ms_level(t3_ms_t *ms, uint32_t now, int16_t dbm);
bool t3_ms_poll(t3_ms_t *ms, uint32_t now, t3_act_t *act);

void t3_ms_slot(t3_ms_t *ms, uint32_t now, uint8_t slot, bool at_known, bool at_busy);
// The slot named by the CACH whose AT bit tells whether the TSCC's inbound for logical slot `slot` is free. Aligned
// timing (the Aloha's Offset 0) sends logical channel 1 on inbound slot 2 (361-1 table 5.1), and a CACH describes the
// inbound slot its TC names (361-1 4.5): the other slot's CACH, the one right after the TSCC's own burst (361-4
// 6.2.1.1.3). Offset timing: the same slot
uint8_t t3_ms_at_tc(const t3_ms_t *ms, uint8_t slot);

// Access (CONFIG_DMR_TRUNK_TX). t3_ms_request asks for a voice call (PTT) to dst, a talkgroup or an individual. False
// (ax.result says why) when it can't start. Watch ax.result for the outcome
bool t3_ms_request(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group, bool emergency);
// Stunned (6.4.9) or not: the radio core keeps it across a restart of the engine and gives it back after t3_ms_start
void t3_ms_set_stunned(t3_ms_t *ms, bool stunned);
void t3_ms_set_killed(t3_ms_t *ms, bool killed);
// Send a status (0-126) to dst, a talkgroup or an MS, or poll an MS (T3_STATUS_POLL; ax.info has its answer). False
// (ax.result says why) when it can't start
bool t3_ms_status(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group, uint8_t status);
// Send UDT short data (1-46 octets in format, T3_UDTF_*; ISO 7 bit: 1-52 characters, one an octet) to dst, a talkgroup
// or an MS. False (ax.result says why) when it can't start
bool t3_ms_sdata(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group, uint8_t format, const uint8_t *data, uint8_t len);
// Calls through a gateway (6.6.2.2.3, 6.6.4.2): t3_ms_request, t3_ms_sdata or t3_ms_pdata to a dialing gateway
// (t3_dial_gw) upload cfg.dial when it asks. t3_dial_pack packs "0-9", '*', '#' (1-T3_DIAL_MAX) for it: the digits'
// number, 0 for a bad string. Without CONFIG_DMR_TRUNK_GW a request to a dialing gateway is refused
uint8_t t3_dial_pack(const char *digits, uint8_t bcd[T3_DIAL_MAX / 2]);
// PSTNI, PABXI, LINEI, DISPATI and their offset-timing addresses (A.4)
bool t3_dial_gw(uint32_t id);
// n BCD digits (the first in the high nibble) as text, "0-9*#" (another code: '?'); out holds n + 1
void t3_bcd_text(const uint8_t *bcd, uint8_t n, char *out);
// Ask for a packet data channel to an MS or talkgroup (6.6.3.2.1). Once granted the engine follows it (call.data) and
// t3_ms_data_ready says when the radio may send its packet there
bool t3_ms_pdata(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group);
// Packet data to an IPv4 destination through the system's IP gateway (6.6.3.2.3): a multi-part request to IPI, the
// address uploaded when the TSCC asks; on the payload channel the data goes to IPI
bool t3_ms_pdata_ip(t3_ms_t *ms, uint32_t now, uint32_t ipv4);
// Ask MS dst for its short data (6.6.5.2): UDT_Format format, in 1-4 blocks (a prearranged size). The answer lands in
// sd_rx; ax.result done once it came. False (ax.result says why) when it can't start
bool t3_ms_sdata_poll(t3_ms_t *ms, uint32_t now, uint32_t dst, uint8_t format, uint8_t blocks);
// Ask to listen to an MS's surroundings (ALS, 6.6.2.6.2): an individual call it transmits on by itself
bool t3_ms_request_als(t3_ms_t *ms, uint32_t now, uint32_t dst, bool emergency);
// An ALS call to the radio: transmit now (als_tx); its T_ALS is over and the call ends once the radio is quiet
// (als_over: the caller sends t3_ms_end_call)
bool t3_ms_als_tx(const t3_ms_t *ms, uint32_t now);
bool t3_ms_als_over(const t3_ms_t *ms, uint32_t now);
// Stop the MS talking in the voice call followed (6.4.14.3.3, payload interrupt): leave for the TSCC, send status 125
// to the talker (or the talkgroup), and come back to the call whatever the answer (ax.result: done = accepted).
// end_als: end an ALS call the radio asked for instead (6.4.14.3.4.3): status 123, and the radio stays on the TSCC.
// False when it can't start
bool t3_ms_interrupt(t3_ms_t *ms, uint32_t now, bool end_als);
// Call diversion (6.6.7). Set: the services (T3_DIV_*) go to an MS or talkgroup from now on. Clear: the services' own
// diversion ends (incoming false), or every diversion to this radio does (incoming true). False when it can't start
bool t3_ms_divert(t3_ms_t *ms, uint32_t now, uint8_t services, uint32_t to, bool group);
bool t3_ms_divert_clear(t3_ms_t *ms, uint32_t now, uint8_t services, bool incoming);
// On a packet data payload channel, settled: the data service may send (the caller first, 6.6.3.3)
bool t3_ms_data_ready(const t3_ms_t *ms, uint32_t now);
// The radio's single data item on its packet data channel is over (6.6.3.3.2.5): failed (retries spent), the call ends
// now; sent or acknowledged, it ends T3_DATA_LINGER_MS after the last data heard there (t3_ms_data_heard). Without
// this, a P_CLEAR missed while the radio sent leaves it on a slot whose idles keep TD_Inactive from running out
void t3_ms_data_done(t3_ms_t *ms, uint32_t now, bool ok);
void t3_ms_data_heard(t3_ms_t *ms, uint32_t now);
// PTT released before the grant: withdraw the request, or cancel it at the TSCC once sent
void t3_ms_cancel(t3_ms_t *ms, uint32_t now);
// The user ends the call being followed: P_MAINT when the radio started it or it is individual, then back
void t3_ms_end_call(t3_ms_t *ms, uint32_t now);
// De-register when registered (the debug link's DEREG). True when a de-registration started
bool t3_ms_deregister(t3_ms_t *ms, uint32_t now);
// The talkgroups in cfg changed (another channel of the system): with T3_OPT_REG_TG, register with them again once
// they have stayed T3_REG_TG_DELAY_MS. Otherwise nothing: the new ones are followed and talked to at once
void t3_ms_groups_changed(t3_ms_t *ms, uint32_t now);
// May the user talk on the payload channel now: a voice call, not a broadcast or all-call another radio made, PTT not
// disabled by P_PROTECT
bool t3_ms_can_talk(const t3_ms_t *ms);
// A talkgroup DGNA assigned (6.6.8)
bool t3_ms_is_dgna(const t3_ms_t *ms, uint32_t tg);
// Power save: the receiver may sleep now (t3_ms_poll decides; the radio wakes it the moment this goes false)
bool t3_ms_asleep(const t3_ms_t *ms);
const char *t3_res_name(t3_res_t r);

// Is a talkgroup grant one the radio follows?
bool t3_ms_wants_group(const t3_ms_t *ms, uint32_t tg, bool emergency);

const char *t3_state_name(t3_state_t s);
const char *t3_reg_name(t3_reg_t r);

#endif
