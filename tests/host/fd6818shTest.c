// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FD6818 superhet driver (drivers/rf/fd6818/fd6818sh): the band, deviation, PA, microphone, volume, CTCSS and tone
// words, the 0x7813 VCO sections, and the write order of init, receive at the IF, FM TX with sub-audio and its tail,
// the DMR burst tune and the LO tune / off / resume. The chip is a register file with a page select
#include <stdio.h>
#include <string.h>
#include "rf/fd6818/fd6818sh.h"
#include "rf/subaudio/subaudio.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

// --- fake chip: two pages of 128 registers, a write log (page-1 writes logged as reg | 0x100) ---

static uint16_t regs[2][128];
static unsigned page;
static struct {
	uint16_t reg;
	uint16_t val;
} log_[512];
static unsigned nlog;
static uint32_t waited_ms;

static void w(rfbus_t *b, uint8_t reg, uint16_t val)
{
	(void)b;
	if (nlog < sizeof log_ / sizeof log_[0]) {
		log_[nlog].reg = (uint16_t)(reg | (page && reg != 0x7F ? 0x100u : 0u));
		log_[nlog].val = val;
	}
	nlog++;
	if (reg == 0x7F) {
		page = val & 1u;
	} else {
		regs[page][reg & 0x7F] = val;
	}
}

static uint16_t r(rfbus_t *b, uint8_t reg) { (void)b; return regs[page][reg & 0x7F]; }
static void d(uint32_t ms) { waited_ms += ms; }

static rfbus_t bus = { .write = w, .read = r, .delay_ms = d };

static void reset(void)
{
	memset(regs, 0, sizeof regs);
	page = 0;
	nlog = 0;
	waited_ms = 0;
}

// The log from index from holds exactly these (reg, val) pairs in order
static bool logged(unsigned from, const uint16_t *pairs, unsigned n)
{
	if (nlog - from != n) {
		printf("  log has %u writes, expected %u\n", nlog - from, n);
		return false;
	}
	for (unsigned i = 0; i < n; i++) {
		if (log_[from + i].reg != pairs[2 * i] || log_[from + i].val != pairs[2 * i + 1]) {
			printf("  write %u: %02x=%04x, expected %02x=%04x\n", i, log_[from + i].reg, log_[from + i].val,
			       pairs[2 * i], pairs[2 * i + 1]);
			return false;
		}
	}
	return true;
}

#define LOGGED(from, ...)                                                                                          \
	logged(from, (const uint16_t[]){ __VA_ARGS__ }, sizeof((const uint16_t[]){ __VA_ARGS__ }) / 4u)

// Last value written to reg (page 0), or -1
static int last(uint8_t reg)
{
	for (unsigned i = nlog; i-- > 0;) {
		if (log_[i].reg == reg) {
			return log_[i].val;
		}
	}
	return -1;
}

// --- tests ---

