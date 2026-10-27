// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The zone share record and its messages (zoneShare.h, docs/spec/zone-share.md)
#include <string.h>
#include "dmr/zoneShare.h"

_Static_assert(ZS_FRAGMENTS_MAX <= 8, "a transfer's fragments fit zsAssembly_t.have (and the 4-bit count)");
_Static_assert((ZS_SEND_CHUNK >= ZS_CHUNK_MIN) && (ZS_SEND_CHUNK <= ZS_CHUNK_MAX), "our own fragments are ones we take");
_Static_assert(ZS_SEND_FRAGMENTS_MAX <= 4, "the first builds take at most 4 fragments");

#define REC_HEADER		10		// version, length (2), zone ID (4), flags, channels, contacts; the name follows

#define CH_DMR			0x01
#define CH_WIDE			0x02
#define CH_TX_OFFSET	0x04
#define CH_RX_TONE		0x08
#define CH_TX_TONE		0x10
#define CH_RX_ONLY		0x20
#define CH_POWER_SHIFT	6

#define DMR_SLOT2		0x10
#define DMR_DMO			0x20

#define CTCSS_MIN		600		// 60.0 Hz
#define CTCSS_MAX		2600	// 260.0 Hz
#define DCS_MAX			0777

// --- Writing ---

static void put8(zsWriter_t *w, uint8_t v)
{
	if (w->at >= w->size)
	{
		w->failed = true;
		return;
	}
	w->out[w->at++] = v;
}

static void put16(zsWriter_t *w, uint16_t v)
{
	put8(w, (uint8_t)(v >> 8));
	put8(w, (uint8_t)v);
}

static void put32(zsWriter_t *w, uint32_t v)
{
	put16(w, (uint16_t)(v >> 16));
	put16(w, (uint16_t)v);
}

// Up to ZS_NAME_MAX octets of printable ASCII (anything else becomes '?': the receiver refuses other octets)
static void putName(zsWriter_t *w, const char *name)
{
	int n = 0;

	while ((name != NULL) && (n < ZS_NAME_MAX) && (name[n] != '\0'))
	{
		n++;
	}
	put8(w, (uint8_t)n);
	for (int i = 0; i < n; i++)
	{
		uint8_t c = (uint8_t)name[i];

		put8(w, ((c >= 0x20) && (c <= 0x7E)) ? c : '?');
	}
}

bool zsToneValid(uint8_t type, uint16_t value)
{
	switch (type)
	{
		case ZS_TONE_NONE:
			return value == 0;
		case ZS_TONE_CTCSS:
			return (value >= CTCSS_MIN) && (value <= CTCSS_MAX);
		case ZS_TONE_DCS:
		case ZS_TONE_DCS_INV:
			return value <= DCS_MAX;
		default:
			return false;
	}
}

void zsWriterBegin(zsWriter_t *w, uint8_t *out, int size, uint32_t zoneId, uint8_t flags, const char *zoneName)
{
	memset(w, 0, sizeof(*w));
	w->out = out;
	w->size = (size > ZS_RECORD_MAX) ? ZS_RECORD_MAX : size;
	w->failed = (zoneId == 0) || (out == NULL);
	put8(w, ZS_VERSION);
	put16(w, 0);// the length, at the end
	put32(w, zoneId);
	put8(w, flags);
	put8(w, 0);// counts, at the end
	put8(w, 0);
	putName(w, zoneName);
}

bool zsWriterContact(zsWriter_t *w, const zsContact_t *c)
{
	if ((w->channels != 0) || (w->contacts >= ZS_CONTACTS_MAX) || (c->type > ZS_CONTACT_ALL_CALL) || (c->timeslot > 2) ||
			(c->id == 0) || (c->id > 0xFFFFFF))
	{
		w->failed = true;
		return false;
	}
	put8(w, (uint8_t)(c->type | (c->timeslot << 2)));
	put8(w, (uint8_t)(c->id >> 16));
	put16(w, (uint16_t)c->id);
	putName(w, c->name);
	w->contacts++;
	return !w->failed;
}

