// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The split duplex TX half's continuous BS outbound on the HR_C6000 (see hrc6000_bs.h)
#include <string.h>
#include "hrc6000_bs.h"

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT	// the whole file

#define R_MODE		0x10
#define R_CHAN		0x11
#define R_ACCESS0	0x20
#define R_ACCESS1	0x21
#define R_CODEC_CTL	0x22
#define R_L2_CTL	0x40
#define R_SLOT_CTL	0x41
#define R_TX_TYPE	0x50
#define R_CACH_TX	0x52	// written: the BS downstream CACH's AT (bit 3) and TC (bit 2) (modem-hrc6000.md 2)
#define R_INT_FLAGS	0x82
#define R_INT_CLEAR	0x83

#define L2_ACTIVE	0xE3	// own timing (modem-hrc6000.md 6.2)
#define L2_PASSIVE	0xC3
#define L2_MCU_CRC	0x08
#define CTL_TX		0x80
#define CTL_NONE	0x00
#define MODE_DMR	0x6E	// the operation value (hrc6000_dmr_enter)
#define MODE_LAYER_MASK	0x18	// 0x10 bits 4-3: 00 physical layer, 01 layer 2, 10 layer 3
#define RAW_HALF0	0x00	// TX RAM (page 2): the physical layer mode's two slot buffers
#define RAW_HALF1	0x24
#define CHAN_DIRECT	0x80	// direct simplex (modem-hrc6000.md 2)
#define PAGE2_SLC	0x24	// TX RAM: the short LC, 28 bits (0x24-0x27 bits 7-4) and its CRC-8 (0x28)
#define STALL_US	200000u

static const uint8_t SILENCE[DMR_VOICE_BYTES] = {
	0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B, 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
	0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
};
// The idle message's information bits (ETSI TS 102 361-1 table D.2)
static const uint8_t IDLE_INFO[12] = { 0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91 };

static bool w(hrc6000_dmr_t *e, uint8_t reg, uint8_t val)
{
	return hrc_write(e->dev, HRC_PAGE_SYS, reg, val);
}

static void lock(hrc6000_dmr_t *e)
{
	if (e->ops->lock != NULL) {
		e->ops->lock(e->ops->ctx);
	}
}

static void unlock(hrc6000_dmr_t *e)
{
	if (e->ops->unlock != NULL) {
		e->ops->unlock(e->ops->ctx);
	}
}

static int8_t ts_of(const hrc6000_bs_t *bs, uint32_t tick)
{
	return (((tick - bs->base_tick) & 1u) != 0) ? 1 : 2;	// base_tick + 1 starts TS1
}

// Our framing (variants 0, 255 with a raw slot builder): the physical layer mode; variant 1: the chip's layer 2 BS
// framing (0x10 = 0x6E as in normal operation)
static bool raw_mode(const hrc6000_bs_t *bs)
{
	return (bs->cfg.variant != HRC6000_BS_VARIANT_CHIP) && (bs->raw_slot != NULL);
}

static void variant_regs(hrc6000_dmr_t *e, hrc6000_bs_t *bs, bool on)
{
	w(e, R_MODE, (on && raw_mode(bs)) ? (uint8_t)(MODE_DMR & ~MODE_LAYER_MASK) : MODE_DMR);
	for (uint8_t i = 0; on && (i < bs->cfg.nregs) && (i < DMR_BS_REGS_MAX); i++) {
		// Restored at unkey to the values of the init sequence the registers came from: only 0x10 is known, so a
		// raw write elsewhere stays until the next DMR entry (hrc6000_dmr_enter)
		w(e, bs->cfg.regs[i][0], bs->cfg.regs[i][1]);
	}
	if (!on && (bs->cfg.nregs > 0)) {
		w(e, R_CHAN, CHAN_DIRECT);
		w(e, R_L2_CTL, bs->reg40);
	}
	bs->regs_written = on;
}

bool hrc6000_bs_start(hrc6000_bs_t *bs, hrc6000_dmr_t *e, const dmr_bs_config_t *cfg, hrc6000_bs_raw_fn raw_slot)
{
	lock(e);
	bool ok = e->running && (e->bs == NULL) && (e->tx_state == 0);

	if (ok) {
		memset(bs, 0, sizeof(*bs));
		bs->cfg = *cfg;
		bs->raw_slot = raw_slot;
		hrc6000_dmr_set_cc(e->dev, cfg->cc);
		hrc6000_dmr_set_sync(e->dev, DMR_SYNC_BS);	// BS data sync; the chip sends BS voice sync as its inverse
		w(e, R_ACCESS0, 0x00);
		w(e, R_ACCESS1, hrc6000_dmr_access_reg(DMR_ACCESS_IMPOLITE));
		w(e, R_CODEC_CTL, 0x86);			// encoder start (modem-hrc6000.md 6.2)
		w(e, R_SLOT_CTL, CTL_NONE);
		bs->reg40 = L2_ACTIVE;
		w(e, R_L2_CTL, L2_ACTIVE);			// own timing: the slot clock runs from here
		bs->base_tick = e->tick;
		e->sync_cur = DMR_SYNC_BS;
		e->bs = bs;
	}
	unlock(e);
	return ok;
}

