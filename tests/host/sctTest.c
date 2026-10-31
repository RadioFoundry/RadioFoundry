// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SCT baseband driver (drivers/modem/sct): packet building and parsing against the datasheet's and the vendor
// scripts' byte sequences, both chips' boot and the voice set-up after it, and the layer 2 DMR engine against a fake
// chip (frames paced by the chip's answers, voice bursts taken in their slots, the end of a transmission, timeouts,
// received bursts)
#include <stdio.h>
#include <string.h>
#include "modem/sct/sct_pkt.h"
#include "modem/sct/sct_host.h"
#include "modem/sct/sct_dmr.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// --- Packets ---

static void test_build(void)
{
	sct_pkt_t p;

	// The vendor script's software version query: 84 A9 61 00 01 23 32 + pad
	sct_pkt_begin(&p, 0x23);
	sct_pkt_add(&p, SCT_F_Q_SW_VER, NULL, 0);
	static const uint8_t q[] = { 0x84, 0xA9, 0x61, 0x00, 0x01, 0x23, 0x32, 0x00 };
	CHECK(sct_pkt_end(&p, false) == sizeof(q));
	CHECK(memcmp(p.buf, q, sizeof(q)) == 0);

	// WORK_MODE TX (`18 02 00 00`)
	static const uint8_t wm[3] = { 2, 0, 0 };
	sct_pkt_begin(&p, SCT_TYPE_CONTROL);
	sct_pkt_add(&p, SCT_F_WORK_MODE, wm, 3);
	static const uint8_t w[] = { 0x84, 0xA9, 0x61, 0x00, 0x04, 0x00, 0x18, 0x02, 0x00, 0x00 };
	CHECK(sct_pkt_end(&p, false) == sizeof(w));
	CHECK(memcmp(p.buf, w, sizeof(w)) == 0);

	// A wrong length makes the packet bad
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	sct_pkt_add(&p, SCT_F_DMR_CC, wm, 2);
	CHECK(sct_pkt_end(&p, false) == 0);

	// A data frame: the vendor script's unconfirmed data header `43 06 0A ...`
	static const uint8_t hdr[10] = { 0x82, 0x44, 0, 0, 1, 0, 0, 1, 0x84, 0 };
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	sct_pkt_add_frame(&p, 0x06, hdr, 10);
	CHECK(sct_pkt_end(&p, false) == 6 + 13 + 1);
	CHECK(p.buf[4] == 13 && p.buf[6] == 0x43 && p.buf[7] == 0x06 && p.buf[8] == 10 && p.buf[19] == 0);

	// Parity: XOR of LEN through the marker; a changed byte fails the check
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	sct_pkt_add1(&p, SCT_F_DMR_CC, 7);
	uint16_t n = sct_pkt_end(&p, true);
	CHECK(n == 10 && p.buf[4] == 4 && p.buf[8] == SCT_PARITY_MARK);
	uint8_t x = 0;
	for (int i = 3; i <= 8; i++)
		x ^= p.buf[i];
	CHECK(p.buf[9] == x);
	sct_fields_t it;
	CHECK(sct_fields_begin(&it, p.buf, n, true));
	CHECK(it.left == 2);
	p.buf[7] ^= 1;
	CHECK(!sct_fields_begin(&it, p.buf, n, true));

	uint8_t bcd[4];
	sct_id_bcd(1234567, bcd);
	CHECK(bcd[0] == 0x01 && bcd[1] == 0x23 && bcd[2] == 0x45 && bcd[3] == 0x67);
	sct_id_bcd(16777215, bcd);
	CHECK(bcd[0] == 0x16 && bcd[3] == 0x15);
}

static uint16_t feed_all(sct_rx_t *rx, const uint8_t *b, uint16_t n, uint8_t *out)
{
	uint16_t got = 0;

	for (uint16_t i = 0; i < n; i++) {
		uint16_t len;

		if (sct_rx_feed(rx, b[i], &len)) {
			memcpy(out, rx->buf, len);
			got = len;
		}
	}
	return got;
}

