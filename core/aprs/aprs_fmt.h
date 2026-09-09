// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// APRS information fields (APRS Protocol Reference 1.0.1, chapters 5-14, and the 1.1 / 1.2 addenda; docs/spec/aprs.md
// 5): our position reports (uncompressed and compressed, symbol, course / speed, altitude, comment, ambiguity),
// status, messages and acks built; positions (uncompressed, compressed, Mic-E, objects, items), status and messages
// parsed from what is heard. Positions are integers in 1e-7 degree. Distance and bearing for the station list.
#ifndef ODR_APRS_FMT_H
#define ODR_APRS_FMT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "aprs/ax25.h"

#define APRS_DEG		10000000L	// position units per degree
#define APRS_COURSE_NONE	0xFFFFu
#define APRS_SPEED_NONE		0xFFFFu
#define APRS_ALT_NONE		INT32_MIN
#define APRS_COMMENT_MAX	43	// what fits after a position with course / speed and altitude in common use
#define APRS_ADDRESSEE_LEN	9
#define APRS_MSG_TEXT_MAX	67
#define APRS_MSGNO_MAX		5
#define APRS_NAME_MAX		9	// object / item names

typedef struct {
	int32_t lat, lon;	// 1e-7 degree, north / east positive
	uint16_t course;	// degrees 1..360 (360 = north), APRS_COURSE_NONE = unknown (0 means unknown in APRS too)
	uint16_t speed_kn;	// knots, APRS_SPEED_NONE = unknown
	int32_t alt_m;		// meters, APRS_ALT_NONE = unknown
	char sym_table;		// '/' primary, '\\' alternate, or an overlay character
	char sym_code;
} aprs_pos_t;

// --- Our reports ---

typedef struct {
	aprs_pos_t pos;
	bool compressed;	// base-91 compressed position (13 characters, about 0.3 m resolution)
	bool messaging;		// '=' (we take messages) rather than '!'
	uint8_t ambiguity;	// uncompressed only: 0..4 trailing digits blanked (about 1 nm .. 60 nm)
	const char *comment;	// NULL or "" for none; cut at APRS_COMMENT_MAX
} aprs_report_t;

// The information field of a position report; returns the length (0 if it doesn't fit)
size_t aprs_build_position(const aprs_report_t *r, char *out, size_t max);
// ">text"
size_t aprs_build_status(const char *text, char *out, size_t max);
// ":ADDRESSEE:text{msgno" (msgno NULL or "" for an unacknowledged message)
size_t aprs_build_message(const char *addressee, const char *text, const char *msgno, char *out, size_t max);
// ":ADDRESSEE:ack12345" / "rej"
size_t aprs_build_ack(const char *addressee, const char *msgno, bool reject, char *out, size_t max);

// --- Parsing what is heard ---

typedef enum {
	APRS_T_UNKNOWN, APRS_T_POSITION, APRS_T_MICE, APRS_T_OBJECT, APRS_T_ITEM, APRS_T_STATUS, APRS_T_MESSAGE,
	APRS_T_THIRD_PARTY, APRS_T_QUERY, APRS_T_WEATHER, APRS_T_TELEMETRY, APRS_T_OTHER,
} aprs_type_t;

typedef struct {
	uint8_t type;		// aprs_type_t
	bool has_pos;
	aprs_pos_t pos;
	bool messaging;		// the station takes messages ('=' / '@' / Mic-E message capable)
	bool killed;		// an object or item killed ('_')
	char name[APRS_NAME_MAX + 1];			// object / item
	char addressee[APRS_ADDRESSEE_LEN + 1];		// message: trailing spaces removed
	char msgno[APRS_MSGNO_MAX + 1];			// message: "" if none
	bool ack, rej;		// the message is an ack / reject of msgno
	uint8_t mice_msg;	// Mic-E message: 0..6 = M0 off duty .. M6 priority, 7 = emergency; custom C0..C6 = 8..14
	const char *text;	// comment, status or message text (points into the info field), NUL not guaranteed
	uint16_t text_len;
} aprs_parsed_t;

// Parse an information field. dst is the frame's destination (Mic-E carries latitude there). False only when the
// field is empty; unknown types come back as APRS_T_UNKNOWN / APRS_T_OTHER
bool aprs_parse(const ax25_addr_t *dst, const uint8_t *info, size_t n, aprs_parsed_t *p);

// --- Geometry ---

// Great circle distance in meters and initial bearing in degrees (0..359) from a to b
uint32_t aprs_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);
uint16_t aprs_bearing_deg(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);

// Base 91 helpers (exposed for the tests)
void aprs_base91(uint32_t v, uint8_t digits, char *out);
uint32_t aprs_base91_value(const char *s, uint8_t digits);

#endif