static void test_words(void)
{
	CHECK(fd6818sh_band_word(51550000) == 0x5F42);
	CHECK(fd6818sh_band_word(146520000) == 0xAF28);
	CHECK(fd6818sh_band_word(446000000) == 0x2F50);
	CHECK(fd6818sh_band_word(185999999) == 0xAF28);		// 185 MHz is the edge: still that band
	CHECK(fd6818sh_band_word(186000000) == 0x3F48);
	CHECK(fd6818sh_band_word(497550000) == 0x2F50);		// a UHF LO
	CHECK(fd6818sh_band_word(1200000000) == 0x0000);

	CHECK(fd6818sh_tx_dev(146520000, false) == 0x375C);
	CHECK(fd6818sh_tx_dev(146520000, true) == 0x3732);
	CHECK(fd6818sh_tx_dev(446000000, false) == 0x39D0);
	CHECK(fd6818sh_tx_dev(446000000, true) == 0x3998);
	CHECK(fd6818sh_tx_dev(600000000, false) == 0x0000);
	CHECK(fd6818sh_tx_dev(20000000, true) == 0x3432);

	CHECK(fd6818sh_pa_word(0) == 0x00);
	CHECK(fd6818sh_pa_word(7) == 0x07);
	CHECK(fd6818sh_pa_word(8) == 0x38);
	CHECK(fd6818sh_pa_word(15) == 0x3F);
	CHECK(fd6818sh_pa_word(40) == 0x3F);
	CHECK(fd6818sh_mic_word(48) == 0x3006);
	CHECK(fd6818sh_mic_word(27) == 0x1B06);
	CHECK(fd6818sh_mic_word(99) == 0x3F06);

	CHECK(fd6818sh_volume_word(0) == 0x941D);
	CHECK(fd6818sh_volume_word(15) == (0x19 << 5 | 0xF41D));
	CHECK(fd6818sh_volume_word(200) == fd6818sh_volume_word(15));

	// The working firmware's CTCSS words
	CHECK(subaudio_ctcss_word(670, false) == 0x0567);
	CHECK(subaudio_ctcss_word(885, false) == 0x0723);
	CHECK(subaudio_ctcss_word(1000, false) == 0x0810);
	CHECK(subaudio_ctcss_word(1514, false) == 0x0C36);
	CHECK(subaudio_ctcss_word(2541, false) == 0x147E);
	CHECK(subaudio_ctcss_word(550, false) == 0x046F);		// its 55 Hz tail word is a constant, 0x0470
	// Its tone word is truncated, its DTMF table rounded
	CHECK(subaudio_tone_word(1750, false) == 18067);
	CHECK(subaudio_tone_word(487, false) == 5028 && subaudio_tone_word(2725, false) == 28134);	// as its Hz x 2^21 / 203125
	CHECK(subaudio_tone_word(697, true) == 0x1C1C);
	CHECK(subaudio_tone_word(941, true) == 0x25F3);
	CHECK(subaudio_tone_word(1209, true) == 0x30C2);
	CHECK(subaudio_tone_word(1633, true) == 0x41DC);

	CHECK(fd6818sh_7813_section(32000000) == 0);
	CHECK(fd6818sh_7813_section(33000000) == 1);
	CHECK(fd6818sh_7813_section(200000000) == 6);		// a VHF LO
	CHECK(fd6818sh_7813_section(353000000) == 7);
	CHECK(fd6818sh_7813_section(497550000) == 8);		// a UHF LO
}

static void test_init_main(void)
{
	fd6818sh_t c;
	reset();
	regs[0][0x7E] = 0x3425;
	regs[0][0x26] = 0x1235;
	regs[0][0x27] = 0x0F0F;
	regs[0][0x25] = 0x1818;
	fd6818sh_attach(&c, &bus, FD6818SH_MAIN);
	fd6818sh_init_main(&c);
	CHECK(log_[0].reg == 0x00 && log_[0].val == 0x8000);
	CHECK(log_[1].reg == 0x00 && log_[1].val == 0x0000);
	CHECK(page == 0);
	CHECK(regs[1][0x36] == 0x9D06);				// the last page-1 entry went to page 1
	CHECK(regs[0][0x35] == 0xF008 && regs[0][0x40] == 0x39D0);
	CHECK(regs[0][0x3B] == 0x5880 && regs[0][0x3C] == 0x4F00 && regs[0][0x3D] == 0x2762);
	CHECK(regs[0][0x7D] == 0x42E2);
	CHECK(waited_ms >= 50);
	CHECK(regs[0][0x03] == 0x0000 && c.state == FD6818SH_STATE_IDLE);
	CHECK(c.r7e == 0xB425 && regs[0][0x7E] == 0xB425);
	CHECK((regs[0][0x26] & 0xF00F) == 0x0004);		// GPIO0 = compare result, GPIO3 = plain output
	CHECK(regs[0][0x26] == 0x0234);
	CHECK(regs[0][0x27] == 0x020F - 0x000F);			// GPIO6 = squelch, GPIO4 = plain output
	CHECK(regs[0][0x25] == 0x0808);				// GPIO4 an output, low
}

