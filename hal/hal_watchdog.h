// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SoC's hardware watchdog (CONFIG_WATCHDOG, docs/spec/watchdog.md). The platform owns the registers and starts it
// in main() before the scheduler; the idle task feeds it (core/app/watchdog.c), so any starvation or hang of a few
// seconds resets the chip. Long work that keeps the idle task out on purpose feeds it from inside its loop.
#ifndef HAL_WATCHDOG_H
#define HAL_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#define HAL_WATCHDOG_REGS	6	// raw words hal_watchdog_regs() may return

// Start it with the platform's timeout. Platform main() only
void hal_watchdog_start(void);
// Reload the counter. Any context, interrupts on or off; a no-op while it isn't running
void hal_watchdog_feed(void);
// Stop it, before leaving the application for code that never feeds it. False where the hardware can't (the STM32
// IWDG runs until the next reset once started)
bool hal_watchdog_stop(void);
// The nominal timeout in ms while running, 0 when stopped
uint32_t hal_watchdog_timeout_ms(void);
// The bench: the platform's raw registers (up to HAL_WATCHDOG_REGS words) for the debug link; returns how many
uint8_t hal_watchdog_regs(uint32_t *out);
// The bench: re-arm with a raw load in the hardware's own units, to measure its clock (docs/spec/watchdog.md 5).
// arg[0..2] are platform defined (HR_C7000: OT_LOAD_H, OT_LOAD_L, RST_LOAD or 0xFFFFFFFF to leave it). False where
// the platform has no such test. arg NULL only asks whether it has
bool hal_watchdog_test_load(const uint32_t arg[3]);

#endif
