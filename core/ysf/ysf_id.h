// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio ID (RID) of YSF (docs/spec/ysf.md 6): five characters 0-9, A-Z that every transmission
// carries, by which repeaters, rooms and other radios tell one radio from another. Two radios with the same RID look
// like one, so it MUST differ from radio to radio. It is made here from the MCU's factory-unique ID and nothing
// else: never stored in the codeplug (codeplugs are copied between radios) and never set by hand.
#ifndef RFOUNDRY_YSF_ID_H
#define RFOUNDRY_YSF_ID_H

#include <stdbool.h>
#include <stdint.h>

#define YSF_RID_LEN	5
#define YSF_RID_SPACE	60466176u	// 36^5 different RIDs

// The RID of a radio whose MCU has the 96-bit unique ID uid (hal_chip_t.uid): the same on every start, spread
// evenly over the 36^5 values, so two radios share one with a chance of 1 in 60 million
void ysf_rid_from_uid(const uint32_t uid[3], char out[YSF_RID_LEN + 1]);
// Whether uid is one (not all zero or all ones, as a part without the ID reads)
bool ysf_uid_usable(const uint32_t uid[3]);
// The fallback for a radio without a usable unique ID: from its DMR ID and callsign, so it is only as unique as
// they are. The radio core says so where it shows the RID
void ysf_rid_fallback(uint32_t dmr_id, const char *callsign, char out[YSF_RID_LEN + 1]);

#endif