static void test_parse(void)
{
	sct_rx_t rx;
	uint8_t pkt[SCT_PKT_MAX];
	sct_fields_t it;
	uint8_t id, n;
	const uint8_t *d;
	bool ack;

	memset(&rx, 0, sizeof(rx));
	// Noise, a stray sync byte, then the chip's version answer
	static const uint8_t ver[] = { 0x00, 0x13, 0x84, 0x84, 0xA9, 0x61, 0x00, 0x0C, 0x23, 0x32, 0x0A,
				       'V', '2', '.', '0', '1', '.', '0', '1', 'P', 'F' };
	uint16_t len = feed_all(&rx, ver, sizeof(ver), pkt);
	CHECK(len == 18);
	CHECK(rx.skipped == 3);
	CHECK(sct_fields_begin(&it, pkt, len, false));
	CHECK(sct_fields_next(&it, &id, &d, &n, &ack));
	CHECK(id == SCT_F_Q_SW_VER && n == 10 && !ack && memcmp(d, "V2.01.01PF", 10) == 0);
	CHECK(!sct_fields_next(&it, &id, &d, &n, &ack));

	// The boot answer
	static const uint8_t boot[] = { 0x84, 0xA9, 0x61, 0x00, 0x02, 0x20, 0x17, 0x01 };
	CHECK(sct_ack_code(boot, sizeof(boot)) == SCT_ACK_RESET_READY);

	// Answers to writes (no read bit): ID and status each
	static const uint8_t ans[] = { 0x84, 0xA9, 0x61, 0x00, 0x04, 0x05, 0x77, 0x00, 0x18, 0x04 };
	CHECK(sct_fields_begin(&it, ans, sizeof(ans), false));
	CHECK(sct_fields_next(&it, &id, &d, &n, &ack) && id == 0x77 && ack && d[0] == 0);
	CHECK(sct_fields_next(&it, &id, &d, &n, &ack) && id == 0x18 && ack && d[0] == SCT_ACK_UNKNOWN);
	CHECK(!sct_fields_next(&it, &id, &d, &n, &ack));

	// A report: CC, then a voice LC header frame (`30 77 0D 43 01 09 ...`)
	static const uint8_t rep[] = { 0x84, 0xA9, 0x61, 0x00, 0x0E, 0x30, 0x77, 0x0D, 0x43, 0x01, 0x09,
				       0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C, 0x00 };
	len = feed_all(&rx, rep, sizeof(rep), pkt);
	CHECK(len == 20);
	CHECK(sct_fields_begin(&it, pkt, len, false));
	CHECK(sct_fields_next(&it, &id, &d, &n, &ack) && id == 0x77 && !ack && n == 1 && d[0] == 0x0D);
	CHECK(sct_fields_next(&it, &id, &d, &n, &ack) && id == 0x43 && n == 11 && d[0] == 0x01 && d[1] == 9);
	CHECK(it.left == 0);

	// An unknown ID ends the walk
	static const uint8_t unk[] = { 0x84, 0xA9, 0x61, 0x00, 0x03, 0x30, 0x03, 0x01, 0x02 };
	CHECK(sct_fields_begin(&it, unk, sizeof(unk), false));
	CHECK(!sct_fields_next(&it, &id, &d, &n, &ack));

	// A length past the buffer is dropped, and the next packet still comes through
	static const uint8_t big[] = { 0x84, 0xA9, 0x61, 0x7F, 0xFF, 0x20 };
	len = feed_all(&rx, big, sizeof(big), pkt);
	CHECK(len == 0 && rx.overruns == 1);
	CHECK(feed_all(&rx, boot, sizeof(boot), pkt) == sizeof(boot));
}

// --- Boot, on a fake bus ---

static struct {
	uint8_t in[256];	// what the chip has waiting
	uint16_t in_len, in_pos;
	uint32_t now;
	uint16_t writes;
	uint16_t last_write_len;
	uint8_t last_write[8];
	bool greet;		// the boot ROM greets at reset
	int answer_block;	// ACK code after the boot block / start command, -1 none
	bool voice;		// answer the voice set-up's packets (every write but the boot block)
	int refuse_id;		// a field (or 0x40, a codec write) the chip refuses, -1 none
	bool mute_id;		// ... or doesn't answer at all
	bool processing;	// an ACK_MESSAGE "processing" before each answer
	uint8_t log[16][10];	// the voice set-up's packets as sent
	uint16_t nlog;
} bus;

static void chip_says(const uint8_t *b, uint16_t n)
{
	memcpy(&bus.in[bus.in_len], b, n);
	bus.in_len = (uint16_t)(bus.in_len + n);
}

static void chip_ack(uint8_t code)
{
	const uint8_t a[] = { 0x84, 0xA9, 0x61, 0x00, 0x02, 0x20, 0x17, code };

	chip_says(a, sizeof(a));
}

static bool b_write(const uint8_t *buf, uint16_t len, void *ctx)
{
	bus.writes++;
	bus.last_write_len = len;
	memcpy(bus.last_write, buf, len < 8 ? len : 8);
	if (bus.voice && len != SCT_BOOT_BLOCK_BYTES) {
		if (bus.nlog < 16)
			memcpy(bus.log[bus.nlog++], buf, len < 10 ? len : 10);
		// A write's answer: TYPE 0 echoes the field and its status, a codec write gets an ACK_MESSAGE
		bool codec = buf[5] == SCT_TYPE_CODEC;
		int id = codec ? SCT_TYPE_CODEC : buf[6];

		if (id == bus.refuse_id && bus.mute_id)
			return true;
		uint8_t st = id == bus.refuse_id ? SCT_ACK_UNKNOWN : SCT_ACK_OK;

		if (bus.processing)
			chip_ack(SCT_ACK_PROCESSING);
		const uint8_t a[] = { 0x84, 0xA9, 0x61, 0x00, 0x02, buf[5], codec ? (uint8_t)SCT_F_ACK : buf[6], st };

		chip_says(a, sizeof(a));
		return true;
	}
	if (bus.answer_block >= 0)
		chip_ack((uint8_t)bus.answer_block);
	return true;
}

static uint16_t b_read(uint8_t *buf, uint16_t max, void *ctx)
{
	uint16_t n = (uint16_t)(bus.in_len - bus.in_pos);

	if (n > max)
		n = max;
	memcpy(buf, &bus.in[bus.in_pos], n);
	bus.in_pos = (uint16_t)(bus.in_pos + n);
	if (bus.in_pos == bus.in_len)
		bus.in_pos = bus.in_len = 0;
	return n;
}

static void b_reset(void *ctx)
{
	bus.in_len = bus.in_pos = 0;
	if (bus.greet)
		chip_ack(SCT_ACK_NOT_LOADED);
}

static void b_delay(uint32_t ms)
{
	bus.now += ms;
}

static uint32_t b_now(void)
{
	return bus.now;
}

