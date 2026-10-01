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
#include "dmr/dmr_emergency.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define OUR_ID		1234
#define OTHER_ID	0x234567
#define TG		9
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
static const dmrDataChannel_t channel = { .ownId = OUR_ID, .talkGroup = TG, .rmo = false, .canTransmit = true,
					  .channelKey = 1 };
static const dmr_emergency_channel_t em_channel = { .can_transmit = true, .talk_id = TG, .talk_private = false };

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
			dmr_emergency_tick(t / 1000u, &em_channel);
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
	dmr_emergency_init(NULL);
	dmr_emergency_reset_stats();
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


// --- Emergency (docs/spec/dmr-emergency.md) ---

// An emergency alarm to our talkgroup: the event, the alert (once for the repeats), no answer on air. One to a group
// we don't listen to, and our own alarm repeated back, are only counted
static void test_emergency_alarm_in(void)
{
	setup();
	uint8_t c[10];
	dmrBurst_t b[4];
	dmrDataBuildEmergencyAlarm(TG, OTHER_ID, true, DMR_FID_MOTOROLA, c);
	CHECK(c[0] == (0x80 | DMR_CSBKO_EMERGENCY_ALARM) && c[1] == DMR_FID_MOTOROLA && id24(&c[4]) == TG &&
	      id24(&c[7]) == OTHER_ID);
	dmrDataBuildCSBK(c, &b[0]);
	b[1] = b[0];		// the sender repeats it
	dmrDataBuildEmergencyAlarm(4242, OTHER_ID + 1, true, DMR_FID_MOTOROLA, c);
	dmrDataBuildCSBK(c, &b[2]);
	dmrDataBuildEmergencyAlarm(TG, OUR_ID, true, DMR_FID_ETSI, c);
	dmrDataBuildCSBK(c, &b[3]);
	queue_rx(b, 4);
	run(1500);
	dmrDataStats_t st;
	dmrDataServiceGetStats(&st);
	CHECK(st.emergencyAlarmsHeard == 3 && st.emergencyAlarmsReceived == 2);
	CHECK(events[DMR_DATA_EV_EMERGENCY_ALARM] == 2);
	dmr_emergency_alert_t a;
	CHECK(dmr_emergency_alert(&a) && a.kind == DMR_EMERGENCY_ALERT_ALARM && a.src == OTHER_ID && a.dst == TG && a.group);
	CHECK(a.pending == 1);
	CHECK(dmr_emergency_take_tone() && !dmr_emergency_take_tone());
	dmr_emergency_stats_t es;
	dmr_emergency_get_stats(&es);
	CHECK(es.alerts == 1 && es.alerts_alarm == 1);
	CHECK(ntx == 0);	// nothing answers an alarm
	dmr_emergency_dismiss();
	run(50);
	CHECK(!dmr_emergency_alert(&a) && a.src == OTHER_ID);
}

// The emergency key in "alarm only" mode: three alarm CSBKs about a second apart to the channel's talkgroup, then the
// radio stays in emergency until canceled
static void test_emergency_alarm_out(void)
{
	setup();
	dmr_emergency_settings_t es;
	dmr_emergency_settings_default(&es);
	es.mode = DMR_EMERGENCY_MODE_ALARM;
	es.alarm_tries = 3;
	dmr_emergency_set_settings(&es);
	run(20);
	CHECK(dmr_emergency_start());
	run(200);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_ALARM && dmr_emergency_in_progress());
	run(4000);
	int alarms = 0;
	uint32_t slots[3] = { 0 };
	for (int i = 0; i < ntx; i++) {
		if (txl[i].kind == DMR_BURST_CSBK && (txl[i].payload[0] & 0x3F) == DMR_CSBKO_EMERGENCY_ALARM) {
			CHECK(txl[i].mcu_crc && dmrDataCheckCRC16(txl[i].payload, 0xA5));
			CHECK(txl[i].payload[1] == DMR_FID_MOTOROLA && (txl[i].payload[2] & 0x80) == 0);
			CHECK(id24(&txl[i].payload[4]) == TG && id24(&txl[i].payload[7]) == OUR_ID);
			if (alarms < 3) {
				slots[alarms] = txl[i].tick;
			}
			alarms++;
		}
	}
	CHECK(alarms == 3);
	CHECK((slots[1] - slots[0]) * 30u >= DMR_DATA_EMERGENCY_GAP_MS && (slots[2] - slots[1]) * 30u >= DMR_DATA_EMERGENCY_GAP_MS);
	CHECK(events[DMR_DATA_EV_EMERGENCY_SENT] == 1);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_ACTIVE && !dmr_emergency_hot_mic());
	uint32_t id;
	bool priv;
	dmr_emergency_target(&id, &priv);
	CHECK(id == TG && !priv);
	dmr_emergency_cancel();
	run(50);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_IDLE && !dmr_emergency_in_progress());
	dmrDataStats_t st;
	dmrDataServiceGetStats(&st);
	CHECK(st.emergencyAlarmsSent == 3);
}

