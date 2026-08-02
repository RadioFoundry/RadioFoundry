// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 DMR: register sequences, burst status decode, receive de-duplication, and the TX queue / slot scheduling of
// the slot engine, against a fake config port, voice port and RF switch with a fake microsecond clock
#include <stdio.h>
#include <string.h>
#include "modem/hrc6000/hrc6000_dmr.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- Fake buses ---

typedef struct {
	uint8_t page, reg, n;
	bool read;
	uint8_t data[32];
} xfer_t;

static xfer_t xlog[4096];
static int nx;
static uint8_t rd[8][256];	// what reads return (page 4 status registers, page 2 RX RAM)
static uint8_t voice_rx[27];
static uint8_t voice_tx_last[27];
static int voice_writes, voice_reads;

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint8_t page = hdr[0] & 7u, reg = hdr[1];
	bool read = (hdr[0] & 0x80u) != 0;
	if (nx < (int)(sizeof(xlog) / sizeof(xlog[0]))) {
		xfer_t *x = &xlog[nx];
		x->page = page;
		x->reg = reg;
		x->n = (uint8_t)len;
		x->read = read;
		if (!read && tx != NULL) {
			memcpy(x->data, tx, len < 32 ? len : 32);
		}
	}
	nx++;
	if (read && rx != NULL) {
		for (uint16_t i = 0; i < len; i++) {
			rx[i] = rd[page][(uint8_t)(reg + i)];
		}
	}
	return true;
}

static bool fake_voice(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	if (write) {
		memcpy(voice_tx_last, buf, len);
		voice_writes++;
	} else {
		memcpy(buf, voice_rx, len);
		voice_reads++;
	}
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc6000_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, fake_voice };
static hrc6000_t dev;

static uint32_t t_us;
static int rf_on_calls, rf_off_calls, locks, unlocks, notifies;
static bool rf_ok = true;

static bool fake_rf(void *ctx, bool tx)
{
	if (tx) {
		rf_on_calls++;
	} else {
		rf_off_calls++;
	}
	return rf_ok;
}
static uint32_t fake_now(void *ctx) { return t_us; }
static void fake_lock(void *ctx) { locks++; }
static void fake_unlock(void *ctx) { unlocks++; }
static void fake_notify(void *ctx) { notifies++; }

static const hrc6000_dmr_ops_t ops = { fake_rf, fake_now, fake_lock, fake_unlock, fake_notify, NULL, NULL };
static hrc6000_dmr_t eng;

static void log_reset(void)
{
	nx = 0;
}

// Values written to a page-4 register in log order; returns how many, fills out[]
static int writes_of(uint8_t reg, uint8_t *out, int max)
{
	int n = 0;
	for (int i = 0; i < nx && i < (int)(sizeof(xlog) / sizeof(xlog[0])); i++) {
		if (!xlog[i].read && xlog[i].page == 4 && xlog[i].reg == reg && xlog[i].n == 1) {
			if (n < max) {
				out[n] = xlog[i].data[0];
			}
			n++;
		}
	}
	return n;
}

static int last_write(uint8_t page, uint8_t reg)
{
	int v = -1;
	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].reg == reg) {
			v = xlog[i].data[0];
		}
	}
	return v;
}

// Index of the first write to page/reg in the log, -1 if none
static int first_write_at(uint8_t page, uint8_t reg)
{
	for (int i = 0; i < nx; i++) {
		if (!xlog[i].read && xlog[i].page == page && xlog[i].reg == reg) {
			return i;
		}
	}
	return -1;
}

// --- Events ---

static dmr_event_t evs[256];
static dmr_burst_t bursts[256];
static int nev;

static void drain(void)
{
	dmr_event_t ev;
	dmr_burst_t b;
	while (hrc6000_dmr_pop(&eng, &ev, &b)) {
		if (nev < 256) {
			evs[nev] = ev;
			if (ev.kind == DMR_EV_BURST_RX) {
				bursts[nev] = b;
			}
			nev++;
		}
	}
}

