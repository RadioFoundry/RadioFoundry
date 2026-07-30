// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Motorola LRRP (Location Request / Response Protocol, UDP 4001), the radio's side: requests from a location server
 * decoded, answers and reports built. MBXML binary documents: a document ID, the length of the rest, then tokens.
 * Token values from RadioDesk's codec (radiodesk/dmr/location.py, after ok-dmrlib, node-dmr-lib, Moto.Net and
 * TRBO-NET captures), which is also the test oracle.
 */

#ifndef ODR_DMRLRRP_H
#define ODR_DMRLRRP_H

#include <stdbool.h>
#include <stdint.h>
#include "dmr/dmrData.h"

#define LRRP_REQUEST_ID_MAX			16
#define LRRP_MAX_DOCUMENT			64

typedef enum
{
	LRRP_REQUEST_NONE = 0,
	LRRP_REQUEST_IMMEDIATE,					// docs 0x04 / 0x05: one position now
	LRRP_REQUEST_TRIGGERED_START,			// 0x08 / 0x09: periodic, distance or emergency reports from now on
	LRRP_REQUEST_TRIGGERED_STOP,			// 0x0E / 0x0F
	LRRP_REQUEST_PROTOCOL					// 0x14
} lrrpRequestType_t;

#define LRRP_TRIGGER_PERIODIC		0x01
#define LRRP_TRIGGER_DISTANCE		0x02
#define LRRP_TRIGGER_EMERGENCY		0x04
#define LRRP_TRIGGER_ONESHOT		0x08

#define LRRP_WANT_TIME				0x01
#define LRRP_WANT_ACCURACY			0x02
#define LRRP_WANT_SPEED				0x04
#define LRRP_WANT_DIRECTION			0x08
#define LRRP_WANT_ALTITUDE			0x10

typedef struct
{
	lrrpRequestType_t type;
	uint8_t  requestId[LRRP_REQUEST_ID_MAX];
	uint8_t  requestIdLength;
	uint8_t  triggers;						// LRRP_TRIGGER_xxx
	uint8_t  wanted;						// LRRP_WANT_xxx
	uint32_t intervalSeconds;				// 0 when not given
	uint32_t distanceMeters;				// 0 when not given
} lrrpRequest_t;

// Result codes (token 0x37); success is sent as token 0x38
#define LRRP_RESULT_SUCCESS			0x00
#define LRRP_RESULT_NOT_UNDERSTOOD	0x0A
#define LRRP_RESULT_NO_FIX			0x10

// A position for an answer: the NMEA one, with the date for the info time
typedef struct
{
	dmrDataLocation_t location;
	uint16_t utcYear;						// 0 when the date isn't known (no info time sent)
	uint8_t  utcMonth;
	uint8_t  utcDay;
	bool     hasCourse;
	uint16_t courseDegrees;
} lrrpPosition_t;

bool lrrpDecodeRequest(const uint8_t *payload, int length, lrrpRequest_t *request);

// The answers, each returns the document's length (0 if it doesn't fit). position may be NULL (no fix): the answer
// then carries LRRP_RESULT_NO_FIX
int lrrpBuildImmediateResponse(const lrrpRequest_t *request, const lrrpPosition_t *position, uint8_t *out);
int lrrpBuildTriggeredStartResponse(const uint8_t *requestId, int requestIdLength, uint32_t result, uint8_t *out);
int lrrpBuildTriggeredReport(const uint8_t *requestId, int requestIdLength, uint8_t wanted, const lrrpPosition_t *position, uint8_t *out);
int lrrpBuildTriggeredStopResponse(const uint8_t *requestId, int requestIdLength, uint8_t *out);

// Great circle distance in meters (equirectangular, fine for a few km)
uint32_t lrrpDistanceMeters(const dmrDataLocation_t *a, const dmrDataLocation_t *b);

#endif
