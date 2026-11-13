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
	uint8_t log[16][12];	// the voice set-up's packets as sent
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
			memcpy(bus.log[bus.nlog++], buf, len < sizeof(bus.log[0]) ? len : sizeof(bus.log[0]));
		// A write's answer: TYPE 0 echoes the field and its status, a codec write gets an ACK_MESSAGE
		bool codec = buf[5] == SCT_TYPE_CODEC;
		int id = codec ? SCT_TYPE_CODEC : buf[6];

		if (id == bus.refuse_id && bus.mute_id)
			return true;
		uint8_t st = id == bus.refuse_id ? SCT_ACK_UNKNOWN : SCT_ACK_OK;

		if (bus.processing)
			chip_ack(SCT_ACK_FIELD_ERROR);
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
	// The codec writes in the six-byte TLV320AIC3204 layout: a radio refused the three-byte one with ACK 0x05
	static const uint8_t c1[] = { 0x84, 0xA9, 0x61, 0x00, 0x06, 0x40, 0x00, 0x01, 0x12, 0x08, 0x00, 0x00 };
	static const uint8_t c2[] = { 0x84, 0xA9, 0x61, 0x00, 0x06, 0x40, 0x00, 0x00, 0x56, 0xA0, 0x00, 0x00 };
	CHECK(memcmp(bus.log[0], cs, sizeof(cs)) == 0);
	CHECK(memcmp(bus.log[1], vs, sizeof(vs)) == 0);
	CHECK(memcmp(bus.log[2], f1, sizeof(f1)) == 0);
	CHECK(memcmp(bus.log[3], f2, sizeof(f2)) == 0);
	CHECK(memcmp(bus.log[4], c1, sizeof(c1)) == 0);
	CHECK(memcmp(bus.log[5], c2, sizeof(c2)) == 0);

	// ACK 0x06 is the manual's "error in processing field": the step fails
	bus.nlog = 0;
	bus.processing = true;
	CHECK(sct_host_boot(&h, &rx, 1000) == SCT_BOOT_NO_VOICE && bus.nlog == 1);
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

static bool wm_silent;	// the fake chip answers WORK_MODE transmit as tx_begin returns, unless silent
static uint32_t fake_now;	// the time of the test's last call in
#define sct_dmr_poll(t) (fake_now = (t), (sct_dmr_poll)(fake_now))


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
static bool keep_ticks;		// slot ticks are recorded only for the test that looks at them

static void on_event(const dmr_event_t *ev, void *ctx)
{
	if (ev->kind == DMR_EV_SLOT_TICK && !keep_ticks)
		return;
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
	fake_now = now;
	sct_dmr_packet(p, (uint16_t)(6 + n), now);
}

static bool tx_begin_answered(dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	static const uint8_t a[] = { SCT_F_WORK_MODE, SCT_ACK_OK };
	bool r = sct_dmr_modem.tx_begin(timing, ts, access);

	if (r && !wm_silent)
		chip_packet(a, sizeof(a), SCT_TYPE_CONTROL, fake_now);
	return r;
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
	fake_now = 0;
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
	CHECK(nsent == 4);
	d = sent_field(0, SCT_F_PROCESS_MODE, NULL);
	CHECK(d != NULL && d[0] == 0x02);
	d = sent_field(1, SCT_F_DMR_CC, NULL);
	CHECK(d != NULL && d[0] == 2);
	d = sent_field(1, SCT_F_DMR_OWN_ID, &n);
	CHECK(d != NULL && n == 4 && d[0] == 0x01 && d[3] == 0x67);
	// Byte 0's traffic mode is the engine's (1, whatever the board gave); the rest is the board's
	d = sent_field(1, SCT_F_DMR_CALL_OPTION, &n);
	CHECK(d != NULL && n == 5 && d[0] == 0x41 && d[1] == 0x00 && d[2] == 0x31 && d[3] == 0x07 && d[4] == 0x00);
	d = sent_field(0, SCT_F_REPORT_FIELD, NULL);
	CHECK(d != NULL && d[0] == 0xE1);
	CHECK(sent_field(1, SCT_F_DMR_OFFSET, NULL) == NULL && sent_field(0, SCT_F_MODOUT_CONFIG, NULL) == NULL);
	CHECK(sent[0].pkt[5] == SCT_TYPE_CONTROL && sent[1].pkt[5] == SCT_TYPE_DMR);
	d = sent_field(2, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 1);
	d = sent_field(3, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 0);	// direct mode: slot number 0

	m->rx_lock_slot(2);
	d = sent_field(nsent - 1, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 0);

	// A repeater channel: both slots until the stack locks one
	dmr_config_t rmo = { .own_id = 1234567, .cc = 2, .access = DMR_ACCESS_IMPOLITE, .repeater = true };

	sct_dmr_init(&port);
	nsent = 0;
	CHECK(m->start(&rmo, on_event, NULL));
	d = sent_field(3, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 3);
	m->rx_lock_slot(2);
	d = sent_field(nsent - 1, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 2);
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 2, DMR_ACCESS_IMPOLITE));
	d = sent_field(nsent - 2, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 0x82);
	m->tx_abort();
}

static void test_dmr_voice_tx(void)
{
	const uint8_t *d;
	uint8_t n;
	uint32_t t = 1000;

	start();
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 2, DMR_ACCESS_IMPOLITE));
	CHECK(m->is_transmitting());
	d = sent_field(nsent - 1, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 2);
	d = sent_field(nsent - 2, SCT_F_DMR_CALL_SLOT, NULL);
	CHECK(d != NULL && d[0] == 0x80);	// direct mode: slot number 0

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
	CHECK(d != NULL && d[0] == SCT_PROP_VOICE_LC && d[1] == 9 && d[2] == 0x10);
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
	CHECK(d != NULL && d[0] == SCT_PROP_VOICE_LC && d[10] == 7);
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
	d = sent_field(nsent - 2, SCT_F_WORK_MODE, NULL);
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
	CHECK(tx_begin_answered(DMR_TIMING_ASAP, 1, DMR_ACCESS_IMPOLITE));
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

	// Unanswered: the next goes a slot later anyway
	sct_dmr_poll(t + SCT_DMR_SLOT_MS - 1);
	CHECK(frames_sent() == 1);
	sct_dmr_poll(t + SCT_DMR_SLOT_MS);
	CHECK(frames_sent() == 2);
	d = sent_field(nsent - 1, SCT_F_DATA_FRAME, &n);
	CHECK(d != NULL && d[0] == DMR_BURST_RATE_34_DATA && d[1] == 18);

	// A refusal frees the slot at once
	t += SCT_DMR_SLOT_MS;
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
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	b = burst(DMR_BURST_CSBK, 0, 12);
	CHECK(m->tx_queue(&b));
	int before = nsent;
	m->tx_abort();
	CHECK(!m->is_transmitting());
	CHECK(nsent == before + 2 && sent_field(nsent - 2, SCT_F_WORK_MODE, NULL)[0] == 1);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);
}