static int count_ev(uint8_t kind)
{
	int n = 0;
	for (int i = 0; i < nev; i++) {
		n += evs[i].kind == kind;
	}
	return n;
}

static const dmr_burst_t *last_burst(void)
{
	for (int i = nev - 1; i >= 0; i--) {
		if (evs[i].kind == DMR_EV_BURST_RX) {
			return &bursts[i];
		}
	}
	return NULL;
}

static void fresh_engine(void)
{
	memset(rd, 0, sizeof(rd));
	t_us = 1000000;
	rf_on_calls = rf_off_calls = 0;
	rf_ok = true;
	hrc6000_dmr_init(&eng, &dev, &ops);
	hrc6000_dmr_run(&eng);
	drain();
	nev = 0;
	log_reset();
}

static void ts_at(uint32_t t)
{
	t_us = t;
	hrc6000_dmr_isr_timeslot(&eng);
}

static void report_at(uint32_t t, uint8_t r51, uint8_t r52, uint8_t r5f)
{
	t_us = t;
	rd[4][0x82] = 0x08;
	rd[4][0x51] = r51;
	rd[4][0x52] = r52;
	rd[4][0x5F] = r5f;
	hrc6000_dmr_isr_sys(&eng);
	rd[4][0x82] = 0;
}

// --- Tests ---

static void test_chip_sequences(void)
{
	log_reset();
	CHECK(hrc6000_dmr_enter(&dev, 0x11, 0x22, 7));
	CHECK(last_write(4, 0x01) == 0xF0);
	CHECK(last_write(4, 0x45) == 0x11 && last_write(4, 0x46) == 0x22);
	CHECK(last_write(4, 0x10) == 0x6E);	// operation value, not the init 0x6B
	CHECK(last_write(4, 0xE0) == 0xC9);
	CHECK(last_write(4, 0xE4) == 0x27);
	CHECK(last_write(4, 0x26) == 0xFD);

	log_reset();
	CHECK(hrc6000_dmr_chip_rx_restart(&dev, 0));
	uint8_t v[8];
	CHECK(writes_of(0x40, v, 8) == 1 && v[0] == 0xC3);
	CHECK(writes_of(0x41, v, 8) == 4 && v[0] == 0x20 && v[1] == 0x00 && v[2] == 0x20 && v[3] == 0x50);
	CHECK(first_write_at(4, 0x40) < first_write_at(4, 0x41));

	log_reset();
	hrc6000_dmr_set_cc(&dev, 2);
	CHECK(last_write(4, 0x1F) == 0x20);
	log_reset();
	hrc6000_dmr_set_sync(&dev, DMR_SYNC_TS2);
	CHECK(nx == 1 && xlog[0].page == 1 && xlog[0].reg == 0x04 && xlog[0].n == 6 && xlog[0].data[0] == 0xD7
	      && xlog[0].data[5] == 0xF5);

	CHECK(hrc6000_dmr_access_reg(DMR_ACCESS_POLITE_CC) == 0xA2);
	CHECK(hrc6000_dmr_access_reg(DMR_ACCESS_IMPOLITE) == 0x02);
	CHECK(hrc6000_dmr_reg50(DMR_BURST_VOICE, 0) == 0x08 && hrc6000_dmr_reg50(DMR_BURST_VOICE, 5) == 0x58);
	CHECK(hrc6000_dmr_reg50(DMR_BURST_VOICE_LC_HEADER, 0) == 0x10);
	CHECK(hrc6000_dmr_reg50(DMR_BURST_TERMINATOR_LC, 0) == 0x20);
	CHECK(hrc6000_dmr_reg50(DMR_BURST_CSBK, 0) == 0x30);
	CHECK(hrc6000_dmr_payload_len(DMR_BURST_RATE_34_DATA) == 18 && hrc6000_dmr_payload_len(DMR_BURST_RATE_1_DATA) == 24);
	CHECK(hrc6000_dmr_payload_len(DMR_BURST_CSBK) == 12 && hrc6000_dmr_payload_len(DMR_BURST_VOICE) == 27);
}