bool zsWriterChannel(zsWriter_t *w, const zsChannel_t *ch)
{
	int64_t tx = (int64_t)ch->rxHz + ch->txOffsetHz;

	if ((w->channels >= ZS_CHANNELS_MAX) || (ch->rxHz == 0) || (tx <= 0) || (tx > 0x7FFFFFFF) || (ch->power > ZS_POWER_HIGH) ||
			(ch->dmr && ((ch->colorCode > 15) || (ch->timeslot < 1) || (ch->timeslot > 2) || (ch->contact > w->contacts))) ||
			(!ch->dmr && (!zsToneValid(ch->rxToneType, ch->rxTone) || !zsToneValid(ch->txToneType, ch->txTone))))
	{
		w->failed = true;
		return false;
	}
	bool rxTone = !ch->dmr && (ch->rxToneType != ZS_TONE_NONE);
	bool txTone = !ch->dmr && (ch->txToneType != ZS_TONE_NONE);
	uint8_t a = (uint8_t)((ch->dmr ? CH_DMR : 0) | (ch->wide ? CH_WIDE : 0) | ((ch->txOffsetHz != 0) ? CH_TX_OFFSET : 0) |
			(rxTone ? CH_RX_TONE : 0) | (txTone ? CH_TX_TONE : 0) | (ch->rxOnly ? CH_RX_ONLY : 0) |
			(ch->power << CH_POWER_SHIFT));

	put8(w, a);
	put32(w, ch->rxHz);
	if (ch->txOffsetHz != 0)
	{
		put32(w, (uint32_t)ch->txOffsetHz);
	}
	if (ch->dmr)
	{
		put8(w, (uint8_t)(ch->colorCode | ((ch->timeslot == 2) ? DMR_SLOT2 : 0) | (ch->dmo ? DMR_DMO : 0)));
		put8(w, ch->contact);
	}
	if (rxTone)
	{
		put16(w, (uint16_t)((ch->rxToneType << 14) | ch->rxTone));
	}
	if (txTone)
	{
		put16(w, (uint16_t)((ch->txToneType << 14) | ch->txTone));
	}
	putName(w, ch->name);
	w->channels++;
	return !w->failed;
}

int zsWriterEnd(zsWriter_t *w)
{
	if (w->failed || (w->channels == 0) || (w->at > w->size))
	{
		return 0;
	}
	w->out[1] = (uint8_t)(w->at >> 8);
	w->out[2] = (uint8_t)w->at;
	w->out[8] = w->channels;
	w->out[9] = w->contacts;
	return w->at;
}

static int nameSize(const char *name)
{
	int n = 0;

	while ((name != NULL) && (n < ZS_NAME_MAX) && (name[n] != '\0'))
	{
		n++;
	}
	return 1 + n;
}

int zsHeaderSize(const char *zoneName)
{
	return REC_HEADER + nameSize(zoneName);
}

int zsContactSize(const zsContact_t *c)
{
	return 4 + nameSize(c->name);
}

int zsChannelSize(const zsChannel_t *ch)
{
	return 1 + 4 + ((ch->txOffsetHz != 0) ? 4 : 0) + (ch->dmr ? 2 : 0) +
			((!ch->dmr && (ch->rxToneType != ZS_TONE_NONE)) ? 2 : 0) + ((!ch->dmr && (ch->txToneType != ZS_TONE_NONE)) ? 2 : 0) +
			nameSize(ch->name);
}

// --- Reading ---

typedef struct
{
	const uint8_t *p;
	int length;
	int at;
	bool bad;
} reader_t;

static uint8_t get8(reader_t *r)
{
	if (r->at >= r->length)
	{
		r->bad = true;
		return 0;
	}
	return r->p[r->at++];
}

static uint16_t get16(reader_t *r)
{
	uint16_t hi = get8(r);

	return (uint16_t)((hi << 8) | get8(r));
}

