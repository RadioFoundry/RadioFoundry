// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug's Tier III records (docs/spec/dmr-trunking.md 12): their layout, for the files that read and write them
// (radio_trunk_cp.c, radio_trunk_share.c). Not for other code: radio_trunk.h is the interface
#ifndef RFOUNDRY_RADIO_TRUNK_REC_H
#define RFOUNDRY_RADIO_TRUNK_REC_H

#include <stdint.h>

#define SYS_LEN		64u
#define CHAN_LEN	13u
#define KEY_LEN		16u
#define TAG_LEN		8u
#define CHAN_CTRL	0x01u
#define PLAN_TX_LOW	0x01u
#define PLAN_NO_LEARN	0x02u
#define SYS_AUTHENTICATES	0x0800u		// the system challenges: not usable without a key
#define SYS_IP_GW		0x1000u		// off-net IP through the system's IP gateway (the radio core's, not the engine's)
#define SYS_SHARING	27u			// offsets in a system record
#define SYS_FB_CHAN	29u
#define SYS_FB_CHECK	62u

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

// The lowest channel key above `after` that belongs to system n, its record in rec (CHAN_LEN); 0 when none. A walk is
// in the area's order, and the radio takes channels in key order (a writer's order in the area isn't the user's)
uint16_t radio_t3_next_chan(uint8_t n, uint16_t after, uint8_t *rec);

#endif