void hrc6000_bs_key(hrc6000_dmr_t *e, bool on)
{
	lock(e);
	hrc6000_bs_t *bs = e->bs;

	if (bs != NULL) {
		if (on && !bs->keyed) {
			bs->st.keyups++;
		}
		bs->keyed = on;
		if (!on) {
			bs->head[0] = bs->tail[0];
			bs->head[1] = bs->tail[1];
		}
	}
	unlock(e);
}

static uint8_t q_count(const hrc6000_bs_t *bs, int k)
{
	return (uint8_t)((bs->head[k] + HRC6000_BS_Q + 1u - bs->tail[k]) % (HRC6000_BS_Q + 1u));
}

bool hrc6000_bs_queue(hrc6000_dmr_t *e, int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach,
		      const uint8_t *emb_lc)
{
	lock(e);
	hrc6000_bs_t *bs = e->bs;
	int k = (ts == 1) ? 0 : 1;
	bool ok = (bs != NULL) && (q_count(bs, k) < HRC6000_BS_Q);

	if (ok) {
		// Every place of the ring holds a burst: one place was once left without storage, and the ISR read
		// past q[k] there (every fourth TS2 burst went out as a zeroed PI header; gotchas 8.x)
		uint8_t at = bs->head[k];

		bs->q[k][at] = *b;
		bs->q_cach_valid[k][at] = cach != NULL;
		if (cach != NULL) {
			bs->q_cach[k][at] = *cach;
		}
		bs->q_lc_valid[k][at] = emb_lc != NULL;
		if (emb_lc != NULL) {
			memcpy(bs->q_lc[k][at], emb_lc, 9);
		}
		bs->head[k] = (uint8_t)((bs->head[k] + 1u) % (HRC6000_BS_Q + 1u));
		bs->st.cach_given += cach != NULL;
	}
	unlock(e);
	return ok;
}

uint8_t hrc6000_bs_space(const hrc6000_dmr_t *e, int8_t ts)
{
	const hrc6000_bs_t *bs = e->bs;

	if (bs == NULL) {
		return 0;
	}
	return (uint8_t)(HRC6000_BS_Q - q_count(bs, (ts == 1) ? 0 : 1));
}

void hrc6000_bs_stop(hrc6000_dmr_t *e)
{
	lock(e);
	hrc6000_bs_t *bs = e->bs;

	if (bs != NULL) {
		if (bs->pa) {
			if (!e->ops->rf(e->ops->ctx, false)) {
				bs->st.rf_fail++;
			}
			bs->pa = false;
			e->pa_on = false;
		}
		if (bs->regs_written) {
			variant_regs(e, bs, false);
		}
		w(e, R_SLOT_CTL, CTL_NONE);
		hrc6000_dmr_set_sync(e->dev, DMR_SYNC_MS);
		e->sync_cur = DMR_SYNC_MS;
		w(e, R_L2_CTL, L2_PASSIVE);
		e->reg40 = L2_PASSIVE;
		e->bs = NULL;
	}
	unlock(e);
	hrc6000_dmr_rx_restart(e);	// the slot engine receives again
}

bool hrc6000_bs_slot_clock(const hrc6000_dmr_t *e, uint32_t *counter, uint32_t *t_us, int8_t *ts)
{
	const hrc6000_bs_t *bs = e->bs;

	if (!e->timing) {
		return false;
	}
	*counter = e->tick;
	*t_us = e->t_tick;
	*ts = (bs != NULL) ? bs->ts_tick : 0;
	return true;
}

bool hrc6000_bs_keyed(const hrc6000_dmr_t *e)
{
	return (e->bs != NULL) && e->bs->pa;
}

// --- ISR side ---

static void write_page2(hrc6000_dmr_t *e, uint8_t addr, const uint8_t *data, uint8_t len, uint8_t size)
{
	uint8_t buf[24];

	if (size > sizeof(buf)) {
		size = sizeof(buf);
	}
	memset(buf, 0, size);
	memcpy(buf, data, (len < size) ? len : size);
	hrc_write_burst(e->dev, HRC_PAGE_DATA, addr, buf, size);
}

