// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD32F30x free watchdog, FWDGT (hal_watchdog.h, CONFIG_WATCHDOG; docs/spec/watchdog.md). It runs from the IRC40K
// RC oscillator, which starts by itself with it, and once started nothing but a reset stops it. The register layout
// and keys are the STM32's IWDG (platform/stm32f4/watchdog.c).
#include "config.h"

#if CONFIG_WATCHDOG
#include "platform.h"
#include "hal_watchdog.h"

#define KEY_FEED	0xAAAAu
#define KEY_ACCESS	0x5555u		// PSC and RLD writable
#define KEY_START	0xCCCCu
#define IRC40K_HZ	40000u		// typical; the data sheet allows 30 to 60 kHz
#define PSC_DIV64	4u		// IRC40K / 64: 1.6 ms a count at 40 kHz
#define PSC_DIV256	6u
#define RLD_MAX		0xFFFu
// 5 s at the typical clock, 3.3 s at its fastest, 6.7 s at its slowest
#define TIMEOUT_MS	5000u
#define RELOAD		(IRC40K_HZ / 64u * TIMEOUT_MS / 1000u)
#define STAT_SPINS	100000u		// PSC / RLD take a few IRC40K periods to reach the counter's domain

static bool running;
static uint32_t timeout_ms;

static void load(uint32_t psc, uint32_t rld)
{
	FWDGT_CTL = KEY_ACCESS;
	FWDGT_PSC = psc;
	FWDGT_RLD = rld;
	for (uint32_t i = 0; i < STAT_SPINS && FWDGT_STAT != 0; i++) {
	}
	FWDGT_CTL = KEY_FEED;
}

void hal_watchdog_start(void)
{
	DBG_CTL0 |= DBG_CTL0_FWDGT_HOLD;	// stopped while a debugger halts the core
	FWDGT_CTL = KEY_START;
	load(PSC_DIV64, RELOAD - 1u);
	running = true;
	timeout_ms = TIMEOUT_MS;
}

void hal_watchdog_feed(void)
{
	if (running) {
		FWDGT_CTL = KEY_FEED;
	}
}

bool hal_watchdog_stop(void)
{
	return false;	// runs until the next reset
}

#if CONFIG_STUB_ENTRY
void gd32_watchdog_stretch(void)
{
	if (!running) {
		return;
	}
	load(PSC_DIV256, RLD_MAX);	// 26 s at 40 kHz, at least 17 s
	timeout_ms = (RLD_MAX + 1u) * 256u / (IRC40K_HZ / 1000u);
}
#endif

uint32_t hal_watchdog_timeout_ms(void)
{
	return running ? timeout_ms : 0;
}

uint8_t hal_watchdog_regs(uint32_t *out)
{
	out[0] = FWDGT_PSC;
	out[1] = FWDGT_RLD;
	out[2] = FWDGT_STAT;
	return 3;
}

bool hal_watchdog_test_load(const uint32_t arg[3])
{
	(void)arg;
	return false;
}

#endif	// CONFIG_WATCHDOG
