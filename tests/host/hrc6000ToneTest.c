// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C6000 FM TX tone generator (drivers/modem/hrc6000, hrc6000_tx_pair / hrc6000_tx_sig_key; modem-hrc6000.md 8):
// a single tone goes out from the 2-tone mode with every tone entry rewritten, a DTMF pair as 16 codes of digit "1"
// with the 697 and 1209 Hz table entries rewritten, the microphone inputs off; keying zeroes the deviation; the service starts the sequence again before it runs out;
// the end gives back every register and table entry it borrowed. Against a fake register file
#include <stdio.h>
#include <string.h>
#include "modem/hrc6000/hrc6000.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

static uint8_t regs[8][256];
static uint8_t ext[8][0x800];
static int writes_60;

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	(void)ctx;
	uint8_t page = hdr[0] & 7u;
	bool read = (hdr[0] & 0x80u) != 0;
	if (hdr_len == 3) {
		uint16_t a = (uint16_t)(hdr[1] | ((uint16_t)hdr[2] << 8));
		for (uint16_t i = 0; i < len; i++) {
			if (read) {
				rx[i] = ext[page][(a + i) & 0x7FFu];
			} else {
				ext[page][(a + i) & 0x7FFu] = tx[i];
			}
		}
		return true;
	}
	for (uint16_t i = 0; i < len; i++) {
		uint8_t r = (uint8_t)(hdr[1] + i);
		if (read) {
			if (rx != NULL) {
				rx[i] = regs[page][r];
			}
		} else {
			regs[page][r] = tx != NULL ? tx[i] : 0xFFu;
			writes_60 += page == 4 && r == 0x60;
		}
	}
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, NULL };
static hrc_t dev;

static uint16_t entry(uint16_t addr)
{
	return (uint16_t)(ext[1][addr] | (ext[1][addr + 1] << 8));
}