// One burst for the next slot (modem-hrc6000.md 6.1, 6.4); ts is that slot
static void setup(hrc6000_dmr_t *e, hrc6000_bs_t *bs, int8_t ts, const dmr_tx_burst_t *b, const uint8_t *lc,
		  const dmr_cach_t *cach)
{
	int k = ts - 1;

	if (raw_mode(bs)) {
		// The whole slot is ours: the chip sends TX RAM 0x00-0x23 and 0x24-0x47 in alternate slots and takes its half
		// at the slot start, so the frame goes to both (the parity isn't known; modem-hrc6000.md 9.1)
		uint8_t raw[HRC6000_BS_RAW_BYTES];

		if ((b->kind == DMR_BURST_VOICE) && ((b->voice_index % 6u) == 0) && (lc != NULL)) {
			memcpy(bs->lc[k], lc, 9);
			bs->lc_valid[k] = true;
		}
		if (!bs->raw_slot(b, cach, ts, bs->cfg.cc, bs->lc_valid[k] ? bs->lc[k] : NULL, raw)) {
			bs->st.uncoded++;
		}
		hrc_write_burst(e->dev, HRC_PAGE_DATA, RAW_HALF0, raw, sizeof(raw));
		hrc_write_burst(e->dev, HRC_PAGE_DATA, RAW_HALF1, raw, sizeof(raw));
		bs->st.raw++;
		bs->st.cach_written += cach != NULL;
		w(e, R_SLOT_CTL, CTL_TX);
		bs->st.bursts++;
		return;
	}
	bool voice = b->kind == DMR_BURST_VOICE;
	uint8_t idx = (uint8_t)(b->voice_index % 6u);
	bool mcu_crc = !voice && (b->kind != DMR_BURST_IDLE) && b->host_crc;
	uint8_t r40 = (uint8_t)((bs->reg40 & (uint8_t)~L2_MCU_CRC) | (mcu_crc ? L2_MCU_CRC : 0u));

	if (r40 != bs->reg40) {
		w(e, R_L2_CTL, r40);
		bs->reg40 = r40;
	}
	if (voice) {
		if ((idx == 0) && (lc != NULL)) {
			memcpy(bs->lc[k], lc, 9);
			bs->lc_valid[k] = true;
		}
		// Page 2 0x00 is shared by both slots (the other slot's header or data overwrites it): this slot's
		// embedded LC goes back before its A-E
		if ((idx <= 4) && bs->lc_valid[k] && (bs->page2_slot != ts)) {
			write_page2(e, 0x00, bs->lc[k], 9, 12);
			bs->page2_slot = ts;
			bs->st.page2_lc++;
		}
		uint8_t frame[DMR_VOICE_BYTES];

		memcpy(frame, (b->len >= DMR_VOICE_BYTES) ? b->payload : SILENCE, DMR_VOICE_BYTES);
		hrc6000_dmr_voice(e->dev, true, frame);
		bs->last_index[k] = idx;
	} else if (b->kind == DMR_BURST_IDLE) {
		write_page2(e, 0x18, (b->len > 0) ? b->payload : IDLE_INFO, (b->len > 0) ? b->len : 12, 12);
	} else {
		uint8_t size = hrc6000_dmr_payload_len(b->kind);
		uint8_t len = b->len;

		if (!b->host_crc && ((b->kind == DMR_BURST_VOICE_LC_HEADER) || (b->kind == DMR_BURST_TERMINATOR_LC)) &&
		    (len > 9)) {
			len = 9;	// the chip computes the RS(12,9) parity
		}
		write_page2(e, 0x00, b->payload, len, size);
		bs->page2_slot = 0;
		if (b->kind == DMR_BURST_TERMINATOR_LC) {
			uint8_t frame[DMR_VOICE_BYTES];

			memcpy(frame, SILENCE, sizeof(frame));
			hrc6000_dmr_voice(e->dev, true, frame);
		}
	}
	// The CACH before this burst: the manual has the chip frame it from AT / TC in 0x52 and the short LC in page 2;
	// on air it stays all zero (modem-hrc6000.md 9.2), which is why our framing is the default

	if (cach != NULL) {
		w(e, R_CACH_TX, (uint8_t)(((cach->at & 1u) << 3) | ((cach->tc & 1u) << 2)));
		if (cach->lcss == 1u) {	// the first fragment (dmr_cach_t): the set's short LC
			uint8_t slc[5] = { (uint8_t)(cach->slc >> 20), (uint8_t)(cach->slc >> 12), (uint8_t)(cach->slc >> 4),
					   (uint8_t)(cach->slc << 4), cach->slc_crc };
			hrc_write_burst(e->dev, HRC_PAGE_DATA, PAGE2_SLC, slc, sizeof(slc));
		}
		bs->st.cach_written++;
	}
	w(e, R_SLOT_CTL, CTL_TX);
	w(e, R_TX_TYPE, hrc6000_dmr_reg50(b->kind, idx));
	bs->st.bursts++;
}