static void test_rx_and_modes(void)
{
	fd6818sh_t c;
	reset();
	fd6818sh_attach(&c, &bus, FD6818SH_MAIN);
	fd6818sh_set_af_gain(&c, 8);
	fd6818sh_analog_mode(&c);
	CHECK(last(0x73) == 0x4E42);
	CHECK(regs[1][0x17] == 0x3006 && regs[0][0x18] == 0x2125);
	CHECK(regs[0][0x43] == 0x6648 && regs[0][0x4D] == 0x348D);
	CHECK(regs[0][0x53] == fd6818sh_volume_word(8));
	CHECK(regs[0][0x47] == FD6818SH_AF_MUTE);

	// To the IF and receive: the working firmware's RX handler
	unsigned from = nlog;
	fd6818sh_set_state(&c, FD6818SH_STATE_IDLE);
	fd6818sh_tune(&c, 51550000);
	fd6818sh_set_state(&c, FD6818SH_STATE_RX);
	CHECK(LOGGED(from, 0x03, 0x0000, 0x1A, 0x5F42, 0x38, 0xA8B8, 0x39, 0x004E, 0x03, 0x0000, 0x03, 0xBFF1));

	fd6818sh_set_bandwidth(&c, true);
	CHECK(regs[0][0x43] == 0x3028);

	// The volume is written only when it changes, and not in the digital mode
	from = nlog;
	fd6818sh_set_af_gain(&c, 8);
	CHECK(nlog == from);
	fd6818sh_dmr_mode(&c);
	CHECK(regs[0][0x53] == 0x957D && regs[0][0x43] == 0x7E48 && regs[0][0x40] == 0x0000);
	CHECK(regs[0][0x7E] == (uint16_t)(c.r7e | 0x0080u));
	CHECK(regs[1][0x17] == 0x1B06 && regs[0][0x51] == 0x1050 && regs[0][0x73] == 0x4E52);
	CHECK(regs[0][0x47] == FD6818SH_AF_DMR);
	fd6818sh_set_af(&c, FD6818SH_AF_RX);
	CHECK(regs[0][0x47] == FD6818SH_AF_DMR);
	fd6818sh_set_af_gain(&c, 3);
	CHECK(regs[0][0x53] == 0x957D);
	fd6818sh_set_bandwidth(&c, false);
	CHECK(regs[0][0x43] == 0x7E48);
	fd6818sh_analog_mode(&c);
	CHECK(regs[0][0x53] == fd6818sh_volume_word(3));
	CHECK(regs[0][0x43] == 0x6648);				// the bandwidth set during DMR

	// RX sub-audio; a high-side LO inverts DCS
	c.rx_inverted = true;
	from = nlog;
	fd6818sh_set_rx_tone(&c, FD6818SH_TONE_CTCSS, 885);
	CHECK(LOGGED(from, 0x51, 0x0000, 0x07, 0x0723, 0x07, 0x2470, 0x51, 0x90C5, 0x52, 0x028F));
	from = nlog;
	fd6818sh_set_rx_tone(&c, FD6818SH_TONE_DCS, 023);
	CHECK(LOGGED(from, 0x51, 0x0000, 0x07, 0x0AD7, 0x07, 0x2AD7, 0x08, 0x0813, 0x08, 0x8763, 0x52, 0x028F,
		     0x51, 0xA0B8));
	fd6818sh_set_rx_tone(&c, FD6818SH_TONE_DCS_INV, 023);
	CHECK(regs[0][0x51] == 0x80B8);
	fd6818sh_set_rx_tone(&c, FD6818SH_TONE_NONE, 0);
	CHECK(regs[0][0x51] == 0x0000);
}