// --- ETSI short data (docs/spec/dmr-data.md 5.2) ---

// A defined short data packet with any DD format and payload (the bit padding below an octet given)
static int build_dd(uint8_t dd, const uint8_t *data, int len, uint8_t extra_pad_bits, bool response, dmrBurst_t *out)
{
	uint8_t pkt[96];
	int blocks = (len + 4 + 11) / 12;
	int pad = blocks * 12 - 4 - len;
	uint8_t h[12] = { 0 };
	h[0] = (uint8_t)((response ? 0x40 : 0) | (((blocks >> 4) & 3) << 4) | DMR_DPF_DEFINED_SHORT);
	h[1] = (uint8_t)((DMR_SAP_SHORT_DATA << 4) | (blocks & 0x0F));
	h[2] = (uint8_t)(OUR_ID >> 16); h[3] = (uint8_t)(OUR_ID >> 8); h[4] = (uint8_t)OUR_ID;
	h[5] = (uint8_t)(OTHER_ID >> 16); h[6] = (uint8_t)(OTHER_ID >> 8); h[7] = (uint8_t)OTHER_ID;
	h[8] = (uint8_t)((dd << 2) | 1);
	h[9] = (uint8_t)(pad * 8 + extra_pad_bits);
	uint16_t crc = 0;	// CRC-CCITT of the header, masked 0xCCCC
	for (int i = 0; i < 10; i++) {
		crc ^= (uint16_t)(h[i] << 8);
		for (int k = 0; k < 8; k++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	crc = (uint16_t)(~crc ^ 0xCCCC);
	h[10] = (uint8_t)(crc >> 8);
	h[11] = (uint8_t)crc;
	out[0].dataType = DT_DATA_HEADER;
	out[0].length = 12;
	out[0].flags = 0;
	memcpy(out[0].payload, h, 12);
	memset(pkt, 0, sizeof(pkt));
	memcpy(pkt, data, len);
	uint32_t c32 = dmrDataCRC32(pkt, len + pad);
	pkt[len + pad] = (uint8_t)c32;
	pkt[len + pad + 1] = (uint8_t)(c32 >> 8);
	pkt[len + pad + 2] = (uint8_t)(c32 >> 16);
	pkt[len + pad + 3] = (uint8_t)(c32 >> 24);
	for (int i = 0; i < blocks; i++) {
		out[1 + i].dataType = DT_RATE_12_DATA;
		out[1 + i].length = 12;
		out[1 + i].flags = 0;
		memcpy(out[1 + i].payload, &pkt[i * 12], 12);
	}
	return 1 + blocks;
}

static bool decode_bursts(const dmrBurst_t *b, int n, char *text, int size)
{
	dmrDataRxReset();
	for (int i = 0; i < n; i++) {
		if (dmrDataRxBurst(&b[i]) == DMR_DATA_RX_PACKET) {
			return dmrDataDecodeEtsiText(&dmrDataRxPacket, text, size);
		}
	}
	return false;
}

// The text formats a receiver may meet: ISO 8859-1, 7-bit packed, UTF-16 BE / LE / with a BOM, UTF-8, UDT text
static void test_etsi_text_formats(void)
{
	char text[64];
	dmrBurst_t b[8];
	static const uint8_t latin[] = { 'C', 'a', 'f', 0xE9 };
	CHECK(decode_bursts(b, build_dd(DMR_DD_ISO8859_1, latin, 4, 0, false, b), text, sizeof(text)) &&
	      strcmp(text, "Caf\xE9") == 0);
	// "Hi!" in 7 bits: 1001000 1101001 0100001 = 21 bits in 3 octets, 3 bits of padding
	static const uint8_t seven[] = { 0x91, 0xA5, 0x08 };
	CHECK(decode_bursts(b, build_dd(DMR_DD_7BIT, seven, 3, 3, false, b), text, sizeof(text)) && strcmp(text, "Hi!") == 0);
	static const uint8_t be[] = { 0x00, 'O', 0x00, 'K', 0x20, 0xAC };
	CHECK(decode_bursts(b, build_dd(DMR_DD_UTF16BE, be, 6, 0, false, b), text, sizeof(text)) && strcmp(text, "OK?") == 0);
	static const uint8_t le[] = { 'O', 0x00, 'K', 0x00 };
	CHECK(decode_bursts(b, build_dd(DMR_DD_UTF16LE, le, 4, 0, false, b), text, sizeof(text)) && strcmp(text, "OK") == 0);
	static const uint8_t bom[] = { 0xFF, 0xFE, 'b', 0x00, 'o', 0x00, 'm', 0x00 };
	CHECK(decode_bursts(b, build_dd(DMR_DD_UTF16, bom, 8, 0, false, b), text, sizeof(text)) && strcmp(text, "bom") == 0);
	static const uint8_t u8[] = { 'n', 0xC3, 0xA9, 0xE2, 0x82, 0xAC };
	CHECK(decode_bursts(b, build_dd(DMR_DD_UTF8, u8, 6, 0, false, b), text, sizeof(text)) && strcmp(text, "n\xE9?") == 0);
	CHECK(!decode_bursts(b, build_dd(DMR_DD_BINARY, latin, 4, 0, false, b), text, sizeof(text)));
	// Our own builder, read back
	int n = dmrDataBuildEtsiText(false, OUR_ID, OTHER_ID, "Round trip \xC4", true, 0, b, 8);
	CHECK(n == 3 && decode_bursts(b, n, text, sizeof(text)) && strcmp(text, "Round trip \xC4") == 0);
	CHECK(dmrDataRxPacket.dpf == DMR_DPF_DEFINED_SHORT && dmrDataRxPacket.sap == DMR_SAP_SHORT_DATA &&
	      dmrDataRxPacket.responseRequested && dmrDataRxPacket.ddFormat == DMR_DD_ISO8859_1);
	dmrDataRxReset();
}

// "ODR ETSI 541" from 9990301 to 3101276, response requested, as ETSI TS 102 361-1 V2.7.1 lays it out, computed apart
// from this code (a short script from the clauses): DD_HEAD (Figure 8.9, 9.2.12) with the DD format 0x03 for ISO/IEC
// 8859-1 (Table 9.50; 0x02 is the 7-bit format), S 0, F 1, 64 bits of padding (9.3.44), CRC-CCITT (B.3.8) masked
// 0xCCCC; then the text, 8 pad octets and the CRC-32 (B.3.9) in two rate 1/2 blocks. HIL 2026-10-10: the bench sent
// this text with DD format 0x02, and the radio rightly read it as 7-bit characters ('Q"(S$HS!)
static const uint8_t etsi_vec[3][12] = {
	{ 0x4D, 0xA2, 0x2F, 0x52, 0x5C, 0x98, 0x70, 0x9D, 0x0D, 0x40, 0x26, 0xB0 },
	{ 0x4F, 0x44, 0x52, 0x20, 0x45, 0x54, 0x53, 0x49, 0x20, 0x35, 0x34, 0x31 },
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9B, 0xB8, 0xBA, 0x23 },
};

static void test_etsi_text_vector(void)
{
	char text[64];
	dmrBurst_t b[8];
	for (int i = 0; i < 3; i++) {
		b[i].dataType = i == 0 ? DT_DATA_HEADER : DT_RATE_12_DATA;
		b[i].length = 12;
		b[i].flags = 0;
		memcpy(b[i].payload, etsi_vec[i], 12);
	}
	CHECK(decode_bursts(b, 3, text, sizeof(text)) && strcmp(text, "ODR ETSI 541") == 0);
	CHECK(dmrDataRxPacket.ddFormat == DMR_DD_ISO8859_1 && dmrDataRxPacket.src == 9990301 &&
	      dmrDataRxPacket.dst == 3101276 && dmrDataRxPacket.responseRequested);
	dmrDataRxReset();
	// Our own ETSI text goes out the same, octet for octet
	int n = dmrDataBuildEtsiText(false, 3101276, 9990301, "ODR ETSI 541", true, 0, b, 8);
	CHECK(n == 3);
	for (int i = 0; i < n && i < 3; i++) {
		CHECK(b[i].length == 12 && memcmp(b[i].payload, etsi_vec[i], 12) == 0);
	}
	CHECK(b[0].dataType == DT_DATA_HEADER && b[1].dataType == DT_RATE_12_DATA && b[2].dataType == DT_RATE_12_DATA);
}

// An ETSI text message to us with a response requested: kept in the inbox as ETSI, the ETSI response (ACK) goes out
static void test_etsi_sms_in(void)
{
	setup();
	dmrBurst_t b[DMR_DATA_MAX_BURSTS];
	int n = dmrDataBuildEtsiText(false, OUR_ID, OTHER_ID, "ETSI short data in", true, 1, b, DMR_DATA_MAX_BURSTS);
	CHECK(n >= 3);
	queue_rx(b, n);
	run((uint32_t)n * 60u + 1500u);
	dmrDataStats_t st;
	dmrDataServiceGetStats(&st);
	CHECK(st.smsReceived == 1 && st.etsiSmsReceived == 1 && events[DMR_DATA_EV_SMS_RECEIVED] == 1);
	dmrDataMessage_t m;
	CHECK(sms_inbox_get(0, &m) && strcmp(m.text, "ETSI short data in") == 0 && m.format == DMR_SMS_FORMAT_ETSI &&
	      m.src == OTHER_ID);
	CHECK(st.responsesSent == 1 && st.tmsAcksSent == 0 && ntx == 1);
	if (ntx >= 1) {
		CHECK(txl[0].kind == DMR_BURST_DATA_HEADER && dmrDataCheckCRC16(txl[0].payload, 0xCC));
		CHECK((txl[0].payload[0] & 0x0F) == DMR_DPF_RESPONSE && (txl[0].payload[1] >> 4) == DMR_SAP_SHORT_DATA);
		CHECK(DMR_RESPONSE_IS_ACK(txl[0].payload[9]) && id24(&txl[0].payload[2]) == OTHER_ID &&
		      id24(&txl[0].payload[5]) == OUR_ID);
	}
}

// Our ETSI message (the setting's default format): defined short data on air, OK once the ETSI response is heard
static void test_etsi_sms_out(void)
{
	setup();
	dmrDataSettings_t set;
	dmrDataServiceGetSettings(&set);
	set.smsFormat = DMR_SMS_FORMAT_ETSI;
	dmrDataServiceSetSettings(&set);
	CHECK(sms_send(OTHER_ID, false, "Out in ETSI form", true, DMR_SMS_FORMAT_DEFAULT));
	run(1500);
	CHECK(sms_send_state() == DMR_REQ_SENDING);
	char text[64];
	dmrBurst_t b[16];
	int n = captured(0, b, 16);
	CHECK(n >= 3 && decode_bursts(b, n, text, sizeof(text)) && strcmp(text, "Out in ETSI form") == 0);
	CHECK(dmrDataRxPacket.dst == OTHER_ID && dmrDataRxPacket.src == OUR_ID && dmrDataRxPacket.responseRequested);
	dmrDataRxReset();
	dmrBurst_t r;
	dmrDataBuildResponseAck(DMR_SAP_SHORT_DATA, OUR_ID, OTHER_ID, 0, &r);
	queue_rx(&r, 1);
	run(500);
	CHECK(sms_send_state() == DMR_REQ_OK && events[DMR_DATA_EV_SMS_SENT] == 1);
	dmrDataStats_t st;
	dmrDataServiceGetStats(&st);
	CHECK(st.etsiSmsSent == 1 && st.etsiResponsesReceived == 1 && st.smsAcked == 1);
	set.smsFormat = DMR_SMS_FORMAT_TMS;
	dmrDataServiceSetSettings(&set);
}

// Our radio check in the ETSI form (FID 0x00), answered in the ETSI order (to us from the checked radio)
static void test_radio_check_etsi(void)
{
	setup();
	dmrDataSettings_t set;
	dmrDataServiceGetSettings(&set);
	set.radioCheckEtsi = true;
	dmrDataServiceSetSettings(&set);
	CHECK(dmr_signal_radio_check(OTHER_ID));
	run(400);
	CHECK(ntx >= 1);
	if (ntx >= 1) {
		CHECK((txl[0].payload[0] & 0x3F) == DMR_CSBKO_RADIO_CHECK && txl[0].payload[1] == DMR_FID_ETSI &&
		      txl[0].payload[3] == 0x80 && id24(&txl[0].payload[4]) == OTHER_ID && id24(&txl[0].payload[7]) == OUR_ID);
	}
	dmrBurst_t b;
	csbk_burst(DMR_CSBKO_RADIO_CHECK, DMR_FID_ETSI, 0, OUR_ID, OTHER_ID, &b);
	queue_rx(&b, 1);
	run(600);
	CHECK(dmr_signal_state(NULL, NULL) == DMR_REQ_OK && events[DMR_DATA_EV_RADIO_CHECK_OK] == 1);
	// An ETSI-form check to us is answered too (the request echoed with octet 3 cleared, FID kept)
	int sent = ntx;
	csbk_burst(DMR_CSBKO_RADIO_CHECK, DMR_FID_ETSI, 0x80, OUR_ID, OTHER_ID, &b);
	queue_rx(&b, 1);
	run(800);
	CHECK(ntx == sent + 1);
	if (ntx == sent + 1) {
		CHECK((txl[sent].payload[0] & 0x3F) == DMR_CSBKO_RADIO_CHECK && txl[sent].payload[1] == DMR_FID_ETSI &&
		      txl[sent].payload[3] == 0x00 && id24(&txl[sent].payload[4]) == OUR_ID);
	}
	set.radioCheckEtsi = false;
	dmrDataServiceSetSettings(&set);
}

int main(void)
{
	hrc6000_attach(&dev, &bus);
	test_call_alert_in();
	test_sms_in(DT_RATE_12_DATA, "Hello from the chain");
	test_sms_in(DT_RATE_34_DATA, "Rate three quarters, a little longer text");
	test_sms_in(DT_RATE_1_DATA, "Rate one");
	test_radio_check_out();
	test_radio_check_etsi();	// before a check is left unanswered (the state carries over)
	test_call_alert_out_no_answer();
	test_send_after_recent_activity();
	test_emergency_alarm_in();
	test_emergency_alarm_out();
	test_etsi_text_formats();
	test_etsi_text_vector();
	test_etsi_sms_in();
	test_etsi_sms_out();
	if (failures) {
		printf("# dmrDataChainTest: %d FAILED\n", failures);
		return 1;
	}
	printf("# dmrDataChainTest: PASSED\n");
	return 0;
}