static void test_boot(void)
{
	static uint8_t block[SCT_BOOT_BLOCK_BYTES];
	sct_rx_t rx;
	sct_host_t h = { .write = b_write, .read = b_read, .reset = b_reset, .delay_ms = b_delay, .now_ms = b_now,
			 .boot = SCT_BOOT_BLOCK, .boot_block = block };

	memset(&rx, 0, sizeof(rx));
	memset(&bus, 0, sizeof(bus));
	bus.answer_block = SCT_ACK_RESET_READY;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_OK);
	CHECK(bus.writes == 1 && bus.last_write_len == SCT_BOOT_BLOCK_BYTES);

	// No application in the chip's flash
	bus.answer_block = SCT_ACK_OK;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_NO_APP);

	// Silent chip: the wait ends at the deadline
	bus.answer_block = -1;
	bus.now = 0;
	CHECK(sct_host_boot(&h, &rx, 500) == SCT_BOOT_SILENT);
	CHECK(bus.now >= 500 && bus.now < 510);

	h.boot_block = NULL;
	CHECK(sct_host_boot(&h, &rx, 500) == SCT_BOOT_NO_BLOCK);

	// Boot ROM chip: greeting, start command, ready
	h.boot = SCT_BOOT_ROM;
	bus.greet = true;
	bus.writes = 0;
	bus.answer_block = SCT_ACK_RESET_READY;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_OK);
	CHECK(bus.writes == 1 && bus.last_write_len == 6 && bus.last_write[0] == 0x74 && bus.last_write[1] == 0xA9);
}

static void b_reset_ready(void *ctx)
{
	bus.in_len = bus.in_pos = 0;
	chip_ack(SCT_ACK_RESET_READY);
}

static void test_boot_rom_self(void)
{
	sct_rx_t rx;
	sct_host_t h = { .write = b_write, .read = b_read, .reset = b_reset_ready, .delay_ms = b_delay,
			 .now_ms = b_now, .boot = SCT_BOOT_ROM };

	memset(&rx, 0, sizeof(rx));
	memset(&bus, 0, sizeof(bus));
	bus.answer_block = -1;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_OK);
	CHECK(bus.writes == 0);
}

// The voice set-up after the boot: CODEC_SELECT, VOCODER_SEL, the control fields, then the codec writes, each waiting
// for its answer
static void test_voice(void)
{
	static uint8_t block[SCT_BOOT_BLOCK_BYTES];
	static const sct_codec_write_t w[] = { { 1, 0x12, 0x08 }, { 0, 0x56, 0xA0 } };
	static const uint8_t spk[2] = { 0x05, 0x46 };
	static const uint8_t mic[2] = { 0x07, 0xD0 };
	static const sct_field_t f[] = { { SCT_F_DIGI_SPK_GAIN, 2, spk }, { SCT_F_DIGI_MIC_GAIN, 2, mic } };
	sct_voice_t v = { .codec = 1, .vocoder = 4, .fields = f, .field_count = 2,
			  .codec_writes = w, .codec_count = 2 };
	sct_rx_t rx;
	sct_host_t h = { .write = b_write, .read = b_read, .reset = b_reset, .delay_ms = b_delay, .now_ms = b_now,
			 .boot = SCT_BOOT_BLOCK, .boot_block = block, .voice = &v };

	memset(&rx, 0, sizeof(rx));
	memset(&bus, 0, sizeof(bus));
	bus.answer_block = SCT_ACK_RESET_READY;
	bus.voice = true;
	bus.refuse_id = -1;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_OK);
	CHECK(bus.nlog == 6);
	static const uint8_t cs[] = { 0x84, 0xA9, 0x61, 0x00, 0x02, 0x00, 0x2D, 0x01 };
	static const uint8_t vs[] = { 0x84, 0xA9, 0x61, 0x00, 0x02, 0x00, 0x10, 0x04 };
	static const uint8_t f1[] = { 0x84, 0xA9, 0x61, 0x00, 0x03, 0x00, 0x69, 0x05, 0x46, 0x00 };
	static const uint8_t f2[] = { 0x84, 0xA9, 0x61, 0x00, 0x03, 0x00, 0x68, 0x07, 0xD0, 0x00 };
	static const uint8_t c1[] = { 0x84, 0xA9, 0x61, 0x00, 0x03, 0x40, 0x01, 0x12, 0x08, 0x00 };
	static const uint8_t c2[] = { 0x84, 0xA9, 0x61, 0x00, 0x03, 0x40, 0x00, 0x56, 0xA0, 0x00 };
	CHECK(memcmp(bus.log[0], cs, sizeof(cs)) == 0);
	CHECK(memcmp(bus.log[1], vs, sizeof(vs)) == 0);
	CHECK(memcmp(bus.log[2], f1, sizeof(f1)) == 0);
	CHECK(memcmp(bus.log[3], f2, sizeof(f2)) == 0);
	CHECK(memcmp(bus.log[4], c1, sizeof(c1)) == 0);
	CHECK(memcmp(bus.log[5], c2, sizeof(c2)) == 0);

	// "Processing" before the answer is waited through
	bus.nlog = 0;
	bus.processing = true;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_OK && bus.nlog == 6);
	bus.processing = false;

	// A refused field ends the set-up there; one unanswered waits its 200 ms
	bus.nlog = 0;
	bus.refuse_id = SCT_F_DIGI_SPK_GAIN;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_NO_VOICE && bus.nlog == 3);
	bus.nlog = 0;
	bus.refuse_id = SCT_F_DIGI_MIC_GAIN;
	bus.mute_id = true;
	bus.now = 0;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_NO_VOICE && bus.nlog == 4);
	CHECK(bus.now >= 200 && bus.now < 210);
	bus.mute_id = false;

	// A field of the wrong length is not sent and fails the set-up
	static const sct_field_t bad[] = { { SCT_F_DIGI_SPK_GAIN, 1, spk } };
	v.fields = bad;
	v.field_count = 1;
	bus.nlog = 0;
	bus.refuse_id = -1;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_NO_VOICE && bus.nlog == 2);
	v.fields = f;
	v.field_count = 2;

	// A refused vocoder ends the set-up there
	bus.nlog = 0;
	bus.refuse_id = SCT_F_VOCODER_SEL;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_NO_VOICE);
	CHECK(bus.nlog == 2);

	// An unanswered codec write: the wait ends after its 200 ms
	bus.nlog = 0;
	bus.refuse_id = SCT_TYPE_CODEC;
	bus.mute_id = true;
	bus.now = 0;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_NO_VOICE);
	CHECK(bus.nlog == 5 && bus.now >= 200 && bus.now < 210);

	// Zero selections and no writes: nothing but the boot
	sct_voice_t none = { 0 };
	h.voice = &none;
	bus.nlog = 0;
	bus.refuse_id = -1;
	bus.mute_id = false;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_OK && bus.nlog == 0);

	// A failed boot sends no set-up
	h.voice = &v;
	bus.answer_block = SCT_ACK_OK;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_NO_APP && bus.nlog == 0);
}

