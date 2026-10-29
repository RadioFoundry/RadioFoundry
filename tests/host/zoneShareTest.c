// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The zone share record and messages (core/dmr/zoneShare.c, docs/spec/zone-share.md): a zone written and read back
// field by field, the writer's limits, every malformed form the parser must refuse (truncated, trailing octets, bad
// counts, references past the contacts, bad tones, names), fragments and their reassembly (out of order, duplicates,
// another sender, a restart, the time-out), requests and refusals; then mutated records and messages (fuzz.h), each
// accepted one read back within bounds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz.h"
#include "dmr/zoneShare.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Parsed from an exact-size heap copy (ASan sees a read past the end)
static bool parse(const uint8_t *p, int n, zsRecord_t *r)
{
	uint8_t *q = fuzz_exact(p, n);
	bool ok = zsParse(q, n, r);

	// Read every entry while the copy lives
	for (int i = 0; ok && (i < r->contacts); i++)
	{
		zsContact_t c;
		ok = zsContactAt(r, i, &c);
	}
	for (int i = 0; ok && (i < r->channels); i++)
	{
		zsChannel_t ch;
		ok = zsChannelAt(r, i, &ch);
	}
	free(q);
	r->data = NULL;
	return ok;
}

static zsChannel_t dmrChannel(const char *name, uint32_t rx, int32_t off, uint8_t cc, uint8_t ts, uint8_t contact)
{
	zsChannel_t ch = { .dmr = true, .rxHz = rx, .txOffsetHz = off, .colorCode = cc, .timeslot = ts, .contact = contact,
					   .power = ZS_POWER_HIGH };
	snprintf(ch.name, sizeof(ch.name), "%s", name);
	return ch;
}

// A zone: 3 contacts, 16 channels (DMR repeater and simplex, FM with tones)
static int sampleZone(uint8_t *out, int size, int channels)
{
	zsWriter_t w;
	zsContact_t c1 = { .type = ZS_CONTACT_GROUP, .id = 3100, .name = "USA" };
	zsContact_t c2 = { .type = ZS_CONTACT_GROUP, .id = 91, .timeslot = 1, .name = "Worldwide" };
	zsContact_t c3 = { .type = ZS_CONTACT_PRIVATE, .id = 3101276, .name = "N4NQV" };

	zsWriterBegin(&w, out, size, 0xA1B2C3D4, 0, "Site Zone");
	zsWriterContact(&w, &c1);
	zsWriterContact(&w, &c2);
	zsWriterContact(&w, &c3);
	for (int i = 0; i < channels; i++)
	{
		char name[17];
		zsChannel_t ch;

		snprintf(name, sizeof(name), "Chan %02d", i + 1);
		if (i % 4 == 3)
		{
			ch = (zsChannel_t){ .rxHz = 146520000 + 15000 * (uint32_t)i, .wide = true, .rxToneType = ZS_TONE_CTCSS,
								.rxTone = 1000, .txToneType = ZS_TONE_DCS_INV, .txTone = 023, .power = ZS_POWER_LOW };
			snprintf(ch.name, sizeof(ch.name), "%s", name);
		}
		else
		{
			ch = dmrChannel(name, 444000000 + 12500 * (uint32_t)i, (i % 2) ? 5000000 : 0, (uint8_t)(i % 16),
							(uint8_t)(1 + i % 2), (uint8_t)(i % 4));
		}
		zsWriterChannel(&w, &ch);
	}
	return zsWriterEnd(&w);
}

