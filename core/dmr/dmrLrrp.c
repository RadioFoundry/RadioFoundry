// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Motorola LRRP (Location Request / Response Protocol, UDP 4001), the radio's side, see dmrLrrp.h
 */

#include <string.h>
#include <math.h>
#include "dmr/dmrLrrp.h"

// Document IDs: requests come with or without a constant data table (even / odd), the answers are sent without
#define DOC_IMMEDIATE_RESPONSE		0x07
#define DOC_START_RESPONSE			0x0B
#define DOC_TRIGGERED_REPORT		0x0D
#define DOC_STOP_RESPONSE			0x11

// Tokens, requests
#define TOK_REQUEST_ID				0x22	// length, bytes
#define TOK_REQUEST_ID_1			0x23	// one byte
#define TOK_REQUEST_ID_INT			0x24	// uintvar
#define TOK_INTERVAL				0x31
#define TOK_ONESHOT					0x33
#define TOK_PERIODIC				0x34
#define TOK_PROTOCOL_VERSION		0x3F
#define TOK_MAX_INFO_AGE			0x42
#define TOK_TRG_CONDITION			0x4A
#define TOK_DISTANCE				0x78
#define TRG_CONDITION_EMERGENCY		0x02

// Tokens, answers
#define TOK_INFO_TIME				0x34	// 5 octets
#define TOK_RESULT					0x37	// uintvar
#define TOK_RESULT_OK				0x38
#define TOK_POSITION_RADIUS			0x51	// lat, lon, radius (ufloatvar)
#define TOK_DIRECTION				0x56	// one octet, degrees / 2
#define TOK_SPEED_HOR				0x6C	// ufloatvar, m/s

typedef struct
{
	const uint8_t *d;
	int i;
	int end;
	bool error;
} reader_t;

static uint8_t readByte(reader_t *r)
{
	if (r->i >= r->end)
	{
		r->error = true;
		return 0;
	}
	return r->d[r->i++];
}

static uint32_t readUintvar(reader_t *r)
{
	uint32_t v = 0;

	for (int n = 0; n < 5; n++)
	{
		uint8_t b = readByte(r);

		v = (v << 7) | (b & 0x7F);
		if (!(b & 0x80))
		{
			return v;
		}
	}

	r->error = true;
	return 0;
}

static void skipUfloatvar(reader_t *r)
{
	readUintvar(r);
	readUintvar(r);
}

static int putUintvar(uint8_t *out, uint32_t v)
{
	uint8_t tmp[5];
	int n = 0;

	do
	{
		tmp[n++] = v & 0x7F;
		v >>= 7;
	} while (v != 0);

	for (int i = 0; i < n; i++)
	{
		out[i] = tmp[n - 1 - i] | ((i < (n - 1)) ? 0x80 : 0x00);
	}

	return n;
}

