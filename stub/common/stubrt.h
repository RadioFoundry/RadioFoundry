// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flash stubs' Cortex-M runtime (docs/spec/stub-stm32f4.md 13): what every stub target needs and no chip
// register decides. The freestanding C library pieces the compiler may call, the millisecond clock on the core's
// cycle counter (DWT CYCCNT, at the core clock the target names), .bss zeroing, hex formatting for the USB serial
// number, the system reset, and the main loop that feeds the protocol engine from the USB device (stubusb.h). Only
// architecture registers (DWT, CoreDebug, SCB) are touched, by address.
#ifndef STUBRT_H
#define STUBRT_H

#include <stdbool.h>
#include <stdint.h>
#include "stubeng.h"

#define STUBRT_IDLE_MS	200u	// a request that stops arriving for this long is dropped (as the debug link's)

// Start the cycle counter at core_hz (0 or under 1 MHz: 16 MHz assumed, a safe guess for timeouts only)
void stubrt_clock_start(uint32_t core_hz);
// The core clock changed: what was counted so far stays counted at the old rate
void stubrt_clock_rate(uint32_t core_hz);
uint32_t stub_ms(void);
void stub_delay_ms(uint32_t ms);

// Zero the image's .bss (the linker script's _sbss.._ebss)
void stubrt_zero_bss(void);

// n bytes as 2n upper case hex digits, first byte first, high nibble first; out gets 2n + 1 bytes with the NUL
void stubrt_hex(char *out, const uint8_t *in, unsigned n);

// The engine's reset callback: let the RESET reply leave, then a system reset (SYSRESETREQ)
void stubrt_reset(void *ctx);

// Serve the engine forever: poll USB, feed what arrives, drop a request that stalls for STUBRT_IDLE_MS
void stubrt_serve(stubeng_t *e) __attribute__((noreturn));

#endif