static uint32_t get32(reader_t *r)
{
	uint32_t hi = get16(r);

	return (hi << 16) | get16(r);
}

// A name: its length (at most ZS_NAME_MAX) and that many printable ASCII octets; out may be NULL (skipped)
static void getName(reader_t *r, char *out)
{
	uint8_t n = get8(r);

	if (n > ZS_NAME_MAX)
	{
		r->bad = true;
		return;
	}
	for (int i = 0; i < n; i++)
	{
		uint8_t c = get8(r);

		r->bad = r->bad || (c < 0x20) || (c > 0x7E);
		if (out != NULL)
		{
			out[i] = (char)c;
		}
	}
	if (out != NULL)
	{
		out[r->bad ? 0 : n] = '\0';
	}
}

static void readContact(reader_t *r, zsContact_t *c)
{
	uint8_t t = get8(r);

	c->type = t & 0x03;
	c->timeslot = (t >> 2) & 0x03;
	c->id = (uint32_t)get8(r) << 16;
	c->id |= get16(r);
	getName(r, c->name);
	r->bad = r->bad || (c->type > ZS_CONTACT_ALL_CALL) || (c->timeslot > 2) || (c->id == 0) || (t & 0xF0);
}

static void readChannel(reader_t *r, uint8_t contacts, zsChannel_t *ch)
{
	uint8_t a = get8(r);

	memset(ch, 0, sizeof(*ch));
	ch->dmr = (a & CH_DMR) != 0;
	ch->wide = (a & CH_WIDE) != 0;
	ch->rxOnly = (a & CH_RX_ONLY) != 0;
	ch->power = a >> CH_POWER_SHIFT;
	ch->rxHz = get32(r);
	if (a & CH_TX_OFFSET)
	{
		ch->txOffsetHz = (int32_t)get32(r);
		r->bad = r->bad || (ch->txOffsetHz == 0);// present means nonzero: one encoding per channel
	}
	if (ch->dmr)
	{
		uint8_t d = get8(r);

		ch->colorCode = d & 0x0F;
		ch->timeslot = (d & DMR_SLOT2) ? 2 : 1;
		ch->dmo = (d & DMR_DMO) != 0;
		ch->contact = get8(r);
		r->bad = r->bad || (ch->contact > contacts) || (a & (CH_RX_TONE | CH_TX_TONE)) || (d & 0xC0);
	}
	if (a & CH_RX_TONE)
	{
		uint16_t t = get16(r);

		ch->rxToneType = (uint8_t)(t >> 14);
		ch->rxTone = t & 0x3FFF;
		r->bad = r->bad || (ch->rxToneType == ZS_TONE_NONE) || !zsToneValid(ch->rxToneType, ch->rxTone);
	}
	if (a & CH_TX_TONE)
	{
		uint16_t t = get16(r);

		ch->txToneType = (uint8_t)(t >> 14);
		ch->txTone = t & 0x3FFF;
		r->bad = r->bad || (ch->txToneType == ZS_TONE_NONE) || !zsToneValid(ch->txToneType, ch->txTone);
	}
	getName(r, ch->name);
	int64_t tx = (int64_t)ch->rxHz + ch->txOffsetHz;
	r->bad = r->bad || (ch->rxHz == 0) || (ch->rxHz > 0x7FFFFFFF) || (tx <= 0) || (tx > 0x7FFFFFFF);
}

