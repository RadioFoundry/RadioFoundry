// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Watchdog, PLL, peripheral clocks and the XIP flash cache (docs/spec/hardware-dm32.md 1.1, 1.3, 3.5).
//
// The Baofeng bootloader starts us with the chip already running (it drives a display and the UART) on the 168 MHz PLL.
// The PLL steps run only when the system clock still comes from the 24 MHz oscillator (CLK_MGR_CTRL bits 3:2 set), or
// when the board's loader leaves another rate (HRC_LOADER_CPU_HZ differs from HRC_CPU_HZ: the DB25-D's IAP loader, 72
// MHz). Reprogramming a PLL the CPU is clocked from is not safe, so that case first moves the SoC clock to the
// oscillator, then follows the PLL re-configuration flow of the user guide (4.2.3.2). Everything here runs from RAM: it
// changes the XIP flash controller's timing, and polls with a cycle loop (no timer yet).
#include "platform.h"

#define WDT_LOCK	REG32(WDT_BASE + 0x00)
#define WDT_EN		REG32(WDT_BASE + 0x10)
#define WDT_KEY		0x5ADA7200u

#define CACHE_CTRL	REG32(CACHE_BASE + 0x00)
#define CACHE_INV	REG32(CACHE_BASE + 0x04)
#define CACHE_REGION	REG32(CACHE_BASE + 0x08)

#define PLL_REFDIV	1u
#define PLL_FBDIV	28u	// 24 MHz x 28 = 672 MHz VCO
#define PLL_POSTDIV1	2u
#define PLL_POSTDIV2	2u	// / 2 / 2 = 168 MHz
#define PLL_OUT_HZ	(HRC_XTAL_HZ / PLL_REFDIV * PLL_FBDIV / PLL_POSTDIV1 / PLL_POSTDIV2)
_Static_assert(PLL_OUT_HZ == 168000000u, "the PLL set-up below is the 168 MHz one");
#if HRC_LOADER_CPU_HZ != HRC_CPU_HZ
_Static_assert(HRC_CPU_HZ == PLL_OUT_HZ, "a board that reclocks runs at the PLL set-up's rate");
#endif
#define POLL_LIMIT	20000u	// x about 100 us: 2 s, then give up and stay on the oscillator
#define CTRL_SOC_OSC	(1u << 3)	// bclk_work_sel: the SoC (CPU, AHB, APB) runs from the oscillator
#define CTRL_BPLL_CFG	(1u << 1)	// bclk_re_cfg_reg: the BPLL configuration is ready (a rising edge starts it)
#define DIV_FIELD	0xFu		// CLK_MGR_DIV: bit 0 CPU:AHB, bits 3:1 AHB:APB (1 = APB is half the AHB clock)
#define LOCK_SETTLE	800u		// spins: over 100 us at 24 MHz, the user guide's wait before the lock flag is read

// What the loader left, for hrc_clock_restore_loader(). Written once at start-up, before anything else runs
static struct {
	uint32_t pll, div, usb_sdio;
	bool changed;	// the PLL was reprogrammed from the loader's rate: restore has something to undo
} loader;

static HRC_RAMFUNC void spin(uint32_t n)
{
	for (volatile uint32_t i = 0; i < n; i++) {
	}
}

static HRC_RAMFUNC bool wait_bit(uint32_t bit)
{
	for (uint32_t i = 0; i < POLL_LIMIT; i++) {
		if (CLK_MGR_CTRL & bit) {
			return true;
		}
		spin(100);	// about 100 us at 24 MHz, less at 168 MHz
	}
	return false;
}

// The SoC clock (CPU, AHB, APB) to the oscillator (src_osc) or to the PLL; waits for the switch
static HRC_RAMFUNC bool soc_clock(bool src_osc)
{
	if (src_osc) {
		CLK_MGR_CTRL |= CTRL_SOC_OSC;
	} else {
		CLK_MGR_CTRL &= ~CTRL_SOC_OSC;
	}
	spin(LOCK_SETTLE);
	return wait_bit(1u << 30);
}

