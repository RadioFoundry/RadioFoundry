// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The KTMicro AM/FM receiver driver (drivers/bcast/ktamfm) on a register file: the ID check, the set-up's writes and
// its ready wait, AM mode, the AM tune word, broadcast FM mode, its tune word and the three channels set apart, its
// station test, the level and the squelch readings, and a dead bus
#include <stdio.h>
#include <string.h>
#include "bcast/ktamfm/ktamfm.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct {
	uint8_t reg[256];
	int ready_after;	// reads of 0x1B before bit 2 sets; -1 never
	int reads_1b;
	bool dead;
	uint32_t slept_us;
	uint8_t log_reg[64], log_val[64];
	int nlog;
} chip;

static bool rd(uint8_t reg, uint8_t *val)
{
	if (chip.dead) {
		return false;
	}
	if (reg == 0x1B) {
		chip.reads_1b++;
		if (chip.ready_after >= 0 && chip.reads_1b > chip.ready_after) {
			chip.reg[0x1B] |= 0x04u;
		}
	}
	*val = chip.reg[reg];
	return true;
}

static bool wr(uint8_t reg, uint8_t val)
{
	if (chip.dead) {
		return false;
	}
	chip.reg[reg] = val;
	if (chip.nlog < 64) {
		chip.log_reg[chip.nlog] = reg;
		chip.log_val[chip.nlog++] = val;
	}
	return true;
}

static void delay(uint32_t us) { chip.slept_us += us; }

static const ktamfm_bus_t bus = { .read = rd, .write = wr, .delay_us = delay };

static void reset(void)
{
	memset(&chip, 0, sizeof(chip));
	memset(chip.reg, 0xFF, sizeof(chip.reg));	// every kept bit set, so a cleared one shows
	chip.reg[0x02] = 0x4B;
	chip.reg[0x03] = 0x54;
	chip.reg[0x1B] = 0x00;
	chip.ready_after = 3;
}

static int write_index(uint8_t reg)
{
	for (int i = 0; i < chip.nlog; i++) {
		if (chip.log_reg[i] == reg) {
			return i;
		}
	}
	return -1;
}

