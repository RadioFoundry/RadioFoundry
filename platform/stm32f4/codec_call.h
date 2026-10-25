// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Call a function of a stock codec image that sits at a fixed address (docs/spec/gotchas.md 4.1, 4.4).
#ifndef PLATFORM_STM32F4_CODEC_CALL_H
#define PLATFORM_STM32F4_CODEC_CALL_H

#include <stdint.h>

#define CODEC_CALL_MAX_ARGS	12

// AAPCS call of the Thumb function at fn (address without or with the Thumb bit) with nargs 32-bit arguments:
// args[0..3] in r0-r3, args[4..] on the stack in order, the stack 8-byte aligned at the call. Returns r0.
// Hand-written assembly with its own frame, so it doesn't depend on how the caller was compiled. To the compiler it
// is an ordinary external call: memory the codec may touch is written back before and reloaded after.
uint32_t stm32_codec_call(uint32_t fn, const uint32_t *args, uint32_t nargs);

#endif
