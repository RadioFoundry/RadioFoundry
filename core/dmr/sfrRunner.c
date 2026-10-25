// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SFR repeater roles on hal_dmr_modem_t (see sfrRunner.h)
#include "config.h"
#if CONFIG_SFR	// the whole file

#include <string.h>
#include "dmr/sfrRunner.h"
#include "dmr/dmrData.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/ambe/ambeFec.h"

#define VOICE_TYPE_NIBBLE	0x0F	// stats.last_rx: a voice burst

static uint8_t to_sfr_sync(uint8_t family)
{
	switch (family) {
	case DMR_SYNC_BS:
		return SFR_SYNC_BS;
	case DMR_SYNC_TS1:
		return SFR_SYNC_TS1;
	case DMR_SYNC_TS2:
		return SFR_SYNC_TS2;
	default:
		return SFR_SYNC_MS;	// MS, or none reported (voice B-F carry no sync)
	}
}

static uint8_t to_family(uint8_t sfr_sync)
{
	switch (sfr_sync) {
	case SFR_SYNC_BS:
		return DMR_SYNC_BS;
	case SFR_SYNC_TS1:
		return DMR_SYNC_TS1;
	case SFR_SYNC_TS2:
		return DMR_SYNC_TS2;
	default:
		return DMR_SYNC_MS;
	}
}

