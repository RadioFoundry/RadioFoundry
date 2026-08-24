// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The MMDVM hotspot's SFR mode (see hotspotSfr.h)
#if CONFIG_SFR	// the whole file

#include <string.h>
#include "hotspot/hotspotSfr.h"
#include "hotspot/hotspot.h"
#include "hotspot/mmdvm.h"
#include "hotspot/dmrDataFrame.h"
#include "dmr/dmrData.h"
#include "dmr/fec/dmrFec.h"

// Command 0xD5's switches (spec 6.4): kept and reported in the extra status bytes; the ones that apply to any modem
// act here (framing, no data hold), the rest are the HR_C6000 driver's (not acted on by this generic layer)
#define DEBUG2_NO_DATA_HOLD	0x40

// The extra status bytes (spec 6.3), index = frame offset - 13
enum {
	X_BURSTS_WRITTEN = 1, X_RX_BURSTS = 2, X_RX_REPEATED = 3, X_SEND_REJECTED = 4, X_STATE = 11, X_FLAGS = 12,
	X_RX_ECHO = 15, X_RX_IGNORED = 16, X_RX_DUPLICATE = 17, X_LAST_RX = 18, X_LAST_RX_CC = 19, X_REG10 = 22,
	X_REG12 = 23, X_RX_IGNORED_SYNC = 25, X_RX_IGNORED_NO_CRC = 26, X_RSSI_MIN = 42, X_RAW_AREA = 47,
	X_LC_REBUILT = 86, X_REG11 = 93, X_FLAGS2 = 94, X_LATE_ENTRY_HELD = 104,
};
#define STATE_KEYED		0x01
#define STATE_OWN_TIMING	0x02
#define STATE_RF_CALL		0x10
#define STATE_BUSY		0x20

static sfrRunner_t run;
static bool wanted;		// the host's SET_CONFIG / SET_FREQ ask for it
static bool on;
static sfrChannelSettings_t settings = {
	.role = SFR_ROLE_HOTSPOT, .framing = SFR_FRAMING_AUTO, .inboundSlot = 0, .hangIndex = 0,
};
static uint8_t tot15;
static struct {
	uint8_t flags, framing, reg10, reg12, rssi_min, reg11, flags2;
} dbg = { .reg12 = 0xFF };
static uint8_t net_index;	// last voice position the host sent (for control byte 0)
static bool net_last_term;	// the host's last frame was a terminator: the repeats of it are ACKed and dropped

static void build_config(sfrConfig_t *c)
{
	sfrChannelSettings_t s = settings;

	s.role = SFR_ROLE_HOTSPOT;
	if ((dbg.framing != SFR_FRAMING_AUTO) && (dbg.framing < SFR_FRAMING_NUM)) {
		s.framing = dbg.framing;
	}
	s.outboundCCSet = false;
	sfrConfigBuild(&s, hotspot_color_code(), tot15, c);
}

static uint8_t up_command(bool lost)
{
	bool ts1 = sfrRunnerConfig(&run)->inboundSlot == 0;

	if (lost) {
		return ts1 ? MMDVM_DMR_LOST1 : MMDVM_DMR_LOST2;
	}
	return ts1 ? MMDVM_DMR_DATA1 : MMDVM_DMR_DATA2;
}

static void up_burst(void *ctx, const sfrBurst_t *b, const uint8_t *header_lc, int16_t rssi_dbm)
{
	uint8_t cmd = up_command(false);

	(void)ctx;
	if (header_lc != NULL) {
		hotspot_up_lc(cmd, DT_VOICE_LC_HEADER, header_lc, 0);	// made from the embedded LC (late entry)
	}
	if (b->kind == SFR_BURST_VOICE) {
		hotspot_up_voice(cmd, b->payload, b->type, sfrRunnerCallLc(&run), rssi_dbm);
	} else if ((b->type == DT_VOICE_LC_HEADER) || (b->type == DT_TERMINATOR_WITH_LC)) {
		hotspot_up_lc(cmd, b->type, b->payload, rssi_dbm);
	} else {
		hotspot_up_data(cmd, b->type, b->payload, b->length, rssi_dbm);
	}
}

static void up_lost(void *ctx)
{
	(void)ctx;
	hotspot_up_lost(up_command(true));
}

static const sfrRunnerUplink_t uplink = { .burst = up_burst, .lost = up_lost, .ctx = NULL };

static void start_runner(void)
{
	sfrConfig_t c;

	build_config(&c);
	sfrRunnerStart(&run, hotspot_modem(), &c, &uplink);
	sfrRunnerSetNoDataHold(&run, (dbg.flags2 & DEBUG2_NO_DATA_HOLD) != 0);
	net_last_term = false;
}

static void turn_on(void)
{
	if (on || !wanted || !hotspot_running() || (hotspot_modem() == NULL)) {
		return;
	}
	hotspot_alt_takeover();		// the simplex engine drops what it had
	start_runner();
	on = true;
}

