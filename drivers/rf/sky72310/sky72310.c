// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// SKY72310 synthesizer (sky72310.h; docs/spec/hardware-md380.md 5.2)
#include <stddef.h>
#include "sky72310.h"

bool sky72310_ratio(uint32_t f_osc_hz, uint8_t ref_div, uint32_t f_vco_hz, sky72310_ratio_t *out)
{
	if (f_osc_hz == 0 || ref_div < 1 || ref_div > 32 || f_osc_hz / ref_div > SKY72310_FPD_MAX_HZ) {
		return false;
	}
	// N x 2^18 = f_vco x ref_div x 2^18 / f_osc, rounded to the nearest step, then split into the nearest whole N and
	// a signed fraction of -0.5 .. +0.5 (signed mode). Sent unsigned, a fraction over one half put the
	// synthesizer one phase detector step (8.4 MHz) low, at about half of all channels (issue 108)
	uint64_t num = ((uint64_t)f_vco_hz * ref_div) << SKY72310_FRAC_BITS;
	uint64_t total = (num + f_osc_hz / 2u) / f_osc_hz;
	uint64_t half = 1u << (SKY72310_FRAC_BITS - 1);
	uint32_t n = (uint32_t)((total + half) >> SKY72310_FRAC_BITS);
	if (n < SKY72310_N_MIN || n > SKY72310_N_MAX) {
		return false;
	}
	int32_t frac = (int32_t)((int64_t)total - ((int64_t)n << SKY72310_FRAC_BITS));
	out->n_reg = (uint16_t)(n - 32u);
	out->dividend = (uint32_t)frac & ((1u << SKY72310_FRAC_BITS) - 1u);	// 18-bit two's complement
	return true;
}

uint32_t sky72310_vco_hz(uint32_t f_osc_hz, uint8_t ref_div, const sky72310_ratio_t *r)
{
	if (ref_div == 0) {
		return 0;
	}
	int32_t frac = (int32_t)(r->dividend << (32 - SKY72310_FRAC_BITS)) >> (32 - SKY72310_FRAC_BITS);	// sign extend
	uint64_t total = (uint64_t)(((int64_t)(r->n_reg + 32u) << SKY72310_FRAC_BITS) + frac);
	return (uint32_t)((total * f_osc_hz / ref_div) >> SKY72310_FRAC_BITS);
}

static bool wr(sky72310_t *dev, uint8_t reg, uint16_t data)
{
	dev->writes++;
	return dev->bus->write(dev->bus->ctx, sky72310_word(reg, data));
}

static uint16_t power_word(const sky72310_t *dev, bool down)
{
	return (uint16_t)(((uint16_t)(dev->mux & 7u) << SKY72310_MUX_SHIFT) | (down ? SKY72310_PWR_FULL_DOWN : 0u));
}

void sky72310_attach(sky72310_t *dev, const sky72310_bus_t *bus, uint32_t f_osc_hz, uint8_t pd_gain)
{
	dev->bus = bus;
	dev->f_osc_hz = f_osc_hz;
	dev->ref_div = 0;
	dev->pd_gain = pd_gain > 31u ? 31u : pd_gain;
	dev->mux = SKY72310_MUX_OSC;
	dev->vco_hz = 0;
	dev->writes = 0;
}

bool sky72310_init(sky72310_t *dev)
{
	bool ok = wr(dev, SKY72310_REG_POWER_MUX, power_word(dev, false));	// powered, fractional, 18-bit
	ok &= wr(dev, SKY72310_REG_DELTA_SIGMA, SKY72310_DS_BASE | SKY72310_DS_SIGNED);	// signed dividend
	ok &= wr(dev, SKY72310_REG_MOD_CTRL, 0);				// no modulation offset
	ok &= wr(dev, SKY72310_REG_PD_GAIN, dev->pd_gain);
	dev->ref_div = 0;
	dev->vco_hz = 0;
	return ok;
}

bool sky72310_set_vco(sky72310_t *dev, uint8_t ref_div, uint32_t f_vco_hz)
{
	sky72310_ratio_t r;
	if (!sky72310_ratio(dev->f_osc_hz, ref_div, f_vco_hz, &r)) {
		return false;
	}
	bool ok = true;
	if (ref_div != dev->ref_div) {
		ok &= wr(dev, SKY72310_REG_REF_DIV, (uint16_t)(ref_div - 1u));
		dev->ref_div = ok ? ref_div : 0;
	}
	ok &= wr(dev, SKY72310_REG_DIVIDER, r.n_reg);
	ok &= wr(dev, SKY72310_REG_DIVIDEND_LSB, (uint16_t)(r.dividend & 0xFFu));
	ok &= wr(dev, SKY72310_REG_DIVIDEND_MSB, (uint16_t)(r.dividend >> 8));	// the new ratio takes effect here
	dev->vco_hz = ok ? f_vco_hz : 0;
	return ok;
}

bool sky72310_power_down(sky72310_t *dev, bool down)
{
	return wr(dev, SKY72310_REG_POWER_MUX, power_word(dev, down));
}

bool sky72310_wait_lock(sky72310_t *dev, uint32_t timeout_us)
{
	if (dev->bus->locked == NULL) {
		return true;
	}
	for (uint32_t t = 0;; t += 10u) {
		if (dev->bus->locked(dev->bus->ctx)) {
			return true;
		}
		if (t >= timeout_us) {
			return false;
		}
		dev->bus->delay_us(10);
	}
}
