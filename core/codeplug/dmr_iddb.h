// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR ID database a CPS uploads (callsign and name per DMR ID, docs/spec/legacy-cps-interop.md 7.5), read only,
// looked up by binary search straight from the board's data flash. No RAM beyond a few header fields.
#ifndef DMR_IDDB_H
#define DMR_IDDB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

#if CONFIG_DMR
#define DMR_IDDB_TEXT_MAX	48	// longest record text kept ("callsign name ..."), characters

// Read (or re-read, after an upload) the database header. False if there is no database
bool dmr_iddb_init(void);
// Records in the database, 0 if none
uint32_t dmr_iddb_count(void);
// The text for an ID ("callsign name [city ...]", trailing spaces trimmed), cut to len - 1 characters. False if the
// ID is not in the database. Reads about log2(count) records
bool dmr_iddb_lookup(uint32_t id, char *out, size_t len);
#endif

#endif
