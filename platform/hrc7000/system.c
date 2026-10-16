// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// hal_system_t for the HR_C7000: memory reads and reboot. No chip identity registers are known (the eFuse behind SPI
// master 3 is not mapped out), so chip() reports nothing.
#include <string.h>
#include "config.h"
#include "platform.h"
#include "vocoder.h"

#define SOFT_RESET_SPINS	1000000u
#define WDT_KEY			0x5ADA7200u

static bool chip(hal_chip_t *c)
{
	(void)c;
	return false;
}

static void rd(uint32_t addr, void *dst, size_t n)
{
	memcpy(dst, (const void *)(uintptr_t)addr, n);
}

HRC_RAMFUNC void hrc_soft_reset(void)
{
	__asm__ volatile ("psrclr ie");
	QUAD_ENABLE = 0;	// XIP controller back to its reset mode, so the ROM and bootloader can read flash. The soft
	LCSFC_BAUDR = 4;	// reset keeps the divider: single-bit reads at divider 2 fail and the radio never boots
	SYS_SOFT_RSTN = 0;
	for (volatile uint32_t i = 0; i < SOFT_RESET_SPINS; i++) {
	}
	for (;;) {	// did not reset: the battery has to come out
	}
}

// Only a normal reset: the ROM serial loader needs an FTDI cable and Dahua's tool (hardware-dm32.md 13.3), and the
// Baofeng update mode needs PTT + SK1 held at power-on, which software can't do
static bool reboot(uint8_t mode)
{
	if (mode != HAL_REBOOT_NORMAL) {
		return false;
	}
	hrc_soft_reset();
}

#if CONFIG_STUB_ENTRY
// Runtime RAM-monitor entry (docs/spec/hardware-dm32.md 17.2): the stock-codec RAM window. A radio that hosts its
// codec runs it there, so the codec stops first (bench 2026-10-15: the upload overwrote the running codec and hung the
// radio on a DMR channel)
static uint8_t *stub_region(uint32_t *size)
{
#if CONFIG_DMR
	hrc_vocoder_park();
#endif
	*size = HRC_STUB_SIZE;
	return (uint8_t *)(uintptr_t)HRC_STUB_BASE;
}

// Quiesce the SoC and jump to the loaded image. Runs from RAM: after QUAD_ENABLE = 0 no code may fetch from XIP flash,
// and the image lives in SRAM. It never returns on success (recover by a power cycle), only if entry is out of range
HRC_RAMFUNC static bool enter_stub(uint32_t entry)
{
	if (entry < HRC_STUB_BASE || entry >= HRC_STUB_BASE + HRC_STUB_SIZE) {
		return false;
	}
	__asm__ volatile ("psrclr ie");			// CPU interrupts off
	PIC_MASK = 0xFFFFFFFFu;				// mask every PIC source, both banks (no peripheral can interrupt)
	PIC_MASK_1 = 0xFFFFFFFFu;
	REG32(WDT_BASE + 0x00) = WDT_KEY;		// watchdog off (defensive; clock_init already did)
	REG32(WDT_BASE + 0x10) = 0;
	REG32(WDT_BASE + 0x00) = 0;
	QUAD_ENABLE = 0;				// XIP controller back to reset mode: the program flash is the SSI's
	LCSFC_BAUDR = 4;				// (both registers: a later soft reset keeps the divider)
							// again, as hrc_soft_reset does. No DMA reaches SRAM on this board
	hrc_barrier();
	((void (*)(void))(uintptr_t)entry)();		// run the stub; it serves the host until power-off
	for (;;) {					// unreachable: the stub does not return
	}
}
#endif

const hal_system_t hrc_system = {
	.chip = chip,
	.read = rd,
	.reboot = reboot,
#if CONFIG_STUB_ENTRY
	.stub_region = stub_region,
	.enter_stub = enter_stub,
#endif
};
