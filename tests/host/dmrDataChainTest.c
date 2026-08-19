// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DMR data end to end on the host: data and control bursts reported by a model of the HR_C6000 (class 2 receive
// reports, the payload in page 2) go through the real slot engine (drivers/modem/hrc6000/hrc6000_dmr.c), a board-like
// task, the real call stack (core/dmr/call/dmr_call.c, its data / CSBK dispatch) and the real data service
// (core/dmr/dmrDataService.c). What the service sends back goes out through the stack's burst list TX and the engine:
// the page 2 writes, 0x50 (the data type) and 0x40 bit 3 (CRC_MCU_Control) at each set-up are captured and decoded
// again with core/dmr/dmrData.c. The glue between the stack and the service is the firmware's (core/radio/radio_sm.c)
// in miniature.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modem/hrc6000/hrc6000_dmr.h"
#include "dmr/call/dmr_call.h"
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define OUR_ID		1234
#define OTHER_ID	0x234567
#define CC		2

// --- Fake chip: reads from rd[page][reg]; writes to page 2, 0x40 and 0x50 captured as transmitted bursts ---

static uint8_t rd[8][256];

typedef struct {
	uint8_t kind;		// the data type written to 0x50 (bits 7-4)
	bool mcu_crc;		// 0x40 bit 3 at that time
	uint8_t payload[24];	// page 2 from 0x00 as last written
	uint32_t tick;
} txrec_t;

static txrec_t txl[128];
static int ntx;
static uint8_t page2[24];
static uint8_t reg40 = 0xC3;
static uint8_t last41;
static uint32_t cur_tick;

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint8_t page = hdr[0] & 7u, reg = hdr[1];
	if ((hdr[0] & 0x80u) != 0 && rx != NULL) {
		for (uint16_t i = 0; i < len; i++) {
			rx[i] = rd[page][(uint8_t)(reg + i)];
		}
		return true;
	}
	if (tx == NULL) {
		return true;
	}
	if (page == 4 && reg == 0x83 && len == 1) {
		rd[4][0x82] &= (uint8_t)~tx[0];
	} else if (page == 2 && reg == 0x00) {
		memset(page2, 0, sizeof(page2));
		memcpy(page2, tx, len < sizeof(page2) ? len : sizeof(page2));
	} else if (page == 4 && reg == 0x40 && len == 1) {
		reg40 = tx[0];
	} else if (page == 4 && reg == 0x41 && len == 1) {
		last41 = tx[0];
	} else if (page == 4 && reg == 0x50 && len == 1 && ntx < (int)(sizeof(txl) / sizeof(txl[0]))) {
		txrec_t *t = &txl[ntx++];
		t->kind = (uint8_t)(tx[0] >> 4);
		t->mcu_crc = (reg40 & 0x08u) != 0;
		memcpy(t->payload, page2, sizeof(page2));
		t->tick = cur_tick;
	}
	return true;
}

static bool fake_voice(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	if (!write) {
		memset(buf, 0, len);
	}
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc6000_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, fake_voice };
static hrc6000_t dev;
static hrc6000_dmr_t eng;
static uint32_t t_us;
static unsigned pa_on_calls;

static bool fake_rf(void *ctx, bool tx) { if (tx) pa_on_calls++; return true; }
static uint32_t fake_now(void *ctx) { return t_us; }

static const hrc6000_dmr_ops_t ops = { fake_rf, fake_now, NULL, NULL, NULL, NULL, NULL };

// --- The board glue: hal_dmr_modem_t over the engine, and a task draining the engine's events into the stack ---

static dmr_event_cb_t stack_cb;
static void *stack_ctx;