static void test_decode(void)
{
	hrc6000_rx_status_t s;
	hrc6000_dmr_decode(0x91, 0x24, 0x00, &s);	// voice A, voice sync, CC 2, TC 1, MS
	CHECK(s.kind == DMR_BURST_VOICE && s.voice_index == 0 && s.crc_ok && !s.pi && s.cc == 2 && s.tc == 1);
	CHECK(s.sync == DMR_SYNC_MS && s.sync_class == 1);
	hrc6000_dmr_decode(0xE0, 0x10, 0x01, &s);	// voice F, no sync (EMB), BS
	CHECK(s.kind == DMR_BURST_VOICE && s.voice_index == 5 && s.sync == DMR_SYNC_BS);
	hrc6000_dmr_decode(0x92, 0x00, 0x00, &s);	// 0x9 with data sync is idle, not voice A
	CHECK(s.kind == DMR_BURST_IDLE);
	hrc6000_dmr_decode(0x36, 0x00, 0x02, &s);	// CSBK, CRC error, TS1 direct
	CHECK(s.kind == DMR_BURST_CSBK && !s.crc_ok && s.sync == DMR_SYNC_TS1);
	hrc6000_dmr_decode(0x1A, 0x00, 0x03, &s);	// voice header, PI, TS2 direct
	CHECK(s.kind == DMR_BURST_VOICE_LC_HEADER && s.pi && s.sync == DMR_SYNC_TS2);
	hrc6000_dmr_decode(0x83, 0x00, 0x00, &s);	// reverse channel
	CHECK(s.kind == HRC6000_KIND_NONE);
	hrc6000_dmr_decode(0x81, 0x00, 0x00, &s);	// voice sync with burst number 0: unusable
	CHECK(s.kind == HRC6000_KIND_NONE);
	hrc6000_dmr_decode(0x30, 0x00, 0x00, &s);	// EMB-only report without a voice number
	CHECK(s.kind == HRC6000_KIND_NONE);
	hrc6000_dmr_decode(0xD2, 0x00, 0x00, &s);	// reserved data type 13
	CHECK(s.kind == HRC6000_KIND_NONE);
}