bool lrrpDecodeRequest(const uint8_t *payload, int length, lrrpRequest_t *request)
{
	reader_t r = { .d = payload, .i = 0, .end = length, .error = false };

	memset(request, 0, sizeof(*request));

	uint32_t docId = readUintvar(&r);
	uint32_t docLength = readUintvar(&r);

	if (r.error)
	{
		return false;
	}

	// A document shorter than the datagram ends there; a longer one (cut in transit) at the datagram's end. Compared
	// unsigned: a length near 2^32 must not wrap
	if (docLength < (uint32_t)(r.end - r.i))
	{
		r.end = r.i + (int)docLength;
	}

	switch (docId)
	{
		case 0x04:
		case 0x05:
			request->type = LRRP_REQUEST_IMMEDIATE;
			break;
		case 0x08:
		case 0x09:
			request->type = LRRP_REQUEST_TRIGGERED_START;
			break;
		case 0x0E:
		case 0x0F:
			request->type = LRRP_REQUEST_TRIGGERED_STOP;
			break;
		case 0x14:
			request->type = LRRP_REQUEST_PROTOCOL;
			break;
		default:
			return false;// an answer, or something else
	}

	while ((r.i < r.end) && !r.error)
	{
		uint32_t tok = readUintvar(&r);

		switch (tok)
		{
			case TOK_REQUEST_ID:
				{
					uint32_t n = readUintvar(&r);

					if ((n == 0) || (n > LRRP_REQUEST_ID_MAX) || ((r.i + (int)n) > r.end))
					{
						return false;
					}
					memcpy(request->requestId, &r.d[r.i], n);
					request->requestIdLength = n;
					r.i += n;
				}
				break;
			case TOK_REQUEST_ID_1:
				request->requestId[0] = readByte(&r);
				request->requestIdLength = 1;
				break;
			case TOK_REQUEST_ID_INT:
				{
					// Kept as the octets of its value, big endian
					uint32_t v = readUintvar(&r);

					request->requestIdLength = 4;
					request->requestId[0] = v >> 24;
					request->requestId[1] = v >> 16;
					request->requestId[2] = v >> 8;
					request->requestId[3] = v;
				}
				break;
			case 0x50:
				request->wanted |= LRRP_WANT_ACCURACY;
				break;
			case 0x51:
				request->wanted |= LRRP_WANT_ACCURACY | LRRP_WANT_TIME;
				break;
			case 0x52:
				request->wanted |= LRRP_WANT_TIME;
				break;
			case 0x53:
				break;
			case 0x54:
				request->wanted |= LRRP_WANT_ALTITUDE;
				break;
			case 0x57:
				request->wanted |= LRRP_WANT_DIRECTION;
				break;
			case 0x62:
				request->wanted |= LRRP_WANT_SPEED;
				break;
			case TOK_PERIODIC:
				request->triggers |= LRRP_TRIGGER_PERIODIC;
				break;
			case TOK_ONESHOT:
				request->triggers |= LRRP_TRIGGER_ONESHOT;
				break;
			case TOK_INTERVAL:
				request->intervalSeconds = readUintvar(&r);
				break;
			case TOK_DISTANCE:
				request->distanceMeters = readUintvar(&r);
				request->triggers |= LRRP_TRIGGER_DISTANCE;
				break;
			case TOK_TRG_CONDITION:
				if (readUintvar(&r) == TRG_CONDITION_EMERGENCY)
				{
					request->triggers |= LRRP_TRIGGER_EMERGENCY;
				}
				break;
			case TOK_PROTOCOL_VERSION:
			case TOK_MAX_INFO_AGE:
			case 0x55:
			case 0x5F:
			case 0x67:
			case 0x71:
				readUintvar(&r);
				break;
			case 0x56:
			case 0x60:
			case 0x68:
			case 0x72:
				skipUfloatvar(&r);
				break;
			case 0x61:
			case 0x73:
				readByte(&r);
				break;
			case 0x64:
			case 0x66:
			case 0x69:
			case 0x74:
			case 0x76:
				break;
			default:
				r.i = r.end;// a token we don't know: what came before is still used, as rfoundry_sdrbench does
				break;
		}
	}

	return !r.error && (request->requestIdLength > 0);
}

// The document around its tokens: ID, length, tokens
static int finishDocument(uint8_t docId, const uint8_t *tokens, int tokensLength, uint8_t *out)
{
	int n = putUintvar(out, docId);

	n += putUintvar(&out[n], tokensLength);
	if ((n + tokensLength) > LRRP_MAX_DOCUMENT)
	{
		return 0;
	}
	memcpy(&out[n], tokens, tokensLength);

	return n + tokensLength;
}

static int putRequestId(uint8_t *out, const uint8_t *requestId, int requestIdLength)
{
	int n = 0;

	out[n++] = TOK_REQUEST_ID;
	n += putUintvar(&out[n], requestIdLength);
	memcpy(&out[n], requestId, requestIdLength);

	return n + requestIdLength;
}

static double degreesOf(uint8_t degrees, uint32_t minutes10000, bool positive)
{
	double d = degrees + (minutes10000 / 600000.0);

	return positive ? d : -d;
}