static void test_roundtrip(void)
{
	uint8_t buf[ZS_RECORD_MAX];
	int n = sampleZone(buf, sizeof(buf), 16);
	zsRecord_t r;

	CHECK(n > 0 && n < 520);// a 16 channel zone with its contacts: about 30 octets a channel (the issue's estimate)
	printf("# 16 channels, 3 contacts: %d octets, %d fragments\n", n, zsFragmentCount(n));
	CHECK(zsParse(buf, n, &r));
	CHECK(r.zoneId == 0xA1B2C3D4 && r.channels == 16 && r.contacts == 3 && strcmp(r.name, "Site Zone") == 0);
	zsContact_t c;
	CHECK(zsContactAt(&r, 1, &c) && c.id == 91 && c.type == ZS_CONTACT_GROUP && c.timeslot == 1 &&
		  strcmp(c.name, "Worldwide") == 0);
	CHECK(zsContactAt(&r, 2, &c) && c.id == 3101276 && c.type == ZS_CONTACT_PRIVATE);
	CHECK(!zsContactAt(&r, 3, &c) && !zsContactAt(&r, -1, &c));
	zsChannel_t ch;
	CHECK(zsChannelAt(&r, 1, &ch) && ch.dmr && ch.rxHz == 444012500 && ch.txOffsetHz == 5000000 && ch.colorCode == 1 &&
		  ch.timeslot == 2 && ch.contact == 1 && ch.power == ZS_POWER_HIGH && strcmp(ch.name, "Chan 02") == 0);
	CHECK(zsChannelAt(&r, 3, &ch) && !ch.dmr && ch.wide && ch.rxToneType == ZS_TONE_CTCSS && ch.rxTone == 1000 &&
		  ch.txToneType == ZS_TONE_DCS_INV && ch.txTone == 023 && ch.txOffsetHz == 0 && ch.power == ZS_POWER_LOW);
	CHECK(!zsChannelAt(&r, 16, &ch));

	// A negative offset (a repeater input below its output)
	zsWriter_t w;
	zsChannel_t neg = dmrChannel("Neg", 147000000, -600000, 3, 1, 0);
	zsWriterBegin(&w, buf, sizeof(buf), 7, ZS_FLAG_CHANNEL, "One");
	zsWriterChannel(&w, &neg);
	n = zsWriterEnd(&w);
	CHECK(zsParse(buf, n, &r) && (r.flags & ZS_FLAG_CHANNEL) && zsChannelAt(&r, 0, &ch) && ch.txOffsetHz == -600000);

	// Names: cut at ZS_NAME_MAX, other octets than printable ASCII sent as '?'
	zsChannel_t odd = dmrChannel("", 446000000, 0, 1, 1, 0);
	memcpy(odd.name, "A\tB\x80" "CDEFGHIJKLMN", 16);
	odd.name[16] = '\0';
	zsWriterBegin(&w, buf, sizeof(buf), 7, 0, "A zone name that is far too long");
	zsWriterChannel(&w, &odd);
	n = zsWriterEnd(&w);
	CHECK(zsParse(buf, n, &r) && strcmp(r.name, "A zone name that") == 0);
	CHECK(zsChannelAt(&r, 0, &ch) && strcmp(ch.name, "A?B?CDEFGHIJKLMN") == 0);
}