static void test_rx(void)
{
	fresh_engine();
	CHECK(eng.running);
	uint32_t t = 2000000;
	ts_at(t);
	CHECK(last_write(4, 0x41) == 0x50);	// receive the next slot
	drain();
	CHECK(count_ev(DMR_EV_SLOT_TICK) == 1);

	for (int i = 0; i < 27; i++) {
		voice_rx[i] = (uint8_t)i;
	}
	// The LC buffer holds the previous call's LC when this reception starts
	static const uint8_t old_lc[9] = { 0, 0, 0, 0, 0, 9, 0, 0, 1 };
	static const uint8_t new_lc[9] = { 0, 0, 0, 0, 0, 91, 0x31, 0x00, 0x01 };
	memcpy(&rd[2][0], old_lc, 9);

	// Voice A reported 4 ms after the timeslot interrupt: in step
	report_at(t + 4000, 0x91, 0x20, 0x00);
	drain();
	const dmr_burst_t *b = last_burst();
	CHECK(b != NULL && b->kind == DMR_BURST_VOICE && b->voice_index == 0 && b->in_step && b->crc_ok);
	CHECK(b != NULL && b->cc == 2 && b->len == 27 && b->payload[26] == 26 && b->ts == 1);
	CHECK(last_write(4, 0x83) == 0x08);	// flags cleared with the value read
	CHECK(eng.st.last_report_ms == 4);

	// Its EMB report mid-slot: dropped
	int before = count_ev(DMR_EV_BURST_RX);
	report_at(t + 16000, 0xA0, 0x20, 0x00);
	drain();
	CHECK(count_ev(DMR_EV_BURST_RX) == before);
	CHECK(eng.st.rx_dup >= 1);

	// B..F, one per 60 ms, each with an EMB report 12 ms later
	memcpy(&rd[2][0], new_lc, 9);
	for (int k = 1; k <= 5; k++) {
		t += 30000;
		ts_at(t);
		t += 30000;
		ts_at(t);
		report_at(t + 3500, (uint8_t)(((0x8 | (k + 1)) << 4) | 0), 0x20, 0x00);
		report_at(t + 15500, (uint8_t)(((0x8 | (k + 1)) << 4) | 0), 0x20, 0x00);
	}
	drain();
	int voices = 0, last_idx = -1;
	bool in_order = true, f_lc = false;
	for (int i = 0; i < nev; i++) {
		if (evs[i].kind == DMR_EV_BURST_RX && bursts[i].kind == DMR_BURST_VOICE) {
			if (bursts[i].voice_index != (uint8_t)(last_idx + 1)) {
				in_order = false;
			}
			last_idx = bursts[i].voice_index;
			voices++;
			if (bursts[i].voice_index == 5) {
				f_lc = bursts[i].lc_valid && memcmp(bursts[i].lc, new_lc, 9) == 0;
			}
		}
	}
	CHECK(voices == 6 && in_order);
	CHECK(f_lc);	// the embedded LC differs from the stale one: valid at F
	CHECK(eng.st.rx_voice == 6);

	// A voice header heard: its LC, valid
	t += 30000;
	ts_at(t);
	t += 30000;
	ts_at(t);
	memcpy(&rd[2][0], new_lc, 9);
	report_at(t + 4000, 0x12, 0x20, 0x00);
	drain();
	b = last_burst();
	CHECK(b != NULL && b->kind == DMR_BURST_VOICE_LC_HEADER && b->lc_valid && b->len == 12
	      && memcmp(b->lc, new_lc, 9) == 0);

	// Rate 3/4 block: 18 bytes from page 2
	t += 30000;
	ts_at(t);
	report_at(t + 4000, 0x82, 0x20, 0x00);
	drain();
	b = last_burst();
	CHECK(b != NULL && b->kind == DMR_BURST_RATE_34_DATA && b->len == 18);

	// Silence: after 13 more ticks without a burst the receiver is restarted
	uint32_t restarts = eng.st.rx_restarts;
	for (int k = 0; k < 15; k++) {
		t += 30000;
		ts_at(t);
	}
	CHECK(eng.st.rx_restarts == restarts + 1);
}

static void test_rx_stale_lc(void)
{
	fresh_engine();
	static const uint8_t old_lc[9] = { 0, 0, 0, 0, 0, 9, 0, 0, 1 };
	memcpy(&rd[2][0], old_lc, 9);	// a late entry: the chip still holds the last call's LC
	uint32_t t = 3000000;
	bool f_valid[2] = { true, true };
	int sf = 0;
	for (int n = 0; n < 12; n++) {
		ts_at(t);
		t += 30000;
		ts_at(t);
		report_at(t + 4000, (uint8_t)(((0x8 | (n % 6 + 1)) << 4) | (n % 6 == 0 ? 1 : 0)), 0x20, 0x00);
		t += 30000;
		drain();
		const dmr_burst_t *b = last_burst();
		if (b != NULL && b->voice_index == 5) {
			f_valid[sf++] = b->lc_valid;
		}
	}
	CHECK(sf == 2);
	CHECK(!f_valid[0]);	// unchanged LC at the first F: not trusted
	CHECK(f_valid[1]);	// after more than a superframe it is
}

static void test_out_of_step_resync(void)
{
	fresh_engine();
	uint32_t t = 4000000;
	uint32_t restarts = eng.st.rx_restarts;
	for (int k = 0; k < 3; k++) {
		ts_at(t);
		report_at(t + 27000, 0x32, 0x20, 0x00);	// 27 ms after the tick: the chip's timing is off the sender's
		t += 60000;
		ts_at(t - 30000);
	}
	ts_at(t);
	CHECK(eng.st.rx_out_of_step >= 2);
	CHECK(eng.st.rx_restarts > restarts);
	drain();
	CHECK(count_ev(DMR_EV_SYNC_LOST) >= 1);
}