static bool m_start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	hrc6000_dmr_configure(&eng, cfg);
	stack_cb = cb;
	stack_ctx = ctx;
	return true;
}
static void m_stop(void) { stack_cb = NULL; }
static void m_configure(const dmr_config_t *cfg) { hrc6000_dmr_configure(&eng, cfg); }
static void m_rx_restart(void) { hrc6000_dmr_rx_restart(&eng); }
static void m_rx_lock_slot(int8_t ts) { hrc6000_dmr_rx_lock_slot(&eng, ts); }
static bool m_tx_begin(dmr_timing_t t, int8_t ts, dmr_access_t a) { return hrc6000_dmr_tx_begin(&eng, t, ts, a); }
static bool m_tx_queue(const dmr_tx_burst_t *b) { return hrc6000_dmr_tx_queue(&eng, b); }
static uint8_t m_tx_queue_space(void) { return hrc6000_dmr_tx_queue_space(&eng); }
static void m_tx_end(void) { hrc6000_dmr_tx_end(&eng); }
static void m_tx_abort(void) { hrc6000_dmr_tx_abort(&eng); }
static void m_set_embedded_lc(const uint8_t lc[9]) { hrc6000_dmr_set_embedded_lc(&eng, lc); }
static void m_set_cc_filter(int8_t cc) { hrc6000_dmr_set_cc_filter(&eng, cc); }
static bool m_is_transmitting(void) { return hrc6000_dmr_is_transmitting(&eng); }

static const hal_dmr_modem_t modem = {
	.start = m_start, .stop = m_stop, .configure = m_configure, .rx_restart = m_rx_restart,
	.rx_lock_slot = m_rx_lock_slot, .tx_begin = m_tx_begin, .tx_queue = m_tx_queue,
	.tx_queue_space = m_tx_queue_space, .tx_end = m_tx_end, .tx_abort = m_tx_abort,
	.set_embedded_lc = m_set_embedded_lc, .set_cc_filter = m_set_cc_filter, .is_transmitting = m_is_transmitting,
};

static void board_task(void)
{
	dmr_event_t ev;
	dmr_burst_t b;
	while (hrc6000_dmr_pop(&eng, &ev, &b)) {
		if (stack_cb != NULL) {
			stack_cb(&ev, stack_ctx);
		}
	}
}

// --- The radio core's glue (core/radio/radio_sm.c in miniature) ---

static struct {
	const dmrBurst_t *list;
	int count, next;
} dtx;
static int events[16];
static unsigned tx_starts;

static bool data_next(dmr_tx_burst_t *out, void *ctx)
{
	if (dtx.list == NULL || dtx.next >= dtx.count) {
		return false;
	}
	const dmrBurst_t *d = &dtx.list[dtx.next++];
	memset(out, 0, sizeof(*out));
	out->kind = d->dataType;
	out->host_crc = (d->flags & DMR_BURST_FLAG_MCU_CRC) != 0;
	out->sync_override = DMR_SYNC_NONE;
	out->len = d->length <= DMR_PAYLOAD_MAX ? d->length : DMR_PAYLOAD_MAX;
	memcpy(out->payload, d->payload, out->len);
	return true;
}

static void data_done(uint8_t err, void *ctx)
{
	dtx.list = NULL;
	dmrDataServiceTxDone(err);
}

static void data_rx(const dmr_burst_t *b, uint32_t now_ms, void *ctx)
{
	dmrBurst_t d;
	d.dataType = b->kind;
	d.length = b->len <= DMR_BURST_PAYLOAD_MAX ? b->len : DMR_BURST_PAYLOAD_MAX;
	d.flags = b->crc_ok ? 0 : DMR_BURST_FLAG_RX_CRC_ERROR;
	memcpy(d.payload, b->payload, d.length);
	dmrDataServiceRxBurst(&d, now_ms);
}

static bool env_tx_start(const dmrBurst_t *bursts, int count, void *ctx)
{
	dtx.list = bursts;
	dtx.count = count;
	dtx.next = 0;
	dmr_data_source_t src = { .next = data_next, .done = data_done, .ctx = NULL, .count = (uint8_t)count };
	if (!dmr_call_data_tx(&src)) {
		dtx.list = NULL;
		return false;
	}
	tx_starts++;
	return true;
}

static bool env_clear(uint32_t now_ms, void *ctx) { return dmr_call_channel_clear(now_ms); }
static void env_notify(const dmrDataEvent_t *e, void *ctx) { if (e->kind < 16) events[e->kind]++; }

static const dmrDataEnv_t env = { .txStart = env_tx_start, .channelClear = env_clear, .notify = env_notify };
static const dmrDataChannel_t channel = { .ownId = OUR_ID, .talkGroup = 9, .rmo = false, .canTransmit = true,
					  .channelKey = 1 };

// --- The clock: timeslot interrupts every 30 ms (the RF TX interrupt 1.5 ms before one that follows a set-up),
// receive reports 4 ms after a tick, the board task every 10 ms, the radio task (stack poll and service tick) every
// 10 ms, 5 ms off the board task ---

