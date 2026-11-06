// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// KTMicro AM/FM receiver: the stock firmware's register sequences (ktamfm.h)
#include "ktamfm.h"

#define READY_POLLS	10000u		// 0x1B bit 2, every 100 us: up to 1 s (the stock firmware waits up to 6.5 s)

static uint8_t rd(ktamfm_t *k, uint8_t reg)
{
	uint8_t v = 0;
	if (!k->bus->read(reg, &v)) {
		k->ok = false;
	}
	return v;
}

static void wr(ktamfm_t *k, uint8_t reg, uint8_t val)
{
	if (!k->bus->write(reg, val)) {
		k->ok = false;
	}
}

// Bits outside keep take set's
static void upd(ktamfm_t *k, uint8_t reg, uint8_t keep, uint8_t set)
{
	wr(k, reg, (uint8_t)((rd(k, reg) & keep) | set));
}

void ktamfm_attach(ktamfm_t *k, const ktamfm_bus_t *bus)
{
	k->bus = bus;
	k->ok = true;
	k->ready = false;
	k->am = false;
	k->fm = false;
}

bool ktamfm_probe(ktamfm_t *k)
{
	k->ok = true;
	uint16_t id = (uint16_t)(rd(k, 0x02) << 8 | rd(k, 0x03));
	return k->ok && id == KTAMFM_ID;
}

// Out of standby (inferred from the order: two enables, then a power-down bit cleared)
static void wake(ktamfm_t *k)
{
	upd(k, 0x0E, (uint8_t)~0x20u, 0);
	k->bus->delay_us(1000);
	upd(k, 0x76, 0xFF, 0x02);
	k->bus->delay_us(5000);
	upd(k, 0x10, (uint8_t)~0x40u, 0);
}

bool ktamfm_init(ktamfm_t *k)
{
	if (k->ready) {
		return true;
	}
	k->ok = true;
	wake(k);
	k->bus->delay_us(50000);
	upd(k, 0x06, 0xF8, 0x02);
	wr(k, 0x07, 0x9C);
	upd(k, 0x08, 0xF0, 0x08);
	wr(k, 0x09, 0x00);
	wr(k, 0x0A, 0x00);
	upd(k, 0x0D, (uint8_t)~0x10u, 0);
	upd(k, 0x04, 0xF8, 0x80);
	bool up = false;
	for (uint32_t i = 0; i < READY_POLLS && k->ok; i++) {
		if (rd(k, 0x1B) & 0x04u) {
			up = true;
			break;
		}
		k->bus->delay_us(100);
	}
	upd(k, 0x66, (uint8_t)~0x08u, 0);
	upd(k, 0x38, 0x8F, 0x50);
	wr(k, 0x62, 0x51);
	upd(k, 0x2B, 0x76, 0x08);
	wr(k, 0x2F, 0x3F);
	upd(k, 0x7E, 0x0F, 0x20);
	upd(k, 0x16, 0xEF, 0x10);
	wr(k, 0x57, 0x20);
	upd(k, 0x24, 0x7F, 0);
	upd(k, 0x41, 0xC0, 0x10);
	upd(k, 0x43, 0x3F, 0x40);
	upd(k, 0x2D, 0x9F, 0);
	wr(k, 0x42, 0x32);
	upd(k, 0x3B, 0xFF, 0x80);
	upd(k, 0x0F, (uint8_t)~KTAMFM_VOL_MASK, KTAMFM_VOL_MUTE);
	k->ready = up && k->ok;
	k->am = false;
	k->fm = false;
	return k->ready;
}

bool ktamfm_am(ktamfm_t *k)
{
	k->ok = true;
	upd(k, 0x88, 0xFF, 0x40);	// AM (clear: FM)
	wr(k, 0x8D, 0x4C);
	upd(k, 0x8C, 0x80, 0x04);
	upd(k, 0x3A, 0xC0, 0x19);
	upd(k, 0x52, 0xC0, 0x19);
	upd(k, 0x0E, 0xFF, 0x10);
	k->am = k->ok;
	k->fm = false;
	return k->ok;
}

bool ktamfm_tune_am(ktamfm_t *k, uint32_t khz)
{
	if (khz > 0x7FFFu) {
		return false;
	}
	k->ok = true;
	wr(k, 0x8D, (uint8_t)khz);
	upd(k, 0x8C, 0x80, (uint8_t)((khz >> 8) & 0x7Fu));
	upd(k, 0x88, 0xFF, 0x80);	// tune
	return k->ok;
}

bool ktamfm_fm(ktamfm_t *k)
{
	k->ok = true;
	upd(k, 0x88, (uint8_t)~0x40u, 0);
	k->fm = k->ok;
	k->am = false;
	return k->ok;
}

// The three channels the stock firmware treats apart (84.45, 103.70 and 105.60 MHz): 0x87 bit 3 set and 0x85's low
// nibble; every other channel clears 0x87 bit 3 and leaves 0x85. What they avoid isn't known (spurs, inferred)
#define CH_SPUR_A	0x699u
#define CH_SPUR_B	0x81Au
#define CH_SPUR_C	0x840u

bool ktamfm_tune_fm(ktamfm_t *k, uint32_t khz)
{
	if (khz == 0 || khz > KTAMFM_FM_MAX_KHZ || khz % KTAMFM_FM_STEP_KHZ != 0) {
		return false;
	}
	uint32_t ch = khz / KTAMFM_FM_STEP_KHZ;
	k->ok = true;
	upd(k, 0x2F, (uint8_t)~0x02u, 0);
	if (ch == CH_SPUR_A || ch == CH_SPUR_B || ch == CH_SPUR_C) {
		upd(k, 0x87, 0xFF, 0x08);
		upd(k, 0x85, 0xF0, ch == CH_SPUR_A ? 0x03u : 0x08u);
	} else {
		upd(k, 0x87, (uint8_t)~0x08u, 0);
	}
	wr(k, 0x89, (uint8_t)ch);
	upd(k, 0x88, 0xF0, (uint8_t)(0x80u | (ch >> 8)));	// bits 11:8 and the tune strobe; bit 6 (AM) kept
	return k->ok;
}

bool ktamfm_fm_status(ktamfm_t *k, ktamfm_fm_status_t *st)
{
	k->ok = true;
	st->rssi = rd(k, 0xE6) & 0x7Fu;
	st->snr = rd(k, 0xE2) & 0x3Fu;
	int ref = (int)(rd(k, 0x79) & 0x7Fu) * 2;
	int e9 = rd(k, 0xE9);
	st->offset = (uint8_t)(ref > e9 ? ref - e9 : e9 - ref);
	st->station = st->rssi > 14u && st->snr >= 12u && st->offset < 12u;
	return k->ok;
}

bool ktamfm_volume(ktamfm_t *k, uint8_t level)
{
	k->ok = true;
	upd(k, 0x0F, (uint8_t)~KTAMFM_VOL_MASK, (uint8_t)(level & KTAMFM_VOL_MASK));
	return k->ok;
}

bool ktamfm_status(ktamfm_t *k, ktamfm_status_t *st)
{
	k->ok = true;
	st->level = rd(k, 0xEA) & 0x7Fu;
	st->carrier = rd(k, 0xEC) & 0x7Fu;
	return k->ok;
}