void hrc6000_bs_isr_timeslot(hrc6000_dmr_t *e)
{
	hrc6000_bs_t *bs = e->bs;
	uint32_t t = e->ops->now_us(e->ops->ctx);

	e->tick++;
	e->st.ticks++;
	e->t_tick = t;
	e->timing = true;
	bs->st.slots++;
	bs->ts_tick = ts_of(bs, e->tick);
	int8_t next = ts_of(bs, e->tick + 1u);

	if (!bs->keyed) {
		if (bs->pa) {
			if (!e->ops->rf(e->ops->ctx, false)) {
				bs->st.rf_fail++;
			}
			bs->pa = false;
			e->pa_on = false;
		}
		if (bs->regs_written) {
			variant_regs(e, bs, false);
		}
		bs->on_air = false;
		w(e, R_SLOT_CTL, CTL_NONE);
		return;
	}
	if (!bs->on_air && (next != 1)) {
		w(e, R_SLOT_CTL, CTL_NONE);	// a key-up starts with an outbound TS1 slot (hal_dmr_modem.h)
		return;
	}
	if (!bs->regs_written) {
		variant_regs(e, bs, true);
	}
	bs->on_air = true;
	int k = next - 1;
	dmr_tx_burst_t idle;
	const dmr_tx_burst_t *b;
	const uint8_t *lc = NULL;

	if (q_count(bs, k) > 0) {
		uint8_t at = bs->tail[k];

		b = &bs->q[k][at];
		lc = bs->q_lc_valid[k][at] ? bs->q_lc[k][at] : NULL;
		setup(e, bs, next, b, lc, bs->q_cach_valid[k][at] ? &bs->q_cach[k][at] : NULL);
		bs->tail[k] = (uint8_t)((at + 1u) % (HRC6000_BS_Q + 1u));
	} else {
		memset(&idle, 0, sizeof(idle));
		idle.kind = DMR_BURST_IDLE;
		idle.len = sizeof(IDLE_INFO);
		memcpy(idle.payload, IDLE_INFO, sizeof(IDLE_INFO));
		setup(e, bs, next, &idle, NULL, NULL);
		bs->st.underruns++;
	}
}

void hrc6000_bs_isr_sys(hrc6000_dmr_t *e)
{
	uint8_t flags = 0;

	(void)hrc_read(e->dev, HRC_PAGE_SYS, R_INT_FLAGS, &flags);
	if (flags != 0) {
		w(e, R_INT_CLEAR, flags);	// nothing is received meanwhile: keep the line from staying low
	}
	e->st.sys_ints++;
}

void hrc6000_bs_isr_rftx(hrc6000_dmr_t *e)
{
	hrc6000_bs_t *bs = e->bs;

	e->st.rftx_ints++;
	if (!bs->keyed || bs->pa) {
		return;		// keyed once for the whole outbound
	}
	if (e->ops->rf(e->ops->ctx, true)) {
		bs->pa = true;
		e->pa_on = true;
		e->t_pa_on = e->ops->now_us(e->ops->ctx);
	} else {
		bs->st.rf_fail++;	// tried again at the next slot's interrupt
	}
}

void hrc6000_bs_poll(hrc6000_dmr_t *e)
{
	lock(e);
	hrc6000_bs_t *bs = e->bs;

	if (bs != NULL) {
		uint32_t t = e->ops->now_us(e->ops->ctx);

		if (e->timing && ((uint32_t)(t - e->t_tick) > STALL_US)) {
			e->timing = false;		// the slot clock stopped: never leave the PA up without it
			if (bs->keyed || bs->pa) {
				bs->st.stalls++;
			}
			bs->keyed = false;
			if (bs->pa) {
				if (!e->ops->rf(e->ops->ctx, false)) {
					bs->st.rf_fail++;
				}
				bs->pa = false;
				e->pa_on = false;
			}
		}
	}
	unlock(e);
}

#endif	// CONFIG_HOTSPOT_SPLIT