// Info time, latitude / longitude with a radius, and speed and direction when asked for and known
static int putPosition(uint8_t *out, uint8_t wanted, const lrrpPosition_t *position)
{
	const dmrDataLocation_t *loc = &position->location;
	int n = 0;

	if (position->utcYear != 0)
	{
		uint64_t t = ((uint64_t)position->utcYear << 26) | ((uint64_t)position->utcMonth << 22) | ((uint64_t)position->utcDay << 17) |
				((uint64_t)loc->utcHours << 12) | ((uint64_t)loc->utcMinutes << 6) | loc->utcSeconds;

		out[n++] = TOK_INFO_TIME;
		for (int i = 4; i >= 0; i--)
		{
			out[n++] = (t >> (i * 8)) & 0xFF;
		}
	}

	double lat = degreesOf(loc->latDegrees, loc->latMinutes10000, loc->north);
	double lon = degreesOf(loc->lonDegrees, loc->lonMinutes10000, loc->east);
	uint32_t latRaw = (uint32_t)((fabs(lat) * 2147483648.0 / 90.0) + 0.5);
	int32_t lonRaw = (int32_t)lround(lon * 4294967296.0 / 360.0);

	if (latRaw > 0x7FFFFFFF)
	{
		latRaw = 0x7FFFFFFF;
	}
	if (lat < 0)
	{
		latRaw |= 0x80000000;
	}

	out[n++] = TOK_POSITION_RADIUS;
	for (int i = 3; i >= 0; i--)
	{
		out[n++] = (latRaw >> (i * 8)) & 0xFF;
	}
	for (int i = 3; i >= 0; i--)
	{
		out[n++] = ((uint32_t)lonRaw >> (i * 8)) & 0xFF;
	}
	n += putUintvar(&out[n], 10);// radius: 10 m (no HDOP kept), no fraction
	out[n++] = 0x00;

	if ((wanted & LRRP_WANT_SPEED) && (loc->speedKnots != DMR_LOCATION_SPEED_UNKNOWN))
	{
		uint32_t mps128 = (uint32_t)((loc->speedKnots * 0.514444 * 128.0) + 0.5);// m/s in 1/128

		out[n++] = TOK_SPEED_HOR;
		n += putUintvar(&out[n], mps128 >> 7);
		out[n++] = mps128 & 0x7F;
	}

	if ((wanted & LRRP_WANT_DIRECTION) && position->hasCourse)
	{
		out[n++] = TOK_DIRECTION;
		out[n++] = (position->courseDegrees % 360) / 2;
	}

	return n;
}

int lrrpBuildImmediateResponse(const lrrpRequest_t *request, const lrrpPosition_t *position, uint8_t *out)
{
	uint8_t tokens[LRRP_MAX_DOCUMENT];
	int n = putRequestId(tokens, request->requestId, request->requestIdLength);

	if ((position == NULL) || !position->location.fixValid)
	{
		tokens[n++] = TOK_RESULT;
		n += putUintvar(&tokens[n], LRRP_RESULT_NO_FIX);
	}

	if (position != NULL)
	{
		n += putPosition(&tokens[n], request->wanted, position);// the last known one, also without a fix
	}

	return finishDocument(DOC_IMMEDIATE_RESPONSE, tokens, n, out);
}

int lrrpBuildTriggeredStartResponse(const uint8_t *requestId, int requestIdLength, uint32_t result, uint8_t *out)
{
	uint8_t tokens[LRRP_MAX_DOCUMENT];
	int n = putRequestId(tokens, requestId, requestIdLength);

	if (result == LRRP_RESULT_SUCCESS)
	{
		tokens[n++] = TOK_RESULT_OK;
	}
	else
	{
		tokens[n++] = TOK_RESULT;
		n += putUintvar(&tokens[n], result);
	}

	return finishDocument(DOC_START_RESPONSE, tokens, n, out);
}

int lrrpBuildTriggeredReport(const uint8_t *requestId, int requestIdLength, uint8_t wanted, const lrrpPosition_t *position, uint8_t *out)
{
	uint8_t tokens[LRRP_MAX_DOCUMENT];
	int n = putRequestId(tokens, requestId, requestIdLength);

	if (!position->location.fixValid)
	{
		tokens[n++] = TOK_RESULT;
		n += putUintvar(&tokens[n], LRRP_RESULT_NO_FIX);
	}
	n += putPosition(&tokens[n], wanted, position);

	return finishDocument(DOC_TRIGGERED_REPORT, tokens, n, out);
}

int lrrpBuildTriggeredStopResponse(const uint8_t *requestId, int requestIdLength, uint8_t *out)
{
	uint8_t tokens[LRRP_MAX_DOCUMENT];
	int n = putRequestId(tokens, requestId, requestIdLength);

	tokens[n++] = TOK_RESULT_OK;

	return finishDocument(DOC_STOP_RESPONSE, tokens, n, out);
}

uint32_t lrrpDistanceMeters(const dmrDataLocation_t *a, const dmrDataLocation_t *b)
{
	double lat1 = degreesOf(a->latDegrees, a->latMinutes10000, a->north) * (M_PI / 180.0);
	double lat2 = degreesOf(b->latDegrees, b->latMinutes10000, b->north) * (M_PI / 180.0);
	double dLon = (degreesOf(b->lonDegrees, b->lonMinutes10000, b->east) - degreesOf(a->lonDegrees, a->lonMinutes10000, a->east)) * (M_PI / 180.0);
	double x = dLon * cos((lat1 + lat2) / 2.0);
	double y = lat2 - lat1;

	return (uint32_t)(sqrt((x * x) + (y * y)) * 6371000.0);
}