// A chip that answers no frame with `43 00`, only with ACK 0x0A once its frames are on the air (two slots after the
// send or later, and once for frames sent back to back): the frames still go a slot apart, and the transmission ends
// on the 0x0A after the last one
static void test_dmr_tx_answer_on_air(void)
{
	uint32_t t = 9000;

	start();
	sct_dmr_poll(t);
	CHECK(tx_begin_answered(DMR_TIMING_ASAP, 1, DMR_ACCESS_IMPOLITE));
	dmr_tx_burst_t b = burst(DMR_BURST_VOICE_LC_HEADER, 0, 12);
	for (int i = 0; i < 3; i++)
		CHECK(m->tx_queue(&b));
	b = burst(DMR_BURST_VOICE, 0, 27);
	CHECK(m->tx_queue(&b));
	CHECK(frames_sent() == 1);
	for (uint32_t ms = 1; ms <= 180; ms++) {
		sct_dmr_poll(t + ms);
		CHECK(frames_sent() == 1 + (int)(ms / SCT_DMR_SLOT_MS));	// headers, then the voice LC, a slot apart
	}
	CHECK(count_ev(DMR_EV_TX_SLOT_STARTED) == 3);

	// The terminator; a 0x0A within a slot of it is an earlier frame's and doesn't end the transmission
	t += 240;
	b = burst(DMR_BURST_TERMINATOR_LC, 0, 12);
	CHECK(m->tx_queue(&b));
	m->tx_end();
	sct_dmr_poll(t);
	CHECK(frames_sent() == 5);
	far_done(t + 20);
	sct_dmr_poll(t + 20);
	CHECK(count_ev(DMR_EV_TX_DONE) == 0);
	// The terminator's own, two slots on: done then, not SCT_DMR_DONE_MS later
	sct_dmr_poll(t + 60);
	far_done(t + 120);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);
	CHECK(!m->is_transmitting());
	sct_dmr_stats_t st;
	sct_dmr_get_stats(&st);
	CHECK(st.tx_frames == 5 && st.tx_paced == 0);
}

// A board's options: its call option and report bits, more start fields, and a codec write each way around a
// transmission (a radio's values)
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
	static const uint8_t hdr[7] = { 0x84, 0xA9, 0x61, 0x00, 0x06, SCT_TYPE_CODEC, 0x00 };

	return i >= 0 && i < nsent && sent[i].len == SCT_CODEC_PKT_BYTES && memcmp(sent[i].pkt, hdr, 7) == 0 &&
	       sent[i].pkt[7] == w->page && sent[i].pkt[8] == w->reg && sent[i].pkt[9] == w->value &&
	       sent[i].pkt[10] == 0 && sent[i].pkt[11] == 0;
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
	CHECK(nsent == 4);
	d = sent_field(1, SCT_F_DMR_CALL_OPTION, &n);
	CHECK(d != NULL && n == 5 && d[0] == 0x41 && d[1] == 0x40 && d[2] == 0x42 && d[3] == 0x47 && d[4] == 0x08);
	d = sent_field(0, SCT_F_REPORT_FIELD, NULL);
	CHECK(d != NULL && d[0] == 0xFF);
	d = sent_field(0, SCT_F_MODOUT_CONFIG, &n);
	CHECK(d != NULL && n == 5 && d[0] == 0x00 && d[1] == 0x10);
	d = sent_field(1, SCT_F_DMR_OFFSET, &n);
	CHECK(d != NULL && n == 8 && memcmp(d, t_offset, 8) == 0);
	d = sent_field(0, SCT_F_SQ_LEVEL, &n);
	CHECK(d != NULL && n == 12 && memcmp(d, t_sq, 12) == 0);
	d = sent_field(0, SCT_F_DEMOD_GAIN, &n);
	CHECK(d != NULL && n == 2 && d[0] == 0 && d[1] == 0);
	CHECK(sent[1].pkt[5] == SCT_TYPE_DMR && sent_field(0, SCT_F_CARRIER_READY, NULL) == NULL);
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
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	CHECK(nsent == 7 && is_codec(4, &t_mute));
	d = sent_field(6, SCT_F_WORK_MODE, NULL);
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
	d = sent_field(nsent - 3, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 1 && is_codec(nsent - 1, &t_unmute));
	CHECK(codec_count(&t_mute) == 1 && codec_count(&t_unmute) == 1);

	// tx_abort and stop undo it too, once
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	m->tx_abort();
	CHECK(is_codec(nsent - 1, &t_unmute) && codec_count(&t_unmute) == 2);
	m->tx_abort();
	CHECK(codec_count(&t_unmute) == 2);
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
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
	CHECK(!m->start(&cfg, on_event, NULL) && nsent == 1);
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