static void test_fm_tx(void)
{
	fd6818sh_t c;
	reset();
	fd6818sh_attach(&c, &bus, FD6818SH_MAIN);
	c.rx_inverted = true;

	unsigned from = nlog;
	fd6818sh_tx_prepare(&c, FD6818SH_TONE_DCS, 023, 446000000, 12);
	// TX DCS is not inverted: the main chip transmits on the channel itself
	CHECK(LOGGED(from, 0x40, 0x3998, 0x07, 0x0AD7, 0x07, 0x2AD7, 0x08, 0x0813, 0x08, 0x8763, 0x52, 0x028F,
		     0x51, 0x80B8, 0x28, 0x003C));
	from = nlog;
	fd6818sh_tx_tune(&c, 446000000);
	CHECK(LOGGED(from, 0x1A, 0x2F50, 0x38, 0x8AC0, 0x39, 0x02A8, 0x03, 0x0000, 0x03, 0xC3FE));

	// A tone: straight to 0xC3FA, back to 0xC3FE at its end
	fd6818sh_tone_start(&c, 1750, 30);
	CHECK(regs[0][0x71] == 18067 && regs[0][0x70] == 0x9E00 && regs[0][0x03] == 0xC3FA);
	CHECK(regs[0][0x47] == FD6818SH_AF_SIDETONE);
	fd6818sh_tone_key(&c, false);
	CHECK(regs[0][0x70] == 0x0000);
	fd6818sh_tone_key(&c, true);
	CHECK(regs[0][0x70] == 0x9E00);
	fd6818sh_tone_start(&c, 0, 0);
	CHECK(regs[0][0x70] == 0x0000 && regs[0][0x03] == 0xC3FE && c.tone_hz == 0);

	fd6818sh_dtmf_start(&c, 697, 1209);
	CHECK(regs[0][0x70] == 0x8989 && regs[0][0x71] == 0x1C1C && regs[0][0x72] == 0x30C2 && regs[0][0x03] == 0xC3FA);
	fd6818sh_dtmf_start(&c, 0, 0);
	CHECK(regs[0][0x03] == 0xC3FE);

	// The tail: CTCSS on the second slot (134.4 Hz after DCS), the tail flag; then the end back to RX
	CHECK(fd6818sh_tx_tail(&c, true));
	CHECK(regs[0][0x51] == 0x90C5 && regs[0][0x52] == 0x828F);
	from = nlog;
	fd6818sh_tx_end(&c);
	CHECK(LOGGED(from, 0x52, 0x028F, 0x70, 0x0000, 0x47, 0x7040, 0x51, 0x0000, 0x03, 0x0000, 0x03, 0xBFF1));
	CHECK(!c.tail_on && !c.tx_sub);

	// No sub-audio: plain deviation, 0x51 off, no tail
	fd6818sh_tx_prepare(&c, FD6818SH_TONE_NONE, 0, 146520000, 3);
	CHECK(regs[0][0x40] == 0x375C && regs[0][0x51] == 0x1050 && regs[0][0x28] == 0x0003);
	from = nlog;
	CHECK(!fd6818sh_tx_tail(&c, true));
	CHECK(nlog == from);

	fd6818sh_tx_prepare(&c, FD6818SH_TONE_CTCSS, 1000, 146520000, 3);
	CHECK(regs[0][0x40] == 0x3732 && regs[0][0x07] == 0x2470 && regs[0][0x51] == 0x90C5);
}

static void test_dmr_burst(void)
{
	fd6818sh_t c;
	reset();
	fd6818sh_attach(&c, &bus, FD6818SH_MAIN);
	fd6818sh_dmr_mode(&c);
	fd6818sh_dmr_tx_prepare(&c, 9);
	unsigned from = nlog;
	fd6818sh_burst_tune(&c, 446000000, true);
	CHECK(LOGGED(from, 0x1A, 0x2F50, 0x28, 0x0039, 0x38, 0x8AC0, 0x39, 0x02A8, 0x03, 0x0000, 0x03, 0xC3FE));
	from = nlog;
	fd6818sh_burst_tune(&c, 51550000, false);
	CHECK(LOGGED(from, 0x1A, 0x5F42, 0x38, 0xA8B8, 0x39, 0x004E, 0x03, 0x0000, 0x03, 0xBFF1));
}

