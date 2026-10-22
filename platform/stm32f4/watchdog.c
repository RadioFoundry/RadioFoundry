// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 independent watchdog, IWDG (hal_watchdog.h, CONFIG_WATCHDOG; docs/spec/watchdog.md 3.1). It runs from the
// LSI RC oscillator, which starts by itself with it. Once started nothing but a reset stops it: a system reset does
// (NVIC_SystemReset, the ROM DFU request's path), so main() starts it only after stm32_sysboot_check() has had its
// chance to jump to the ROM bootloader. The option bytes select the software watchdog (stub-stm32f4.md 3), so the
// stock bootloader never runs with it.
#include "config.h"

#if CONFIG_WATCHDOG
#include "platform.h"
#include "hal_watchdog.h"

#define KR_FEED		0xAAAAu
#define KR_ACCESS	0x5555u		// PR and RLR writable
#define KR_START	0xCCCCu
#define LSI_HZ		32000u		// typical; the data sheet allows 17 to 47 kHz
#define PR_DIV64	4u		// LSI / 64: 2 ms a count at 32 kHz
#define PR_DIV256	6u
#define RLR_MAX		0xFFFu
// 5 s at the typical LSI, 3.4 s at its fastest (47 kHz), 9.4 s at its slowest (17 kHz)
#define TIMEOUT_MS	5000u
#define RELOAD		(LSI_HZ / 64u * TIMEOUT_MS / 1000u)
#define SR_SPINS	100000u		// PR / RLR take a few LSI periods to reach the counter's domain

static bool running;
static uint32_t timeout_ms;

static void load(uint32_t pr, uint32_t rlr)
{
	IWDG->KR = KR_ACCESS;
	IWDG->PR = pr;
	IWDG->RLR = rlr;
	for (uint32_t i = 0; i < SR_SPINS && IWDG->SR != 0; i++) {
	}
	IWDG->KR = KR_FEED;	// reload with the new values (also locks PR and RLR again)
}

void hal_watchdog_start(void)
{
	DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;	// stopped while a debugger halts the core
	IWDG->KR = KR_START;
	load(PR_DIV64, RELOAD - 1u);
	running = true;
	timeout_ms = TIMEOUT_MS;
}

void hal_watchdog_feed(void)
{
	if (running) {
		IWDG->KR = KR_FEED;
	}
}

bool hal_watchdog_stop(void)
{
	return false;	// the IWDG runs until the next reset
}

void stm32_watchdog_stretch(void)
{
	if (!running) {
		return;
	}
	load(PR_DIV256, RLR_MAX);	// 32.8 s at 32 kHz, at least 22 s
	timeout_ms = (RLR_MAX + 1u) * 256u / (LSI_HZ / 1000u);
}

uint32_t hal_watchdog_timeout_ms(void)
{
	return running ? timeout_ms : 0;
}

uint8_t hal_watchdog_regs(uint32_t *out)
{
	out[0] = IWDG->PR;
	out[1] = IWDG->RLR;
	out[2] = IWDG->SR;
	return 3;
}

bool hal_watchdog_test_load(const uint32_t arg[3])
{
	(void)arg;
	return false;	// the IWDG's clock is known; nothing to measure
}

#endif	// CONFIG_WATCHDOG