static void test_writer_limits(void)
{
	uint8_t buf[ZS_RECORD_MAX];
	zsWriter_t w;
	zsChannel_t ch = dmrChannel("X", 446000000, 0, 1, 1, 0);

	// No channels, a zero zone ID, a contact after a channel, a channel referring past the contacts
	zsWriterBegin(&w, buf, sizeof(buf), 1, 0, "Empty");
	CHECK(zsWriterEnd(&w) == 0);
	zsWriterBegin(&w, buf, sizeof(buf), 0, 0, "Zero");
	zsWriterChannel(&w, &ch);
	CHECK(zsWriterEnd(&w) == 0);
	zsContact_t c = { .type = ZS_CONTACT_GROUP, .id = 9 };
	zsWriterBegin(&w, buf, sizeof(buf), 1, 0, "Order");
	zsWriterChannel(&w, &ch);
	CHECK(!zsWriterContact(&w, &c) && zsWriterEnd(&w) == 0);
	ch.contact = 1;
	zsWriterBegin(&w, buf, sizeof(buf), 1, 0, "Ref");
	CHECK(!zsWriterChannel(&w, &ch) && zsWriterEnd(&w) == 0);

	// Bad fields
	zsChannel_t bad = dmrChannel("X", 446000000, 0, 16, 1, 0);
	zsWriterBegin(&w, buf, sizeof(buf), 1, 0, "CC");
	CHECK(!zsWriterChannel(&w, &bad));
	zsChannel_t fm = { .rxHz = 146520000, .rxToneType = ZS_TONE_CTCSS, .rxTone = 300 };
	zsWriterBegin(&w, buf, sizeof(buf), 1, 0, "Tone");
	CHECK(!zsWriterChannel(&w, &fm));
	fm.rxToneType = ZS_TONE_DCS;
	fm.rxTone = 01000;
	CHECK(!zsWriterChannel(&w, &fm));
	zsChannel_t under = dmrChannel("X", 1000000, -2000000, 1, 1, 0);
	zsWriterBegin(&w, buf, sizeof(buf), 1, 0, "Tx");
	CHECK(!zsWriterChannel(&w, &under));

	// Too long for the buffer, and more channels than a record holds
	int n = sampleZone(buf, 100, 16);
	CHECK(n == 0);
	n = sampleZone(buf, sizeof(buf), ZS_CHANNELS_MAX + 1);
	CHECK(n == 0);
	// The longest records: 80 short channels fit, 80 channels with full names don't
	zsWriterBegin(&w, buf, sizeof(buf), 1, 0, "Max");
	for (int i = 0; i < ZS_CHANNELS_MAX; i++)
	{
		zsChannel_t s = dmrChannel("", 446000000, 0, 1, 1, 0);
		zsWriterChannel(&w, &s);
	}
	n = zsWriterEnd(&w);
	zsRecord_t r;
	CHECK(n > 0 && zsParse(buf, n, &r) && r.channels == ZS_CHANNELS_MAX);
	CHECK(zsFragmentCount(n) <= ZS_FRAGMENTS_MAX);
	zsWriterBegin(&w, buf, sizeof(buf), 1, 0, "Max");
	for (int i = 0; i < ZS_CHANNELS_MAX; i++)
	{
		zsChannel_t s = dmrChannel("ABCDEFGHIJKLMNOP", 446000000, 5000000, 1, 1, 0);
		zsWriterChannel(&w, &s);
	}
	CHECK(zsWriterEnd(&w) == 0);
	CHECK(zsFragmentCount(ZS_RECORD_MAX) == ZS_SEND_FRAGMENTS_MAX && zsFragmentCount(0) == 0 &&
		  zsFragmentCount(ZS_RECORD_MAX + 1) == 0);
}

