// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III system identity and access rules (ETSI TS 102 361-4 6.2, 6.3.2.2, 6.4): the C_SYScode partition by
// network model, the TSCC authorization checks, SYS_AREA, the Mask rule and the access delay tables.
#ifndef RFOUNDRY_T3_SYS_H
#define RFOUNDRY_T3_SYS_H

#include <stdbool.h>
#include <stdint.h>
#include "dmr/trunk/t3_pdu.h"	// T3_ID_GATEWAY_FIRST

#define T3_MODEL_TINY	0
#define T3_MODEL_SMALL	1
#define T3_MODEL_LARGE	2
#define T3_MODEL_HUGE	3

// PAR (6.3.2.2.1.4): which control categories may use the TSCC
#define T3_PAR_A	1
#define T3_PAR_B	2
#define T3_PAR_AB	3

typedef struct {
	uint8_t model;
	uint16_t net;
	uint16_t site;
	uint8_t par;
} t3_syscode_t;

// NET and SITE widths of a model (table 6.5, figure 6.15)
uint8_t t3_net_bits(uint8_t model);
uint8_t t3_site_bits(uint8_t model);

void t3_syscode_split(uint16_t code, t3_syscode_t *out);
uint16_t t3_syscode_join(const t3_syscode_t *in);
// The 14 most significant bits (MODEL, NET, SITE) as the short LC and C_BCAST parms carry them
static inline uint16_t t3_syscode_14(uint16_t code)
{
	return (uint16_t)(code >> 2);
}

// SYS_AREA: the dmrla most significant bits of SITE (6.3.2.2.3, 6.4.4). dmrla is clamped to the SITE width. The
// engine's registration area puts MODEL and NET in front of it (t3_ms.c reg_area)
uint16_t t3_sys_area(uint16_t code, uint8_t dmrla);

// The six bits Response_Check carries (table 7.41 note: NET + SITE bits 8-3 of figure 6.19)
uint8_t t3_response_check(uint16_t code);

// What the MS holds for one system (its "fixed non-volatile storage" of 6.3.2.2.2)
typedef struct {
	uint8_t model;
	uint16_t net;
	const uint16_t *sites;	// allowed SITEs; nsites 0 = any
	uint8_t nsites;
	uint8_t category;	// T3_PAR_A or T3_PAR_B
} t3_auth_t;

typedef enum {
	T3_AUTH_OK,
	T3_AUTH_MODEL,	// wrong network model
	T3_AUTH_NET,	// another network
	T3_AUTH_SITE,	// a site not in the list
	T3_AUTH_PAR,	// the TSCC doesn't serve our control category
} t3_auth_result_t;

// TSCC authorization (6.3.2.2.2 a-d)
t3_auth_result_t t3_authorize(const t3_auth_t *a, uint16_t code);

// MS authentication (6.4.8): the answer to a TSCC's 24-bit challenge. RC4 keyed with the challenge (3 octets, most
// significant first) followed by the 128-bit key K; the keystream's octets 257-259 (most significant first), XOR
// the PSN (0: none)
#define T3_AUTH_KEY_LEN		16
#define T3_AUTH_CHALLENGE_MAX	0xFFFCDFu	// table 6.17
uint32_t t3_auth_response(const uint8_t key[T3_AUTH_KEY_LEN], uint32_t challenge, uint32_t psn);

// The Mask rule (6.2.1.1.1): the PDU applies to the MS when the mask least significant bits of the two addresses match
bool t3_mask_match(uint8_t mask, uint32_t pdu_addr, uint32_t own);

// NRand_Wait to TDMA frames (table 6.2), the backoff number to TDMA frames (table 6.3; 0 is reserved and reads 1)
uint8_t t3_nwait_frames(uint8_t nrand_wait);
uint8_t t3_backoff_frames(uint8_t backoff);

// Is this a gateway or special address (A.4) rather than an MS or talkgroup?
static inline bool t3_is_gateway(uint32_t id)
{
	return id >= T3_ID_GATEWAY_FIRST;
}

#endif
