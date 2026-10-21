// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// K22F peripheral arithmetic, pure so the host tests check it: the I2C clock divider (I2C_F), the DSPI chip select
// delays (CTAR PCSSCK/CSSCK, PASC/ASC, PDT/DT) and the FTM's PWM period and duty.
#ifndef K22F_PERIPH_MATH_H
#define K22F_PERIPH_MATH_H

#include <stdint.h>
#include <stdbool.h>

// I2C SCL divider by ICR (reference manual, I2C divider and hold values); SCL = bus / (mul x divider), mul = 1, 2, 4
static const uint16_t k22f_i2c_scl_div[64] = {
	20, 22, 24, 26, 28, 30, 34, 40, 28, 32, 36, 40, 44, 48, 56, 68,
	48, 56, 64, 72, 80, 88, 104, 128, 80, 96, 112, 128, 144, 160, 192, 240,
	160, 192, 224, 256, 288, 320, 384, 480, 320, 384, 448, 512, 576, 640, 768, 960,
	640, 768, 896, 1024, 1152, 1280, 1536, 1920, 1280, 1536, 1792, 2048, 2304, 2560, 3072, 3840,
};

// The I2C_F value whose SCL is the fastest at or below max_hz, and that SCL in *hz. A multiplier (MULT x2, x4) only
// when the divider table alone can't get that slow. 0xFF when even the slowest setting is too fast
static inline uint8_t k22f_i2c_f(uint32_t bus_hz, uint32_t max_hz, uint32_t *hz)
{
	uint32_t best_hz = 0;
	uint8_t best = 0xFFu;
	for (uint8_t mult = 0; mult < 3u && best == 0xFFu; mult++) {
		for (uint8_t icr = 0; icr < 64u; icr++) {
			uint32_t f = bus_hz / ((1u << mult) * k22f_i2c_scl_div[icr]);
			if (f <= max_hz && f > best_hz) {
				best_hz = f;
				best = (uint8_t)((mult << 6) | icr);
			}
		}
	}
	if (hz != 0) {
		*hz = best_hz;
	}
	return best;
}

// A DSPI delay field pair: prescaler code 0..3 (x1, 3, 5, 7) and scaler code 0..15 (x 2^(code + 1)); the shortest
// delay of at least ns at bus_hz. False when even the longest is shorter
static inline bool k22f_dspi_delay(uint32_t bus_hz, uint32_t ns, uint8_t *pre, uint8_t *scale)
{
	static const uint8_t p[4] = { 1, 3, 5, 7 };
	uint64_t need = ((uint64_t)bus_hz * ns + 999999999u) / 1000000000u;	// bus cycles, rounded up
	uint64_t best = UINT64_MAX;
	for (uint8_t i = 0; i < 4u; i++) {
		for (uint8_t s = 0; s < 16u; s++) {
			uint64_t c = (uint64_t)p[i] << (s + 1u);
			if (c >= need && c < best) {
				best = c;
				*pre = i;
				*scale = s;
			}
		}
	}
	return best != UINT64_MAX;
}

// FTM edge-aligned PWM: the prescaler code (divide by 2^ps) and MOD for a period close to pwm_hz with MOD <= 0xFFFF,
// the finest step first. False when pwm_hz is out of reach
static inline bool k22f_pwm_period(uint32_t bus_hz, uint32_t pwm_hz, uint8_t *ps, uint16_t *mod)
{
	if (pwm_hz == 0u) {
		return false;
	}
	for (uint8_t p = 0; p < 8u; p++) {
		uint32_t counts = (bus_hz >> p) / pwm_hz;
		if (counts >= 2u && counts <= 0x10000u) {
			*ps = p;
			*mod = (uint16_t)(counts - 1u);
			return true;
		}
	}
	return false;
}

// The channel value for percent 0..100 of the period (MOD + 1 counts): 0 = always low, 100 = always high
static inline uint32_t k22f_pwm_cv(uint16_t mod, uint8_t percent)
{
	if (percent >= 100u) {
		return (uint32_t)mod + 1u;	// above MOD: the output never clears
	}
	return ((uint32_t)mod + 1u) * percent / 100u;
}

#endif