bool zsParse(const uint8_t *data, int length, zsRecord_t *rec)
{
	memset(rec, 0, sizeof(*rec));
	if ((data == NULL) || (length < REC_HEADER + 1) || (length > ZS_RECORD_MAX) || (data[0] != ZS_VERSION) ||
			(((data[1] << 8) | data[2]) != length))
	{
		return false;
	}
	reader_t r = { .p = data, .length = length, .at = 3 };

	rec->zoneId = get32(&r);
	rec->flags = get8(&r);
	rec->channels = get8(&r);
	rec->contacts = get8(&r);
	getName(&r, rec->name);
	if (r.bad || (rec->zoneId == 0) || (rec->channels == 0) || (rec->channels > ZS_CHANNELS_MAX) ||
			(rec->contacts > ZS_CONTACTS_MAX) || (rec->flags & ~ZS_FLAG_CHANNEL) ||
			((rec->flags & ZS_FLAG_CHANNEL) && (rec->channels != 1)))
	{
		return false;
	}
	for (int i = 0; i < rec->contacts; i++)
	{
		zsContact_t c;

		rec->contactAt[i] = (uint16_t)r.at;
		readContact(&r, &c);
		if (r.bad)
		{
			return false;
		}
	}
	for (int i = 0; i < rec->channels; i++)
	{
		zsChannel_t ch;

		rec->channelAt[i] = (uint16_t)r.at;
		readChannel(&r, rec->contacts, &ch);
		if (r.bad)
		{
			return false;
		}
	}
	if (r.at != length)
	{
		return false;// trailing octets: not the record its header describes
	}
	rec->data = data;
	rec->length = length;
	return true;
}

bool zsContactAt(const zsRecord_t *rec, int index, zsContact_t *c)
{
	if ((rec->data == NULL) || (index < 0) || (index >= rec->contacts))
	{
		return false;
	}
	reader_t r = { .p = rec->data, .length = rec->length, .at = rec->contactAt[index] };

	readContact(&r, c);
	return !r.bad;
}

bool zsChannelAt(const zsRecord_t *rec, int index, zsChannel_t *ch)
{
	if ((rec->data == NULL) || (index < 0) || (index >= rec->channels))
	{
		return false;
	}
	reader_t r = { .p = rec->data, .length = rec->length, .at = rec->channelAt[index] };

	readChannel(&r, rec->contacts, ch);
	return !r.bad;
}

// --- Messages ---

int zsFragmentCount(int recordLength)
{
	if ((recordLength <= 0) || (recordLength > ZS_RECORD_MAX))
	{
		return 0;
	}
	return (recordLength + ZS_SEND_CHUNK - 1) / ZS_SEND_CHUNK;
}

int zsFragmentHeader(uint8_t transfer, int recordLength, int index, uint8_t out[ZS_MSG_HEADER], int *offset)
{
	int count = zsFragmentCount(recordLength);

	if ((count == 0) || (index < 0) || (index >= count))
	{
		return 0;
	}
	int at = index * ZS_SEND_CHUNK;
	int n = recordLength - at;

	n = (n > ZS_SEND_CHUNK) ? ZS_SEND_CHUNK : n;
	out[0] = ZS_VERSION;
	out[1] = ZS_MSG_FRAGMENT;
	out[2] = transfer;
	out[3] = (uint8_t)((index << 4) | count);
	out[4] = (uint8_t)(at >> 8);
	out[5] = (uint8_t)at;
	*offset = at;
	return n;
}

int zsBuildFragment(uint8_t transfer, const uint8_t *record, int recordLength, int index, uint8_t out[ZS_MSG_HEADER + ZS_SEND_CHUNK])
{
	int offset = 0;
	int n = zsFragmentHeader(transfer, recordLength, index, out, &offset);

	if (n == 0)
	{
		return 0;
	}
	memcpy(&out[ZS_MSG_HEADER], &record[offset], (size_t)n);
	return ZS_MSG_HEADER + n;
}

int zsBuildRequest(uint8_t request, uint8_t want, uint8_t out[ZS_REQUEST_LEN])
{
	out[0] = ZS_VERSION;
	out[1] = ZS_MSG_REQUEST;
	out[2] = request;
	out[3] = want;
	out[4] = 0;// options: none defined
	return ZS_REQUEST_LEN;
}

int zsBuildRefusal(uint8_t request, uint8_t code, uint8_t out[ZS_REFUSAL_LEN])
{
	out[0] = ZS_VERSION;
	out[1] = ZS_MSG_REFUSAL;
	out[2] = request;
	out[3] = code;
	return ZS_REFUSAL_LEN;
}