static dmr_tx_burst_t mk(uint8_t kind, uint8_t idx, bool host_crc)
{
	dmr_tx_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.voice_index = idx;
	b.host_crc = host_crc;
	if (kind == DMR_BURST_VOICE) {
		b.len = 27;
		memset(b.payload, 0x40 + idx, 27);
	} else {
		b.len = host_crc ? 12 : 9;
		for (int i = 0; i < b.len; i++) {
			b.payload[i] = (uint8_t)(0xA0 + i);
		}
	}
	return b;
}

// One timeslot interrupt with the RF TX interrupt 1.5 ms before it when a burst was set up at the previous one;
// returns the 0x50 value written at this tick (-1 none) and the 0x41 value (-1 none)
static bool pending_burst;

static void tx_tick(uint32_t t, int *r50, int *r41)
{
	if (pending_burst) {
		t_us = t - 1500;
		hrc6000_dmr_isr_rftx(&eng);
	}
	log_reset();
	ts_at(t);
	*r50 = last_write(4, 0x50);
	*r41 = last_write(4, 0x41);
	pending_burst = *r41 == 0x80;
}

static void test_tx_voice_dmo(void)
{
	fresh_engine();
	pending_burst = false;
	CHECK(!hrc6000_dmr_is_transmitting(&eng));
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 0, DMR_ACCESS_POLITE_CC));
	CHECK(last_write(4, 0x20) == 0x00 && last_write(4, 0x21) == 0xA2 && last_write(4, 0x22) == 0x86);
	CHECK(last_write(4, 0x40) == 0xE3);	// own timing
	CHECK(hrc6000_dmr_is_transmitting(&eng));
	CHECK(!hrc6000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 0, DMR_ACCESS_POLITE_CC));	// one at a time

	static const uint8_t lc[9] = { 0, 0, 0, 0, 0, 9, 0x31, 0x00, 0x01 };
	hrc6000_dmr_set_embedded_lc(&eng, lc);
	dmr_tx_burst_t pi = mk(DMR_BURST_PI_HEADER, 0, false);
	CHECK(hrc6000_dmr_tx_queue(&eng, &pi));
	hrc6000_dmr_tx_abort(&eng);	// before any timeslot interrupt: nothing went out
	drain();
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);	// aborted before any burst: done at once
	CHECK(!hrc6000_dmr_is_transmitting(&eng));
	nev = 0;

	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 0, DMR_ACCESS_POLITE_CC));
	hrc6000_dmr_set_embedded_lc(&eng, lc);
	for (int i = 0; i < 3; i++) {
		dmr_tx_burst_t h = mk(DMR_BURST_VOICE_LC_HEADER, 0, false);
		CHECK(hrc6000_dmr_tx_queue(&eng, &h));
	}
	for (int i = 0; i < 6; i++) {
		dmr_tx_burst_t v = mk(DMR_BURST_VOICE, (uint8_t)i, false);
		CHECK(hrc6000_dmr_tx_queue(&eng, &v));
	}
	dmr_tx_burst_t term = mk(DMR_BURST_TERMINATOR_LC, 0, false);
	CHECK(hrc6000_dmr_tx_queue(&eng, &term));
	CHECK(hrc6000_dmr_tx_queue_space(&eng) == HRC6000_DMR_TXQ_DEPTH - 1 - 10);
	hrc6000_dmr_tx_end(&eng);

	static const int want50[] = { 0x10, 0x10, 0x10, 0x08, 0x18, 0x28, 0x38, 0x48, 0x58, 0x20 };
	int got50[16], n50 = 0, setup_tick[16];
	bool alternates = true, tx_through = true, emb_written = false, hdr_parity_zero = true;
	int done_tick = -1;
	uint32_t t = 5000000;
	for (int k = 0; k < 40 && done_tick < 0; k++) {
		int r50, r41;
		tx_tick(t, &r50, &r41);
		if (r41 == 0x80) {
			if (n50 < 16) {
				setup_tick[n50] = k;
				got50[n50++] = r50;
			}
			if (n50 >= 2 && setup_tick[n50 - 1] - setup_tick[n50 - 2] != 2) {
				alternates = false;
			}
			for (int i = 0; i < nx; i++) {
				if (xlog[i].page == 2 && xlog[i].reg == 0x00 && !xlog[i].read) {
					if (r50 == 0x08 && memcmp(xlog[i].data, lc, 9) == 0) {
						emb_written = true;	// the embedded LC at voice A
					}
					if (r50 == 0x10 && (xlog[i].n != 12 || xlog[i].data[9] || xlog[i].data[10] || xlog[i].data[11])) {
						hdr_parity_zero = false;
					}
				}
			}
		} else if (r41 >= 0 && r41 != 0x00 && hrc6000_dmr_is_transmitting(&eng)) {
			alternates = false;
		}
		drain();
		if (count_ev(DMR_EV_TX_DONE) > 0) {
			done_tick = k;
		} else if (!hrc6000_dmr_is_transmitting(&eng)) {
			tx_through = false;
		}
		t += 30000;
	}
	CHECK(n50 == 10);
	CHECK(memcmp(got50, want50, sizeof(want50)) == 0);
	CHECK(alternates);
	CHECK(tx_through);	// "transmitting" from the slot state for the whole call
	CHECK(emb_written);
	CHECK(hdr_parity_zero);	// LC header: 9 bytes + 3 zero bytes for the chip's RS parity
	CHECK(rf_on_calls == 10 && rf_off_calls == 10);
	CHECK(done_tick >= setup_tick[9] + 2);	// after the terminator's own slot (gotchas 3.2)
	CHECK(!hrc6000_dmr_is_transmitting(&eng));
	CHECK(last_write(4, 0x40) == 0xC3);	// passive timing again, CRC_MCU_Control clear
	uint8_t v[8];
	CHECK(writes_of(0x41, v, 8) >= 4 && v[0] == 0x20);	// direct mode: receiver restarted
	CHECK(count_ev(DMR_EV_TX_SLOT_STARTED) == 10 && count_ev(DMR_EV_TX_SLOT_ENDED) == 10);
	CHECK(eng.st.tx_bursts == 10 && eng.st.tx_underrun == 0);
	CHECK(memcmp(voice_tx_last, (const uint8_t[]){ 0xB9, 0xE8, 0x81 }, 3) == 0);	// terminator: silence AMBE
}