static dmrBurst_t rxq[64];
static int rxq_n, rxq_next;
static uint32_t next_tick_us, next_task_us, next_radio_us;
static bool burst_set_up;	// the last tick set up a burst of ours (0x41 = 0x80)

static void report(const dmrBurst_t *b)
{
	memset(&rd[2][0], 0, 24);
	memcpy(&rd[2][0], b->payload, b->length);
	rd[4][0x82] = 0x08;
	rd[4][0x51] = (uint8_t)((b->dataType << 4) | 0x02u);	// class 2: the data type, CRC good
	rd[4][0x52] = (uint8_t)(CC << 4);
	rd[4][0x5F] = 0x00;	// MS sync
	hrc6000_dmr_isr_sys(&eng);
	rd[4][0x82] = 0;
}

// Runs the clock for ms; queued receive bursts go out one per frame (every other tick) while the engine isn't
// transmitting
static void run(uint32_t ms)
{
	uint32_t until = t_us + ms * 1000u;
	while (t_us < until) {
		uint32_t t = next_tick_us;
		if (next_task_us < t) t = next_task_us;
		if (next_radio_us < t) t = next_radio_us;
		t_us = t;
		if (t == next_tick_us) {
			if (burst_set_up) {
				t_us = t - 1500;
				hrc6000_dmr_isr_rftx(&eng);
				t_us = t;
			}
			last41 = 0xFF;
			cur_tick++;
			hrc6000_dmr_isr_timeslot(&eng);
			burst_set_up = last41 == 0x80;
			next_tick_us += 30000;
			if (rxq_next < rxq_n && (cur_tick & 1u) == 0 && !hrc6000_dmr_is_transmitting(&eng)) {
				t_us = t + 4000;
				report(&rxq[rxq_next++]);
				t_us = t;
			}
		} else if (t == next_task_us) {
			board_task();
			next_task_us += 10000;
		} else {
			dmr_call_poll(t / 1000u);
			dmrDataServiceTick(t / 1000u, &channel);
			next_radio_us += 10000;
		}
	}
}

static void queue_rx(const dmrBurst_t *b, int n)
{
	for (int i = 0; i < n && rxq_n < (int)(sizeof(rxq) / sizeof(rxq[0])); i++) {
		rxq[rxq_n++] = b[i];
	}
}

static void setup(void)
{
	memset(rd, 0, sizeof(rd));
	memset(events, 0, sizeof(events));
	ntx = 0;
	rxq_n = rxq_next = 0;
	tx_starts = 0;
	pa_on_calls = 0;
	cur_tick = 0;
	t_us = 1000000;
	next_tick_us = t_us + 30000;
	next_task_us = t_us + 2000;
	next_radio_us = t_us + 7000;
	burst_set_up = false;

	hrc6000_dmr_init(&eng, &dev, &ops);
	hrc6000_dmr_run(&eng);
	dmr_call_init(&modem);
	dmr_call_config_t cfg;
	dmr_call_config_default(&cfg);
	cfg.own_id = OUR_ID;
	cfg.cc = CC;
	cfg.ts = 1;
	cfg.talk_target.id = 9;
	dmr_call_configure(&cfg);
	CHECK(dmr_call_start());

	dmrDataServiceInit(&env);
	dmrDataSettings_t st = { .arsServer = 0, .radioCheckReply = true, .callAlertAck = true, .lrrpReply = true,
				 .hasGps = false };
	dmrDataServiceSetSettings(&st);
	dmrDataServiceResetStats();
	dmr_call_set_data_rx(data_rx, NULL);
	run(300);	// the receiver settles
}

// The captured transmission from index from on, as dmrBurst_t; returns the count
static int captured(int from, dmrBurst_t *out, int max)
{
	int n = 0;
	for (int i = from; i < ntx && n < max; i++, n++) {
		out[n].dataType = txl[i].kind;
		out[n].length = (uint8_t)hrc6000_dmr_payload_len(txl[i].kind);
		out[n].flags = 0;
		memcpy(out[n].payload, txl[i].payload, out[n].length);
	}
	return n;
}

// Decodes the captured bursts into a packet: true when one came out whole
static bool captured_packet(int from)
{
	dmrBurst_t b[64];
	int n = captured(from, b, 64);
	dmrDataRxReset();
	for (int i = 0; i < n; i++) {
		if (dmrDataRxBurst(&b[i]) == DMR_DATA_RX_PACKET) {
			return true;
		}
	}
	return false;
}

