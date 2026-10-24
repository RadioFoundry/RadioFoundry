// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Zone sharing over DMR data (docs/spec/zone-share.md): the share record (a zone or one channel, with the contacts its
 * channels need) and the messages that carry it as UDP payload on ZS_PORT. Pure encode and decode, no state but the
 * reassembly buffer the caller owns, so the host tests run it as is.
 *
 * Everything decoded here comes off the air from anyone: every length, count and reference is checked before use, and
 * a record that fails any check is refused whole.
 */

#ifndef RFOUNDRY_ZONESHARE_H
#define RFOUNDRY_ZONESHARE_H

#include <stdbool.h>
#include <stdint.h>

#define ZS_PORT					4100	// UDP source and destination port (above 4000-4099: compressed headers on auto)
#define ZS_VERSION				1		// record and message version
#define ZS_RECORD_MAX			1024	// a record is at most this long (4 fragments)
#define ZS_NAME_MAX				16		// zone, channel and contact names, octets (no terminator on air)
#define ZS_CHANNELS_MAX			80		// channels in one record
#define ZS_CONTACTS_MAX			32		// contacts in one record
#define ZS_MSG_MAX				328		// UDP payload of one message: fits a confirmed packet with the full IPv4 header
#define ZS_MSG_HEADER			6		// a fragment's header
#define ZS_CHUNK_MAX			(ZS_MSG_MAX - ZS_MSG_HEADER)
#define ZS_FRAGMENTS_MAX		4
#define ZS_REASSEMBLY_MS		30000	// a transfer whose fragments stop coming is dropped after this

// Message types (octet 1)
#define ZS_MSG_FRAGMENT			1
#define ZS_MSG_REQUEST			2
#define ZS_MSG_REFUSAL			3

// What a directory request asks for
#define ZS_WANT_SITE_ZONE		1		// the zone of the repeater or hotspot the request came in through

// Refusal codes
#define ZS_REFUSE_UNKNOWN		1		// the service doesn't know the site
#define ZS_REFUSE_STALE			2		// the site hasn't been seen for a long time
#define ZS_REFUSE_NO_FREQUENCY	3		// the site has no frequency on record
#define ZS_REFUSE_BUSY			4		// try later
#define ZS_REFUSE_UNSUPPORTED	5		// version or request not supported
#define ZS_REFUSE_OTHER			0xFF

// Record flags
#define ZS_FLAG_CHANNEL			0x01	// one channel, not a zone: the receiver adds it to a zone it chooses

// Contact types on air
#define ZS_CONTACT_GROUP		0
#define ZS_CONTACT_PRIVATE		1
#define ZS_CONTACT_ALL_CALL		2

// Power classes on air (the receiver maps them to its own levels)
#define ZS_POWER_DEFAULT		0
#define ZS_POWER_LOW			1
#define ZS_POWER_MID			2
#define ZS_POWER_HIGH			3

// Tones on air: 2 octets, the type in bits 15-14, the value in 13-0 (CTCSS: Hz x 10, 60.0-260.0 Hz; DCS: the code
// as an octal number, 023 -> 19, at most 0777)
#define ZS_TONE_NONE			0
#define ZS_TONE_CTCSS			1
#define ZS_TONE_DCS				2
#define ZS_TONE_DCS_INV			3

typedef struct
{
	uint8_t  type;						// ZS_CONTACT_*
	uint8_t  timeslot;					// 0: the channel's, 1 or 2: forced
	uint32_t id;						// 24 bits
	char     name[ZS_NAME_MAX + 1];
} zsContact_t;

typedef struct
{
	bool     dmr;
	bool     wide;						// 25 kHz
	bool     rxOnly;					// the sender's channel is receive only
	uint8_t  power;						// ZS_POWER_*
	uint32_t rxHz;
	int32_t  txOffsetHz;				// tx = rx + offset (0: simplex)
	uint8_t  colorCode;					// DMR
	uint8_t  timeslot;					// DMR: 1 or 2
	bool     dmo;						// DMR: direct mode (no repeater timing)
	uint8_t  contact;					// DMR: 1-based index into the record's contacts, 0: none
	uint8_t  rxToneType, txToneType;	// FM: ZS_TONE_*
	uint16_t rxTone, txTone;
	char     name[ZS_NAME_MAX + 1];
} zsChannel_t;