static void test_tx_crc_and_underrun(void)
{
	fresh_engine();
	pending_burst = false;
	// ASAP with no timing becomes own timing
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_ASAP, 0, DMR_ACCESS_IMPOLITE));
	CHECK(last_write(4, 0x40) == 0xE3 && last_write(4, 0x21) == 0x02);
	dmr_tx_burst_t csbk = mk(DMR_BURST_CSBK, 0, true);
	dmr_tx_burst_t blk = mk(DMR_BURST_RATE_12_DATA, 0, false);
	hrc6000_dmr_tx_queue(&eng, &csbk);
	hrc6000_dmr_tx_queue(&eng, &blk);
	uint32_t t = 6000000;
	int r50, r41;
	tx_tick(t, &r50, &r41);
	CHECK(r41 == 0x80 && r50 == 0x30);
	CHECK(last_write(4, 0x40) == 0xEB);	// CRC_MCU_Control set before the CSBK
	CHECK(first_write_at(4, 0x40) < first_write_at(4, 0x50));
	t += 30000;
	tx_tick(t, &r50, &r41);
	CHECK(r41 == 0x00);
	t += 30000;
	tx_tick(t, &r50, &r41);
	CHECK(r41 == 0x80 && r50 == 0x70 && last_write(4, 0x40) == 0xE3);	// cleared for a block without its CRC
	// Data list ran dry without tx_end: our slot stays empty, still transmitting
	t += 30000;
	tx_tick(t, &r50, &r41);
	t += 30000;
	tx_tick(t, &r50, &r41);
	CHECK(r41 == 0x00 && hrc6000_dmr_is_transmitting(&eng));
	hrc6000_dmr_tx_end(&eng);
	for (int k = 0; k < 6; k++) {
		t += 30000;
		tx_tick(t, &r50, &r41);
	}
	drain();
	CHECK(count_ev(DMR_EV_TX_DONE) == 1 && !hrc6000_dmr_is_transmitting(&eng));

	// Voice underrun: AMBE silence with the next burst number
	nev = 0;
	uint32_t underruns = eng.st.tx_underrun;
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 0, DMR_ACCESS_IMPOLITE));
	dmr_tx_burst_t a = mk(DMR_BURST_VOICE, 0, false);
	hrc6000_dmr_tx_queue(&eng, &a);
	tx_tick(t += 30000, &r50, &r41);
	CHECK(r50 == 0x08);
	tx_tick(t += 30000, &r50, &r41);
	voice_tx_last[0] = 0;
	tx_tick(t += 30000, &r50, &r41);
	CHECK(r41 == 0x80 && r50 == 0x18 && voice_tx_last[0] == 0xB9);
	CHECK(eng.st.tx_underrun == underruns + 1);

	// Queue depth
	int queued = 0;
	dmr_tx_burst_t v = mk(DMR_BURST_VOICE, 2, false);
	while (hrc6000_dmr_tx_queue(&eng, &v) && queued < 100) {
		queued++;
	}
	CHECK(queued == HRC6000_DMR_TXQ_DEPTH - 1 && queued >= 8);
	CHECK(hrc6000_dmr_tx_queue_space(&eng) == 0);
	hrc6000_dmr_tx_abort(&eng);
	CHECK(hrc6000_dmr_tx_queue_space(&eng) == HRC6000_DMR_TXQ_DEPTH - 1);
	for (int k = 0; k < 6; k++) {
		tx_tick(t += 30000, &r50, &r41);
	}
	drain();
	CHECK(count_ev(DMR_EV_TX_DONE) == 1 && !hrc6000_dmr_is_transmitting(&eng));
}