static uint32_t id24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

static void csbk_burst(uint8_t opcode, uint8_t fid, uint8_t octet3, uint32_t target, uint32_t source, dmrBurst_t *b)
{
	uint8_t c[10] = { (uint8_t)(0x80 | opcode), fid, 0, octet3, (uint8_t)(target >> 16), (uint8_t)(target >> 8),
			  (uint8_t)target, (uint8_t)(source >> 16), (uint8_t)(source >> 8), (uint8_t)source };
	CHECK(dmrDataBuildCSBK(c, b) == 1);
}

// --- Tests ---

// A call alert to us: the user is alerted, and the ack goes out as one CSBK with its own CRC (CRC_MCU_Control)
static void test_call_alert_in(void)
{
	setup();
	dmrBurst_t b;
	csbk_burst(DMR_CSBKO_CALL_ALERT, DMR_FID_ETSI, 0, OUR_ID, OTHER_ID, &b);
	queue_rx(&b, 1);
	run(1500);
	dmrDataStats_t st;
	dmrDataServiceGetStats(&st);
	CHECK(st.rxCsbk == 1 && st.callAlertsReceived == 1 && st.rxCrcFlagMismatch == 0);
	CHECK(events[DMR_DATA_EV_CALL_ALERT] == 1);
	CHECK(st.csbkAnswersSent == 1 && st.txLists == 1 && st.txOk == 1 && st.txFailed == 0);
	CHECK(ntx == 1);
	if (ntx >= 1) {
		CHECK(txl[0].kind == DMR_BURST_CSBK && txl[0].mcu_crc);
		CHECK((txl[0].payload[0] & 0x3F) == DMR_CSBKO_CALL_ALERT_ACK);
		CHECK(dmrDataCheckCRC16(txl[0].payload, 0xA5));
		CHECK(id24(&txl[0].payload[4]) == OTHER_ID && id24(&txl[0].payload[7]) == OUR_ID);
	}
	CHECK(pa_on_calls >= 1 && eng.st.tx_done == 1 && !hrc6000_dmr_is_transmitting(&eng));
	CHECK(!dmr_call_data_busy() && !dmrDataServiceIsBusy());
}

// A text message to us (a preamble, a header, rate 1/2 / 3/4 / 1 blocks): kept in the inbox, and the TMS ACK goes
// back out as a whole packet (preamble CSBK, header, blocks)
static void test_sms_in(uint8_t block_type, const char *text)
{
	setup();
	int before = sms_inbox_count();	// the service keeps its inbox across dmrDataServiceInit
	dmrBurst_t b[DMR_DATA_MAX_BURSTS];
	int n = dmrDataBuildTMS(false, OUR_ID, OTHER_ID, text, 5, true, block_type, 1, b, DMR_DATA_MAX_BURSTS);
	CHECK(n >= 3);
	queue_rx(b, n);
	run((uint32_t)n * 60u + 2000u);
	dmrDataStats_t st;
	dmrDataServiceGetStats(&st);
	CHECK(st.rxHeaders == 1 && st.rxPackets == 1 && st.rxForUs == 1 && st.rxCrcErrors == 0);
	CHECK(st.smsReceived == 1 && events[DMR_DATA_EV_SMS_RECEIVED] == 1);
	dmrDataMessage_t m;
	CHECK(sms_inbox_count() == (before < DMR_DATA_INBOX_SIZE ? before + 1 : before) && sms_inbox_get(0, &m) && strcmp(m.text, text) == 0 && m.src == OTHER_ID);
	CHECK(st.tmsAcksSent == 1 && st.txOk == 1);
	CHECK(ntx >= 3);
	bool header_crc = false;
	for (int i = 0; i < ntx; i++) {
		if (txl[i].kind == DMR_BURST_DATA_HEADER) {
			header_crc = txl[i].mcu_crc && dmrDataCheckCRC16(txl[i].payload, 0xCC);
		}
	}
	CHECK(header_crc);
	CHECK(captured_packet(0));
	dmrDataUDP_t udp;
	dmrDataTMS_t tms;
	CHECK(dmrDataRxPacket.dst == OTHER_ID && dmrDataRxPacket.src == OUR_ID);
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && udp.appPort == DMR_UDP_PORT_TMS &&
	      dmrDataDecodeTMS(udp.payload, udp.length, &tms) && tms.isAck && (tms.seq & 0x1F) == 5);
	dmrDataRxReset();
}

