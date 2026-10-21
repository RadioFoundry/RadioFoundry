// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 frequency plan: band, LO and the synthesizer and IF words per board revision (rfplan_md9600.h;
// docs/spec/hardware-md9600.md 5.2-5.4). Each revision's synthesizer code is built only with its switch.
#include <stddef.h>
#include "rfplan_md9600.h"
#include "config.h"
#if CONFIG_MD9600_V1V3
#include "rf/sky72310/sky72310.h"
#endif
#if CONFIG_MD9600_V4
#include "rf/pll1590/pll1590.h"
#endif
#if CONFIG_MD9600_V5
#include "rf/lmx2571/lmx2571.h"
#endif
#if CONFIG_MD9600_V1V3 || CONFIG_MD9600_V4
#include "rf/ak2365a/ak2365a.h"
#endif

// Bands (hardware-md9600.md 5.1, unverified)
#define VHF_LO_HZ	136000000u
#define VHF_HI_HZ	174000000u
#define UHF_LO_HZ	400000000u
#define UHF_HI_HZ	480000000u

bool md9600_band_of(uint32_t hz, uint8_t *band)
{
	if (hz >= VHF_LO_HZ && hz <= VHF_HI_HZ) {
		*band = MD9600_VHF;
		return true;
	}
	if (hz >= UHF_LO_HZ && hz <= UHF_HI_HZ) {
		*band = MD9600_UHF;
		return true;
	}
	return false;
}

#if CONFIG_MD9600_V1V3
// SKY72310: R 6 on VHF (2.8 MHz), 4 on UHF (4.2 MHz); phase detector gain 31; register 7 = 0x3D0
#define SKY_R_VHF	6u
#define SKY_R_UHF	4u
#define SKY_PD_GAIN	31u
#define SKY_POWER_MUX	0x3D0u

static bool plan_sky72310(md9600_pll_plan_t *p)
{
	uint16_t w[SKY72310_PROGRAM_WORDS];
	uint8_t r = p->band == MD9600_VHF ? SKY_R_VHF : SKY_R_UHF;
	uint8_t n = sky72310_program_words(MD9600_F_REF_HZ, r, p->vco_hz, SKY_PD_GAIN, SKY_POWER_MUX, w);
	for (uint8_t i = 0; i < n; i++) {
		p->words[i] = w[i];
	}
	p->count = n;
	return n != 0;
}
#endif

#if CONFIG_MD9600_V4
// "PLL1590": R 12 on VHF (1.4 MHz), 8 on UHF receive (2.1 MHz), 4 on UHF transmit (4.2 MHz); the band prefix in
// register 2 and the register 4 value per band and direction
static bool plan_pll1590(md9600_pll_plan_t *p, bool tx)
{
	pll1590_cfg_t c = { .f_osc_hz = MD9600_F_REF_HZ };
	if (p->band == MD9600_VHF) {
		c.ref_div = 12;
		c.n_prefix = 0x20000u;
		c.ctrl = 0x1FF40u;
	} else {
		c.ref_div = tx ? 4 : 8;
		c.n_prefix = 0x38000u;
		c.ctrl = tx ? 0x19F40u : 0x1FF40u;
	}
	p->count = pll1590_words(&c, p->vco_hz, p->words);
	return p->count != 0;
}
#endif

#if CONFIG_MD9600_V5
// LMX2571 in PLL mode: pre-divider 8 on VHF (2.1 MHz), 4 on UHF (4.2 MHz); third-order modulator
static bool plan_lmx2571(md9600_pll_plan_t *p)
{
	const lmx2571_cfg_t c = {
		.f_osc_hz = MD9600_F_REF_HZ,
		.r_pre = p->band == MD9600_VHF ? 8 : 4,
		.frac_order = 3,
		.r0 = 0x003Eu,
		.r6 = 0x8581u,
		.r7 = 0x1024u,
		.r8 = 0x0430u,
		.r41 = 0x0230u,
		.r42 = 0x0108u,
	};
	p->count = lmx2571_words(&c, p->vco_hz, p->words);
	return p->count != 0;
}
#endif

bool md9600_pll_plan(md9600_rev_t rev, uint32_t hz, bool tx, md9600_pll_plan_t *p)
{
	const md9600_rev_desc_t *d = md9600_rev_desc(rev);
	uint8_t band;
	if (d == NULL || !md9600_rev_built(rev) || !md9600_band_of(hz, &band)) {
		return false;
	}
	p->band = band;
	// Receive: high-side LO on VHF, low-side on UHF. Transmit: the VCO on the carrier
	p->lo_hz = tx ? hz : (band == MD9600_VHF ? hz + MD9600_IF_HZ : hz - MD9600_IF_HZ);
	p->vco_hz = (band == MD9600_VHF && d->vhf_vco_x2) ? 2u * p->lo_hz : p->lo_hz;
	p->bits = d->pll_bits;
	p->count = 0;
	switch (d->pll) {
#if CONFIG_MD9600_V1V3
	case MD9600_PLL_SKY72310:
		return plan_sky72310(p);
#endif
#if CONFIG_MD9600_V4
	case MD9600_PLL_PLL1590:
		return plan_pll1590(p, tx);
#endif
#if CONFIG_MD9600_V5
	case MD9600_PLL_LMX2571:
		return plan_lmx2571(p);
#endif
	default:
		return false;
	}
}

uint8_t md9600_if_words(bool wide, uint16_t out[MD9600_IF_WORDS])
{
#if CONFIG_MD9600_V1V3 || CONFIG_MD9600_V4
	// Everything on, 450 kHz out of the 50.4 MHz second LO; the 25 kHz setting with the +-6 kHz filter and the wide
	// demodulator level, 12.5 kHz with +-3 kHz and the narrow level. AGC time 3, discriminator calibration started,
	// IF buffer 6 dB, AGC1 gain 1, AGC2 gain at its reset value
	static const ak2365a_cfg_t cfg = {
		.ctrl1_wide = AK2365A_CTRL1(7, true, 0, 1),	// 0xF1
		.ctrl1_narrow = AK2365A_CTRL1(7, false, 2, 1),	// 0xE9
		.ctrl2 = 0x1D,
		.ctrl3 = 0x00,
		.ctrl4 = 0x01,
		.ctrl5 = 0x80,
	};
	return ak2365a_setup_words(&cfg, wide, out);
#else
	(void)wide;
	(void)out;
	return 0;
#endif
}

uint8_t md9600_if_lines_v5(uint8_t band, bool wide)
{
	uint8_t bits = wide ? 0x03u : 0x00u;
	bits |= band == MD9600_VHF ? 0x04u : 0x08u;
	return bits;
}
