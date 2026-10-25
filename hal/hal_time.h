// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Time: a millisecond clock that works before and after the RTOS starts
#ifndef HAL_TIME_H
#define HAL_TIME_H

#include <stdint.h>

uint32_t hal_millis(void);
// Microseconds, wraps every 71 minutes: for timing inside a millisecond
uint32_t hal_micros(void);
// Sleeps the calling task once the RTOS runs; busy-waits before that
void hal_delay_ms(uint32_t ms);
// Always a busy wait
void hal_delay_us(uint32_t us);

#endif