// A decoded record: the header, and where its contacts and channels start (read them with zsContactAt / zsChannelAt)
typedef struct
{
	const uint8_t *data;
	int      length;
	uint32_t zoneId;
	uint8_t  flags;
	uint8_t  channels;
	uint8_t  contacts;
	char     name[ZS_NAME_MAX + 1];
	uint16_t contactAt[ZS_CONTACTS_MAX];
	uint16_t channelAt[ZS_CHANNELS_MAX];
} zsRecord_t;

// --- Writing a record ---

typedef struct
{
	uint8_t *out;
	int      size;
	int      at;
	uint8_t  channels;
	uint8_t  contacts;
	bool     failed;					// too long or too many: zsWriterEnd returns 0
} zsWriter_t;

// The contacts first, then the channels (a channel's contact is the contact's 1-based position)
void zsWriterBegin(zsWriter_t *w, uint8_t *out, int size, uint32_t zoneId, uint8_t flags, const char *zoneName);
bool zsWriterContact(zsWriter_t *w, const zsContact_t *c);
bool zsWriterChannel(zsWriter_t *w, const zsChannel_t *ch);
// The record's length, 0 when it didn't fit or anything was wrong
int zsWriterEnd(zsWriter_t *w);

// --- Reading a record ---

// Checks the whole record (version, length, counts, every field, references); false: refuse it
bool zsParse(const uint8_t *data, int length, zsRecord_t *rec);
bool zsContactAt(const zsRecord_t *rec, int index, zsContact_t *c);
bool zsChannelAt(const zsRecord_t *rec, int index, zsChannel_t *ch);

// --- Messages ---

// Fragment `index` of `count` for a record (offset = index * ZS_CHUNK_MAX); the message's length
int zsBuildFragment(uint8_t transfer, const uint8_t *record, int recordLength, int index, uint8_t out[ZS_MSG_MAX]);
int zsFragmentCount(int recordLength);
int zsBuildRequest(uint8_t request, uint8_t want, uint8_t out[ZS_MSG_MAX]);
int zsBuildRefusal(uint8_t request, uint8_t code, uint8_t out[ZS_MSG_MAX]);

typedef struct
{
	uint8_t type;						// ZS_MSG_*; 0: not a message of ours
	uint8_t number;						// transfer or request number
	// Fragment
	uint8_t index, count;
	uint16_t offset;
	const uint8_t *chunk;
	int     chunkLength;
	// Request: what; refusal: the code
	uint8_t code;
} zsMessage_t;

bool zsParseMessage(const uint8_t *payload, int length, zsMessage_t *m);

// --- Reassembly: one transfer at a time, from one sender ---

typedef struct
{
	uint32_t src;
	uint32_t startedMs;
	uint8_t  transfer;
	uint8_t  count;
	uint8_t  have;						// bit i: fragment i is in
	bool     active;
	uint16_t length;					// the record's length (known from the last fragment)
	uint8_t  data[ZS_RECORD_MAX];
} zsAssembly_t;

typedef enum
{
	ZS_ASM_TAKEN,						// kept, more to come
	ZS_ASM_DONE,						// the record is whole: data[0..length)
	ZS_ASM_DUPLICATE,					// a fragment we have
	ZS_ASM_BUSY,						// another sender's transfer is in progress
	ZS_ASM_BAD,							// inconsistent with the transfer or out of range
} zsAsmResult_t;

void zsAssemblyReset(zsAssembly_t *a);
zsAsmResult_t zsAssemble(zsAssembly_t *a, uint32_t src, const zsMessage_t *m, uint32_t nowMs);

#endif