static void turn_off(void)
{
	if (on) {
		sfrRunnerStop(&run);
		on = false;
	}
}

// ---- hotspot_alt_t ----

static bool alt_active(void)
{
	return on;
}

static void alt_poll(uint32_t now_ms)
{
	if (!on) {
		turn_on();
	}
	if (on) {
		sfrRunnerPoll(&run, now_ms);
	}
}

static void alt_event(uint8_t kind, const dmr_burst_t *burst, uint32_t now_ms)
{
	sfrRunnerEvent(&run, kind, burst, now_ms);
}

static uint8_t alt_net_frame(uint8_t command, uint8_t control, const uint8_t *frame, uint32_t now_ms)
{
	sfrBurst_t b;
	uint8_t c = hotspot_frame_control(control, frame);

	(void)command;		// either slot's command: the outbound slot's stream (spec 6.2)
	memset(&b, 0, sizeof(b));
	if (c & MMDVM_DMR_SYNC_DATA) {
		uint8_t dt = c & 0x0F;

		if (dt == DT_IDLE) {
			return 0;	// the radio makes its own
		}
		if (dt == DT_TERMINATOR_WITH_LC) {
			if (net_last_term) {
				return 0;	// the second and third terminators
			}
		}
		b.kind = SFR_BURST_DATA;
		b.type = dt;
		b.flags = SFR_BURST_FLAG_MCU_CRC;
		if ((dt == DT_VOICE_LC_HEADER) || (dt == DT_TERMINATOR_WITH_LC) || (dt == DT_VOICE_PI_HEADER)) {
			dmrFecBptcDecode(frame, b.payload);	// 12 bytes: the LC (or PI) and its checksum
			b.length = 12;
		} else {
			dmrBurst_t d;

			if (!dmrDataFrameToBurst(dt, frame, &d) || (d.length > SFR_VOICE_LENGTH)) {
				return MMDVM_NAK_INVALID;
			}
			b.length = d.length;
			memcpy(b.payload, d.payload, d.length);
		}
	} else {
		uint8_t index;

		if (c & MMDVM_DMR_SYNC_AUDIO) {
			index = 0;
		} else if (((c & 0x0F) >= 1) && ((c & 0x0F) <= 5)) {
			index = c & 0x0F;
		} else {
			index = (uint8_t)((net_index + 1) % 6);	// control byte 0: B-F follow in order
		}
		b.kind = SFR_BURST_VOICE;
		b.type = index;
		b.length = SFR_VOICE_LENGTH;
		hotspot_frame_voice(frame, b.payload);
		net_index = index;
	}

	uint8_t r = sfrRunnerNetBurst(&run, &b, now_ms);

	if (r == 0) {
		net_last_term = (b.kind == SFR_BURST_DATA) && (b.type == DT_TERMINATOR_WITH_LC);
	}
	return r;
}

static void alt_net_abort(void)
{
	sfrRunnerNetAbort(&run);
}

static bool alt_transmitting(void)
{
	return sfrRunnerTransmitting(&run);
}

static void alt_stopped(void)
{
	turn_off();
}

static const hotspot_alt_t alt = {
	.active = alt_active, .poll = alt_poll, .event = alt_event, .net_frame = alt_net_frame,
	.net_abort = alt_net_abort, .transmitting = alt_transmitting, .stopped = alt_stopped,
};

// ---- mmdvm_ext_t ----

static uint8_t ext_vendor(uint8_t command, const uint8_t *p, uint8_t len)
{
	(void)command;
	if (len < 2) {
		return MMDVM_NAK_INVALID;
	}
	uint8_t old_framing = dbg.framing;

	dbg.flags = p[0];
	dbg.framing = (p[1] < SFR_FRAMING_NUM) ? p[1] : SFR_FRAMING_AUTO;
	dbg.reg10 = (len > 2) ? p[2] : 0;
	dbg.reg12 = (len > 3) ? p[3] : 0xFF;
	dbg.rssi_min = (len > 4) ? p[4] : 0;
	dbg.reg11 = (len > 5) ? p[5] : 0;
	dbg.flags2 = (len > 6) ? p[6] : 0;
	if (on) {
		if (dbg.framing != old_framing) {
			sfrRunnerStop(&run);	// new framing: unkey, start over
			start_runner();
		} else {
			sfrRunnerSetNoDataHold(&run, (dbg.flags2 & DEBUG2_NO_DATA_HOLD) != 0);
		}
	}
	return 0;
}

