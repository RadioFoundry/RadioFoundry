// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Digital APRS (docs/spec/aprs.md 8): our position over DMR to a location service, as a Motorola LRRP triggered
// report (UDP 4001, core/dmr/dmrLrrp.c) or an ETSI UDT short NMEA location (core/dmr/dmrData.c), through the DMR
// data service. BrandMeister's location service turns either into an APRS position under the callsign registered
// for the sending DMR ID. Radio task only (the data service's own context).
#ifndef ODR_APRS_DMR_H
#define ODR_APRS_DMR_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	int32_t lat, lon;	// 1e-7 degree
	uint16_t speed_kn;	// 0xFFFF unknown
	uint16_t course;	// 0..359, 0xFFFF unknown
	uint32_t utc_hms;	// hhmmss, 0xFFFFFFFF unknown
	uint32_t date;		// ddmmyy, 0 unknown
} aprs_dmr_pos_t;

// Queue the report to dst (a private call) from own_id. False when the data service can't take it now (busy, not on
// a DMR channel, or built without DMR data)
bool aprs_dmr_send(bool udt, uint32_t dst, uint32_t own_id, const aprs_dmr_pos_t *pos);

#endif