static void test_tx_aligned_rmo(void)
{
	fresh_engine();
	pending_burst = false;
	dmr_config_t cfg = { .own_id = 3100001, .cc = 1, .access = DMR_ACCESS_POLITE_CC, .repeater = true };
	hrc6000_dmr_configure(&eng, &cfg);
	CHECK(last_write(4, 0x14) == (3100001 >> 16) && last_write(4, 0x16) == (3100001 & 0xFF));
	CHECK(last_write(4, 0x1F) == 0x10);
	// The repeater's CACH: the slot starting at tick k is TS1 on odd ticks, TS2 on even ones
	uint32_t t = 7000000;
	for (int k = 0; k < 8; k++) {
		uint32_t next = eng.tick + 1;
		rd[4][0x52] = (next & 1) ? 0x00 : 0x04;
		ts_at(t);
		t += 30000;
	}
	CHECK(eng.phase_ts[1] == 1 && eng.phase_ts[0] == 2);
	log_reset();
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_ALIGNED, 2, DMR_ACCESS_POLITE_CC));
	CHECK(last_write(4, 0x40) == -1);	// passive timing kept
	dmr_tx_burst_t h = mk(DMR_BURST_VOICE_LC_HEADER, 0, false);
	hrc6000_dmr_tx_queue(&eng, &h);
	hrc6000_dmr_tx_end(&eng);
	int setup_tick = -1;
	for (int k = 0; k < 4 && setup_tick < 0; k++) {
		uint32_t next = eng.tick + 1;
		rd[4][0x52] = (next & 1) ? 0x00 : 0x04;
		int r50, r41;
		tx_tick(t, &r50, &r41);
		if (r41 == 0x80) {
			setup_tick = (int)eng.tick;
		}
		t += 30000;
	}
	CHECK(setup_tick >= 0 && ((setup_tick + 1) & 1) == 0);	// set up for a TS2 slot
	for (int k = 0; k < 6; k++) {
		int r50, r41;
		tx_tick(t, &r50, &r41);
		t += 30000;
	}
	drain();
	CHECK(count_ev(DMR_EV_TX_DONE) == 1);
	uint8_t v[8];
	CHECK(writes_of(0x41, v, 8) >= 1);
	CHECK(last_write(4, 0x41) == 0x50);	// repeater: back to receiving on its timing, no resync
}