// --- The DMR engine, on a fake chip ---

#define SENT_MAX 64
static struct {
	uint8_t pkt[SCT_PKT_MAX];
	uint16_t len;
} sent[SENT_MAX];
static int nsent;

static bool d_send(const uint8_t *buf, uint16_t len, void *ctx)
{
	if (nsent < SENT_MAX) {
		memcpy(sent[nsent].pkt, buf, len);
		sent[nsent].len = len;
	}
	nsent++;
	return true;
}

#define EV_MAX 64
static dmr_event_t evs[EV_MAX];
static dmr_burst_t ev_bursts[EV_MAX];
static int nev;

static void on_event(const dmr_event_t *ev, void *ctx)
{
	if (nev < EV_MAX) {
		evs[nev] = *ev;
		if (ev->burst != NULL)
			ev_bursts[nev] = *ev->burst;
	}
	nev++;
}

static int count_ev(uint8_t kind)
{
	int c = 0;

	for (int i = 0; i < nev && i < EV_MAX; i++)
		c += evs[i].kind == kind;
	return c;
}

// The value of field id in sent packet i, NULL if absent
static const uint8_t *sent_field(int i, uint8_t want, uint8_t *len)
{
	sct_fields_t it;
	uint8_t id, n;
	const uint8_t *d;
	bool ack;

	if (i >= nsent || !sct_fields_begin(&it, sent[i].pkt, sent[i].len, false))
		return NULL;
	it.type |= SCT_TYPE_READ;	// what we send has full fields, never write answers
	while (sct_fields_next(&it, &id, &d, &n, &ack))
		if (id == want) {
			if (len != NULL)
				*len = n;
			return d;
		}
	return NULL;
}

static int frames_sent(void)
{
	int c = 0;

	for (int i = 0; i < nsent && i < SENT_MAX; i++)
		c += sent_field(i, SCT_F_DATA_FRAME, NULL) != NULL;
	return c;
}

static void chip_packet(const uint8_t *body, uint8_t n, uint8_t type, uint32_t now)
{
	uint8_t p[SCT_PKT_MAX] = { 0x84, 0xA9, 0x61, 0x00, n, type };

	memcpy(&p[6], body, n);
	sct_dmr_packet(p, (uint16_t)(6 + n), now);
}

static void frame_ok(uint32_t now)
{
	static const uint8_t a[] = { 0x43, 0x00 };

	chip_packet(a, 2, SCT_TYPE_DMR, now);
}

static void far_done(uint32_t now)
{
	static const uint8_t a[] = { 0x17, SCT_ACK_FAR_DONE };

	chip_packet(a, 2, 0x20, now);
}

static int lock_depth, lock_max;

static void d_lock(void *ctx)
{
	if (++lock_depth > lock_max)
		lock_max = lock_depth;
}

static void d_unlock(void *ctx)
{
	lock_depth--;
}

static bool booted = true;

static bool d_ready(void *ctx)
{
	return booted;
}

static const sct_dmr_port_t port = { .send = d_send, .call_option = { 0x42, 0x00, 0x31, 0x07, 0x00 },
				     .ready = d_ready, .lock = d_lock, .unlock = d_unlock };
static const hal_dmr_modem_t *m = &sct_dmr_modem;

static void start(void)
{
	dmr_config_t cfg = { .own_id = 1234567, .cc = 2, .access = DMR_ACCESS_IMPOLITE };

	sct_dmr_init(&port);
	nsent = nev = 0;
	CHECK(m->start(&cfg, on_event, NULL));
}

static dmr_tx_burst_t burst(uint8_t kind, uint8_t index, uint8_t len)
{
	dmr_tx_burst_t b;

	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.voice_index = index;
	b.len = len;
	for (int i = 0; i < len; i++)
		b.payload[i] = (uint8_t)(0x10 + i);
	return b;
}

static void test_dmr_start(void)
{
	uint8_t n;
	const uint8_t *d;

	start();
	CHECK(nsent == 2);
	d = sent_field(0, SCT_F_PROCESS_MODE, NULL);
	CHECK(d != NULL && d[0] == 0x02);
	d = sent_field(0, SCT_F_DMR_CC, NULL);
	CHECK(d != NULL && d[0] == 2);
	d = sent_field(0, SCT_F_DMR_OWN_ID, &n);
	CHECK(d != NULL && n == 4 && d[0] == 0x01 && d[3] == 0x67);
	// Byte 0's traffic mode is the engine's (1, whatever the board gave); the rest is the board's
	d = sent_field(0, SCT_F_DMR_CALL_OPTION, &n);
	CHECK(d != NULL && n == 5 && d[0] == 0x41 && d[1] == 0x00 && d[2] == 0x31 && d[3] == 0x07 && d[4] == 0x00);
	d = sent_field(0, SCT_F_REPORT_FIELD, NULL);
	CHECK(d != NULL && d[0] == 0xE1);
	CHECK(sent_field(0, SCT_F_DMR_OFFSET, NULL) == NULL && sent_field(0, SCT_F_MODOUT_CONFIG, NULL) == NULL);
	CHECK(sent[0].pkt[5] == SCT_TYPE_DMR);
	d = sent_field(1, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 1);
	d = sent_field(1, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 3);

	m->rx_lock_slot(2);
	d = sent_field(nsent - 1, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 2);
}