// A repeater's outbound: the chip reports both slots' frames in turn, each with its slot in PROP bit 7, and
// DMR_SLOT_FOUND only now and then. Each frame goes to the stack in its own slot, and each slot's call has its own
// voice bursts: a TS 2 call starting during a TS 1 call is held on its own and outlives it (HIL: a radio on TS 1
// heard a TS 2 call, a TS 1 call's voice bursts went out in TS 2, and a TS 2 call begun mid TS 1 call came again as
// a late entry once the TS 1 call ended)
static void test_dmr_rx_outbound(void)
{
	start();
	uint8_t hdr1[16] = { SCT_F_DMR_SLOT_FOUND, 0x80, 0x43, DMR_BURST_VOICE_LC_HEADER, 9, 0, 0, 0, 0, 0, 0x09, 0x2F,
			     0x52, 0x5C };
	uint8_t idle2[16] = { 0x43, DMR_BURST_IDLE | SCT_PROP_TS2, 12 };
	uint8_t hdr2[16] = { 0x43, DMR_BURST_VOICE_LC_HEADER | SCT_PROP_TS2, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x60, 0x5C };
	uint8_t term2[16] = { 0x43, DMR_BURST_TERMINATOR_LC | SCT_PROP_TS2, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x60, 0x5C };
	uint8_t term1[16] = { 0x43, DMR_BURST_TERMINATOR_LC, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };

	// (after a header the bursts are held SCT_DMR_HDR_HOLD_MS, 210 ms, then A at 90 ms, B and C go together)
	chip_packet(hdr1, 14, 0x30, 100);
	chip_packet(idle2, 15, 0x30, 130);
	sct_dmr_poll(160);
	chip_packet(hdr2, 12, 0x30, 200);
	sct_dmr_poll(250);
	sct_dmr_poll(310);
	sct_dmr_poll(370);
	chip_packet(term1, 12, 0x30, 400);
	sct_dmr_poll(410);
	chip_packet(term2, 12, 0x30, 440);
	sct_dmr_poll(500);
	CHECK(nev == 12);
	static const struct {
		dmr_burst_kind_t kind;
		int8_t ts;
		uint8_t index;
	} want[12] = {
		{ DMR_BURST_VOICE_LC_HEADER, 1, 0 }, { DMR_BURST_IDLE, 2, 0 }, { DMR_BURST_VOICE_LC_HEADER, 2, 0 },
		{ DMR_BURST_VOICE, 1, 0 }, { DMR_BURST_VOICE, 1, 1 }, { DMR_BURST_VOICE, 1, 2 }, { DMR_BURST_VOICE, 1, 3 },
		{ DMR_BURST_TERMINATOR_LC, 1, 0 }, { DMR_BURST_VOICE, 2, 0 }, { DMR_BURST_VOICE, 2, 1 },
		{ DMR_BURST_VOICE, 2, 2 }, { DMR_BURST_TERMINATOR_LC, 2, 0 },
	};
	for (int i = 0; i < 12 && i < nev; i++) {
		const dmr_burst_t *b = &ev_bursts[i];

		CHECK(b->kind == want[i].kind && b->ts == want[i].ts && b->sync == DMR_SYNC_BS);
		if (b->kind == DMR_BURST_VOICE)
			CHECK(b->voice_index == want[i].index);
	}
	CHECK(ev_bursts[2].lc_valid && ev_bursts[2].lc[7] == 0x60);
}

