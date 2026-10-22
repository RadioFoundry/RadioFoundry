// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Host stand-in for FreeRTOS.h, for watchdogTest only: debuglink_watchdog.c masks the kernel's interrupts in one test,
// which the host test never starts (it spins until the reset)
#ifndef WATCHDOG_STUB_FREERTOS_H
#define WATCHDOG_STUB_FREERTOS_H

#define taskDISABLE_INTERRUPTS()	((void)0)

#endif
