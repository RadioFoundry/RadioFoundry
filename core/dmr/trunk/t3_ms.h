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
//   t3_ms_poll()      every 10-30 ms: T3_ACT_TUNE means retune to act->chan and listen to act->slot (0: both)
//   t3_ms_stop()      another zone or channel selected
// This is the receive side: an MS in listen-only mode never transmits. Access (registration, calls) comes with
// CONFIG_DMR_TRUNK_TX.
#ifndef RFOUNDRY_T3_MS_H
#define RFOUNDRY_T3_MS_H

#include <stdbool.h>
#include <stdint.h>
#include "dmr/trunk/t3_pdu.h"
#include "dmr/trunk/t3_sys.h"
#include "dmr/trunk/t3_plan.h"

// Data types of the slot type (TS 102 361-1 9.3.6) the engine looks at
#define T3_DT_CSBK		3
#define T3_DT_MBC_HDR		4
#define T3_DT_MBC_CONT		5

#define T3_CTRL_MAX		4	// programmed control channel candidates
#define T3_HUNT_MAX		8	// extra hunt LCNs (Annex D short hunt), resolved through the plan
#define T3_SITES_MAX		8
#define T3_TG_MAX		32

// Talkgroup flags
#define T3_TG_LISTEN		0x01	// follow calls to it
#define T3_TG_TALK		0x02	// the default talk group (CONFIG_DMR_TRUNK_TX)

// System options
#define T3_OPT_LISTEN_ONLY	0x0001	// never transmit (and so never register)
#define T3_OPT_ALLCALL		0x0002	// follow all-calls (ALLMSID, ALLMSIDL, ALLMSIDZ)
#define T3_OPT_EMERGENCY_ANY	0x0004	// follow emergency grants on any talkgroup
#define T3_OPT_MONITOR		0x0008	// follow every talkgroup grant (a scanner's view; listen-only)
#define T3_OPT_LEARN_SYSCODE	0x0010	// take the first system heard as ours (model/net unset)
#define T3_OPT_PRIVATE		0x0020	// follow private calls to the radio's ID (on by default)

// Defaults (TS 102 361-4 Annex A ranges in the comments)
#define T3_NOSIG_MS		5000	// T_Nosig, 1-15 s: no valid TSCC PDU this long, hunt again
#define T3_DWELL_MS		700	// hunting: time on a candidate (12 TSCC slots and two short LCs)
#define T3_SETTLE_MS		60	// after a retune: what is heard before this belongs to the old channel
#define T3_INACTIVE_MS		2500	// TV_Inactive, 0-20 s: nothing on the followed slot this long, return
#define T3_CALL_MAX_MS		300000	// a followed call ends by this at the latest
#define T3_REGRANT_MS		2000	// a call just left isn't followed again this soon

typedef struct {
	uint32_t id;
	uint8_t flags;
} t3_tg_t;

typedef struct {
	uint32_t own_id;
	t3_auth_t auth;			// model, net, category; auth.sites points at sites[] below
	uint16_t sites[T3_SITES_MAX];
	uint8_t dmrla;
	t3_plan_t plan;
	t3_chan_t ctrl[T3_CTRL_MAX];	// control channel candidates, in hunt order
	uint8_t nctrl;
	uint16_t hunt[T3_HUNT_MAX];
	uint8_t nhunt;
	t3_tg_t tg[T3_TG_MAX];
	uint8_t ntg;
	uint16_t opts;			// T3_OPT_*
	uint16_t nosig_ms;		// 0 = default
	uint16_t inactive_ms;
	uint16_t dwell_ms;
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

typedef enum { T3_ACT_NONE, T3_ACT_TUNE } t3_act_type_t;

typedef struct {
	t3_act_type_t type;
	t3_chan_t chan;
	uint8_t slot;		// 0: both (a control channel), 1 / 2: the payload slot
	bool payload;
} t3_act_t;

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
} t3_call_t;

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
	uint32_t learned;	// Chan_Freq / CG_AP entries learned or changed
	uint16_t last_unmapped;
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
	uint8_t tuned_slot;
	uint32_t tuned_ms;
	t3_act_t pending;

	// hunting
	uint32_t slc_foreign;	// the last foreign C_SYS_Parms code + 1 (0: none): two alike leave a candidate
	bool retry_tscc;	// the lost TSCC is tried before the list
	uint8_t hunt_idx;	// next candidate: ctrl[], then hunt[] through the plan
	uint32_t hunt_deadline;

	// timers
	uint32_t last_valid_ms;	// last PDU from our system on the TSCC
	uint32_t last_act_ms;	// last activity on the followed slot

	// MBC assembly, per slot
	struct {
		bool open;
		t3_csbk_t hdr;
		uint32_t ms;
	} mbc[2];

	t3_call_t call;
	bool protect_dis;	// P_PROTECT DIS_PTT in force on the payload channel
	uint32_t cleared_ms;	// when the last call was cleared (T3_REGRANT_MS)
	t3_call_t cleared;

	t3_stats_t st;
} t3_ms_t;

void t3_ms_start(t3_ms_t *ms, const t3_cfg_t *cfg, uint32_t now);
void t3_ms_stop(t3_ms_t *ms);
void t3_ms_rx(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t dt, uint8_t cc, const uint8_t d[T3_CSBK_BYTES]);
void t3_ms_slc(t3_ms_t *ms, uint32_t now, uint32_t slc);
void t3_ms_activity(t3_ms_t *ms, uint32_t now, uint8_t slot);
bool t3_ms_poll(t3_ms_t *ms, uint32_t now, t3_act_t *act);

// Is a talkgroup grant one the radio follows?
bool t3_ms_wants_group(const t3_ms_t *ms, uint32_t tg, bool emergency);

const char *t3_state_name(t3_state_t s);
const char *t3_reg_name(t3_reg_t r);

#endif