// A call heard from its headers (every one, or two of the lead-in missed): 60 voice bursts on air, the terminator 61
// slots after the last header, its report a little late or a little early. The stack gets exactly 60 voice bursts
// (the count on the slot boundary gave 61 when the terminator's report came after the 61st slot's tick, and bursts
// counted in the missed headers' gap were too many)
static void test_dmr_rx_voice_count(void)
{
	static const uint8_t hdr[16] = { 0x43, DMR_BURST_VOICE_LC_HEADER, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	static const uint8_t term[16] = { 0x43, DMR_BURST_TERMINATOR_LC, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };

	for (int late = -20; late <= 25; late += 5) {
		for (int gap = 0; gap < 2; gap++) {
		start();
		// eight headers, a slot apart; with gap, the chip misses the third and the fourth
		uint32_t t0 = 1000, t = t0;
		for (int h = 0; h < 8; h++) {
			t = t0 + (uint32_t)h * SCT_DMR_SLOT_MS;
			if (!(gap && (h == 2 || h == 3)))
				chip_packet(hdr, 12, 0x30, t);
			for (uint32_t q = t + 5; q < t + SCT_DMR_SLOT_MS && h < 7; q += 5)
				sct_dmr_poll(q);
		}
		t0 = t;		// the last header
		int voice = 0;
		for (int i = 0; i < nev; i++)
			voice += ev_bursts[i].kind == DMR_BURST_VOICE;
		nev = 0;	// (EV_MAX: the headers and the call's bursts don't all fit)
		uint32_t t_term = (uint32_t)((int32_t)(t0 + 61 * SCT_DMR_SLOT_MS) + late);
		for (t = t0 + 5; t < t_term; t += 5)
			sct_dmr_poll(t);
		chip_packet(term, 12, 0x30, t_term);
		for (t = t_term; t < t_term + 300; t += 5)
			sct_dmr_poll(t);
		for (int i = 0; i < nev; i++)
			voice += ev_bursts[i].kind == DMR_BURST_VOICE;
		CHECK(voice == 60);
		if (voice != 60)
			printf("# terminator %d ms off its slot, %s: %d voice bursts\n", late,
			       gap ? "two headers missed" : "every header", voice);
		}
	}
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

	// A superframe's LC (late entry) reaches the stack as voice burst A with the LC embedded, as from a burst modem.
	// Under base station sync each frame's PROP bit 7 is its slot (TS 2 here)
	uint8_t r3[16] = { SCT_F_DMR_SLOT_FOUND, 0xC4, 0x43, SCT_PROP_VOICE | SCT_PROP_TS2, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	chip_packet(r3, 14, 0x30, 400);
	CHECK(nev == 3);
	b = &ev_bursts[2];
	CHECK(b->kind == DMR_BURST_VOICE && b->voice_index == 0 && b->lc_valid && b->crc_ok && b->lc[5] == 0x09);
	CHECK(b->len == 27);
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

	// A header starts the bursts at A, counted half a slot after its time on air; a stalled poll gives at most a few,
	// then the terminator stops them
	uint8_t r5[16] = { 0x43, DMR_BURST_VOICE_LC_HEADER | SCT_PROP_TS2, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	chip_packet(r5, 12, 0x30, 1000);
	CHECK(nev == 7 && ev_bursts[6].kind == DMR_BURST_VOICE_LC_HEADER && ev_bursts[6].lc_valid);
	sct_dmr_poll(1209);
	CHECK(nev == 7);			// held: another header may come
	sct_dmr_poll(1210);
	CHECK(nev == 10 && ev_bursts[7].voice_index == 0 && ev_bursts[8].voice_index == 1 &&
	      ev_bursts[9].voice_index == 2);
	sct_dmr_poll(3000);
	CHECK(nev == 14);
	sct_dmr_poll(3059);
	CHECK(nev == 14);
	uint8_t r6[16] = { 0x43, DMR_BURST_TERMINATOR_LC | SCT_PROP_TS2, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	chip_packet(r6, 12, 0x30, 3060);
	CHECK(nev == 15 && ev_bursts[14].kind == DMR_BURST_TERMINATOR_LC);
	sct_dmr_poll(3200);
	CHECK(nev == 15);

	// Every header reported (DMR_CALL_OPTION byte 3): a repeat starts the bursts at A again, a superframe's LC
	// doesn't, and a second terminator after the end changes nothing
	chip_packet(r5, 12, 0x30, 4000);
	CHECK(nev == 16 && ev_bursts[15].kind == DMR_BURST_VOICE_LC_HEADER);
	sct_dmr_poll(4090);
	CHECK(nev == 16);			// burst A's time, held
	chip_packet(r5, 12, 0x30, 4100);
	CHECK(nev == 17 && ev_bursts[16].kind == DMR_BURST_VOICE_LC_HEADER);
	sct_dmr_poll(4309);
	CHECK(nev == 17);
	sct_dmr_poll(4310);
	CHECK(nev == 20 && ev_bursts[17].voice_index == 0 && ev_bursts[19].voice_index == 2);
	// The call's own LC from a superframe is dropped: the stack would take it for a header and restart at A
	chip_packet(r3, 14, 0x30, 4320);
	sct_dmr_poll(4370);
	CHECK(nev == 21 && ev_bursts[20].kind == DMR_BURST_VOICE && ev_bursts[20].voice_index == 3);
	// ... another LC (a new talker) goes through in place of the next voice burst, and the bursts go on from it
	uint8_t r7[16] = { 0x43, SCT_PROP_VOICE | SCT_PROP_TS2, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x53, 0x5C };
	chip_packet(r7, 12, 0x30, 4380);
	CHECK(nev == 22 && ev_bursts[21].kind == DMR_BURST_VOICE && ev_bursts[21].voice_index == 4);
	CHECK(ev_bursts[21].lc_valid && ev_bursts[21].lc[7] == 0x53);
	sct_dmr_poll(4439);
	CHECK(nev == 22);
	sct_dmr_poll(4440);
	CHECK(nev == 23 && ev_bursts[22].voice_index == 5 && !ev_bursts[22].lc_valid);
	chip_packet(r6, 12, 0x30, 4450);
	chip_packet(r6, 12, 0x30, 4500);
	sct_dmr_poll(4600);
	CHECK(nev == 25 && ev_bursts[23].kind == DMR_BURST_TERMINATOR_LC && ev_bursts[24].kind == DMR_BURST_TERMINATOR_LC);
	nev = 6;

	// Nothing reported while transmitting; a garbled packet is counted
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	chip_packet(r, 19, 0x30, 900);
	CHECK(nev == 6);
	uint8_t junk[4] = { 0x03, 0x00, 0x00, 0x00 };
	chip_packet(junk, 4, 0x30, 900);
	sct_dmr_stats_t st;
	sct_dmr_get_stats(&st);
	CHECK(st.rx_bad == 1 && st.rx_frames == 11 && st.rx_fec == 1 && st.rx_voice == 16);
}

// CARRIER_READY: the board's value, sent on a change in receive only, and again at each return to receive
static void test_dmr_carrier(void)
{
	const uint8_t *d;
	sct_dmr_stats_t st;

	start();
	sct_dmr_poll(1000);
	CHECK(nsent == 4);			// the board said nothing yet: nothing sent
	sct_dmr_carrier(2);
	CHECK(nsent == 5 && sent[4].pkt[5] == SCT_TYPE_CONTROL);
	d = sent_field(4, SCT_F_CARRIER_READY, NULL);
	CHECK(d != NULL && d[0] == 2);
	sct_dmr_carrier(2);
	sct_dmr_poll(1010);
	CHECK(nsent == 5);			// no change, nothing more
	// A loss goes out only once it has lasted the hold: the gaps between one station's bursts don't
	sct_dmr_carrier(0);
	sct_dmr_poll(1040);
	CHECK(nsent == 5);
	sct_dmr_poll(1070);
	sct_dmr_carrier(2);
	sct_dmr_carrier(0);
	sct_dmr_poll(1070 + SCT_DMR_CR_HOLD_MS - 10);
	CHECK(nsent == 5);
	sct_dmr_poll(1070 + SCT_DMR_CR_HOLD_MS);
	d = sent_field(nsent - 1, SCT_F_CARRIER_READY, NULL);
	CHECK(nsent == 6 && d != NULL && d[0] == 0);
	sct_dmr_carrier(2);
	CHECK(nsent == 7);

	// Transmitting: nothing, and the reading from before doesn't come back after it
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	int n = nsent;
	sct_dmr_carrier(2);
	sct_dmr_poll(1500);
	CHECK(nsent == n);
	m->tx_abort();
	sct_dmr_poll(1510);
	d = sent_field(nsent - 1, SCT_F_CARRIER_READY, NULL);
	CHECK(d != NULL && d[0] == 0);		// receive again: "lost" until the board's next reading
	sct_dmr_carrier(2);
	d = sent_field(nsent - 1, SCT_F_CARRIER_READY, NULL);
	CHECK(d != NULL && d[0] == 2);

	// A receive restart (WORK_MODE receive) sends the current value again
	n = nsent;
	m->rx_restart();
	sct_dmr_poll(1520);
	d = sent_field(nsent - 1, SCT_F_CARRIER_READY, NULL);
	CHECK(nsent == n + 3 && d != NULL && d[0] == 2);
	sct_dmr_get_stats(&st);
	CHECK(st.carrier == 2 && st.carrier_sent == 6);

	// Stopped: nothing
	m->stop();
	n = nsent;
	sct_dmr_carrier(0);
	CHECK(nsent == n);
}

// The board's carrier gone for the hold ends a call the chip plays (its terminators missed on a weak signal): the
// paced voice stops with SYNC_LOST, no seconds-long wait for the chip's own
static void test_dmr_carrier_ends_call(void)
{
	start();
	sct_dmr_carrier(2);
	uint8_t hdr[16] = { 0x43, DMR_BURST_VOICE_LC_HEADER | SCT_PROP_TS2, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	chip_packet(hdr, 12, 0x30, 1000);
	sct_dmr_poll(1090);
	sct_dmr_carrier(2);			// the board's reading at each status poll
	sct_dmr_poll(1150);
	sct_dmr_carrier(2);
	sct_dmr_poll(1210);
	sct_dmr_carrier(2);
	int n = nev;
	CHECK(n >= 3 && ev_bursts[n - 1].kind == DMR_BURST_VOICE);
	sct_dmr_carrier(0);			// the signal goes, the chip says nothing
	sct_dmr_poll(1210 + SCT_DMR_CR_HOLD_MS - 10);
	CHECK(evs[nev - 1].kind != DMR_EV_SYNC_LOST);	// within the hold the bursts go on
	n = nev;
	sct_dmr_poll(1210 + SCT_DMR_CR_HOLD_MS);
	CHECK(nev > n && evs[nev - 1].kind == DMR_EV_SYNC_LOST);
	n = nev;
	sct_dmr_poll(3000);
	CHECK(nev == n);			// no more paced bursts

	// A loss with no call playing reports nothing
	sct_dmr_carrier(2);
	sct_dmr_carrier(0);
	sct_dmr_poll(3000 + SCT_DMR_CR_HOLD_MS);
	CHECK(nev == n);
}

// The health keys (hal_dmr_modem_t.health): a transmission whose drain hears nothing from the chip, and a received
// voice burst skipped because the poll came too late to catch up
static void test_dmr_health(void)
{
	dmr_modem_health_t h;

	start();
	CHECK(m->health != NULL);
	m->health(&h);
	CHECK(h.stalls == 0 && h.stuck == 0 && h.first_ms == 0);

	// Nothing sent: an empty transmission's drain time-out is no stall
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	m->tx_end();
	sct_dmr_poll(100);
	sct_dmr_poll(100 + SCT_DMR_DONE_MS);
	m->health(&h);
	CHECK(h.stalls == 0);

	// A call heard, then a poll late by more than the four bursts a poll catches up
	uint8_t hdr[16] = { 0x43, DMR_BURST_VOICE_LC_HEADER | SCT_PROP_TS2, 9, 0, 0, 0, 0, 0, 0x09, 0x2F, 0x52, 0x5C };
	chip_packet(hdr, 12, 0x30, 1000);
	sct_dmr_poll(1060);
	m->health(&h);
	CHECK(h.stuck == 0);
	sct_dmr_poll(1060 + 3 * SCT_DMR_SLOT_MS);	// late, but caught up
	m->health(&h);
	CHECK(h.stuck == 0);
	sct_dmr_poll(1060 + 10 * SCT_DMR_SLOT_MS);
	m->health(&h);
	CHECK(h.stuck == 1 && h.first_ms == 1060 + 10 * SCT_DMR_SLOT_MS);

	// A frame sent, then not a word from the chip until the drain time-out: a stall, the first loss kept
	sct_dmr_carrier(0);
	sct_dmr_poll(5000 + SCT_DMR_CR_HOLD_MS);
	dmr_tx_burst_t b = burst(DMR_BURST_VOICE_LC_HEADER, 0, 12);
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	CHECK(m->tx_queue(&b));
	m->tx_end();
	uint32_t t = 6000;
	for (int i = 0; i < 40; i++)
		sct_dmr_poll(t += 30);
	m->health(&h);
	CHECK(h.stalls == 1 && h.stuck == 1 && h.first_ms == 1060 + 10 * SCT_DMR_SLOT_MS);
}

// A port with rx_ready: after a transmission, no CARRIER_READY until the receiver has settled, whatever the board
// reads meanwhile, then rx_ready; the board's readings from there as before
static const sct_dmr_port_t port_settle = {
	.send = d_send, .call_option = { 0x01, 0x40, 0x42, 0x47, 0x08 }, .rx_ready = 2, .rx_settle_ms = 40,
	.ready = d_ready, .lock = d_lock, .unlock = d_unlock,
};

static int cr_sent_since(int from)
{
	int n = 0;

	for (int i = from; i < nsent; i++)
		if (sent_field(i, SCT_F_CARRIER_READY, NULL) != NULL)
			n++;
	return n;
}

static void test_dmr_carrier_settle(void)
{
	dmr_config_t cfg = { .own_id = 1234567, .cc = 1, .access = DMR_ACCESS_IMPOLITE };
	const uint8_t *d;
	uint32_t t = 2000;

	sct_dmr_init(&port_settle);
	nsent = 0;
	sct_dmr_poll(t);
	CHECK(m->start(&cfg, on_event, NULL));
	sct_dmr_poll(t += 10);
	CHECK(cr_sent_since(0) == 0);		// no transmission yet: only the board's readings, none so far

	// A transmission that ends on the drain timeout: receive again at t
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	m->tx_end();
	sct_dmr_poll(t += SCT_DMR_DONE_MS);
	int n = nsent;
	sct_dmr_carrier(0);			// the board reads "lost" while its receiver settles
	sct_dmr_poll(t + 39);
	CHECK(cr_sent_since(n) == 0);
	sct_dmr_poll(t + 40);
	d = sent_field(nsent - 1, SCT_F_CARRIER_READY, NULL);
	CHECK(cr_sent_since(n) == 1 && d != NULL && d[0] == 2);
	// The board's loss goes out once it has lasted the hold, as any loss
	sct_dmr_carrier(0);
	sct_dmr_poll(t + 40 + SCT_DMR_CR_HOLD_MS - 1);
	CHECK(cr_sent_since(n) == 1);
	sct_dmr_poll(t += 40 + SCT_DMR_CR_HOLD_MS);
	d = sent_field(nsent - 1, SCT_F_CARRIER_READY, NULL);
	CHECK(cr_sent_since(n) == 2 && d != NULL && d[0] == 0);

	// An aborted transmission settles the same way; a new one before the time sends nothing in between
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	m->tx_abort();
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	n = nsent;
	sct_dmr_poll(t + 100);
	CHECK(cr_sent_since(n) == 0);
	sct_dmr_poll(t += 110);
	m->tx_abort();
	n = nsent;
	sct_dmr_poll(t + 39);
	CHECK(cr_sent_since(n) == 0);
	sct_dmr_poll(t + 40);
	d = sent_field(nsent - 1, SCT_F_CARRIER_READY, NULL);
	CHECK(cr_sent_since(n) == 1 && d != NULL && d[0] == 2);

	// A receive restart doesn't wait: nothing new to settle
	n = nsent;
	m->rx_restart();
	sct_dmr_poll(t + 41);
	d = sent_field(nsent - 1, SCT_F_CARRIER_READY, NULL);
	CHECK(cr_sent_since(n) == 1 && d != NULL && d[0] == 2);
	m->stop();
}

// A port with the burst line's edge count: a transmission without voice ends at the fall of the burst of its last
// frame, without waiting for the chip's "far end done" or the drain; one with voice still waits for the chip
static uint32_t t_edges = 0xFFFFFFF0u;	// any start: the engine takes differences (and this one wraps)

static uint32_t d_burst_edges(void)
{
	return t_edges;
}

static const sct_dmr_port_t port_edges = {
	.send = d_send, .call_option = { 0x01, 0x40, 0x42, 0x47, 0x08 }, .burst_edges = d_burst_edges,
	.ready = d_ready, .lock = d_lock, .unlock = d_unlock,
};

static void test_dmr_tx_early_end(void)
{
	dmr_config_t cfg = { .own_id = 1234567, .cc = 1, .access = DMR_ACCESS_IMPOLITE };
	const uint8_t *d;
	uint32_t t = 3000;
	sct_dmr_stats_t st;

	sct_dmr_init(&port_edges);
	nsent = nev = 0;
	sct_dmr_poll(t);
	CHECK(m->start(&cfg, on_event, NULL));
	t_edges += 7;				// edges before the key-up don't count

	// Three CSBKs, a slot apart (the chip answers none in time)
	CHECK(tx_begin_answered(DMR_TIMING_ASAP, 1, DMR_ACCESS_IMPOLITE));
	for (int i = 0; i < 3; i++) {
		dmr_tx_burst_t b = burst(DMR_BURST_CSBK, 0, 12);
		CHECK(m->tx_queue(&b));
	}
	m->tx_end();
	sct_dmr_poll(t + SCT_DMR_SLOT_MS);
	CHECK(frames_sent() == 2);
	t_edges += 4;				// two bursts over, one frame still queued
	sct_dmr_poll(t + SCT_DMR_SLOT_MS + 10);
	CHECK(count_ev(DMR_EV_TX_DONE) == 0);
	sct_dmr_poll(t += 2 * SCT_DMR_SLOT_MS);
	CHECK(frames_sent() == 3);
	t_edges += 1;				// the last burst rises
	sct_dmr_poll(t += 30);
	CHECK(count_ev(DMR_EV_TX_DONE) == 0 && m->is_transmitting());
	t_edges += 1;				// ... and falls: done at the next poll
	sct_dmr_poll(t += 1);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1 && !m->is_transmitting());
	d = sent_field(nsent - 2, SCT_F_WORK_MODE, NULL);
	CHECK(d != NULL && d[0] == 1);
	far_done(t + 17);			// the chip's own end comes late: nothing more
	sct_dmr_poll(t + SCT_DMR_DONE_MS);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);
	sct_dmr_get_stats(&st);
	CHECK(st.tx_early == 1);

	// Voice: the chip's own voice bursts aren't frames, so the edges don't end it; the chip's end does
	t += 1000;
	sct_dmr_poll(t);
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	dmr_tx_burst_t b = burst(DMR_BURST_VOICE_LC_HEADER, 0, 12);
	CHECK(m->tx_queue(&b));
	b = burst(DMR_BURST_VOICE, 0, 27);
	CHECK(m->tx_queue(&b));
	frame_ok(t + 5);
	sct_dmr_poll(t += SCT_DMR_SLOT_MS);
	frame_ok(t + 5);
	b = burst(DMR_BURST_TERMINATOR_LC, 0, 12);
	CHECK(m->tx_queue(&b));
	m->tx_end();
	sct_dmr_poll(t += SCT_DMR_SLOT_MS);
	frame_ok(t + 5);
	t_edges += 40;
	sct_dmr_poll(t += 40);
	CHECK(count_ev(DMR_EV_TX_DONE) == 1 && m->is_transmitting());
	far_done(t += 20);
	CHECK(count_ev(DMR_EV_TX_DONE) == 2 && !m->is_transmitting());
	sct_dmr_get_stats(&st);
	CHECK(st.tx_early == 1);

	// A refused frame never goes on the air: its burst never comes, and the end is the chip's or the drain's
	t += 1000;
	sct_dmr_poll(t);
	CHECK(tx_begin_answered(DMR_TIMING_ASAP, 1, DMR_ACCESS_IMPOLITE));
	b = burst(DMR_BURST_CSBK, 0, 12);
	CHECK(m->tx_queue(&b));
	m->tx_end();
	static const uint8_t nak[] = { 0x43, SCT_ACK_LENGTH };
	chip_packet(nak, 2, SCT_TYPE_DMR, t + 5);
	sct_dmr_poll(t += 100);
	CHECK(count_ev(DMR_EV_TX_DONE) == 2 && m->is_transmitting());
	sct_dmr_poll(t + SCT_DMR_DONE_MS);
	CHECK(count_ev(DMR_EV_TX_DONE) == 3 && !m->is_transmitting());
	sct_dmr_get_stats(&st);
	CHECK(st.tx_early == 1);
	m->stop();
}

// Fields the board gives at each transmission (a modulator gain by band): one control packet before WORK_MODE
// transmit, asked for again at every key-up
static uint8_t t_band;	// 0: the board gives none
static const uint8_t t_gain_a[2] = { 0x0C, 0x60 };
static const uint8_t t_gain_b[2] = { 0x24, 0x75 };
static const sct_field_t t_tx_a[] = { { SCT_F_MOD_GAIN, 2, t_gain_a } };
static const sct_field_t t_tx_b[] = { { SCT_F_MOD_GAIN, 2, t_gain_b } };

static uint8_t d_tx_fields(const sct_field_t **f, void *ctx)
{
	(void)ctx;
	if (t_band == 0)
		return 0;
	*f = t_band == 1 ? t_tx_a : t_tx_b;
	return 1;
}

static const sct_dmr_port_t port_txf = {
	.send = d_send, .call_option = { 0x41, 0x40, 0x42, 0x47, 0x08 }, .tx_fields = d_tx_fields,
	.ready = d_ready, .lock = d_lock, .unlock = d_unlock,
};

// codec_start: each start sends the board's codec writes, in order, after the start packet; none without it
static const sct_codec_write_t t_cstart[] = { { 0, 0x3C, 0x11 }, { 1, 0x10, 0x08 } };
static const sct_dmr_port_t port_cstart = {
	.send = d_send, .call_option = { 0x41, 0x40, 0x42, 0x47, 0x08 }, .codec_start = t_cstart, .codec_start_count = 2,
	.ready = d_ready, .lock = d_lock, .unlock = d_unlock,
};

static void test_dmr_codec_start(void)
{
	dmr_config_t cfg = { .own_id = 1234567, .cc = 1, .access = DMR_ACCESS_IMPOLITE };

	sct_dmr_init(&port_cstart);
	for (int k = 0; k < 2; k++) {
		nsent = 0;
		CHECK(m->start(&cfg, on_event, NULL));
		CHECK(nsent == 6 && sent[1].pkt[5] == SCT_TYPE_DMR && is_codec(4, &t_cstart[0]) && is_codec(5, &t_cstart[1]));
		m->stop();
	}
	sct_dmr_init(&port_opts);
	nsent = 0;
	CHECK(m->start(&cfg, on_event, NULL));
	CHECK(codec_count(&t_cstart[0]) == 0);
	m->stop();
}

static void test_dmr_tx_fields(void)
{
	dmr_config_t cfg = { .own_id = 1234567, .cc = 1, .access = DMR_ACCESS_IMPOLITE };
	const uint8_t *d;
	uint8_t n;
	uint32_t t = 5000;
	sct_dmr_stats_t st0, st;

	sct_dmr_init(&port_txf);
	// Start sends them too, after the start packet, so the first transmission's aren't the chip's first
	t_band = 1;
	nsent = 0;
	CHECK(m->start(&cfg, on_event, NULL));
	CHECK(nsent >= 2 && sent[nsent - 1].pkt[5] == SCT_TYPE_CONTROL);
	d = sent_field(nsent - 1, SCT_F_MOD_GAIN, &n);
	CHECK(d != NULL && n == 2 && memcmp(d, t_gain_a, 2) == 0);
	m->stop();
	// None given: start sends no gain
	t_band = 0;
	nsent = 0;
	CHECK(m->start(&cfg, on_event, NULL));
	for (int i = 0; i < nsent; i++)
		CHECK(sent_field(i, SCT_F_MOD_GAIN, NULL) == NULL);
	sct_dmr_poll(t);
	for (uint8_t k = 1; k <= 2; k++) {
		t_band = k;
		nsent = 0;
		CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
		CHECK(nsent == 3 && sent[0].pkt[5] == SCT_TYPE_CONTROL);
		d = sent_field(0, SCT_F_MOD_GAIN, &n);
		CHECK(d != NULL && n == 2 && memcmp(d, k == 1 ? t_gain_a : t_gain_b, 2) == 0);
		d = sent_field(2, SCT_F_WORK_MODE, NULL);
		CHECK(d != NULL && d[0] == 2 && sent_field(2, SCT_F_MOD_GAIN, NULL) == NULL);
		// The field's status is no frame's answer and no NAK
		static const uint8_t a[] = { SCT_F_MOD_GAIN, SCT_ACK_OK };
		sct_dmr_get_stats(&st0);
		chip_packet(a, 2, SCT_TYPE_CONTROL, t + 1);
		sct_dmr_get_stats(&st);
		CHECK(st.naks == st0.naks && st.rx_bad == st0.rx_bad && st.tx_acks == st0.tx_acks && !st.sending);
		m->tx_abort();
	}
	// None given: no control packet
	t_band = 0;
	nsent = 0;
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	CHECK(nsent == 2 && sent_field(1, SCT_F_WORK_MODE, NULL) != NULL);
	m->tx_abort();
	m->stop();
}

// The debug link's field access: a write answered by its echo, a refusal, a query's data, and no answer at all
static void test_chip_answers(void)
{
	sct_dmr_stats_t st;
	static const uint8_t refuse[] = { SCT_F_WORK_MODE, SCT_ACK_UNKNOWN };
	static const uint8_t ready[] = { SCT_F_ACK, SCT_ACK_RESET_READY };
	static const uint8_t bad_vocoder[] = { SCT_F_NEW_EVENT, SCT_EV_WRONG_VOCODER };
	dmr_config_t cfg = { .own_id = 1234567, .cc = 2, .access = DMR_ACCESS_IMPOLITE };
	dmr_tx_burst_t b = burst(DMR_BURST_VOICE_LC_HEADER, 0, 12);
	uint32_t t = 5000;

	// No frame until WORK_MODE is answered, and the answer lets it go
	start();
	sct_dmr_poll(t);
	wm_silent = true;
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	CHECK(m->tx_queue(&b));
	sct_dmr_poll(t + 2);
	CHECK(frames_sent() == 0);
	wm_silent = false;
	// Unanswered, the Packet Interface's 40 ms later it goes anyway, and is counted
	sct_dmr_poll(t + 39);
	CHECK(frames_sent() == 0);
	sct_dmr_poll(t + 40);
	sct_dmr_get_stats(&st);
	CHECK(frames_sent() == 1 && st.mode_timeouts == 1);
	m->tx_abort();

	// WORK_MODE refused: the stack is told, the chip goes back to receive
	t += 1000;
	sct_dmr_poll(t);
	nev = 0;
	wm_silent = true;
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	wm_silent = false;
	chip_packet(refuse, sizeof(refuse), SCT_TYPE_CONTROL, t + 1);
	CHECK(!m->is_transmitting() && count_ev(DMR_EV_TX_REJECTED) == 1);

	// The frame that starts the voice refused: the same. One after it is only counted
	t += 1000;
	sct_dmr_poll(t);
	nev = 0;
	CHECK(tx_begin_answered(DMR_TIMING_OWN, 1, DMR_ACCESS_IMPOLITE));
	sct_dmr_poll(t);
	b = burst(DMR_BURST_VOICE, 0, 27);
	CHECK(m->tx_queue(&b));
	sct_dmr_poll(t + 1);
	CHECK(frames_sent() >= 1);
	static const uint8_t nak[] = { SCT_F_DATA_FRAME, SCT_ACK_FIELD_ERROR };

	chip_packet(nak, sizeof(nak), SCT_TYPE_DMR, t + 2);
	CHECK(!m->is_transmitting() && count_ev(DMR_EV_TX_REJECTED) == 1);

	// ACK 0x01 within a second of the start is the chip's answer to it: nothing happens
	sct_dmr_get_stats(&st);
	uint32_t restarts = st.chip_restarts;

	t += 3000;
	sct_dmr_poll(t);
	CHECK(m->start(&cfg, on_event, NULL));
	nsent = 0;
	chip_packet(ready, sizeof(ready), SCT_TYPE_CONTROL, t + 900);
	sct_dmr_poll(t + 901);
	sct_dmr_get_stats(&st);
	CHECK(st.chip_restarts == restarts && nsent == 0);

	// ACK 0x01 later: the chip restarted, the whole start goes again, at most once a second
	t += 2000;
	sct_dmr_poll(t);
	nsent = 0;
	chip_packet(ready, sizeof(ready), SCT_TYPE_CONTROL, t + 1);
	sct_dmr_get_stats(&st);
	CHECK(st.chip_restarts == restarts + 1);
	sct_dmr_poll(t + 2);
	CHECK(nsent >= 4 && sent_field(0, SCT_F_PROCESS_MODE, NULL) != NULL);
	nsent = 0;
	chip_packet(ready, sizeof(ready), SCT_TYPE_CONTROL, t + 3);
	sct_dmr_poll(t + 4);
	CHECK(nsent == 0);
	chip_packet(ready, sizeof(ready), SCT_TYPE_CONTROL, t + 1003);
	sct_dmr_poll(t + 1004);
	CHECK(nsent >= 4);

	// A field of the start refused: one retry, then left alone
	t += 3000;
	sct_dmr_poll(t);
	CHECK(m->start(&cfg, on_event, NULL));
	nsent = 0;
	static const uint8_t refused_field[] = { SCT_F_PROCESS_MODE, SCT_ACK_FIELD_ERROR };
	static const uint8_t slot_ok[] = { SCT_F_DMR_CALL_SLOT, SCT_ACK_OK };

	chip_packet(refused_field, sizeof(refused_field), SCT_TYPE_CONTROL, t + 1);
	sct_dmr_poll(t + 1001);
	CHECK(nsent >= 4);
	nsent = 0;
	chip_packet(refused_field, sizeof(refused_field), SCT_TYPE_CONTROL, t + 1002);
	sct_dmr_poll(t + 2100);
	CHECK(nsent == 0);
	chip_packet(slot_ok, sizeof(slot_ok), SCT_TYPE_DMR, t + 2101);

	// A hunt that goes on: the receive mode is sent again at the limit, once, and a verified report after that counts
	static const uint8_t hunting[] = { SCT_F_DMR_SLOT_FOUND, 0x8B };
	static const uint8_t verified[] = { SCT_F_DMR_SLOT_FOUND, 0xAB };

	m->rx_restart();
	nsent = 0;
	for (int i = 1; i < SCT_DMR_HUNT_LONG; i++)
		chip_packet(hunting, sizeof(hunting), SCT_TYPE_READ, t + 2101);
	CHECK(nsent == 0);
	chip_packet(hunting, sizeof(hunting), SCT_TYPE_READ, t + 2101);
	CHECK(nsent == 2 && sent_field(0, SCT_F_WORK_MODE, NULL) != NULL);
	sct_dmr_get_stats(&st);
	CHECK(st.rehunts == 1);
	for (int i = 0; i < 2 * SCT_DMR_HUNT_LONG; i++)
		chip_packet(hunting, sizeof(hunting), SCT_TYPE_READ, t + 2101);
	CHECK(nsent == 2 && !m->slot_verified());
	chip_packet(verified, sizeof(verified), SCT_TYPE_READ, t + 2101);
	CHECK(m->slot_verified());

	// A base station sync report is a slot tick for the stack, with the slot from the TC bit; a mobile one isn't
	static const uint8_t bs_ts1[] = { SCT_F_DMR_SLOT_FOUND, 0x83 };
	static const uint8_t bs_ts2[] = { SCT_F_DMR_SLOT_FOUND, 0x87 };
	static const uint8_t ms[] = { SCT_F_DMR_SLOT_FOUND, 0x01 };

	nev = 0;
	keep_ticks = true;
	chip_packet(bs_ts1, sizeof(bs_ts1), SCT_TYPE_READ, t + 2101);
	CHECK(count_ev(DMR_EV_SLOT_TICK) == 1 && evs[0].ts == 1);
	chip_packet(bs_ts2, sizeof(bs_ts2), SCT_TYPE_READ, t + 2101);
	CHECK(count_ev(DMR_EV_SLOT_TICK) == 2 && evs[1].ts == 2);
	chip_packet(ms, sizeof(ms), SCT_TYPE_READ, t + 2101);
	CHECK(count_ev(DMR_EV_SLOT_TICK) == 2);
	keep_ticks = false;

	// NEW_EVENT 15 is counted, nothing else
	chip_packet(bad_vocoder, sizeof(bad_vocoder), SCT_TYPE_READ, t + 2102);
	sct_dmr_get_stats(&st);
	CHECK(st.rx_unsupported == 1);
	m->stop();
}

static void test_field_access(void)
{
	dmr_config_t cfg = { .own_id = 1, .cc = 1 };
	uint8_t d[4], n = 0, v = 0;
	sct_dmr_stats_t st;

	CHECK(sct_dmr_modem.start(&cfg, on_event, NULL));
	nsent = 0;
	CHECK(sct_dmr_field_poll(d, &n) == 0);
	v = 8;
	CHECK(sct_dmr_field_begin(SCT_TYPE_CONTROL, 0x2F, &v, 1, false));
	CHECK(nsent == 1 && sent[0].pkt[5] == SCT_TYPE_CONTROL && sent[0].pkt[6] == 0x2F && sent[0].pkt[7] == 8);
	CHECK(sct_dmr_field_poll(d, &n) == 0);
	const uint8_t ok[] = { 0x84, 0xA9, 0x61, 0x00, 0x02, 0x00, 0x2F, 0x00 };
	sct_dmr_packet(ok, sizeof(ok), 10);
	CHECK(sct_dmr_field_poll(d, &n) == 1);
	CHECK(sct_dmr_field_poll(d, &n) == 0);
	// Refused, and not counted against the engine
	sct_dmr_get_stats(&st);
	uint32_t naks = st.naks;
	CHECK(sct_dmr_field_begin(SCT_TYPE_CONTROL, 0x2F, &v, 1, false));
	const uint8_t no[] = { 0x84, 0xA9, 0x61, 0x00, 0x02, 0x00, 0x2F, SCT_ACK_UNKNOWN };
	sct_dmr_packet(no, sizeof(no), 20);
	CHECK(sct_dmr_field_poll(d, &n) == -1);
	sct_dmr_get_stats(&st);
	CHECK(st.naks == naks);
	// A query: the read bit on, the ID alone; the answer's data comes back
	nsent = 0;
	CHECK(sct_dmr_field_begin(SCT_TYPE_DMR, SCT_F_DMR_CC, NULL, 0, true));
	CHECK(nsent == 1 && sent[0].pkt[5] == (SCT_TYPE_DMR | SCT_TYPE_READ) && sent[0].pkt[4] == 1 && sent[0].pkt[6] == SCT_F_DMR_CC);
	const uint8_t cc[] = { 0x84, 0xA9, 0x61, 0x00, 0x02, 0x25, SCT_F_DMR_CC, 0x07 };
	sct_dmr_packet(cc, sizeof(cc), 30);
	CHECK(sct_dmr_field_poll(d, &n) == 1 && n == 1 && d[0] == 7);
	// A bench change of an option byte goes out at once and stays for the next configure
	nsent = 0;
	v = 0x82;
	CHECK(sct_dmr_call_option(2, &v, true) && v == 0x82);
	uint8_t flen = 0;
	const uint8_t *opt = sent_field(0, SCT_F_DMR_CALL_OPTION, &flen);
	CHECK(nsent == 1 && opt != NULL && flen == 5 && opt[2] == 0x82);
	v = 0;
	CHECK(sct_dmr_call_option(2, &v, false) && v == 0x82);
	CHECK(!sct_dmr_call_option(5, &v, false));
	v = port.call_option[2];
	sct_dmr_call_option(2, &v, true);
	sct_dmr_modem.stop();
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
	test_dmr_tx_answer_on_air();
	test_dmr_rx();
	test_dmr_rx_outbound();
	test_dmr_rx_voice_count();
	test_dmr_options();
	test_dmr_carrier();
	test_dmr_carrier_ends_call();
	test_dmr_health();
	test_dmr_carrier_settle();
	test_dmr_tx_early_end();
	test_dmr_tx_fields();
	test_dmr_codec_start();
	test_chip_answers();
	test_field_access();
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
