// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Watchdog, PLL, peripheral clocks and the XIP flash cache (docs/spec/hardware-dm32.md 1.1, 1.3, 3.5).
//
// The Baofeng bootloader starts us with the chip already running (it drives a display and the UART), very likely on
// the PLL. Reprogramming a PLL the CPU is clocked from is not safe, so the PLL steps run only when the system clock
// still comes from the 24 MHz oscillator (CLK_MGR_CTRL bits 3:2 set). Everything here runs from RAM: it changes the
// XIP flash controller's timing, and polls with a cycle loop (no timer yet).
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
#define POLL_LIMIT	20000u	// x about 100 us: 2 s, then give up and stay on the oscillator

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

HRC_RAMFUNC void hrc_clock_init(void)
{
	// Watchdog off (whether the bootloader leaves it running is unknown)
	WDT_LOCK = WDT_KEY;
	WDT_EN = 0;
	WDT_LOCK = 0;

	bool on_osc = (CLK_MGR_CTRL & 0x0Cu) == 0x0Cu;
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
		CLK_MGR_PLL = (CLK_MGR_PLL & ~0x00FFFFFFu) | (PLL_REFDIV << 18) | (PLL_FBDIV << 6) | (PLL_POSTDIV1 << 3) |
			      PLL_POSTDIV2;
		CLK_MGR_USB_SDIO = (7u << 16) | 6u;		// USB 48 MHz, SDIO divider 6
		CLK_MGR_DIV = (CLK_MGR_DIV & ~0xFu) | 0x2u;	// PLL : AHB = 1 : 1 (APB = AHB / 2)
		CLK_MGR_CTRL |= 0x3u;				// release the PLL resets
		if (wait_bit(1u << 31)) {			// locked
			CLK_MGR_CTRL &= ~(1u << 2);
			CLK_MGR_CTRL &= ~(1u << 3);		// system clock from the PLL
			(void)wait_bit(1u << 30);		// switch done
		}
	}
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