static void test_lo(void)
{
	fd6818sh_t c;
	reset();
	regs[0][0x25] = 0xFFFF;
	fd6818sh_attach(&c, &bus, FD6818SH_LO);
	fd6818sh_init_lo(&c);
	CHECK(regs[0][0x35] == 0xF608 && regs[0][0x53] == 0x77FD && regs[0][0x7D] == 0x8000);
	CHECK(regs[0][0x40] == 0x0000 && regs[0][0x51] == 0x1050 && regs[0][0x73] == 0x4E52 && regs[0][0x28] == 0x003F);
	CHECK(regs[0][0x43] == 0x3028);
	CHECK(regs[0][0x25] == 0xE7E7);				// GPIO3 and GPIO4 outputs, low
	CHECK(regs[0][0x03] == 0x0000);

	unsigned from = nlog;
	fd6818sh_lo_tune(&c, 497550000);
	CHECK(LOGGED(from, 0x1A, 0x2F50, 0x38, 0x3378, 0x39, 0x02F7, 0x03, 0x0000, 0x03, 0xC1FA));
	// Same frequency: only the enable
	from = nlog;
	fd6818sh_lo_tune(&c, 497550000);
	CHECK(LOGGED(from, 0x03, 0x0000, 0x03, 0xC1FA));
	from = nlog;
	fd6818sh_lo_off(&c);
	CHECK(LOGGED(from, 0x03, 0x0000));
	from = nlog;
	fd6818sh_lo_resume(&c);
	CHECK(LOGGED(from, 0x03, 0x0000, 0x03, 0xC1FA));
	fd6818sh_gpio(&c, 3, true);
	CHECK(regs[0][0x25] == 0xE7EF);
	from = nlog;
	fd6818sh_gpio(&c, 3, true);				// unchanged: not written
	CHECK(nlog == from);
}

static void test_lo_7813(void)
{
	fd6818sh_t c;
	reset();
	fd6818sh_attach(&c, &bus, FD6818SH_LO_7813);
	fd6818sh_init_lo(&c);
	CHECK(log_[0].reg == 0x7F && log_[1].reg == 0x00 && log_[1].val == 0x8000);
	CHECK(regs[0][0x34] == 0x086F && regs[0][0x7E] == 0x349E && regs[0][0x53] == 0x93DD && regs[0][0x18] == 0x0125);
	CHECK(regs[1][0x27] == 0x891B && regs[0][0x28] == 0x003F && regs[0][0x51] == 0x1050);

	unsigned from = nlog;
	fd6818sh_lo_tune(&c, 497550000);
	CHECK(LOGGED(from, 0x3A, 0x00E0, 0x40, 0x38D0, 0x43, 0x5009, 0x4D, 0x347D, 0x7F, 0x0001, 0x116, 0x02FE,
		     0x7F, 0x0000, 0x3C, 0x1000, 0x3D, 0x9D8A, 0x40, 0x38D0, 0x1A, 0x1F50, 0x2A, 0x22F2, 0x38, 0x3378,
		     0x39, 0x02F7, 0x40, 0x0000, 0x03, 0x0000, 0x03, 0xC1FA));
	fd6818sh_lo_tune(&c, 198075000);				// a VHF LO: section 6
	CHECK(regs[0][0x3D] == 0x3B14 && regs[0][0x1A] == 0x1F48 && regs[0][0x2A] == 0x32F2);
}

int main(void)
{
	test_words();
	test_init_main();
	test_rx_and_modes();
	test_fm_tx();
	test_dmr_burst();
	test_lo();
	test_lo_7813();
	printf("# fd6818shTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