static void test_malformed(void)
{
	uint8_t good[ZS_RECORD_MAX], buf[ZS_RECORD_MAX + 4];
	int n = sampleZone(good, sizeof(good), 4);
	zsRecord_t r;

	CHECK(n > 0 && parse(good, n, &r));
	// Every truncation, with the length field fixed to match: refused (the content ends early)
	int refused = 0;
	for (int len = 0; len < n; len++)
	{
		memcpy(buf, good, (size_t)n);
		if (len >= 3)
		{
			buf[1] = (uint8_t)(len >> 8);
			buf[2] = (uint8_t)len;
		}
		refused += !parse(buf, len, &r);
	}
	CHECK(refused == n);
	// A trailing octet, with and without the length field saying so
	memcpy(buf, good, (size_t)n);
	buf[n] = 0;
	CHECK(!parse(buf, n + 1, &r));
	buf[1] = (uint8_t)((n + 1) >> 8);
	buf[2] = (uint8_t)(n + 1);
	CHECK(!parse(buf, n + 1, &r));

#define MUTATED(at, value, expect) do { memcpy(buf, good, (size_t)n); buf[at] = (value); CHECK(parse(buf, n, &r) == (expect)); } while (0)
	MUTATED(0, 2, false);			// version
	MUTATED(3, 0, true);			// zone ID 0x00B2C3D4: still nonzero
	memcpy(buf, good, (size_t)n);
	memset(&buf[3], 0, 4);
	CHECK(!parse(buf, n, &r));		// zone ID 0
	MUTATED(7, ZS_FLAG_CHANNEL, false);	// one channel flagged, four in the record
	MUTATED(7, 0x02, false);		// a reserved flag
	MUTATED(8, 0, false);			// no channels
	MUTATED(8, 5, false);			// more channels than present
	MUTATED(8, 3, false);			// fewer: trailing octets
	MUTATED(9, 4, false);			// a contact more than present
	MUTATED(9, ZS_CONTACTS_MAX + 1, false);
	MUTATED(10, ZS_NAME_MAX + 1, false);	// zone name length
	MUTATED(11, 0x07, false);		// a control character in the name
	// The first contact: type 3, slot 3, ID 0
	int c0 = 11 + good[10];
	MUTATED(c0, 0x03, false);
	MUTATED(c0, 0x0C, false);
	MUTATED(c0, (uint8_t)(good[c0] | 0x10), false);	// reserved bits
	memcpy(buf, good, (size_t)n);
	memset(&buf[c0 + 1], 0, 3);
	CHECK(!parse(buf, n, &r));
	// The first channel (DMR simplex): its contact reference past the contacts, reserved DMR bits, tone bits on DMR
	CHECK(zsParse(good, n, &r));
	int ch0 = r.channelAt[0];
	MUTATED(ch0 + 6, 4, false);
	MUTATED(ch0 + 5, 0x40, false);
	MUTATED(ch0, good[ch0] | 0x08, false);
	// RX 0 Hz
	memcpy(buf, good, (size_t)n);
	memset(&buf[ch0 + 1], 0, 4);
	CHECK(!parse(buf, n, &r));
	// The FM channel (the fourth): a tone of type none, a CTCSS out of range
	CHECK(zsParse(good, n, &r));
	int ch3 = r.channelAt[3];
	MUTATED(ch3 + 5, 0x00, false);	// RX tone type none (value high octet cleared too)
	memcpy(buf, good, (size_t)n);
	buf[ch3 + 5] = 0x40;
	buf[ch3 + 6] = 10;			// CTCSS 1.0 Hz
	CHECK(!parse(buf, n, &r));
	CHECK(!zsParse(NULL, 20, &r) && !zsParse(good, ZS_RECORD_MAX + 1, &r));
#undef MUTATED
}

// A fragment as a sender with another chunk size builds it (another radio's limits, or an older build's)
static int foreignFragment(uint8_t transfer, const uint8_t *rec, int n, int chunk, int index, uint8_t *out)
{
	int at = index * chunk;
	int len = (n - at < chunk) ? n - at : chunk;

	out[0] = ZS_VERSION;
	out[1] = ZS_MSG_FRAGMENT;
	out[2] = transfer;
	out[3] = (uint8_t)((index << 4) | ((n + chunk - 1) / chunk));
	out[4] = (uint8_t)(at >> 8);
	out[5] = (uint8_t)at;
	memcpy(&out[ZS_MSG_HEADER], &rec[at], (size_t)len);
	return ZS_MSG_HEADER + len;
}