// Our radio check: the request goes out (Motorola FID, octet 3 0x80), the answer heard comes back as RADIO_CHECK_OK
static void test_radio_check_out(void)
{
	setup();
	CHECK(dmr_signal_radio_check(OTHER_ID));
	run(400);
	CHECK(ntx >= 1);
	if (ntx >= 1) {
		CHECK(txl[0].kind == DMR_BURST_CSBK && txl[0].mcu_crc && dmrDataCheckCRC16(txl[0].payload, 0xA5));
		CHECK((txl[0].payload[0] & 0x3F) == DMR_CSBKO_RADIO_CHECK && txl[0].payload[1] == DMR_FID_MOTOROLA &&
		      txl[0].payload[3] == 0x80 && id24(&txl[0].payload[4]) == OTHER_ID &&
		      id24(&txl[0].payload[7]) == OUR_ID);
	}
	int sent = ntx;
	dmrBurst_t b;
	csbk_burst(DMR_CSBKO_RADIO_CHECK, DMR_FID_MOTOROLA, 0, OTHER_ID, OUR_ID, &b);
	queue_rx(&b, 1);
	run(600);
	uint8_t kind = 0;
	uint32_t dst = 0;
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_OK && kind == DMR_DATA_EV_RADIO_CHECK_OK && dst == OTHER_ID);
	CHECK(events[DMR_DATA_EV_RADIO_CHECK_OK] == 1);
	run(3000);
	CHECK(ntx == sent);	// answered: no retry
}

// Our call alert to a radio that doesn't answer: three tries on air, then CALL_ALERT_FAILED
static void test_call_alert_out_no_answer(void)
{
	setup();
	CHECK(dmr_signal_call_alert(OTHER_ID));
	run(8000);
	int alerts = 0;
	for (int i = 0; i < ntx; i++) {
		if (txl[i].kind == DMR_BURST_CSBK && (txl[i].payload[0] & 0x3F) == DMR_CSBKO_CALL_ALERT) {
			alerts++;
		}
	}
	CHECK(alerts == 3);
	CHECK(events[DMR_DATA_EV_CALL_ALERT_FAILED] == 1);
	dmrDataStats_t st;
	dmrDataServiceGetStats(&st);
	CHECK(st.callAlertsSent == 3 && st.txOk == 3 && st.txFailed == 0);
}

// Something heard 600 ms ago (the receiver has given up on it, the stack still counts it as recent): a request of
// ours still goes out
static void test_send_after_recent_activity(void)
{
	setup();
	dmrBurst_t b;
	csbk_burst(DMR_CSBKO_CALL_ALERT, DMR_FID_ETSI, 0, 9999, OTHER_ID, &b);	// not to us
	queue_rx(&b, 1);
	run(600);
	CHECK(ntx == 0);
	CHECK(dmr_signal_radio_check(OTHER_ID));
	run(2500);
	dmrDataStats_t st;
	dmrDataServiceGetStats(&st);
	if (!(st.radioChecksSent >= 1 && st.txFailed == 0)) {
		printf("  recent activity: checks sent %u lists %u ok %u failed %u gave up %u last error %u, engine rejected %u\n",
		       st.radioChecksSent, st.txLists, st.txOk, st.txFailed, st.txGaveUp, st.txLastError,
		       eng.st.tx_rejected);
	}
	CHECK(st.radioChecksSent >= 1 && st.txFailed == 0);
	CHECK(ntx >= 1);
}

int main(void)
{
	hrc6000_attach(&dev, &bus);
	test_call_alert_in();
	test_sms_in(DT_RATE_12_DATA, "Hello from the chain");
	test_sms_in(DT_RATE_34_DATA, "Rate three quarters, a little longer text");
	test_sms_in(DT_RATE_1_DATA, "Rate one");
	test_radio_check_out();
	test_call_alert_out_no_answer();
	test_send_after_recent_activity();
	if (failures) {
		printf("# dmrDataChainTest: %d FAILED\n", failures);
		return 1;
	}
	printf("# dmrDataChainTest: PASSED\n");
	return 0;
}