static void test_tx_rejected_and_stall(void)
{
	fresh_engine();
	pending_burst = false;
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 0, DMR_ACCESS_POLITE_CC));
	dmr_tx_burst_t h = mk(DMR_BURST_VOICE_LC_HEADER, 0, false);
	hrc6000_dmr_tx_queue(&eng, &h);
	hrc6000_dmr_tx_queue(&eng, &h);
	uint32_t t = 8000000;
	int r50, r41;
	tx_tick(t, &r50, &r41);
	t_us = t + 5000;
	rd[4][0x82] = 0x80;	// send request rejected: channel busy
	hrc6000_dmr_isr_sys(&eng);
	rd[4][0x82] = 0;
	for (int k = 0; k < 6; k++) {
		tx_tick(t += 30000, &r50, &r41);
	}
	drain();
	CHECK(count_ev(DMR_EV_TX_REJECTED) == 1 && count_ev(DMR_EV_TX_DONE) == 1);
	CHECK(eng.st.tx_bursts == 1);

	// Timing stops mid-transmission: the watchdog ends it and restarts the receiver
	nev = 0;
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 0, DMR_ACCESS_IMPOLITE));
	hrc6000_dmr_tx_queue(&eng, &h);
	tx_tick(t += 30000, &r50, &r41);
	t_us = t + 28500;
	hrc6000_dmr_isr_rftx(&eng);	// PA on, then no more timeslot interrupts
	t_us = t + 100000;
	hrc6000_dmr_poll(&eng);
	CHECK(!eng.pa_on);		// PA dropped by the fallback
	t_us = t + 260000;
	hrc6000_dmr_poll(&eng);
	drain();
	CHECK(count_ev(DMR_EV_TX_DONE) == 1 && count_ev(DMR_EV_SYNC_LOST) == 1);
	CHECK(!hrc6000_dmr_is_transmitting(&eng) && eng.st.stalls == 1);
	CHECK(locks == unlocks);
}

static void test_cc_follow(void)
{
	fresh_engine();
	uint32_t t = 9500000;
	ts_at(t);
	report_at(t + 4000, 0x32, 0x20, 0x00);	// CC 2 heard twice within 200 ms
	t += 30000;
	ts_at(t);
	report_at(t + 4000, 0x32, 0x20, 0x00);
	CHECK(last_write(4, 0x1F) == 0x20);
	uint32_t restarts = eng.st.rx_restarts;
	ts_at(t += 30000);			// the receiver restarts at the next tick, no busy wait in the ISR
	CHECK(eng.st.rx_restarts == restarts + 1);

	// A fixed filter: written at once, never followed
	hrc6000_dmr_set_cc_filter(&eng, 5);
	CHECK(last_write(4, 0x1F) == 0x50);
	log_reset();
	for (int k = 0; k < 4; k++) {
		ts_at(t += 30000);
		report_at(t + 4000, 0x32, 0x20, 0x00);
	}
	CHECK(last_write(4, 0x1F) == -1);
}

static void test_halt(void)
{
	fresh_engine();
	CHECK(hrc6000_dmr_tx_begin(&eng, DMR_TIMING_OWN, 0, DMR_ACCESS_IMPOLITE));
	hrc6000_dmr_halt(&eng);
	drain();
	CHECK(!eng.running && count_ev(DMR_EV_TX_DONE) == 1);
	uint32_t ticks = eng.st.ticks;
	ts_at(9000000);
	CHECK(eng.st.ticks == ticks);	// ISRs ignored when off
}

int main(void)
{
	hrc6000_attach(&dev, &bus);
	test_chip_sequences();
	test_decode();
	test_rx();
	test_rx_stale_lc();
	test_out_of_step_resync();
	test_tx_voice_dmo();
	test_tx_crc_and_underrun();
	test_tx_aligned_rmo();
	test_tx_rejected_and_stall();
	test_cc_follow();
	test_halt();
	if (failures) {
		printf("# hrc6000DmrTest: %d FAILED\n", failures);
		return 1;
	}
	printf("# hrc6000DmrTest: PASSED\n");
	return 0;
}