static void test_messages(void)
{
	uint8_t rec[ZS_RECORD_MAX], msg[ZS_MSG_MAX];
	int n = sampleZone(rec, sizeof(rec), 40);
	int count = zsFragmentCount(n);
	zsMessage_t m;

	CHECK(n > ZS_SEND_CHUNK * 2 && count == (n + ZS_SEND_CHUNK - 1) / ZS_SEND_CHUNK && count <= ZS_SEND_FRAGMENTS_MAX);
	printf("# 40 channels: %d octets, %d fragments\n", n, count);

	// Fragments, out of order, with a duplicate; the record comes out whole
	static zsAssembly_t a;
	zsAssemblyReset(&a);
	// The first twice, then the last, then the rest
	int order[ZS_SEND_FRAGMENTS_MAX + 1] = { 0, 0, count - 1 };
	for (int k = 3; k <= count; k++)
	{
		order[k] = k - 2;
	}
	zsAsmResult_t res = ZS_ASM_BAD;
	for (int k = 0; k <= count; k++)
	{
		int len = zsBuildFragment(9, rec, n, order[k], msg);
		CHECK(len > ZS_MSG_HEADER && len <= ZS_MSG_MAX && zsParseMessage(msg, len, &m) && m.type == ZS_MSG_FRAGMENT);
		res = zsAssemble(&a, 1234, &m, 1000);
		CHECK(res == ((k == 1) ? ZS_ASM_DUPLICATE : (k == count) ? ZS_ASM_DONE : ZS_ASM_TAKEN));
	}
	CHECK(res == ZS_ASM_DONE && a.length == n && memcmp(a.data, rec, (size_t)n) == 0);
	CHECK(zsBuildFragment(9, rec, n, count, msg) == 0 && zsBuildFragment(9, rec, 0, 0, msg) == 0);
	// The header alone (what the sender queues before the record's slice) matches the whole fragment
	for (int k = 0; k < count; k++)
	{
		uint8_t whole[ZS_MSG_MAX], head[ZS_MSG_HEADER];
		int offset = -1;
		int len = zsBuildFragment(9, rec, n, k, whole);
		int chunk = zsFragmentHeader(9, n, k, head, &offset);
		CHECK(chunk == len - ZS_MSG_HEADER && offset == k * ZS_SEND_CHUNK && memcmp(head, whole, ZS_MSG_HEADER) == 0 &&
		      memcmp(&rec[offset], &whole[ZS_MSG_HEADER], (size_t)chunk) == 0);
	}
	{
		uint8_t head[ZS_MSG_HEADER];
		int offset = 0;
		CHECK(zsFragmentHeader(9, n, count, head, &offset) == 0 && zsFragmentHeader(9, 0, 0, head, &offset) == 0);
	}

	// Another sender while one is in progress: busy; after the time-out it takes over
	int len = zsBuildFragment(3, rec, n, 0, msg);
	zsParseMessage(msg, len, &m);
	CHECK(zsAssemble(&a, 1234, &m, 2000) == ZS_ASM_TAKEN);
	CHECK(zsAssemble(&a, 5678, &m, 3000) == ZS_ASM_BUSY);
	CHECK(zsAssemble(&a, 5678, &m, 2000 + ZS_REASSEMBLY_MS + 1) == ZS_ASM_TAKEN && a.src == 5678);
	// The same sender starting a new transfer replaces its old one
	len = zsBuildFragment(4, rec, n, 1, msg);
	zsParseMessage(msg, len, &m);
	CHECK(zsAssemble(&a, 5678, &m, 40000) == ZS_ASM_TAKEN && a.transfer == 4 && a.have == 0x02);

	// What the first builds take, which every fragment we send must meet: messages of at most 328 octets, every chunk
	// but the last 322, offset index x 322, at most 4 fragments
	CHECK(ZS_SEND_CHUNK == 322 && zsFragmentCount(ZS_RECORD_MAX) <= 4);
	for (int k = 0; k < zsFragmentCount(ZS_RECORD_MAX); k++)
	{
		static const uint8_t full[ZS_RECORD_MAX];
		int fl = zsBuildFragment(1, full, ZS_RECORD_MAX, k, msg);
		CHECK(fl <= 328 && ((msg[4] << 8) | msg[5]) == k * 322 && ((k == 3) || (fl == 328)));
	}

	// Other senders' chunk sizes: the smallest we take (the most fragments) and the largest (a later build's), last first
	int sizes[] = { ZS_CHUNK_MIN, ZS_CHUNK_MAX };
	for (int i = 0; i < 2; i++)
	{
		int c = (n + sizes[i] - 1) / sizes[i];
		CHECK(c <= ZS_FRAGMENTS_MAX);
		zsAssemblyReset(&a);
		res = ZS_ASM_BAD;
		for (int k = c - 1; k >= 0; k--)
		{
			int fl = foreignFragment(6, rec, n, sizes[i], k, msg);
			CHECK(zsParseMessage(msg, fl, &m) && m.count == c && ((c == 1) || m.full == sizes[i]));
			res = zsAssemble(&a, 42, &m, 0);
			CHECK(res == ((k == 0) ? ZS_ASM_DONE : ZS_ASM_TAKEN));
		}
		CHECK(res == ZS_ASM_DONE && a.length == n && memcmp(a.data, rec, (size_t)n) == 0);
	}
	// Fragments of one transfer that disagree on the chunk size: the odd one is refused
	zsAssemblyReset(&a);
	len = foreignFragment(6, rec, n, ZS_SEND_CHUNK, 0, msg);
	CHECK(zsParseMessage(msg, len, &m) && zsAssemble(&a, 42, &m, 0) == ZS_ASM_TAKEN);
	len = foreignFragment(6, rec, n, 330, 1, msg);
	msg[3] = (uint8_t)(0x10 | ((n + ZS_SEND_CHUNK - 1) / ZS_SEND_CHUNK));	// the same count
	CHECK(zsParseMessage(msg, len, &m) && m.full == 330 && zsAssemble(&a, 42, &m, 0) == ZS_ASM_BAD && a.have == 0x01);

	// A one-fragment record
	uint8_t small[ZS_RECORD_MAX];
	int s = sampleZone(small, sizeof(small), 2);
	len = zsBuildFragment(5, small, s, 0, msg);
	zsAssemblyReset(&a);
	CHECK(zsParseMessage(msg, len, &m) && m.count == 1 && zsAssemble(&a, 1, &m, 0) == ZS_ASM_DONE && a.length == s);

	// Malformed fragments
	len = zsBuildFragment(9, rec, n, 0, msg);
	uint8_t bad[ZS_MSG_MAX + 1];
#define BADMSG(at, value) do { memcpy(bad, msg, (size_t)len); bad[at] = (value); CHECK(!zsParseMessage(bad, len, &m)); } while (0)
	BADMSG(0, 2);				// version
	BADMSG(1, 9);				// type
	BADMSG(3, 0x00);			// count 0
	BADMSG(3, ZS_FRAGMENTS_MAX + 1);	// more fragments than a transfer may have
	BADMSG(3, 0x22);			// index 2 of 2
	BADMSG(3, 0x13);			// index 1 at offset 0
	BADMSG(5, 1);				// offset not index * chunk
#undef BADMSG
	CHECK(!zsParseMessage(msg, ZS_MSG_HEADER, &m));		// no chunk
	CHECK(zsParseMessage(msg, len - 1, &m) && m.full == len - 1 - ZS_MSG_HEADER);	// a first fragment sets the size
	CHECK(!zsParseMessage(msg, ZS_MSG_HEADER + ZS_CHUNK_MIN - 1, &m));	// one too small isn't taken
	memset(bad, 0, sizeof(bad));
	memcpy(bad, msg, ZS_MSG_HEADER);
	CHECK(zsParseMessage(bad, ZS_MSG_MAX, &m) && m.full == ZS_CHUNK_MAX);	// the longest message we take
	CHECK(!zsParseMessage(bad, ZS_MSG_MAX + 1, &m));	// longer
	// The last fragment (here the fourth) may not reach past ZS_RECORD_MAX
	memcpy(bad, msg, ZS_MSG_HEADER);
	bad[3] = 0x34;
	bad[4] = (uint8_t)((3 * ZS_SEND_CHUNK) >> 8);
	bad[5] = (uint8_t)(3 * ZS_SEND_CHUNK);
	CHECK(zsParseMessage(bad, ZS_MSG_HEADER + (ZS_RECORD_MAX - 3 * ZS_SEND_CHUNK), &m) && m.full == ZS_SEND_CHUNK);
	CHECK(!zsParseMessage(bad, ZS_MSG_HEADER + (ZS_RECORD_MAX - 3 * ZS_SEND_CHUNK) + 1, &m));
	// A last fragment's offset must be a whole number of chunks of ZS_CHUNK_MIN to ZS_CHUNK_MAX, and it no longer
#define OFFSET(at) do { bad[4] = (uint8_t)((at) >> 8); bad[5] = (uint8_t)(at); } while (0)
	bad[3] = 0x23;
	OFFSET(401);					// not a multiple of 2
	CHECK(!zsParseMessage(bad, ZS_MSG_HEADER + 10, &m));
	OFFSET(400);					// chunks of 200
	CHECK(zsParseMessage(bad, ZS_MSG_HEADER + 200, &m) && m.full == 200 && !zsParseMessage(bad, ZS_MSG_HEADER + 201, &m));
	OFFSET(2 * (ZS_CHUNK_MIN - 1));	// chunks too small
	CHECK(!zsParseMessage(bad, ZS_MSG_HEADER + 10, &m));
	bad[3] = 0x12;
	OFFSET(ZS_CHUNK_MAX);			// chunks of the largest we take
	CHECK(zsParseMessage(bad, ZS_MSG_HEADER + 10, &m) && m.full == ZS_CHUNK_MAX);
	OFFSET(ZS_CHUNK_MAX + 1);		// too large: no first fragment could carry it
	CHECK(!zsParseMessage(bad, ZS_MSG_HEADER + 10, &m));
	bad[3] = 0x01;					// a lone fragment starts at 0
	OFFSET(4);
	CHECK(!zsParseMessage(bad, ZS_MSG_HEADER + 10, &m));
#undef OFFSET

	// Request and refusal
	len = zsBuildRequest(7, ZS_WANT_SITE_ZONE, msg);
	CHECK(len == 5 && zsParseMessage(msg, len, &m) && m.type == ZS_MSG_REQUEST && m.number == 7 && m.code == ZS_WANT_SITE_ZONE);
	CHECK(!zsParseMessage(msg, 4, &m) && !zsParseMessage(msg, 6, &m));
	len = zsBuildRefusal(7, ZS_REFUSE_STALE, msg);
	CHECK(len == 4 && zsParseMessage(msg, len, &m) && m.type == ZS_MSG_REFUSAL && m.code == ZS_REFUSE_STALE);
	CHECK(!zsParseMessage(msg, 5, &m) && !zsParseMessage(NULL, 4, &m));
	CHECK(zsAssemble(&a, 1, &m, 0) == ZS_ASM_BAD);
}