static uint8_t ext_status_extra(uint8_t *buf, uint8_t max)
{
	if (!on || (max < HOTSPOT_SFR_STATUS_EXTRA)) {
		return 0;
	}
	const sfrRunnerStats_t *st = &run.stats;
	uint8_t state = 0;

	memset(buf, 0, HOTSPOT_SFR_STATUS_EXTRA);
	if ((run.state == SFR_RUN_KEYED) || (run.state == SFR_RUN_ENDING)) {
		state |= STATE_KEYED;
		if (!run.eng.rfCall) {
			state |= STATE_OWN_TIMING;
		}
	}
	if (run.eng.rfCall) {
		state |= STATE_RF_CALL;
	}
	if (sfrRunnerInboundBusy(&run)) {
		state |= STATE_BUSY;
	}
	buf[X_BURSTS_WRITTEN] = (uint8_t)(st->tx_bursts + st->tx_idle);
	buf[X_RX_BURSTS] = (uint8_t)st->rx_bursts;
	buf[X_RX_REPEATED] = (uint8_t)st->rx_repeated;
	buf[X_SEND_REJECTED] = (uint8_t)st->tx_rejected;
	buf[X_STATE] = state;
	buf[X_FLAGS] = dbg.flags;
	buf[X_RX_ECHO] = (uint8_t)st->rx_echo;
	buf[X_RX_IGNORED] = (uint8_t)st->rx_ignored;
	buf[X_RX_DUPLICATE] = (uint8_t)st->rx_duplicate;
	buf[X_LAST_RX] = st->last_rx;
	buf[X_LAST_RX_CC] = st->last_rx_cc;
	buf[X_REG10] = dbg.reg10;
	buf[X_REG12] = dbg.reg12;
	buf[X_RX_IGNORED_SYNC] = (uint8_t)st->rx_ignored_slot;
	buf[X_RX_IGNORED_NO_CRC] = (uint8_t)st->rx_bad_crc;
	buf[X_RSSI_MIN] = dbg.rssi_min;
	buf[X_RAW_AREA] = 0xFF;		// -1: raw areas not used by this layer
	buf[X_LC_REBUILT] = (uint8_t)st->rx_lc_rebuilt;
	buf[X_REG11] = dbg.reg11;
	buf[X_FLAGS2] = dbg.flags2;
	buf[X_LATE_ENTRY_HELD] = (uint8_t)st->rx_late_held;
	return HOTSPOT_SFR_STATUS_EXTRA;
}

static uint8_t ext_version_suffix(char *buf, uint8_t max)
{
	sfrConfig_t c;
	int n;

	build_config(&c);	// announced whether on or not (spec 6.1)
	n = sfrMmdvmVersionSuffix(&c, buf, max);
	if (n < 0) {
		return 0;
	}
	return (uint8_t)((n >= max) ? ((max > 0) ? (max - 1) : 0) : n);
}

static void ext_config_changed(bool simplex, uint32_t rx_hz, uint32_t tx_hz)
{
	wanted = sfrMmdvmTrigger(simplex, rx_hz, tx_hz);
	if (!wanted) {
		if (on) {
			turn_off();
			hotspot_alt_takeover();	// the simplex engine starts clean
		}
		return;
	}
	if (on && (sfrRunnerConfig(&run)->inboundCC != hotspot_color_code())) {
		sfrRunnerStop(&run);		// a new colour code: start over
		start_runner();
	}
	turn_on();
}

static uint8_t ext_status_flags(void)
{
	return (on && sfrRunnerInboundBusy(&run)) ? MMDVM_STATUS_INBOUND_BUSY : 0;
}

static void ext_space(uint8_t *slot1, uint8_t *slot2)
{
	if (on) {
		uint8_t s = sfrRunnerSpace(&run);

		*slot1 = s;	// both carry the outbound slot's space (spec 6.2)
		*slot2 = s;
	}
}

static const mmdvm_ext_t ext = {
	.vendor = ext_vendor, .status_extra = ext_status_extra, .version_suffix = ext_version_suffix,
	.config_changed = ext_config_changed, .status_flags = ext_status_flags, .space = ext_space,
};

void hotspot_sfr_init(void)
{
	memset(&run, 0, sizeof(run));
	wanted = false;
	on = false;
	memset(&dbg, 0, sizeof(dbg));
	dbg.reg12 = 0xFF;
	net_index = 5;
	net_last_term = false;
	hotspot_set_alt(&alt);
	mmdvm_set_ext(&ext);
}

void hotspot_sfr_set_settings(const sfrChannelSettings_t *s, uint8_t tot_15s)
{
	settings = *s;
	tot15 = tot_15s;
}

void hotspot_sfr_get_view(hotspot_sfr_view_t *v)
{
	const sfrConfig_t *c = sfrRunnerConfig(&run);
	sfrConfig_t preview;

	memset(v, 0, sizeof(*v));
	if (!on) {
		build_config(&preview);
		c = &preview;
	}
	v->on = on;
	v->keyed = on && ((run.state == SFR_RUN_KEYED) || (run.state == SFR_RUN_ENDING));
	v->inbound_busy = on && sfrRunnerInboundBusy(&run);
	v->inbound_slot = c->inboundSlot;
	v->framing = c->framing;
	v->stats = run.stats;
}

#endif	// CONFIG_SFR