static void test_dmr_voice_tx(void)
{
	const uint8_t *d;
	uint8_t n;
	uint32_t t = 1000;

	start();
	CHECK(m->tx_begin(DMR_TIMING_OWN, 2, DMR_ACCESS_IMPOLITE));
	CHECK(m->is_transmitting());
	d = sent_field(nsent - 1, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 2);
	d = sent_field(nsent - 1, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 0x82);

	sct_dmr_poll(t);
	dmr_tx_burst_t b = burst(DMR_BURST_VOICE_LC_HEADER, 0, 12);
	CHECK(m->tx_queue(&b));
	for (int i = 0; i < 6; i++) {
		b = burst(DMR_BURST_VOICE, (uint8_t)i, 27);
		CHECK(m->tx_queue(&b));
	}
	CHECK(m->tx_queue_space() == 8 - 6);	// the header went at once
	CHECK(frames_sent() == 1);
	d = sent_field(nsent - 1, SCT_F_DATA_FRAME, &n);
	CHECK(d != NULL && d[0] == DMR_BURST_VOICE_LC_HEADER && d[1] == 9 && n == 11 && d[2] == 0x10);

	// Nothing more until the chip takes it
	sct_dmr_poll(t + 20);
	CHECK(frames_sent() == 1);
	frame_ok(t + 20);
	CHECK(count_ev(DMR_EV_TX_SLOT_STARTED) == 1);

	// The first voice burst carries the LC for the embedded signaling, a slot after the header
	sct_dmr_poll(t + 59);
	CHECK(frames_sent() == 1);
	sct_dmr_poll(t + 60);
	CHECK(frames_sent() == 2);
	d = sent_field(nsent - 1, SCT_F_DATA_FRAME, &n);
	CHECK(d != NULL && d[0] == SCT_PROP_VOICE && d[1] == 9 && d[2] == 0x10);
	frame_ok(t + 70);
	CHECK(count_ev(DMR_EV_TX_SLOT_STARTED) == 2);

	// The other five are the chip's: taken one per slot, no frames
	t += 60;
	for (int i = 1; i <= 5; i++)
		sct_dmr_poll(t + 60u * (uint32_t)i);
	CHECK(frames_sent() == 2);
	CHECK(count_ev(DMR_EV_TX_SLOT_STARTED) == 7);
	CHECK(m->tx_queue_space() == 8);

	// A new embedded LC goes at the next burst A
	static const uint8_t lc2[9] = { 0, 0, 0, 0, 0, 9, 0, 0, 7 };
	m->set_embedded_lc(lc2);
	t += 360;
	b = burst(DMR_BURST_VOICE, 0, 27);
	CHECK(m->tx_queue(&b));
	sct_dmr_poll(t);
	CHECK(frames_sent() == 3);
	d = sent_field(nsent - 1, SCT_F_DATA_FRAME, NULL);
	CHECK(d != NULL && d[0] == SCT_PROP_VOICE && d[10] == 7);
	frame_ok(t);

	// Terminator, end: TX_DONE when the chip says the far end has it
	t += 60;
	b = burst(DMR_BURST_TERMINATOR_LC, 0, 12);
	CHECK(m->tx_queue(&b));
	m->tx_end();
	sct_dmr_poll(t);
	CHECK(frames_sent() == 4);
	d = sent_field(nsent - 1, SCT_F_DATA_FRAME, NULL);
	CHECK(d != NULL && d[0] == DMR_BURST_TERMINATOR_LC);
	frame_ok(t + 5);
	CHECK(count_ev(DMR_EV_TX_DONE) == 0);
	CHECK(!m->tx_queue(&b));	// draining: nothing more
	far_done(t + 300);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);
	CHECK(!m->is_transmitting());
	d = sent_field(nsent - 1, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 1);
	sct_dmr_stats_t st;
	sct_dmr_get_stats(&st);
	CHECK(st.tx_frames == 4 && st.tx_acks == 4 && st.tx_paced == 5 && st.tx_timeouts == 0);
}

