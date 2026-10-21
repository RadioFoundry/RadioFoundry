// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// K22F clock arithmetic (reference manual 24.4.3, data sheet operating limits), pure so the host tests check every
// board's clock plan. The PLL takes the external reference divided by PRDIV0 + 1 (2-4 MHz) and multiplies it by
// VDIV0 + 24; MCGOUTCLK is the PLL output, and SIM_CLKDIV1 divides it for the core, bus, FlexBus and flash clocks.
#ifndef K22F_CLOCK_MATH_H
#define K22F_CLOCK_MATH_H

#include <stdint.h>
#include <stdbool.h>

#define K22F_PLL_REF_MIN	2000000u
#define K22F_PLL_REF_MAX	4000000u
#define K22F_PLL_OUT_MIN	48000000u
#define K22F_PLL_OUT_MAX	120000000u
#define K22F_PRDIV_MAX		23u	// divide by 1..24
#define K22F_VDIV_MAX		31u	// multiply by 24..55
// Operating limits: normal RUN mode, and high-speed RUN (HSRUN)
#define K22F_RUN_CORE_MAX	80000000u
#define K22F_RUN_BUS_MAX	50000000u
#define K22F_HSRUN_CORE_MAX	120000000u
#define K22F_HSRUN_BUS_MAX	60000000u
#define K22F_FLEXBUS_MAX	50000000u
#define K22F_FLASH_MAX		25000000u
// The FLL from the slow internal reference at reset (FEI, DCO low range): what runs before clock set-up, and the
// fallback when the external clock never shows up
#define K22F_FEI_HZ		20971520u
// The fast internal reference the switch passes through (MCG FBI mode)
#define K22F_FIRC_HZ		4000000u

typedef struct {
	uint32_t ref_hz;	// the external reference on EXTAL0
	bool crystal;		// true: a crystal across EXTAL0 / XTAL0 (oscillator on); false: a clock fed into EXTAL0
	uint8_t prdiv;		// MCG_C5 PRDIV0
	uint8_t vdiv;		// MCG_C6 VDIV0
	uint8_t div_core, div_bus, div_flexbus, div_flash;	// SIM_CLKDIV1 dividers, 1..16
	bool hsrun;		// switch to HSRUN first (needed above the RUN limits)
} k22f_clock_t;

static inline uint32_t k22f_pll_ref_hz(const k22f_clock_t *c)
{
	return c->ref_hz / (c->prdiv + 1u);
}

static inline uint32_t k22f_pll_out_hz(const k22f_clock_t *c)
{
	return (uint32_t)((uint64_t)c->ref_hz * (c->vdiv + 24u) / (c->prdiv + 1u));
}

static inline uint32_t k22f_core_hz(const k22f_clock_t *c)
{
	return k22f_pll_out_hz(c) / c->div_core;
}

static inline uint32_t k22f_bus_hz(const k22f_clock_t *c)
{
	return k22f_pll_out_hz(c) / c->div_bus;
}

static inline uint32_t k22f_flash_hz(const k22f_clock_t *c)
{
	return k22f_pll_out_hz(c) / c->div_flash;
}

// NULL when the plan is within every limit, else what it breaks
static inline const char *k22f_clock_check(const k22f_clock_t *c)
{
	if (c->prdiv > K22F_PRDIV_MAX || c->vdiv > K22F_VDIV_MAX) {
		return "PRDIV0 or VDIV0 out of range";
	}
	if (c->div_core < 1u || c->div_core > 16u || c->div_bus < 1u || c->div_bus > 16u || c->div_flexbus < 1u ||
	    c->div_flexbus > 16u || c->div_flash < 1u || c->div_flash > 16u) {
		return "a divider is outside 1..16";
	}
	uint32_t ref = k22f_pll_ref_hz(c);
	if (ref < K22F_PLL_REF_MIN || ref > K22F_PLL_REF_MAX) {
		return "PLL reference outside 2-4 MHz";
	}
	uint32_t out = k22f_pll_out_hz(c);
	if (out < K22F_PLL_OUT_MIN || out > K22F_PLL_OUT_MAX) {
		return "PLL output outside 48-120 MHz";
	}
	uint32_t core_max = c->hsrun ? K22F_HSRUN_CORE_MAX : K22F_RUN_CORE_MAX;
	uint32_t bus_max = c->hsrun ? K22F_HSRUN_BUS_MAX : K22F_RUN_BUS_MAX;
	if (k22f_core_hz(c) > core_max) {
		return "core clock above the mode's limit";
	}
	if (k22f_bus_hz(c) > bus_max || k22f_bus_hz(c) > k22f_core_hz(c)) {
		return "bus clock above the limit or the core clock";
	}
	if (out / c->div_flexbus > K22F_FLEXBUS_MAX || out / c->div_flexbus > k22f_bus_hz(c)) {
		return "FlexBus clock above the limit or the bus clock";
	}
	if (k22f_flash_hz(c) > K22F_FLASH_MAX || k22f_flash_hz(c) > k22f_bus_hz(c)) {
		return "flash clock above 25 MHz or the bus clock";
	}
	return 0;
}

// The RUN-mode form of an HSRUN plan, for flash programming and standby (clock.c k22f_clock_step_down): the core runs
// at the bus clock and nothing else changes. False when that still breaks a RUN limit (a bus clock above 50 MHz)
static inline bool k22f_clock_run_variant(const k22f_clock_t *c, k22f_clock_t *run)
{
	*run = *c;
	run->div_core = c->div_bus;
	run->hsrun = false;
	return k22f_clock_check(run) == 0;
}

// The time base's fold (time.c): d more core cycles into whole milliseconds and a remainder below per_ms. 32-bit
// arithmetic unless the sum wraps (only when nothing read the clock for close to a whole counter period)
static inline void k22f_time_fold(uint64_t *ms, uint32_t *rem, uint32_t d, uint32_t per_ms)
{
	uint32_t t = *rem + d;
	if (t < d) {
		uint64_t t64 = (uint64_t)*rem + d;
		*ms += t64 / per_ms;
		*rem = (uint32_t)(t64 % per_ms);
	} else {
		*ms += t / per_ms;
		*rem = t % per_ms;
	}
}

// A clock change: the part millisecond counted at the old rate carries over in proportion
static inline uint32_t k22f_time_rescale(uint32_t rem, uint32_t old_per_ms, uint32_t new_per_ms)
{
	return (uint32_t)((uint64_t)rem * new_per_ms / old_per_ms);
}

// The DSPI baud rate for CTAR prescaler / scaler codes (PBR 0..3 = 2, 3, 5, 7; BR 0..15 = 2, 4, 6, 8, 16 .. 32768)
static inline uint32_t k22f_dspi_hz(uint32_t bus_hz, uint8_t pbr, uint8_t br, bool dbr)
{
	static const uint8_t pre[4] = { 2, 3, 5, 7 };
	uint32_t scaler = br < 4u ? 2u * (br + 1u) : (1u << (br & 15u));
	return (uint32_t)((uint64_t)bus_hz * (dbr ? 2u : 1u) / ((uint32_t)pre[pbr & 3u] * scaler));
}

// The PIT load value (count down to 0, inclusive) for a period in microseconds at the bus clock
static inline uint32_t k22f_pit_ldval(uint32_t bus_hz, uint32_t period_us)
{
	return (uint32_t)((uint64_t)bus_hz * period_us / 1000000u) - 1u;
}

#endif
