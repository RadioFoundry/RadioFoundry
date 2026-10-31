// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// hal_system_t for the HR_C7000: chip identity, memory reads and reboot. No chip identity registers are known (the eFuse
// behind SPI master 3 is not mapped out), so the UID is the board's data flash's unique ID (hrc_chip_set_uid), and
// chip() reports nothing until the board has read it.
#include <string.h>
#include "config.h"
#include "platform.h"
#include "board.h"
#include "vocoder.h"
#include "mem_window.h"
#include "modem/hrc7000/hrc7000.h"

#define SOFT_RESET_SPINS	1000000u
#define WDT_KEY			0x5ADA7200u

static uint32_t uid[3];
static bool have_uid;

void hrc_chip_set_uid(const uint8_t *id, uint32_t len)
{
	uint32_t w[4] = { 0 };
	bool varies = false;
	for (uint32_t i = 0; i < len && i < sizeof(w); i++) {
		w[i / 4] |= (uint32_t)id[i] << (8u * (i % 4));
		varies = varies || id[i] != id[0];
	}
	if (!varies) {
		return;		// all one value: a bus that read nothing, not an ID
	}
	uid[0] = w[0] ^ w[3];	// all 16 bytes count: the token (uid[0]) differs whichever part of the ID varies
	uid[1] = w[1];
	uid[2] = w[2];
	have_uid = true;
}

static bool chip(hal_chip_t *c)
{
	if (!have_uid) {
		return false;
	}
	memset(c, 0, sizeof(*c));
	c->uid[0] = uid[0];
	c->uid[1] = uid[1];
	c->uid[2] = uid[2];
	c->family = HAL_CHIP_HRC7000;
	return true;
}

// Whole aligned words are read one 32-bit load at a time: that is what a register block needs (mem_window.h refuses
// anything else there), and memcpy may use byte loads
static void rd(uint32_t addr, void *dst, size_t n)
{
	if (((addr | n) & 3u) != 0) {
		memcpy(dst, (const void *)(uintptr_t)addr, n);
		return;
	}
	uint8_t *d = dst;
	for (size_t i = 0; i < n; i += 4u) {
		uint32_t v = REG32(addr + i);
		memcpy(d + i, &v, 4u);
	}
}

static bool mem_window(unsigned i, hal_mem_window_t *w)
{
	return hrc_mem_window_for(i, w, HRC_XIP_FLASH_SIZE);
}

#define TXRX_NONE		0x00u	// LAYER2_TXRX_CTRL: neither slot direction (modem-hrc7000.md; hrc7000_dmr.c)

HRC_RAMFUNC void hrc_rf_safe(void)
{
	board_rf_safe();
	REG32(HRC7000_REG_BASE + C7_FM_PTT) = 0;
	REG32(HRC7000_REG_BASE + C7_LAYER2_TXRX_CTRL) = TXRX_NONE;
	hrc_barrier();
}

// The soft reset keeps some of the chip's state (the clock manager, LCSFC_BAUDR; hardware-dm32.md 1.1), and a radio
// reset this way while transmitting came back with the PA keyed and hung before the display was lit (2026-10-21). So
// the transmitter goes off first, before the clock and flash controller changes
HRC_RAMFUNC void hrc_soft_reset(void)
{
	__asm__ volatile ("psrclr ie");
	HRC_CRASH_SOFT_MARK();	// asked for: not the watchdog (crash-log.md 1.3)
	hrc_rf_safe();
	// Watchdog off: whether a soft reset clears it is unknown, and the bootloader (its update mode, which waits for the
	// PC) never feeds it
	REG32(WDT_BASE + 0x00) = WDT_KEY;
	REG32(WDT_BASE + 0x10) = 0;
	REG32(WDT_BASE + 0x00) = 0;
#if HRC_LOADER_CPU_HZ != HRC_CPU_HZ
	hrc_clock_restore_loader();	// the clock manager survives a soft reset: the loader has to start on its own PLL
#endif
	QUAD_ENABLE = 0;	// XIP controller back to its reset mode, so the ROM and bootloader can read flash. The soft
	LCSFC_BAUDR = 4;	// reset keeps the divider: single-bit reads at divider 2 fail and the radio never boots
	SYS_SOFT_RSTN = 0;
	for (volatile uint32_t i = 0; i < SOFT_RESET_SPINS; i++) {
	}
#if CONFIG_WATCHDOG
	// Did not reset: the watchdog, back on with the loads it had, is the last way out
	REG32(WDT_BASE + 0x00) = WDT_KEY;
	REG32(WDT_BASE + 0x10) = 1;
	REG32(WDT_BASE + 0x00) = 0;
#endif
	for (;;) {	// did not reset: the battery has to come out (or the watchdog resets it)
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
	hrc_rf_safe();					// transmitter off: nothing may radiate while the stub runs
	PIC_MASK = 0xFFFFFFFFu;				// mask every PIC source, both banks (no peripheral can interrupt)
	PIC_MASK_1 = 0xFFFFFFFFu;
#if CONFIG_WATCHDOG
	hrc_watchdog_stretch();				// the watchdog keeps running, 32 s; the stub feeds it
#else
	REG32(WDT_BASE + 0x00) = WDT_KEY;		// watchdog off, whatever the loader left
	REG32(WDT_BASE + 0x10) = 0;
	REG32(WDT_BASE + 0x00) = 0;
#endif
	QUAD_ENABLE = 0;				// XIP controller back to reset mode: the program flash is the SSI's
	LCSFC_BAUDR = 4;				// (both registers: a later soft reset keeps the divider)
							// again, as hrc_soft_reset does. No DMA reaches SRAM on this board
#if HRC_LOADER_CPU_HZ != HRC_CPU_HZ
	// The stub is built for the loader's clocks (its UART divisor and tick come from them): put them back, then the
	// divisor that gives 115200 on them. The transmitter drains first, at our own clock, so the last reply isn't cut
	hrc_host_uart_drain();
	hrc_clock_restore_loader();
	hrc_host_uart_loader_div();
#endif
	hrc_barrier();
	((void (*)(void))(uintptr_t)entry)();		// run the stub; it serves the host until power-off
	for (;;) {					// unreachable: the stub does not return
	}
}
#endif

const hal_system_t hrc_system = {
	.chip = chip,
	.read = rd,
	.mem_window = mem_window,
	.reboot = reboot,
#if CONFIG_STUB_ENTRY
	.stub_region = stub_region,
	.enter_stub = enter_stub,
#endif
};