int main(void)
{
	hrc6000_attach(&dev, &bus);
	CHECK(hrc6000_tone_step(697) == 0x0593);
	CHECK(hrc6000_tone_step(1209) == 0x09AC);
	CHECK(hrc6000_tone_step(1633) == 0x0D10);
	CHECK(hrc6000_tone_step(1750) == 0x0E00);

	// FM TX state as hrc6000_set_mode_fm_tx leaves it, the default table from the power-on init
	regs[4][0xA1] = 0x00;
	regs[4][0xA0] = 0x06;	// CTCSS deviation of a narrow UHF channel
	regs[4][0xD1] = 0xA4;	// high bits that matter
	regs[4][0xE0] = 0xC9;	// MCU control, LINEIN1 (the mic), LINEOUT2, I2S
	regs[4][0x60] = 0x80;
	regs[4][0xA3] = 0x19;
	regs[4][0xA4] = 0xFA;
	for (int i = 0; i < 8; i++) {
		regs[4][0xA8 + i] = (uint8_t)(0x30 + i);	// the CTCSS index and DCS code share this block
	}
	ext[1][0x11A] = 0x93; ext[1][0x11B] = 0x05;
	ext[1][0x122] = 0xAC; ext[1][0x123] = 0x09;
	ext[1][0x124] = 0xB0; ext[1][0x125] = 0x0A;
	regs[4][0xA5] = 0x05;
	regs[4][0xA6] = 0x0A;

	hrc6000_tx_sig_t s = { 0 };
	CHECK(hrc6000_tx_sig_key(&dev, &s, true) == false);	// no tone yet
	CHECK(hrc6000_tx_pair(&dev, &s, 0, 0, 0x12));		// off while off: nothing to do
	CHECK(!s.on && writes_60 == 0);

	// 1750 Hz burst
	CHECK(hrc6000_tx_pair(&dev, &s, 1750, 1750, 0x12));
	CHECK(s.on);
	for (uint16_t a = 0x11A; a <= 0x122; a += 2) {
		CHECK(entry(a) == 0x0E00);
	}
	CHECK(entry(0x124) == 0x0AB0);	// 1336 Hz: past the 2-tone entries
	CHECK(regs[4][0xA1] == 0x10);	// 2-tone
	CHECK(regs[4][0xA0] == 0x12);
	CHECK(regs[4][0xA4] == 0xFF && regs[4][0xA5] == 0xFF && regs[4][0xA6] == 0xFF && regs[4][0xA3] == 0x00);
	CHECK(regs[4][0xAF] == 0x0A && regs[4][0xAE] == 0xB0);	// "A-B"
	CHECK(hrc6000_tx_sig_retune(&dev, &s, 2200, 2200));	// AFSK space
	CHECK(entry(0x11A) == hrc6000_tone_step(2200) && entry(0x122) == hrc6000_tone_step(2200));
	CHECK((regs[4][0xE0] & 0x66u) == 0 && (regs[4][0xE0] & 0x89u) == 0x89u);	// mic off, the rest kept
	CHECK(regs[4][0x60] == 0x80 && writes_60 == 2);

	// the service: the first call notes the time, a restart only past HRC6000_TX_SIG_RENEW_MS
	CHECK(hrc6000_tx_sig_service(&dev, &s, 1000));
	CHECK(writes_60 == 2);
	CHECK(hrc6000_tx_sig_service(&dev, &s, 1000 + HRC6000_TX_TONE_RENEW_MS - 1));
	CHECK(writes_60 == 2);
	CHECK(hrc6000_tx_sig_service(&dev, &s, 1000 + HRC6000_TX_TONE_RENEW_MS));
	CHECK(writes_60 == 4 && regs[4][0x60] == 0x80);
	CHECK(HRC6000_TX_SIG_RENEW_MS + 600u < HRC6000_TX_SIG_MS);	// a poll every 600 ms is in time
	CHECK(HRC6000_TX_TONE_RENEW_MS + 600u < HRC6000_TX_TONE_MS);

	// keyed off and on again: deviation only
	CHECK(hrc6000_tx_sig_key(&dev, &s, false));
	CHECK(regs[4][0xA0] == 0x00);
	CHECK(hrc6000_tx_sig_key(&dev, &s, true));
	CHECK(regs[4][0xA0] == 0x12);

	// a DTMF pair replaces the tone; the saved values stay the first ones
	CHECK(hrc6000_tx_pair(&dev, &s, 941, 1633, 0x12));
	CHECK(entry(0x11A) == 0x0787 && entry(0x122) == 0x0D10);
	CHECK(entry(0x11C) == 0x0629 && entry(0x11E) == 0x06D1);	// the single tone's other entries back
	CHECK(regs[4][0xA1] == 0x02 && regs[4][0xD1] == 0xB0 && regs[4][0xA4] == 0xFF);	// DTMF, 16 codes of 510 ms
	CHECK(regs[4][0xA8] == 0x11 && regs[4][0xAF] == 0x11);	// all digit "1"
	CHECK(s.a0 == 0x06 && s.e0 == 0xC9 && s.d1 == 0xA4);
	int w = writes_60;
	CHECK(hrc6000_tx_sig_service(&dev, &s, 5000));
	CHECK(hrc6000_tx_sig_service(&dev, &s, 5000 + HRC6000_TX_SIG_RENEW_MS));	// a pair renews sooner
	CHECK(writes_60 == w + 2);

	// the end gives everything back
	CHECK(hrc6000_tx_pair(&dev, &s, 0, 0, 0));
	CHECK(!s.on);
	CHECK(regs[4][0xA1] == 0x00 && regs[4][0xA0] == 0x06 && regs[4][0xD1] == 0xA4 && regs[4][0xE0] == 0xC9);
	CHECK(regs[4][0xA3] == 0x19 && regs[4][0xA4] == 0xFA);
	CHECK(regs[4][0xA8] == 0x30 && regs[4][0xAF] == 0x37);
	CHECK(hrc6000_tx_sig_service(&dev, &s, 99999));	// no tone: nothing
	CHECK(writes_60 == w + 2);
	CHECK(regs[4][0xA5] == 0x05 && regs[4][0xA6] == 0x0A);
	for (uint16_t a = 0x11A; a <= 0x122; a += 2) {
		CHECK(entry(a) == hrc6000_tone_step((uint16_t[]){ 697, 770, 852, 941, 1209 }[(a - 0x11A) / 2]));
	}
	CHECK(entry(0x11A) == 0x0593 && entry(0x122) == 0x09AC);
	CHECK(hrc6000_tx_sig_key(&dev, &s, true) == false);

	printf("hrc6000ToneTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