static void hash_payload(const uint8_t *p, uint8_t len, uint8_t out[4])
{
	uint32_t h = 2166136261u;	// FNV-1a

	for (uint8_t i = 0; i < len; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	out[0] = (uint8_t)h;
	out[1] = (uint8_t)(h >> 8);
	out[2] = (uint8_t)(h >> 16);
	out[3] = (uint8_t)(h >> 24);
}

#define FIFO_IDLE	0xFF	// fifo[]: an idle burst
#define FIFO_EMPTY	0xFE	// fifo[]: a placeholder (DMR_BURST_EMPTY) a burst may take (tx_fill)

// A burst going to the modem: its index in sent[], whose echo window starts when it goes on air (aired())
static uint8_t remember_sent(sfrRunner_t *r, const sfrBurst_t *b)
{
	sfrRunnerSent_t *s = &r->sent[r->sent_next];
	uint8_t idx = r->sent_next;

	r->sent_next = (uint8_t)((r->sent_next + 1) % SFR_RUNNER_ECHO_NUM);
	hash_payload(b->payload, b->length, s->hash);
	s->kind = b->kind;
	s->type = b->type;
	s->ms = 0;
	return idx;
}

// A burst queued in the modem (idle or a placeholder: b NULL)
static void remember_queued(sfrRunner_t *r, const sfrBurst_t *b, bool placeholder)
{
	uint8_t idx = (b != NULL) ? remember_sent(r, b) : (placeholder ? FIFO_EMPTY : FIFO_IDLE);

	if (r->fifo_count < sizeof(r->fifo)) {
		r->fifo[(r->fifo_head + r->fifo_count) % sizeof(r->fifo)] = idx;
		r->fifo_count++;
	}
}

static void aired(sfrRunner_t *r, uint32_t now_ms)
{
	if (r->fifo_count == 0) {
		return;
	}
	uint8_t idx = r->fifo[r->fifo_head];

	r->fifo_head = (uint8_t)((r->fifo_head + 1) % sizeof(r->fifo));
	r->fifo_count--;
	if (idx < SFR_RUNNER_ECHO_NUM) {
		r->sent[idx].ms = (now_ms != 0) ? now_ms : 1;
	}
}

// The chip reports what we sent as heard, in empty inbound slots (gotchas 14.2): same kind, position and payload
static bool is_echo(const sfrRunner_t *r, const sfrBurst_t *b, uint32_t now_ms)
{
	uint8_t h[4];

	hash_payload(b->payload, b->length, h);
	for (int i = 0; i < SFR_RUNNER_ECHO_NUM; i++) {
		const sfrRunnerSent_t *s = &r->sent[i];

		if ((s->ms != 0) && ((now_ms - s->ms) < SFR_RUNNER_ECHO_MS) && (s->kind == b->kind) &&
		    (s->type == b->type) && (memcmp(s->hash, h, 4) == 0)) {
			return true;
		}
	}
	return false;
}

sfrRunner_t sfr_runner;

void sfrRunnerStart(sfrRunner_t *r, const hal_dmr_modem_t *modem, const sfrConfig_t *cfg, const sfrRunnerUplink_t *up)
{
	memset(r, 0, sizeof(*r));
	r->modem = modem;
	sfrEngineInit(&r->eng, cfg);
	sfrUplinkReset(&r->up);
	sfrLateEntryReset(&r->late);
	r->has_up = (up != NULL) && (up->burst != NULL);
	if (r->has_up) {
		r->up_ops = *up;
	}
	r->state = SFR_RUN_IDLE;
}

void sfrRunnerStop(sfrRunner_t *r)
{
	if ((r->state == SFR_RUN_KEYED) || (r->state == SFR_RUN_ENDING)) {
		r->modem->tx_abort();
	}
	r->state = SFR_RUN_OFF;
	r->in_modem = 0;
	r->fifo_count = 0;
	r->pending = false;
	sfrEngineFlush(&r->eng);
}

bool sfrRunnerRunning(const sfrRunner_t *r)
{
	return r->state != SFR_RUN_OFF;
}

void sfrRunnerSetBeacon(sfrRunner_t *r, uint16_t interval_s, uint8_t len_s, uint32_t now_ms)
{
	r->beacon_ms = (len_s != 0) ? (uint32_t)interval_s * 1000U : 0U;
	r->beacon_len_ms = (uint32_t)len_s * 1000U;
	if (r->beacon_len_ms >= r->beacon_ms) {
		r->beacon_ms = 0U;	// a beacon as long as the interval is a carrier, not a beacon
	}
	r->beacon_next = (now_ms != 0U) ? now_ms + r->beacon_ms : 0U;	// 0: from the first poll
	r->beaconing = false;
}

static bool due(uint32_t now_ms, uint32_t t)
{
	return (int32_t)(now_ms - t) >= 0;
}

void sfrRunnerSetNoDataHold(sfrRunner_t *r, bool on)
{
	r->eng.noDataHold = on;
}

// ---- Outbound ----

static void make_tx(sfrRunner_t *r, const sfrBurst_t *b, bool idle, dmr_tx_burst_t *out)
{
	dmr_tx_burst_t t;

	memset(&t, 0, sizeof(t));
	t.sync_override = to_family(sfrOutboundSync(&r->eng.cfg));
	if (idle && r->eng.rfCall) {
		t.kind = DMR_BURST_EMPTY;	// a gap inside the inbound call: no idle burst, the slot is received (gotchas 14.4)
		r->stats.tx_gaps++;
	} else if (idle) {
		t.kind = DMR_BURST_IDLE;
		t.host_crc = false;	// the idle message has no CRC (gotchas 14.4)
		t.len = sizeof(SFR_IDLE_INFO);
		memcpy(t.payload, SFR_IDLE_INFO, sizeof(SFR_IDLE_INFO));
		r->stats.tx_idle++;
	} else if (b->kind == SFR_BURST_VOICE) {
		const uint8_t *lc = sfrEngineTxLc(&r->eng);

		// The embedded LC of the superframe this burst A starts (the modem spreads it over B-E from here)
		if ((b->type == 0) && (memcmp(lc, r->emb_lc, SFR_LC_LENGTH) != 0)) {
			memcpy(r->emb_lc, lc, SFR_LC_LENGTH);
			r->modem->set_embedded_lc(lc);
		}
		t.kind = DMR_BURST_VOICE;
		t.voice_index = b->type;
		t.len = DMR_VOICE_BYTES;
		memcpy(t.payload, b->payload, DMR_VOICE_BYTES);
		r->stats.tx_bursts++;
	} else {
		t.kind = b->type;
		t.host_crc = (b->flags & SFR_BURST_FLAG_MCU_CRC) != 0;
		t.len = (b->length <= DMR_PAYLOAD_MAX) ? b->length : DMR_PAYLOAD_MAX;
		memcpy(t.payload, b->payload, t.len);
		r->stats.tx_bursts++;
	}
	*out = t;
}

static void queue_burst(sfrRunner_t *r, const sfrBurst_t *b, bool idle, uint32_t now_ms)
{
	dmr_tx_burst_t t;

	(void)now_ms;
	make_tx(r, b, idle, &t);
	if (r->modem->tx_queue(&t)) {
		r->in_modem++;
		remember_queued(r, idle ? NULL : b, t.kind == DMR_BURST_EMPTY);
	}
}

static bool has_placeholder(const sfrRunner_t *r)
{
	for (uint8_t i = 0; i < r->fifo_count; i++) {
		if (r->fifo[(r->fifo_head + i) % sizeof(r->fifo)] == FIFO_EMPTY) {
			return true;
		}
	}
	return false;
}

// A burst to repeat while placeholders for the call's gap wait in the modem: it takes the oldest one's slot, so it
// goes out 90 ms after the inbound burst (gotchas 14.3) instead of behind them
static void fill_gaps(sfrRunner_t *r, uint32_t now_ms)
{
	while ((r->modem->tx_fill != NULL) && (r->state == SFR_RUN_KEYED) && (r->eng.count > 0) && has_placeholder(r)) {
		sfrBurst_t b;
		dmr_tx_burst_t t;

		if (sfrEngineOutboundSlot(&r->eng, now_ms, &b) != SFR_TX_BURST) {
			return;
		}
		make_tx(r, &b, false, &t);
		int8_t after = r->modem->tx_fill(&t);

		if ((after < 0) || ((uint8_t)after >= r->fifo_count)) {
			// The placeholders went on air meanwhile: queued as usual
			if (r->modem->tx_queue(&t)) {
				r->in_modem++;
				remember_queued(r, &b, false);
			}
			return;
		}
		r->fifo[(r->fifo_head + r->fifo_count - 1u - (uint8_t)after) % sizeof(r->fifo)] = remember_sent(r, &b);
		r->stats.tx_filled++;
	}
}

// The engine's choice for the next outbound slot, while keyed
static void next_slot(sfrRunner_t *r, uint32_t now_ms)
{
	sfrBurst_t b;

	switch (sfrEngineOutboundSlot(&r->eng, now_ms, &b)) {
	case SFR_TX_BURST:
		queue_burst(r, &b, false, now_ms);
		break;
	case SFR_TX_IDLE:
		queue_burst(r, NULL, true, now_ms);
		break;
	case SFR_TX_END:
	case SFR_TX_NONE:
		if (r->beaconing && !due(now_ms, r->beacon_until)) {
			queue_burst(r, NULL, true, now_ms);	// the beacon goes on
			break;
		}
		r->beaconing = false;
		r->modem->tx_end();
		r->state = SFR_RUN_ENDING;
		break;
	}
	if (r->beaconing && (r->state == SFR_RUN_KEYED) && (r->eng.count > 0 || r->eng.rfCall || r->eng.netStream)) {
		r->beaconing = false;	// traffic: the repeater's own rules from here (hang, end)
	}
}

// Not keyed: start with what the engine has for the outbound slot. Aligned to the inbound call when one is being heard,
// else on our own slot clock (a stream from the host, nothing heard)
static void try_key(sfrRunner_t *r, uint32_t now_ms)
{
	if (!r->pending) {
		sfrTxAction_t a = sfrEngineOutboundSlot(&r->eng, now_ms, &r->pending_burst);

		if ((a != SFR_TX_BURST) && (a != SFR_TX_IDLE)) {
			if ((r->beacon_ms == 0U) || !due(now_ms, r->beacon_next) || r->eng.rfHold) {
				return;
			}
			// Nothing to repeat and the beacon is due: idle bursts for its duration
			r->beacon_next += r->beacon_ms;
			if (due(now_ms, r->beacon_next)) {
				r->beacon_next = now_ms + r->beacon_ms;	// fell behind (traffic): keep the interval from now
			}
			r->beacon_until = now_ms + r->beacon_len_ms;
			r->beaconing = true;
			r->stats.tx_beacons++;
			a = SFR_TX_IDLE;
		}
		if (a == SFR_TX_IDLE) {
			r->pending_burst.kind = 0xFF;	// marks an idle burst
		}
		r->pending = true;
	}
	int8_t out_ts = (int8_t)(2 - r->eng.cfg.inboundSlot);
	dmr_timing_t timing = r->eng.rfCall ? DMR_TIMING_ALIGNED : DMR_TIMING_OWN;

	if (!r->modem->tx_begin(timing, out_ts, DMR_ACCESS_IMPOLITE)) {	// a repeater is never polite (gotchas 1.8)
		r->stats.tx_rejected++;
		return;		// the burst waits for the next poll
	}
	if (r->modem->tx_listen != NULL) {
		r->modem->tx_listen();	// the inbound slot is heard between our bursts (sfr.md 3 item 1)
	}
	r->stats.tx_starts++;
	r->state = SFR_RUN_KEYED;
	r->in_modem = 0;
	r->fifo_count = 0;
	r->pending = false;
	queue_burst(r, &r->pending_burst, r->pending_burst.kind == 0xFF, now_ms);
}

void sfrRunnerPoll(sfrRunner_t *r, uint32_t now_ms)
{
	if (r->state == SFR_RUN_OFF) {
		return;
	}
	if ((r->beacon_ms != 0U) && (r->beacon_next == 0U)) {
		r->beacon_next = now_ms + r->beacon_ms;
	}
	if (r->state == SFR_RUN_IDLE) {
		try_key(r, now_ms);
	}
	fill_gaps(r, now_ms);
	while ((r->state == SFR_RUN_KEYED) && (r->in_modem < SFR_RUNNER_IN_MODEM) && (r->modem->tx_queue_space() > 0)) {
		uint8_t before = r->in_modem;

		next_slot(r, now_ms);
		if ((r->state == SFR_RUN_KEYED) && (r->in_modem == before)) {
			break;	// the modem refused it
		}
	}
	if (r->has_up && sfrUplinkLost(&r->up, now_ms)) {
		r->stats.up_lost++;
		if (r->up_ops.lost != NULL) {
			r->up_ops.lost(r->up_ops.ctx);
		}
	}
}

// ---- Inbound ----

static bool is_lc_type(uint8_t type)
{
	return (type == DT_VOICE_LC_HEADER) || (type == DT_TERMINATOR_WITH_LC);
}

static bool has_crc(uint8_t type)
{
	return (type == DT_VOICE_PI_HEADER) || (type == DT_CSBK) || (type == DT_MBC_HEADER) || (type == DT_DATA_HEADER) ||
	       is_lc_type(type);
}

// The modem's burst as the engine's: voice A-F with its AMBE, data with its info bits and checksum (headers and
// terminators remade from their LC, so payload[0-8] is the LC as the engine expects). False to drop it
static bool to_sfr_burst(sfrRunner_t *r, const dmr_burst_t *b, bool keyed, sfrBurst_t *s)
{
	memset(s, 0, sizeof(*s));
	if (b->kind == DMR_BURST_VOICE) {
		if (b->voice_index > 5) {
			return false;
		}
		s->kind = SFR_BURST_VOICE;
		s->type = b->voice_index;
		s->length = SFR_VOICE_LENGTH;
		memcpy(s->payload, b->payload, SFR_VOICE_LENGTH);
		return true;
	}
	if ((b->kind > DMR_BURST_RATE_1_DATA) || (b->len > SFR_VOICE_LENGTH)) {
		return false;
	}
	s->kind = SFR_BURST_DATA;
	s->type = b->kind;
	s->length = b->len;
	s->flags = SFR_BURST_FLAG_MCU_CRC;
	memcpy(s->payload, b->payload, b->len);
	if (b->kind == DMR_BURST_IDLE) {
		return true;	// the engine ignores it
	}

	// While keyed the chip's own CRC verdict is wrong at times (gotchas 14.1): check the info bits ourselves
	bool ok = (keyed || !b->in_step) ? ((b->len >= 12) && sfrDataBurstCrcOk(b->kind, b->payload)) : b->crc_ok;

	if (is_lc_type(b->kind)) {
		uint8_t lc[SFR_LC_LENGTH];
		uint8_t type = b->kind;

		if (ok) {
			memcpy(lc, b->lc_valid ? b->lc : b->payload, SFR_LC_LENGTH);
		} else {
			// A header or terminator of the inbound call with its tail garbled while we sent: rebuilt from the
			// call's LC
			type = keyed ? sfrLcBurstGuess(b->kind, b->payload, r->eng.rfLc, r->voice_since_header) : 0xFF;
			if ((type == 0xFF) || !sfrLcKnown(r->eng.rfLc)) {
				r->stats.rx_bad_crc++;
				return false;
			}
			memcpy(lc, r->eng.rfLc, SFR_LC_LENGTH);
			r->stats.rx_lc_rebuilt++;
		}
		s->type = type;
		s->length = 12;
		dmr_lc_encode_full(lc, (type == DT_VOICE_LC_HEADER) ? DMR_RS_MASK_VOICE_HEADER : DMR_RS_MASK_TERMINATOR,
				   s->payload);
		return true;
	}
	if (has_crc(b->kind) && !ok) {
		r->stats.rx_bad_crc++;
		return false;
	}
	return true;
}

static void rx_voice_fec(sfrRunner_t *r, const sfrBurst_t *s)
{
	for (uint8_t f = 0; f < 3; f++) {
		uint8_t bits[AMBE_PARAM_BITS];
		ambe_fec_result_t res;

		(void)ambeFecDecode(&s->payload[f * AMBE_FRAME_BYTES], bits, &res);
		r->stats.rx_fec_frames++;
		if (res.c0_failed) {
			r->stats.rx_fec_lost++;
		} else {
			r->stats.rx_fec_bits += res.errors;
			r->stats.rx_fec_pos[f] += res.errors;
		}
	}
}

static void rx_burst(sfrRunner_t *r, const dmr_burst_t *b, uint32_t now_ms)
{
	sfrBurst_t s;
	bool keyed = (r->state == SFR_RUN_KEYED) || (r->state == SFR_RUN_ENDING);

	r->stats.rx_bursts++;
	r->stats.last_rx = (uint8_t)((to_sfr_sync(b->sync) << 6) |
				     ((b->kind == DMR_BURST_VOICE) ? VOICE_TYPE_NIBBLE : (b->kind & 0x0F)));
	r->stats.last_rx_cc = (uint8_t)((b->cc & 0x0F) | (b->crc_ok ? 0x80 : 0));

	// A burst the modem places on the outbound slot is ours or another repeater's
	if ((b->ts != 0) && (b->ts != (int8_t)(r->eng.cfg.inboundSlot + 1))) {
		r->stats.rx_ignored++;
		r->stats.rx_ignored_slot++;
		return;
	}
	if (!to_sfr_burst(r, b, keyed, &s)) {
		return;
	}
	// A call's header run and its terminators are identical copies: in an open inbound call they are the caller's,
	// not our echo (HIL 2026-10-12: 2 of the bench's 8 headers repeated; the SFR glue never echo-checked them)
	bool lc_copy = r->eng.rfCall && (s.kind == SFR_BURST_DATA) &&
		       ((s.type == DT_VOICE_LC_HEADER) || (s.type == DT_TERMINATOR_WITH_LC));
	if (!lc_copy && is_echo(r, &s, now_ms)) {
		r->stats.rx_echo++;
		return;
	}
	// A voice burst outside a voice call starts one only as A then B 60 ms later (a lone A is noise; gotchas 14.5)
	if ((s.kind == SFR_BURST_VOICE) &&
	    (!sfrEngineRfVoiceCall(&r->eng) || ((now_ms - r->eng.rfLastMs) > SFR_RF_LOSS_MS))) {
		if (!sfrLateEntryVoice(&r->late, s.type, now_ms)) {
			r->stats.rx_late_held++;
			return;
		}
	}

	sfrRxResult_t res = sfrEngineRfBurst(&r->eng, &s, b->cc & 0x0F, to_sfr_sync(b->sync), now_ms);

	switch (res) {
	case SFR_RX_REPEATED:
		r->stats.rx_repeated++;
		if (s.kind == SFR_BURST_VOICE) {
			rx_voice_fec(r, &s);
		}
		fill_gaps(r, now_ms);
		break;
	case SFR_RX_IGNORED:
		r->stats.rx_ignored++;
		if ((b->cc & 0x0F) != r->eng.cfg.inboundCC) {
			r->stats.rx_ignored_cc++;
		}
		r->stats.last_ignored = (uint8_t)(((b->cc & 0x0F) << 4) | (s.type & 0x0F));
		return;
	case SFR_RX_DUPLICATE:
		r->stats.rx_duplicate++;
		return;
	case SFR_RX_TOT:
		r->stats.rx_tot++;
		break;
	case SFR_RX_FULL:
		r->stats.rx_full++;
		break;
	}
	if (s.kind == SFR_BURST_VOICE) {
		r->voice_since_header = true;
		// Late entry: the call's LC from the embedded signaling (the modem gives it complete with F)
		if (b->lc_valid && !sfrLcKnown(r->eng.rfLc) && dmr_lc_is_voice(b->lc)) {
			sfrEngineRfLc(&r->eng, b->lc);
		}
	} else if (s.type == DT_VOICE_LC_HEADER) {
		r->voice_since_header = false;
	}

	if (r->has_up) {
		uint8_t a = sfrUplinkBurst(&r->up, &s, sfrLcKnown(r->eng.rfLc), now_ms);

		if (a & SFR_UP_SEND) {
			r->stats.up_bursts++;
			r->up_ops.burst(r->up_ops.ctx, &s, (a & SFR_UP_HEADER_FIRST) ? r->eng.rfLc : NULL, b->rssi_dbm);
		}
	}
}

void sfrRunnerEvent(sfrRunner_t *r, uint8_t kind, const dmr_burst_t *b, uint32_t now_ms)
{
	if (r->state == SFR_RUN_OFF) {
		return;
	}
	switch (kind) {
	case DMR_EV_BURST_RX:
		if (b != NULL) {
			rx_burst(r, b, now_ms);
		}
		break;
	case DMR_EV_TX_SLOT_STARTED:
		if (r->in_modem > 0) {
			r->in_modem--;
		}
		aired(r, now_ms);
		if (r->state == SFR_RUN_KEYED) {
			sfrRunnerPoll(r, now_ms);	// the next slot's burst at once
		}
		break;
	case DMR_EV_TX_DONE:
		if (r->state == SFR_RUN_ENDING) {
			r->state = SFR_RUN_IDLE;
			r->in_modem = 0;
		}
		break;
	case DMR_EV_TX_REJECTED:
		r->stats.tx_rejected++;
		if ((r->state == SFR_RUN_KEYED) || (r->state == SFR_RUN_ENDING)) {
			r->modem->tx_abort();
			r->state = SFR_RUN_IDLE;
			r->in_modem = 0;
			sfrEngineEndHang(&r->eng);
		}
		break;
	default:
		break;
	}
}

// ---- Host side ----

uint8_t sfrRunnerNetBurst(sfrRunner_t *r, const sfrBurst_t *b, uint32_t now_ms)
{
	if (r->state == SFR_RUN_OFF) {
		return 2;	// MMDVM wrong mode
	}
	return sfrEngineNetBurst(&r->eng, b, NULL, now_ms);
}

void sfrRunnerNetAbort(sfrRunner_t *r)
{
	sfrEngineNetAbort(&r->eng);
}

uint8_t sfrRunnerSpace(const sfrRunner_t *r)
{
	return (r->state == SFR_RUN_OFF) ? 0 : sfrEngineSpace(&r->eng);
}

bool sfrRunnerTransmitting(const sfrRunner_t *r)
{
	return (r->state == SFR_RUN_KEYED) || (r->state == SFR_RUN_ENDING) || r->pending ||
	       ((r->state == SFR_RUN_IDLE) && (r->eng.count > 0) && !r->eng.rfHold);
}

bool sfrRunnerInboundBusy(const sfrRunner_t *r)
{
	return (r->state != SFR_RUN_OFF) && sfrEngineRfBusy(&r->eng);
}

const uint8_t *sfrRunnerCallLc(const sfrRunner_t *r)
{
	return r->eng.rfLc;
}

const sfrConfig_t *sfrRunnerConfig(const sfrRunner_t *r)
{
	return &r->eng.cfg;
}

#endif	// CONFIG_SFR
