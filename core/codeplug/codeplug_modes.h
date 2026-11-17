// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The registry of channel kinds and system types (docs/spec/channel-modes.md 4): the one list C reads. Each row is
// X(id, token, short name, built), where built is the condition of the code that runs it (a CONFIG_ expression, or one
// that also asks the board; 0 while that code is not in the tree). Ids are only appended and never reused; 0 and 0xFF
// are never ids. The editor's list (site/static/js/codeplug/modes.js) and the spec's tables hold the same rows:
// tools/check_modes.py checks them. The generic layer (codeplug_tag.c, codeplug_records.c) takes any id: a row only
// gives the name shown and whether the radio runs it
#ifndef RFOUNDRY_CODEPLUG_MODES_H
#define RFOUNDRY_CODEPLUG_MODES_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

// What runs P25 conventional: the build (CONFIG_P25) and a board whose air interface can receive it
// (core/radio/radio_p25.c; it asks the board's provider, no state)
#if CONFIG_P25
bool radio_p25_fits(void);
#define CODEPLUG_RUNS_P25	radio_p25_fits()
#else
#define CODEPLUG_RUNS_P25	0
#endif

// What runs NXDN conventional: the build (CONFIG_NXDN) and a board whose air interface carries it at either rate
// (core/radio/radio_nxdn.c)
#if CONFIG_NXDN
bool radio_nxdn_fits(void);
#define CODEPLUG_RUNS_NXDN	radio_nxdn_fits()
#else
#define CODEPLUG_RUNS_NXDN	0
#endif

// Channel kinds: the first byte of a channel tag
#define CODEPLUG_KINDS(X) \
	X(1, DMR_TRUNK, "T3", CONFIG_DMR_TRUNK)	/* DMR Tier III trunked (radio/radio_trunk.h) */ \
	X(2, P25, "P25", CODEPLUG_RUNS_P25)	/* P25 conventional: the radio core runs it (core/radio/radio_p25.c) */ \
	X(3, P25_TRUNK, "P25T", CONFIG_P25_TRUNK && CODEPLUG_RUNS_P25)	/* P25 trunked, receive only (radio_p25.c) */ \
	X(4, NXDN, "NXDN", CODEPLUG_RUNS_NXDN)	/* NXDN conventional: the radio core runs it (core/radio/radio_nxdn.c) */

// System types: the first byte of a system record
#define CODEPLUG_SYSTEM_TYPES(X) \
	X(1, DMR_TRUNK, "T3", CONFIG_DMR_TRUNK)	/* a DMR Tier III system */ \
	X(3, P25_TRUNK, "P25T", CONFIG_P25_TRUNK)	/* a P25 trunked system (2 is reserved for a P25 conventional one) */

// A test (or a build that adds a mode out of tree) may append rows of its own, defined before this header
#ifndef CODEPLUG_KINDS_EXTRA
#define CODEPLUG_KINDS_EXTRA(X)
#endif
#ifndef CODEPLUG_SYSTEM_TYPES_EXTRA
#define CODEPLUG_SYSTEM_TYPES_EXTRA(X)
#endif

// Whether this build reads the mode-scoped record families (codeplug_records.h): the OR of the build conditions of the
// modes that use them: P25 and NXDN read their radio-wide records, Tier III its systems. A test defines it 1
#ifndef CODEPLUG_MODE_RECORDS
#define CODEPLUG_MODE_RECORDS	(CONFIG_P25 || CONFIG_NXDN || CONFIG_DMR_TRUNK)
#endif

#define CODEPLUG_KIND_ENUM(id, token, name, built)	CODEPLUG_KIND_##token = id,
enum { CODEPLUG_KIND_NONE = 0, CODEPLUG_KINDS(CODEPLUG_KIND_ENUM) };
#undef CODEPLUG_KIND_ENUM
#define CODEPLUG_SYSTYPE_ENUM(id, token, name, built)	CODEPLUG_SYSTYPE_##token = id,
enum { CODEPLUG_SYSTYPE_NONE = 0, CODEPLUG_SYSTEM_TYPES(CODEPLUG_SYSTYPE_ENUM) };
#undef CODEPLUG_SYSTYPE_ENUM

// Whether this build runs a kind or a system type: false for 0, 0xFF and every id it doesn't know. The short name shown
// for one: "?" for an id this build doesn't know
#define CODEPLUG_REG_BUILT(id, token, name, built)	case id: return (built) != 0;
#define CODEPLUG_REG_NAME(id, token, name, built)	case id: return name;
static inline bool codeplug_kind_built(uint8_t kind)
{
	switch (kind) {
	CODEPLUG_KINDS(CODEPLUG_REG_BUILT)
	CODEPLUG_KINDS_EXTRA(CODEPLUG_REG_BUILT)
	default: return false;
	}
}

static inline bool codeplug_systype_built(uint8_t type)
{
	switch (type) {
	CODEPLUG_SYSTEM_TYPES(CODEPLUG_REG_BUILT)
	CODEPLUG_SYSTEM_TYPES_EXTRA(CODEPLUG_REG_BUILT)
	default: return false;
	}
}

static inline const char *codeplug_kind_name(uint8_t kind)
{
	switch (kind) {
	CODEPLUG_KINDS(CODEPLUG_REG_NAME)
	CODEPLUG_KINDS_EXTRA(CODEPLUG_REG_NAME)
	default: return "?";
	}
}

static inline const char *codeplug_systype_name(uint8_t type)
{
	switch (type) {
	CODEPLUG_SYSTEM_TYPES(CODEPLUG_REG_NAME)
	CODEPLUG_SYSTEM_TYPES_EXTRA(CODEPLUG_REG_NAME)
	default: return "?";
	}
}
#undef CODEPLUG_REG_BUILT
#undef CODEPLUG_REG_NAME

#endif