static void test_dmr_data_tx(void)
{
	const uint8_t *d;
	uint8_t n;
	uint32_t t = 5000;

	start();
	sct_dmr_poll(t);
	CHECK(m->tx_begin(DMR_TIMING_ASAP, 1, DMR_ACCESS_IMPOLITE));
	// Payloads come with their CRC (12 bytes); the chip gets the information bits only
	dmr_tx_burst_t b = burst(DMR_BURST_DATA_HEADER, 0, 12);
	b.host_crc = true;
	CHECK(m->tx_queue(&b));
	b = burst(DMR_BURST_RATE_34_DATA, 0, 18);
	CHECK(m->tx_queue(&b));
	b = burst(DMR_BURST_RATE_1_DATA, 0, 24);
	CHECK(m->tx_queue(&b));
	b = burst(DMR_BURST_USBD, 0, 12);	// no frame type for it: dropped
	CHECK(m->tx_queue(&b));
	b = burst(DMR_BURST_CSBK, 0, 8);	// short: dropped
	CHECK(m->tx_queue(&b));
	m->tx_end();
	d = sent_field(nsent - 1, SCT_F_DATA_FRAME, &n);
	CHECK(d != NULL && d[0] == DMR_BURST_DATA_HEADER && d[1] == 10 && n == 12);

	// Unanswered: dropped after the timeout, the next goes
	sct_dmr_poll(t + SCT_DMR_ACK_MS - 1);
	CHECK(frames_sent() == 1);
	sct_dmr_poll(t + SCT_DMR_ACK_MS);
	CHECK(frames_sent() == 2);
	d = sent_field(nsent - 1, SCT_F_DATA_FRAME, &n);
	CHECK(d != NULL && d[0] == DMR_BURST_RATE_34_DATA && d[1] == 18);

	// A refusal frees the slot too
	t += SCT_DMR_ACK_MS;
	static const uint8_t nak[] = { 0x43, SCT_ACK_LENGTH };
	chip_packet(nak, 2, SCT_TYPE_DMR, t + 10);
	sct_dmr_poll(t + 10);
	CHECK(frames_sent() == 3);
	d = sent_field(nsent - 1, SCT_F_DATA_FRAME, &n);
	CHECK(d != NULL && d[0] == DMR_BURST_RATE_1_DATA && d[1] == 24);
	frame_ok(t + 20);
	sct_dmr_poll(t + 100);
	CHECK(frames_sent() == 3);

	// No far-end answer: TX_DONE anyway after the wait
	CHECK(count_ev(DMR_EV_TX_DONE) == 0);
	sct_dmr_poll(t + 100 + SCT_DMR_DONE_MS);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);

	sct_dmr_stats_t st;
	sct_dmr_get_stats(&st);
	CHECK(st.tx_timeouts == 1 && st.naks == 1 && st.last_nak == SCT_ACK_LENGTH);

	// tx_abort: back to receive, no TX_DONE
	CHECK(m->tx_begin(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	b = burst(DMR_BURST_CSBK, 0, 12);
	CHECK(m->tx_queue(&b));
	int before = nsent;
	m->tx_abort();
	CHECK(!m->is_transmitting());
	CHECK(nsent == before + 1 && sent_field(nsent - 1, SCT_F_WORK_MODE, NULL)[0] == 1);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);
}

// A board's options: its call option and report bits, more start fields, and a codec write each way around a
// transmission (the values of a radio whose stock firmware sends them)
static const uint8_t t_modout[5] = { 0x00, 0x10, 0x00, 0x00, 0x00 };
static const uint8_t t_offset[8] = { 0x00, 0x82, 0x02, 0xD0, 0x02, 0xD0, 0x02, 0xD0 };
static const uint8_t t_sq[12] = { 0x00, 0x02, 0x00, 0x3C, 0x17, 0x70, 0x15, 0x7C, 0x17, 0x70, 0x15, 0x7C };
static const uint8_t t_demod[2] = { 0x00, 0x00 };
static const sct_field_t t_fields[] = {
	{ SCT_F_MODOUT_CONFIG, 5, t_modout },
	{ SCT_F_DMR_OFFSET, 8, t_offset },
	{ SCT_F_SQ_LEVEL, 12, t_sq },
	{ SCT_F_DEMOD_GAIN, 2, t_demod },
};
static const sct_codec_write_t t_mute = { 1, 0x10, 0x40 };
static const sct_codec_write_t t_unmute = { 1, 0x10, 0x08 };
static const sct_dmr_port_t port_opts = {
	.send = d_send, .call_option = { 0x41, 0x40, 0x42, 0x47, 0x08 }, .report_field = 0xFF,
	.start_fields = t_fields, .start_count = 4, .codec_tx = &t_mute, .codec_rx = &t_unmute,
	.ready = d_ready, .lock = d_lock, .unlock = d_unlock,
};

static bool is_codec(int i, const sct_codec_write_t *w)
{
	static const uint8_t hdr[6] = { 0x84, 0xA9, 0x61, 0x00, 0x03, SCT_TYPE_CODEC };

	return i >= 0 && i < nsent && sent[i].len == SCT_CODEC_PKT_BYTES && memcmp(sent[i].pkt, hdr, 6) == 0 &&
	       sent[i].pkt[6] == w->page && sent[i].pkt[7] == w->reg && sent[i].pkt[8] == w->value &&
	       sent[i].pkt[9] == 0;
}

static int codec_count(const sct_codec_write_t *w)
{
	int c = 0;

	for (int i = 0; i < nsent && i < SENT_MAX; i++)
		c += is_codec(i, w);
	return c;
}