// Re-configure the BPLL from the oscillator: the new words are written while the BPLL is not driving the CPU, then
// bit 1 of CLK_MGR_CTRL goes low and high again so the chip power-cycles the PLL (about 2 us) and locks it. The APLL
// (the modem's clock, bit 0) is not touched. Returns after the lock flag; the caller switches the SoC clock back
static HRC_RAMFUNC bool bpll_program(uint32_t pll_fields, uint32_t div, uint32_t usb_sdio)
{
	CLK_MGR_CTRL &= ~CTRL_BPLL_CFG;
	CLK_MGR_PLL = (CLK_MGR_PLL & ~HRC_PLL_FIELDS) | (pll_fields & HRC_PLL_FIELDS);
	CLK_MGR_USB_SDIO = usb_sdio;
	CLK_MGR_DIV = (CLK_MGR_DIV & ~DIV_FIELD) | (div & DIV_FIELD);
	CLK_MGR_CTRL |= CTRL_BPLL_CFG;
	spin(LOCK_SETTLE);
	return wait_bit(1u << 31);
}

HRC_RAMFUNC void hrc_clock_restore_loader(void)
{
	if (!loader.changed) {
		return;
	}
	(void)soc_clock(true);
	if (bpll_program(loader.pll, loader.div, loader.usb_sdio)) {
		(void)soc_clock(false);
	}
	loader.changed = false;
}

HRC_RAMFUNC void hrc_clock_init(void)
{
	// Watchdog off (whether the bootloader leaves it running is unknown)
	WDT_LOCK = WDT_KEY;
	WDT_EN = 0;
	WDT_LOCK = 0;

	bool on_osc = (CLK_MGR_CTRL & 0x0Cu) == 0x0Cu;
	loader.pll = CLK_MGR_PLL;
	loader.div = CLK_MGR_DIV;
	loader.usb_sdio = CLK_MGR_USB_SDIO;
	loader.changed = false;
	if (on_osc) {
		SYS_SOFT_RSTN = 0x180;	// CPU and system out of reset (1.1 step 1)
	}
	// XIP flash in quad mode at SCLK divider 2 (1.1 steps 2-3), whoever set the PLL. The Baofeng bootloader starts
	// the PLL but leaves the controller in single-bit mode at divider 4, which runs code from flash about 6 times slower
	QUAD_ENABLE = 0x01000001u;
	LCSFC_BAUDR = 0x02;
	hrc_barrier();

	if (on_osc) {
		// Still on the oscillator: the PLL (1.1 steps 4-10)
		CLK_MGR_PLL = hrc_pll_word(CLK_MGR_PLL, PLL_REFDIV, PLL_FBDIV, PLL_POSTDIV1, PLL_POSTDIV2);
		CLK_MGR_USB_SDIO = (7u << 16) | 6u;		// USB 48 MHz, SDIO divider 6
		CLK_MGR_DIV = (CLK_MGR_DIV & ~0xFu) | 0x2u;	// PLL : AHB = 1 : 1 (APB = AHB / 2)
		CLK_MGR_CTRL |= 0x3u;				// release the PLL resets
		if (wait_bit(1u << 31)) {			// locked
			CLK_MGR_CTRL &= ~(1u << 2);
			CLK_MGR_CTRL &= ~(1u << 3);		// system clock from the PLL
			(void)wait_bit(1u << 30);		// switch done
		}
	}
#if HRC_LOADER_CPU_HZ != HRC_CPU_HZ
	else {
		// The loader's PLL (HRC_LOADER_CPU_HZ) runs the CPU: park it on the oscillator, re-program the PLL to ours,
		// come back. If the new PLL never locks, the loader's setting goes back so the CPU is not left at 24 MHz
		loader.changed = true;
		(void)soc_clock(true);
		if (bpll_program(hrc_pll_word(0, PLL_REFDIV, PLL_FBDIV, PLL_POSTDIV1, PLL_POSTDIV2), 0x2u, (7u << 16) | 6u)) {
			(void)soc_clock(false);
		} else {
			hrc_clock_restore_loader();
		}
	}
#endif
	CLK_MGR_GATES = 0xFFFFFFFFu;	// all peripheral clocks on
	IO_DIPLEX2 &= ~0x3u;		// CLK_OUT pin = PLLA (1.1 step 12)

	// Flash cache (1.3): invalidate, write-back 4-way I + D, region 0x03000000 / 8 MB, enable. SRAM is outside the
	// region, so invalidating never loses data
	CACHE_INV = 0x1;
	CACHE_CTRL = 0x04;
	CACHE_REGION = 0x0300002Du;
	CACHE_CTRL |= 0x1;
	hrc_barrier();
}