static void test_fuzz(void)
{
	uint8_t seed[ZS_RECORD_MAX], buf[ZS_RECORD_MAX + 64];
	int n = sampleZone(seed, sizeof(seed), 12);
	fuzz_t f;
	long iters = fuzz_iters(20000), parsed = 0, bad = 0;

	fuzz_init(&f, 0x2054A);
	for (long i = 0; i < iters; i++)
	{
		memcpy(buf, seed, (size_t)n);
		int len = fuzz_mutate(&f, buf, n, ZS_RECORD_MAX);
		// Keep the length field right half the time, so the mutations reach past the header
		if ((i & 1) && (len >= 3))
		{
			buf[1] = (uint8_t)(len >> 8);
			buf[2] = (uint8_t)len;
		}
		uint8_t *p = fuzz_exact(buf, len);
		zsRecord_t r;
		if (zsParse(p, len, &r))
		{
			parsed++;
			for (int k = 0; k < r.channels; k++)
			{
				zsChannel_t ch;
				bool ok = zsChannelAt(&r, k, &ch) && (strlen(ch.name) <= ZS_NAME_MAX) && (ch.contact <= r.contacts) &&
						(ch.rxHz != 0) && (ch.colorCode <= 15);
				bad += !ok;
			}
			for (int k = 0; k < r.contacts; k++)
			{
				zsContact_t c;
				bad += !(zsContactAt(&r, k, &c) && (c.id != 0) && (c.id <= 0xFFFFFF) && (strlen(c.name) <= ZS_NAME_MAX));
			}
		}
		free(p);

		// The same bytes as a message, and through reassembly
		len = (len > ZS_MSG_MAX) ? ZS_MSG_MAX : len;
		p = fuzz_exact(buf, len);
		zsMessage_t m;
		static zsAssembly_t a;
		if (zsParseMessage(p, len, &m) && (m.type == ZS_MSG_FRAGMENT))
		{
			bad += (m.offset + m.chunkLength) > ZS_RECORD_MAX;
			zsAssemble(&a, 1, &m, (uint32_t)i);
			bad += a.length > ZS_RECORD_MAX;
		}
		free(p);
	}
	CHECK(bad == 0 && parsed > 0);
	printf("# zone share fuzz: %ld records, %ld parsed\n", iters, parsed);
}

int main(void)
{
	test_roundtrip();
	test_writer_limits();
	test_malformed();
	test_messages();
	test_fuzz();
	printf("zoneShareTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