int main(void)
{
	ktamfm_t k;

	reset();
	ktamfm_attach(&k, &bus);
	CHECK(ktamfm_probe(&k));
	chip.reg[0x03] = 0x55;
	CHECK(!ktamfm_probe(&k));
	chip.reg[0x03] = 0x54;

	// Set-up: wake (0x0E bit 5 and 0x10 bit 6 cleared, 0x76 bit 1 set), the fixed words, the masked fields, muted
	CHECK(ktamfm_init(&k));
	CHECK(k.ready && !k.am);
	CHECK(chip.reg[0x0E] == 0xDF);
	CHECK(chip.reg[0x76] == 0xFF);
	CHECK(chip.reg[0x10] == 0xBF);
	CHECK(chip.reg[0x06] == 0xFA);
	CHECK(chip.reg[0x07] == 0x9C);
	CHECK(chip.reg[0x08] == 0xF8);
	CHECK(chip.reg[0x09] == 0x00 && chip.reg[0x0A] == 0x00);
	CHECK(chip.reg[0x0D] == 0xEF);
	CHECK(chip.reg[0x04] == 0xF8);	// (0xFF & 0xF8) | 0x80
	CHECK(chip.reg[0x66] == 0xF7);
	CHECK(chip.reg[0x38] == 0xDF);
	CHECK(chip.reg[0x62] == 0x51);
	CHECK(chip.reg[0x2B] == 0x7E);
	CHECK(chip.reg[0x2F] == 0x3F);
	CHECK(chip.reg[0x7E] == 0x2F);
	CHECK(chip.reg[0x16] == 0xFF);
	CHECK(chip.reg[0x57] == 0x20);
	CHECK(chip.reg[0x24] == 0x7F);
	CHECK(chip.reg[0x41] == 0xD0);
	CHECK(chip.reg[0x43] == 0x7F);
	CHECK(chip.reg[0x2D] == 0x9F);
	CHECK(chip.reg[0x42] == 0x32);
	CHECK(chip.reg[0x3B] == 0xFF);
	CHECK((chip.reg[0x0F] & KTAMFM_VOL_MASK) == KTAMFM_VOL_MUTE);
	CHECK(chip.reads_1b == 4);
	CHECK(chip.slept_us >= 56000u);
	// Wake comes first, the fixed words after the 50 ms wait, the level last
	CHECK(write_index(0x0E) < write_index(0x07));
	CHECK(write_index(0x0F) == chip.nlog - 1);

	// Once only
	int n = chip.nlog;
	CHECK(ktamfm_init(&k));
	CHECK(chip.nlog == n);

	// AM: 0x88 bit 6, 0x8D / 0x8C, the two low-6-bit fields, 0x0E bit 4
	chip.reg[0x0E] = 0x00;
	CHECK(ktamfm_am(&k));
	CHECK(k.am);
	CHECK((chip.reg[0x88] & 0x40u) != 0);
	CHECK(chip.reg[0x8D] == 0x4C);
	CHECK(chip.reg[0x8C] == 0x84);
	CHECK(chip.reg[0x3A] == 0xD9 && chip.reg[0x52] == 0xD9);
	CHECK(chip.reg[0x0E] == 0x10);

	// Tune 29250 kHz (0x7242): low byte, then bits 14:8 under 0x8C's kept bit 7, then the tune strobe
	chip.reg[0x88] = 0x40;
	chip.reg[0x8C] = 0x80;
	chip.nlog = 0;
	CHECK(ktamfm_tune_am(&k, 29250));
	CHECK(chip.reg[0x8D] == 0x42);
	CHECK(chip.reg[0x8C] == 0xF2);
	CHECK(chip.reg[0x88] == 0xC0);
	CHECK(chip.nlog == 3 && chip.log_reg[0] == 0x8D && chip.log_reg[1] == 0x8C && chip.log_reg[2] == 0x88);
	CHECK(!ktamfm_tune_am(&k, 40000));

	// FM: 0x88 bit 6 cleared, nothing else written
	chip.reg[0x88] = 0xC5;
	chip.nlog = 0;
	CHECK(ktamfm_fm(&k));
	CHECK(k.fm && !k.am);
	CHECK(chip.reg[0x88] == 0x85);
	CHECK(chip.nlog == 1);

	// Tune 98.1 MHz (channel 1962 = 0x7AA): 0x2F bit 1 clear, 0x87 bit 3 clear, 0x89 the low byte, then 0x88 bits 3:0
	// with the strobe, bits 6:4 kept; 0x85 not written
	chip.reg[0x2F] = 0x3F;
	chip.reg[0x87] = 0xFF;
	chip.reg[0x85] = 0xA5;
	chip.reg[0x88] = 0x0F;
	chip.nlog = 0;
	CHECK(ktamfm_tune_fm(&k, 98100));
	CHECK(chip.reg[0x2F] == 0x3D);
	CHECK(chip.reg[0x87] == 0xF7);
	CHECK(chip.reg[0x85] == 0xA5);
	CHECK(chip.reg[0x89] == 0xAA);
	CHECK(chip.reg[0x88] == 0x87);
	CHECK(chip.nlog == 4 && chip.log_reg[2] == 0x89 && chip.log_reg[3] == 0x88);
	// The strobe keeps the mode bit
	chip.reg[0x88] = 0x40;
	CHECK(ktamfm_tune_fm(&k, 87500));	// channel 1750 = 0x6D6
	CHECK(chip.reg[0x88] == 0xC6 && chip.reg[0x89] == 0xD6);

	// The channels set apart: 103.7 MHz sets 0x87 bit 3 and 0x85's low nibble to 8, 84.45 MHz to 3
	chip.reg[0x87] = 0x00;
	chip.reg[0x85] = 0xA5;
	chip.nlog = 0;
	CHECK(ktamfm_tune_fm(&k, 103700));
	CHECK(chip.reg[0x87] == 0x08 && chip.reg[0x85] == 0xA8);
	CHECK(write_index(0x87) < write_index(0x85) && write_index(0x85) < write_index(0x89));
	CHECK(ktamfm_tune_fm(&k, 105600));
	CHECK(chip.reg[0x85] == 0xA8);
	CHECK(ktamfm_tune_fm(&k, 84450));
	CHECK(chip.reg[0x85] == 0xA3 && chip.reg[0x89] == 0x99 && (chip.reg[0x88] & 0x0Fu) == 0x06);

	// Off the 50 kHz grid, zero or past the 12-bit word: refused, nothing written
	chip.nlog = 0;
	CHECK(!ktamfm_tune_fm(&k, 98125));
	CHECK(!ktamfm_tune_fm(&k, 0));
	CHECK(!ktamfm_tune_fm(&k, KTAMFM_FM_MAX_KHZ + KTAMFM_FM_STEP_KHZ));
	CHECK(ktamfm_tune_fm(&k, KTAMFM_FM_MAX_KHZ));
	CHECK(chip.nlog == 4);

	// AM again clears the FM flag
	CHECK(ktamfm_am(&k));
	CHECK(k.am && !k.fm);

	// FM readings: 0xE6 7 bits, 0xE2 6 bits, the offset |2 x 0x79 - 0xE9|, and the station test on all three
	ktamfm_fm_status_t fs;
	chip.reg[0xE6] = 0x8F;		// 15
	chip.reg[0xE2] = 0xCC;		// 12
	chip.reg[0x79] = 0xC0;		// 0x40 x 2 = 128
	chip.reg[0xE9] = 139;		// offset 11
	CHECK(ktamfm_fm_status(&k, &fs));
	CHECK(fs.rssi == 15 && fs.snr == 12 && fs.offset == 11 && fs.station);
	chip.reg[0xE9] = 116;		// offset 12, below the reference
	CHECK(ktamfm_fm_status(&k, &fs));
	CHECK(fs.offset == 12 && !fs.station);
	chip.reg[0xE9] = 128;
	chip.reg[0xE6] = 14;
	CHECK(ktamfm_fm_status(&k, &fs));
	CHECK(fs.offset == 0 && !fs.station);
	chip.reg[0xE6] = 60;
	chip.reg[0xE2] = 11;
	CHECK(ktamfm_fm_status(&k, &fs));
	CHECK(!fs.station);

	// Level: bits 4:0 only
	chip.reg[0x0F] = 0xE0;
	CHECK(ktamfm_volume(&k, KTAMFM_VOL_AM));
	CHECK(chip.reg[0x0F] == 0xFE);
	CHECK(ktamfm_volume(&k, KTAMFM_VOL_MUTE));
	CHECK(chip.reg[0x0F] == 0xE1);

	// Squelch readings: 7 bits each
	chip.reg[0xEA] = 0xA5;
	chip.reg[0xEC] = 0x3C;
	ktamfm_status_t st;
	CHECK(ktamfm_status(&k, &st));
	CHECK(st.level == 0x25 && st.carrier == 0x3C);

	// Never ready: the set-up fails (and doesn't spin forever)
	reset();
	chip.ready_after = -1;
	ktamfm_attach(&k, &bus);
	CHECK(!ktamfm_init(&k));
	CHECK(!k.ready);

	// A dead bus: nothing succeeds
	reset();
	chip.dead = true;
	ktamfm_attach(&k, &bus);
	CHECK(!ktamfm_probe(&k));
	CHECK(!ktamfm_init(&k));
	CHECK(!ktamfm_am(&k));
	CHECK(!ktamfm_fm(&k));
	CHECK(!k.fm);
	CHECK(!ktamfm_tune_fm(&k, 98100));
	CHECK(!ktamfm_fm_status(&k, &fs));
	CHECK(!ktamfm_status(&k, &st));

	printf("ktamfmTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
