// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 watchdog (hal_watchdog.h, CONFIG_WATCHDOG; docs/spec/hardware-dm32.md 3.5, docs/spec/watchdog.md 3.2).
// Every register write goes through LOCK: the unlock key first, LOCK = 0 after. The counter counts up from the load
// and the timeout is its overflow (see WDT_LOAD).
#include "config.h"

#if CONFIG_WATCHDOG
#include <stddef.h>
#include "platform.h"
#include "hal_watchdog.h"

#define WDT_LOCK	REG32(WDT_BASE + 0x00)
#define WDT_OT_LOAD_H	REG32(WDT_BASE + 0x04)
#define WDT_OT_LOAD_L	REG32(WDT_BASE + 0x08)
#define WDT_RST_LOAD	REG32(WDT_BASE + 0x0C)
#define WDT_EN		REG32(WDT_BASE + 0x10)
#define WDT_KNOCK	REG32(WDT_BASE + 0x14)
#define WDT_KEY		0x5ADA7200u
#define WDT_FEED	0x55AADD22u
#define WDT_LEAVE	0xFFFFFFFFu	// hal_watchdog_test_load: keep RST_LOAD as it is

// The timeout aimed for. It must stay comfortably above 3 s: a CPU fault prints its trace on the host UART three times,
// a second apart, with interrupts off and no feeds, before it saves the crash record and restarts (irq.c hrc_fault).
// The boot also has to fit: main() feeds before the scheduler starts, and the slowest start-up step (a SPI NAND
// mount) feeds per block
#define WDT_TIMEOUT_MS	4000u
// OT_LOAD_H (3 bits) and OT_LOAD_L are one 35-bit start value; the counter counts up from it at the 24 MHz crystal and
// the timeout is the overflow at 2^35 (HR_C7000 manual 2, WDT registers; the rate measured on the bench: 2^28 counts
// in 11.1 s). So the load is 2^35 minus the timeout's counts, and a small load is the longest timeout, not the shortest
#define WDT_HZ		24000000ull
#define WDT_LOAD	((1ull << 35) - WDT_HZ / 1000u * WDT_TIMEOUT_MS)
// While the flash stub runs (hrc_watchdog_stretch): the STM32 radios' longest IWDG timeout, so both platforms give a
// stub the same time between feeds
#define WDT_STUB_TIMEOUT_MS	32000u
#define WDT_STUB_LOAD	((1ull << 35) - WDT_HZ / 1000u * WDT_STUB_TIMEOUT_MS)

static bool running;
static bool test;		// re-armed with a bench load: the timeout is what's being measured

static void arm(uint32_t hi, uint32_t lo, uint32_t rst)
{
	uint32_t psr = hrc_irq_save();
	WDT_LOCK = WDT_KEY;
	WDT_EN = 0;
	WDT_OT_LOAD_H = hi;
	WDT_OT_LOAD_L = lo;
	if (rst != WDT_LEAVE) {
		WDT_RST_LOAD = rst;
	}
	WDT_KNOCK = WDT_FEED;
	WDT_EN = 1;
	WDT_LOCK = 0;
	running = true;
	hrc_irq_restore(psr);
}

void hal_watchdog_start(void)
{
	test = false;
	arm((uint32_t)(WDT_LOAD >> 32) & 7u, (uint32_t)WDT_LOAD, WDT_LEAVE);
}

void hal_watchdog_feed(void)
{
	if (!running) {
		return;
	}
	uint32_t psr = hrc_irq_save();	// LOCK is shared: a feed interrupted between its writes would be lost
	WDT_LOCK = WDT_KEY;
	WDT_KNOCK = WDT_FEED;
	WDT_LOCK = 0;
	hrc_irq_restore(psr);
}

bool hal_watchdog_stop(void)
{
	uint32_t psr = hrc_irq_save();
	WDT_LOCK = WDT_KEY;
	WDT_EN = 0;
	WDT_LOCK = 0;
	running = false;
	hrc_irq_restore(psr);
	return true;
}

// Before the stub entry (system.c enter_stub, RAM code with interrupts off): a running watchdog keeps running with the
// stub's timeout and a feed, and the stub feeds it from then on (docs/spec/watchdog.md 3.2). Register writes only
HRC_RAMFUNC void hrc_watchdog_stretch(void)
{
	if (!running) {
		return;
	}
	WDT_LOCK = WDT_KEY;
	WDT_EN = 0;
	WDT_OT_LOAD_H = (uint32_t)(WDT_STUB_LOAD >> 32) & 7u;
	WDT_OT_LOAD_L = (uint32_t)WDT_STUB_LOAD;
	WDT_KNOCK = WDT_FEED;
	WDT_EN = 1;
	WDT_LOCK = 0;
}

uint32_t hal_watchdog_timeout_ms(void)
{
	return running && !test ? WDT_TIMEOUT_MS : 0;
}

uint8_t hal_watchdog_regs(uint32_t *out)
{
	for (uint32_t i = 0; i < HAL_WATCHDOG_REGS; i++) {
		out[i] = REG32(WDT_BASE + 4u * i);	// LOCK, OT_LOAD_H, OT_LOAD_L, RST_LOAD, EN, KNOCK
	}
	return HAL_WATCHDOG_REGS;
}

bool hal_watchdog_test_load(const uint32_t arg[3])
{
	if (arg == NULL) {
		return true;
	}
	test = true;
	arm(arg[0], arg[1], arg[2]);
	return true;
}

#endif	// CONFIG_WATCHDOG