bool zsParseMessage(const uint8_t *payload, int length, zsMessage_t *m)
{
	memset(m, 0, sizeof(*m));
	if ((payload == NULL) || (length < 4) || (length > ZS_MSG_MAX) || (payload[0] != ZS_VERSION))
	{
		return false;
	}
	m->number = payload[2];
	switch (payload[1])
	{
		case ZS_MSG_FRAGMENT:
			if (length <= ZS_MSG_HEADER)
			{
				return false;
			}
			m->index = payload[3] >> 4;
			m->count = payload[3] & 0x0F;
			m->offset = (uint16_t)((payload[4] << 8) | payload[5]);
			m->chunk = &payload[ZS_MSG_HEADER];
			m->chunkLength = length - ZS_MSG_HEADER;
			if ((m->count == 0) || (m->count > ZS_FRAGMENTS_MAX) || (m->index >= m->count) ||
					((m->offset + m->chunkLength) > ZS_RECORD_MAX))
			{
				return false;
			}
			// The sender's chunk size: a full chunk's length, or what the offset says. Each fragment carries it
			// whole or as its offset, so one can be checked alone; zsAssemble checks they agree
			if (m->index + 1 < m->count)
			{
				m->full = (uint16_t)m->chunkLength;
				if ((m->chunkLength < ZS_CHUNK_MIN) || (m->offset != m->index * m->chunkLength))
				{
					return false;
				}
			}
			else if (m->index > 0)
			{
				m->full = (uint16_t)(m->offset / m->index);
				if ((m->offset % m->index != 0) || (m->full < ZS_CHUNK_MIN) || (m->full > ZS_CHUNK_MAX) ||
						(m->chunkLength > m->full))
				{
					return false;
				}
			}
			else if (m->offset != 0)
			{
				return false;
			}
			break;
		case ZS_MSG_REQUEST:
			if (length != 5)
			{
				return false;
			}
			m->code = payload[3];
			break;
		case ZS_MSG_REFUSAL:
			if (length != 4)
			{
				return false;
			}
			m->code = payload[3];
			break;
		default:
			return false;
	}
	m->type = payload[1];
	return true;
}

// --- Reassembly ---

void zsAssemblyReset(zsAssembly_t *a)
{
	a->active = false;
	a->have = 0;
	a->count = 0;
	a->length = 0;
}

zsAsmResult_t zsAssemble(zsAssembly_t *a, uint32_t src, const zsMessage_t *m, uint32_t nowMs)
{
	if (m->type != ZS_MSG_FRAGMENT)
	{
		return ZS_ASM_BAD;
	}
	if (a->active && ((nowMs - a->startedMs) > ZS_REASSEMBLY_MS))
	{
		zsAssemblyReset(a);// the rest never came
	}
	if (a->active && (src != a->src))
	{
		return ZS_ASM_BUSY;
	}
	if (a->active && ((m->number != a->transfer) || (m->count != a->count)))
	{
		zsAssemblyReset(a);// the same sender started over
	}
	if (a->active && (m->full != 0) && (a->full != 0) && (m->full != a->full))
	{
		return ZS_ASM_BAD;
	}
	if (!a->active)
	{
		a->active = true;
		a->src = src;
		a->transfer = m->number;
		a->count = m->count;
		a->have = 0;
		a->length = 0;
		a->full = 0;
		a->startedMs = nowMs;
	}
	if (m->full != 0)
	{
		a->full = m->full;
	}
	uint8_t bit = (uint8_t)(1u << m->index);

	if (a->have & bit)
	{
		return ZS_ASM_DUPLICATE;
	}
	memcpy(&a->data[m->offset], m->chunk, (size_t)m->chunkLength);
	a->have |= bit;
	if (m->index + 1 == m->count)
	{
		a->length = (uint16_t)(m->offset + m->chunkLength);
	}
	if (a->have != (uint8_t)((1u << a->count) - 1))
	{
		return ZS_ASM_TAKEN;
	}
	a->active = false;
	return ZS_ASM_DONE;
}
