// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// FD6818 set-up of the Baofeng DM-1801 Pro (docs/spec/hardware-dm1801pro.md 5): the same chip family as the DM-32UV
// with other register tables, an extra 0x0200 step in the 0x30 sequences and no band-word table. The values are
// unverified on a bench; the 12.5 kHz word in particular is the one the spec lists second (hardware-dm1801pro.md 5.2)
#include <stddef.h>
#include "fd6818.h"

#define TBL_N(t)	(uint16_t)(sizeof(t) / sizeof((t)[0]))

// 5.3 power-on order; the 0x09 table that follows is the DM-32UV's
static const fd6818_regval_t init_table[] = {
	{ 0x00, 0x8000 }, { 0x00, 0x0000 }, { 0x37, 0x9D1F }, { 0x13, 0x03DF }, { 0x12, 0x03DB }, { 0x11, 0x033A },
	{ 0x10, 0x0318 }, { 0x14, 0x0210 }, { 0x19, 0x1041 }, { 0x1C, 0x0422 }, { 0x1D, 0x2AAB }, { 0x1E, 0x4C58 },
	{ 0x1F, 0x165A }, { 0x25, 0x6DBA }, { 0x28, 0x0A40 }, { 0x29, 0xAA00 }, { 0x2A, 0x6600 }, { 0x2C, 0x0022 },
	{ 0x2F, 0x9890 }, { 0x3A, 0x9A7C }, { 0x3E, 0x94C6 }, { 0x3F, 0x07FE }, { 0x40, 0x34F0 }, { 0x46, 0x6050 },
	{ 0x48, 0xB386 }, { 0x49, 0x2A32 }, { 0x4A, 0x5430 }, { 0x4D, 0xA015 }, { 0x4F, 0x2A28 }, { 0x53, 0x2028 },
	{ 0x73, 0x6681 }, { 0x77, 0x88EF }, { 0x7B, 0x73DC }, { 0x7E, 0x303E }, { 0x4C, 0xE520 }, { 0x4E, 0x1F10 },
	{ 0x36, 0x003F },
	{ 0x09, 0x006F }, { 0x09, 0x106B }, { 0x09, 0x2067 }, { 0x09, 0x3062 }, { 0x09, 0x4050 }, { 0x09, 0x5047 },
	{ 0x09, 0x603A }, { 0x09, 0x702C }, { 0x09, 0x8041 }, { 0x09, 0x9037 }, { 0x09, 0xA025 }, { 0x09, 0xB017 },
	{ 0x09, 0xC0E4 }, { 0x09, 0xD0CB }, { 0x09, 0xE0B5 }, { 0x09, 0xF09F },
	{ 0x21, 0x06D8 }, { 0x24, 0x8C7E },
};

// Analog FM, then the 0x2B mode word. 0x48 is its power-on value again, since the DMR set changes it. 0x47 = 0x0140
// puts the demodulated audio on the chip's output (checked on the radio: the 0x6042 the radio's FM sequence writes left
// the speaker silent on a strong station)
static const fd6818_regval_t fm_table[] = {
	{ 0x30, 0x0000 }, { 0x32, 0x0244 }, { 0x47, 0x0140 }, { 0x48, 0xB386 }, { 0x7E, 0x302E }, { 0x2B, 0xC000 },
};

// 5.3: DMR is these five registers (0x28 is its power-on value, written again), then 0x3F = 0: the radio writes it
// in every DMR receive set-up and at every burst end (5.4), so in DMR it never has the power-on 07FE
static const fd6818_regval_t dmr_table[] = {
	{ 0x47, 0x0140 }, { 0x48, 0xB2F5 }, { 0x7E, 0xB03F }, { 0x2B, 0x0700 }, { 0x28, 0x0A40 }, { 0x3F, 0x0000 },
};

// 5.4: 0x37 first, the DM-32UV's 0x30 steps with a 0x0200 between idle and the final word
static const fd6818_regval_t rx_seq[] = { { 0x37, 0x9D1F }, { 0x30, 0x0200 }, { 0x30, 0xBFF1 } };
static const fd6818_regval_t tx_seq[] = { { 0x37, 0x9D1F }, { 0x52, 0x028F }, { 0x30, 0x0200 }, { 0x30, 0xC1FE } };

// 5.2: no 0x1A word; 0x40 is 0x3480 plus a calibration byte (the power-on value, 0x70, until the calibration is read)
static bool band_words(uint32_t freq_10hz, uint16_t *r1a, uint16_t *r40)
{
	(void)freq_10hz;
	*r1a = 0;
	*r40 = 0x34F0;
	return true;
}

const fd6818_variant_t fd6818_variant_dm1801pro = {
	.init = init_table, .init_n = TBL_N(init_table),
	.fm = fm_table, .fm_n = TBL_N(fm_table),
	.dmr = dmr_table, .dmr_n = TBL_N(dmr_table),
	.rx = rx_seq, .rx_n = TBL_N(rx_seq),
	.tx = tx_seq, .tx_n = TBL_N(tx_seq),
	// The radio's own firmware writes only these two words: 12.5 kHz FM shares the DMR word (hardware-dm1801pro.md 5.2)
	.bw_fm_25k = 0x3028u, .bw_fm_12k5 = 0x3048u, .bw_dmr = 0x3048u,
	.band_words = band_words,
};
