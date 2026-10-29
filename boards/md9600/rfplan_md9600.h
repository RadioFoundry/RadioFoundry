// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 board revisions and frequency plan (docs/spec/hardware-md9600.md 5, 16). Pure: no pins and no RTOS, so the
// host tests (tests/host/md9600RfplanTest.c) run it unchanged.
//
// One image serves every board revision. The revision decides the synthesizer (SKY72310 on V1-V3, the 24-bit
// "PLL1590" part on V4, LMX2571 on V4A / V5), whether the IF receivers are AK2365A chips (V1-V4) or fixed parts behind
// filter and route lines (V5), and a few switching details. Nothing on the board reports it: a stored record is the
// authority, a probe of the receiver may suggest one, and the build carries a default.
#ifndef RFPLAN_MD9600_H
#define RFPLAN_MD9600_H

#include <stdint.h>
#include <stdbool.h>
#include "config_md9600.h"

typedef enum {
	MD9600_REV_UNKNOWN	= 0,
	MD9600_REV_V1V3		= 1,	// SKY72310 x2, AK2365A x2
	MD9600_REV_V4		= 2,	// "PLL1590" x2, AK2365A x2
	MD9600_REV_V5		= 3,	// LMX2571 x2, no programmable IF chip (V4A and V5)
	MD9600_REV_COUNT
} md9600_rev_t;

typedef enum { MD9600_PLL_SKY72310, MD9600_PLL_PLL1590, MD9600_PLL_LMX2571 } md9600_pll_t;

typedef struct {
	const char *name;	// as on the board label: "V1-V3", "V4", "V4A/V5"
	uint8_t pll;		// md9600_pll_t
	uint8_t pll_bits;	// word length on the synthesizer bus
	bool if_chip;		// AK2365A on PE11-PE15; else PE11-PE14 are filter and route lines
	bool vhf_vco_x2;	// the VHF synthesizer runs at twice the LO (V1-V3)
	bool tx_other_c5_off;	// while one band transmits, the other band's C5 supply goes off
	bool vhf_tune_at_power_on;	// the VHF synthesizer must be programmed right after power-on or it never locks
} md9600_rev_desc_t;

// NULL for MD9600_REV_UNKNOWN or out of range
const md9600_rev_desc_t *md9600_rev_desc(md9600_rev_t rev);
// The revision's synthesizer driver is in this image (CONFIG_MD9600_V1V3, _V4, _V5)
bool md9600_rev_built(md9600_rev_t rev);

// The stored revision record, kept in the board's own data flash sector (flash_md9600.c). Erased flash (all 0xFF) or
// a damaged record decodes as MD9600_REV_UNKNOWN
#define MD9600_REV_RECORD_SIZE	8u
void md9600_rev_record_encode(md9600_rev_t rev, uint8_t out[MD9600_REV_RECORD_SIZE]);
md9600_rev_t md9600_rev_record_decode(const uint8_t in[MD9600_REV_RECORD_SIZE]);

typedef enum {
	MD9600_REV_SRC_NONE,		// nothing usable in this image
	MD9600_REV_SRC_STORED,		// the stored record
	MD9600_REV_SRC_PROBE,		// the receiver probe
	MD9600_REV_SRC_DEFAULT,		// the build's default
} md9600_rev_source_t;

typedef struct {
	md9600_rev_t rev;
	uint8_t source;		// md9600_rev_source_t
	bool usable;		// its driver is built: the receiver may be programmed
	bool confirmed;		// set by a person (the stored record): only then may the radio transmit
} md9600_rev_state_t;

// Which revision to drive. stored and probed are MD9600_REV_UNKNOWN when absent; build_default is the image's default
md9600_rev_state_t md9600_rev_resolve(md9600_rev_t stored, md9600_rev_t probed, md9600_rev_t build_default);

// The diagnostics page's revision lines, at most MD9600_REV_TEXT_MAX characters (the 128x64 panel's small font):
// line 0 the revision and its source ("rev V4A/V5 default", "rev V4 unbuilt": not in this image, receiver off), line 1 the prompt to store
// one until there is ("Set rev: Adv>Hardware"), line 2 the probe ("probe off", "probe V4", "probe none"; probe is
// -1 when it didn't run). False past the last line. buf takes len bytes, NUL terminated
#define MD9600_REV_TEXT_MAX	25u
#define MD9600_REV_TEXT_LINES	3u
bool md9600_rev_text(uint8_t line, md9600_rev_state_t s, int16_t probe, char *buf, uint8_t len);

// Probe verdict: baseline is the noise reading with no synthesizer programmed, readings[i] the reading with
// candidate cand[i] programmed. A candidate wins when its change from the baseline is at least threshold and at least
// twice every other candidate's. Otherwise MD9600_REV_UNKNOWN
md9600_rev_t md9600_rev_probe_pick(uint16_t baseline, const uint16_t *readings, const md9600_rev_t *cand, uint8_t n,
				   uint16_t threshold);

// --- Frequency plan ---

#define MD9600_F_REF_HZ		16800000u	// shared TCXO
#define MD9600_IF_HZ		49950000u	// first IF
#define MD9600_BAND_SPLIT_HZ	300000000u	// below: VHF

typedef enum { MD9600_VHF = 0, MD9600_UHF = 1 } md9600_band_t;

// The band a frequency belongs to; false outside both bands
bool md9600_band_of(uint32_t hz, uint8_t *band);

#define MD9600_PLL_MAX_WORDS	14u

typedef struct {
	uint8_t band;		// md9600_band_t: the synthesizer and VCO pair to use
	uint32_t lo_hz;		// the synthesizer's output: the RX LO, or the carrier in TX
	uint32_t vco_hz;	// what the synthesizer locks to (twice lo_hz on V1-V3 VHF)
	uint8_t bits;		// word length
	uint8_t count;
	uint32_t words[MD9600_PLL_MAX_WORDS];	// in write order
} md9600_pll_plan_t;

// The words for receiving (tx false: LO = f + IF on VHF, f - IF on UHF) or transmitting (the carrier) on hz. False
// when hz is outside the bands, the revision is unknown or its ratio is out of range
bool md9600_pll_plan(md9600_rev_t rev, uint32_t hz, bool tx, md9600_pll_plan_t *p);

// The AK2365A set-up for a channel width (V1-V4), in write order; returns the count
#define MD9600_IF_WORDS		6u
uint8_t md9600_if_words(bool wide, uint16_t out[MD9600_IF_WORDS]);

// V5: the levels of PE11-PE14 as bits 0-3 (VHF bandwidth, UHF bandwidth, VHF route, UHF route): the band's IF to the
// HR_C6000 and both filters at the channel width (high = 25 kHz)
uint8_t md9600_if_lines_v5(uint8_t band, bool wide);

#endif