static void test_dmr_options(void)
{
	dmr_config_t cfg = { .own_id = 1234567, .cc = 1, .access = DMR_ACCESS_IMPOLITE };
	const uint8_t *d;
	uint8_t n;
	uint32_t t = 2000;

	sct_dmr_init(&port_opts);
	nsent = nev = 0;
	CHECK(m->start(&cfg, on_event, NULL));
	CHECK(nsent == 2);
	d = sent_field(0, SCT_F_DMR_CALL_OPTION, &n);
	CHECK(d != NULL && n == 5 && d[0] == 0x41 && d[1] == 0x40 && d[2] == 0x42 && d[3] == 0x47 && d[4] == 0x08);
	d = sent_field(0, SCT_F_REPORT_FIELD, NULL);
	CHECK(d != NULL && d[0] == 0xFF);
	d = sent_field(0, SCT_F_MODOUT_CONFIG, &n);
	CHECK(d != NULL && n == 5 && d[0] == 0x00 && d[1] == 0x10);
	d = sent_field(0, SCT_F_DMR_OFFSET, &n);
	CHECK(d != NULL && n == 8 && memcmp(d, t_offset, 8) == 0);
	d = sent_field(0, SCT_F_SQ_LEVEL, &n);
	CHECK(d != NULL && n == 12 && memcmp(d, t_sq, 12) == 0);
	d = sent_field(0, SCT_F_DEMOD_GAIN, &n);
	CHECK(d != NULL && n == 2 && d[0] == 0 && d[1] == 0);
	CHECK(sent[0].pkt[5] == SCT_TYPE_DMR && sent_field(0, SCT_F_CARRIER_READY, NULL) == NULL);
	CHECK(codec_count(&t_mute) == 0 && codec_count(&t_unmute) == 0);

	// The chip's answer to the start: every field's status, all walked
	static const uint8_t a_start[] = { 0x1A, 0, 0x1D, 0, 0x79, 0, 0x77, 0, 0x7E, 0, 0x41, 0, 0x7A, 0, 0x6C, 0,
					   0x1C, 0 };
	sct_dmr_stats_t st0;
	chip_packet(a_start, sizeof(a_start), SCT_TYPE_DMR, t - 10);
	sct_dmr_get_stats(&st0);
	CHECK(st0.rx_bad == 0 && st0.naks == 0);

	// Transmit: the mute goes before WORK_MODE transmit
	sct_dmr_poll(t);
	CHECK(m->tx_begin(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	CHECK(nsent == 4 && is_codec(2, &t_mute));
	d = sent_field(3, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 2);
	dmr_tx_burst_t b = burst(DMR_BURST_CSBK, 0, 12);
	CHECK(m->tx_queue(&b));
	CHECK(frames_sent() == 1);

	// The codec's answers (TYPE 0x40, an ACK_MESSAGE, OK or not) are no frame's answer and no NAK
	static const uint8_t c_ok[] = { 0x17, SCT_ACK_OK };
	static const uint8_t c_err[] = { 0x17, SCT_ACK_UNKNOWN };
	chip_packet(c_ok, 2, SCT_TYPE_CODEC, t + 5);
	chip_packet(c_err, 2, SCT_TYPE_CODEC, t + 6);
	sct_dmr_stats_t st;
	sct_dmr_get_stats(&st);
	CHECK(st.sending && st.tx_acks == 0 && st.naks == 0 && st.tx_timeouts == 0 && st.rx_bad == 0);
	CHECK(count_ev(DMR_EV_TX_SLOT_STARTED) == 0);
	frame_ok(t + 10);
	CHECK(count_ev(DMR_EV_TX_SLOT_STARTED) == 1);

	// Back to receive: WORK_MODE receive, then the undo
	m->tx_end();
	far_done(t + 100);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);
	d = sent_field(nsent - 2, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 1 && is_codec(nsent - 1, &t_unmute));
	CHECK(codec_count(&t_mute) == 1 && codec_count(&t_unmute) == 1);

	// tx_abort and stop undo it too, once
	CHECK(m->tx_begin(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	m->tx_abort();
	CHECK(is_codec(nsent - 1, &t_unmute) && codec_count(&t_unmute) == 2);
	m->tx_abort();
	CHECK(codec_count(&t_unmute) == 2);
	CHECK(m->tx_begin(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	m->stop();
	CHECK(is_codec(nsent - 1, &t_unmute) && codec_count(&t_unmute) == 3);
	m->stop();
	CHECK(codec_count(&t_unmute) == 3 && codec_count(&t_mute) == 3);

	// REPORT_FIELD 0xFF: called and own ID reports with a frame are walked, the frame reported
	CHECK(m->start(&cfg, on_event, NULL));
	nev = 0;
	uint8_t r[32] = { 0x7D, 0x00, 0x00, 0x00, 0x09, 0x7E, 0x01, 0x23, 0x45, 0x67,
			  0x43, DMR_BURST_CSBK, 10, 0xBD, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x03 };
	chip_packet(r, 23, 0x30, t + 500);
	CHECK(nev == 1 && ev_bursts[0].kind == DMR_BURST_CSBK);
	sct_dmr_get_stats(&st);
	CHECK(st.rx_bad == 0);

	// A start field of the wrong length fails start
	static const sct_field_t bad_field = { SCT_F_DMR_OFFSET, 4, t_offset };
	sct_dmr_port_t bad = port_opts;
	bad.start_fields = &bad_field;
	bad.start_count = 1;
	sct_dmr_init(&bad);
	nsent = 0;
	CHECK(!m->start(&cfg, on_event, NULL) && nsent == 0);
}

static uint16_t ref_crc(const uint8_t *b, int n)
{
	// Bit by bit, as TS 102 361-1 B.3.7 writes it: shift register, polynomial x^16 + x^12 + x^5 + 1, inverted
	uint16_t r = 0;

	for (int i = 0; i < n * 8; i++) {
		int bit = (b[i / 8] >> (7 - i % 8)) & 1;
		int top = (r >> 15) & 1;

		r = (uint16_t)(r << 1);
		if (top ^ bit)
			r ^= 0x1021;
	}
	return (uint16_t)~r;
}

static void test_dmr_rx(void)
{
	start();
	// TDMA direct sync on TS2, CC 2, then a CSBK
	uint8_t r[32] = { SCT_F_DMR_SLOT_FOUND, 0x02, SCT_F_DMR_CC, 0x02, SCT_F_NEW_EVENT, SCT_EV_DATA_SYNC,
			  0x43, DMR_BURST_CSBK, 10, 0xBD, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x03 };
	chip_packet(r, 19, 0x30, 100);
	CHECK(nev == 1 && evs[0].kind == DMR_EV_BURST_RX);
	const dmr_burst_t *b = &ev_bursts[0];
	CHECK(b->kind == DMR_BURST_CSBK && b->len == 12 && b->crc_ok && b->cc == 2 && b->ts == 2);
	CHECK(b->sync == DMR_SYNC_TS2 && evs[0].arg != 0);
	uint16_t crc = (uint16_t)(ref_crc(b->payload, 10) ^ 0xA5A5);
	CHECK(b->payload[10] == (crc >> 8) && b->payload[11] == (crc & 0xFF));

	// An FEC error before a rate 1/2 block clears crc_ok
	uint8_t r2[20] = { SCT_F_NEW_EVENT, SCT_EV_DATA_SYNC_FEC, 0x43, DMR_BURST_RATE_12_DATA, 12 };
	chip_packet(r2, 17, 0x30, 160);
	CHECK(nev == 2 && ev_bursts[1].kind == DMR_BURST_RATE_12_DATA && ev_bursts[1].len == 12);
	CHECK(!ev_bursts[1].crc_ok);
	CHECK(evs[1].arg != evs[0].arg);

	// A superframe's LC (late entry) reaches the stack as a voice LC header with the LC
	uint8_t r3[16] = { SCT_F_DMR_SLOT_FOUND, 0xC4, 0x43, SCT_PROP_VOICE, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	chip_packet(r3, 14, 0x30, 400);
	CHECK(nev == 3);
	b = &ev_bursts[2];
	CHECK(b->kind == DMR_BURST_VOICE_LC_HEADER && b->lc_valid && b->crc_ok && b->lc[5] == 0x09 && b->len == 9);
	CHECK(b->sync == DMR_SYNC_BS && b->ts == 2);

	// The chip plays the call: a voice burst per slot from B on, no payload
	sct_dmr_poll(459);
	CHECK(nev == 3);
	sct_dmr_poll(460);
	CHECK(nev == 4 && evs[3].kind == DMR_EV_BURST_RX && ev_bursts[3].kind == DMR_BURST_VOICE);
	CHECK(ev_bursts[3].voice_index == 1 && ev_bursts[3].len == 27 && ev_bursts[3].ts == 2 && ev_bursts[3].crc_ok);
	sct_dmr_poll(520);
	CHECK(nev == 5 && ev_bursts[4].voice_index == 2 && evs[4].arg != evs[3].arg);

	// Sync lost ends it
	uint8_t r4[2] = { SCT_F_NEW_EVENT, SCT_EV_SYNC_LOST };
	chip_packet(r4, 2, 0x30, 800);
	CHECK(nev == 6 && evs[5].kind == DMR_EV_SYNC_LOST);
	sct_dmr_poll(900);
	CHECK(nev == 6);

	// A header starts the bursts at A; a stalled poll gives at most a few, then the terminator stops them
	uint8_t r5[16] = { 0x43, DMR_BURST_VOICE_LC_HEADER, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	chip_packet(r5, 12, 0x30, 1000);
	CHECK(nev == 7 && ev_bursts[6].kind == DMR_BURST_VOICE_LC_HEADER && ev_bursts[6].lc_valid);
	sct_dmr_poll(1060);
	CHECK(nev == 8 && ev_bursts[7].voice_index == 0);
	sct_dmr_poll(3000);
	CHECK(nev == 12);
	sct_dmr_poll(3059);
	CHECK(nev == 12);
	uint8_t r6[16] = { 0x43, DMR_BURST_TERMINATOR_LC, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	chip_packet(r6, 12, 0x30, 3060);
	CHECK(nev == 13 && ev_bursts[12].kind == DMR_BURST_TERMINATOR_LC);
	sct_dmr_poll(3200);
	CHECK(nev == 13);

	// Every header reported (DMR_CALL_OPTION byte 3): a repeat starts the bursts at A again, a superframe's LC
	// doesn't, and a second terminator after the end changes nothing
	chip_packet(r5, 12, 0x30, 4000);
	sct_dmr_poll(4060);
	CHECK(nev == 15 && ev_bursts[14].voice_index == 0);
	chip_packet(r5, 12, 0x30, 4070);
	CHECK(nev == 16 && ev_bursts[15].kind == DMR_BURST_VOICE_LC_HEADER);
	sct_dmr_poll(4129);
	CHECK(nev == 16);
	sct_dmr_poll(4130);
	CHECK(nev == 17 && ev_bursts[16].voice_index == 0);
	chip_packet(r3, 14, 0x30, 4140);
	sct_dmr_poll(4190);
	CHECK(nev == 19 && ev_bursts[17].kind == DMR_BURST_VOICE_LC_HEADER && ev_bursts[18].voice_index == 1);
	chip_packet(r6, 12, 0x30, 4200);
	chip_packet(r6, 12, 0x30, 4260);
	sct_dmr_poll(4400);
	CHECK(nev == 21 && ev_bursts[20].kind == DMR_BURST_TERMINATOR_LC);
	nev = 6;

	// Nothing reported while transmitting; a garbled packet is counted
	CHECK(m->tx_begin(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	chip_packet(r, 19, 0x30, 900);
	CHECK(nev == 6);
	uint8_t junk[4] = { 0x03, 0x00, 0x00, 0x00 };
	chip_packet(junk, 4, 0x30, 900);
	sct_dmr_stats_t st;
	sct_dmr_get_stats(&st);
	CHECK(st.rx_bad == 1 && st.rx_frames == 10 && st.rx_fec == 1 && st.rx_voice == 10);
}

int main(void)
{
	test_build();
	test_parse();
	test_boot();
	test_boot_rom_self();
	test_voice();
	test_dmr_start();
	test_dmr_voice_tx();
	test_dmr_data_tx();
	test_dmr_rx();
	test_dmr_options();
	CHECK(lock_depth == 0 && lock_max == 1);
	// Not booted: start fails and sends nothing
	booted = false;
	dmr_config_t cfg = { .own_id = 1, .cc = 1 };
	sct_dmr_init(&port);
	nsent = 0;
	CHECK(!m->start(&cfg, on_event, NULL) && nsent == 0);
	printf("sctTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
