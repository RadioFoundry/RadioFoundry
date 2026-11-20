// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RDS / RBDS group decoder (CONFIG_BCAST_RDS; docs/spec/airband-and-broadcast.md 4.6). From the public standard
// (IEC 62106 / EN 50067; RBDS is NRSC-4 in the Americas, the same groups). The broadcast receiver chip does the
// demodulation, block sync and error correction; this takes its groups of four 16-bit blocks and keeps what a
// listener reads: the program identification, the program service name (groups 0A / 0B) and RadioText (2A / 2B).
//
// The chip reports errors for blocks A and B only, so a character is taken only when the same segment arrives twice
// with the same text: a wrong block C or D rarely repeats. Pure: no RTOS, no hardware; one caller.
#ifndef RDS_H
#define RDS_H

#include <stdint.h>
#include <stdbool.h>

#define RDS_PS_LEN	8	// program service name
#define RDS_RT_LEN	64	// RadioText (2A; 2B carries 32)

// Block error levels as the chip reports them
enum { RDS_ERR_NONE = 0, RDS_ERR_SMALL = 1, RDS_ERR_LARGE = 2, RDS_ERR_BAD = 3 };

// What the station sends, as the screen and the debug link read it. Text is printable ASCII, NUL terminated, trailing
// spaces dropped; empty until it has been received whole
typedef struct {
	uint16_t pi;			// program identification, 0 = none yet
	uint8_t pty;			// program type 0..31 (its name differs between RDS and RBDS)
	bool tp;			// traffic program
	bool ta;			// traffic announcement now
	char ps[RDS_PS_LEN + 1];
	char rt[RDS_RT_LEN + 1];
} rds_info_t;

typedef struct {
	rds_info_t info;
	uint32_t groups;		// groups taken since the last reset
	// Program identification: a new one must arrive twice in a row before it replaces the current one
	uint16_t pi_cand;
	// PS: per segment (2 characters), the last text heard and the text confirmed
	char ps_heard[RDS_PS_LEN];
	char ps_conf[RDS_PS_LEN];
	uint16_t ps_heard_mask, ps_conf_mask;
	// RadioText: per segment (4 characters in 2A, 2 in 2B), the same
	char rt_heard[RDS_RT_LEN];
	char rt_conf[RDS_RT_LEN];
	uint16_t rt_heard_mask, rt_conf_mask;
	int8_t rt_ab;			// the text A/B flag of the text being built, -1 = none yet
	bool rt_2b;			// the text comes in 2B groups
} rds_t;

// Forget everything (a new frequency, the receiver off)
void rds_reset(rds_t *r);
// One group from the receiver: blocks A, B, C, D and the error levels of A and B (RDS_ERR_*). True when info changed
bool rds_group(rds_t *r, const uint16_t blk[4], uint8_t err_a, uint8_t err_b);

#endif
