// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS.h, for crashCaptureTest only: the interrupt switch core/crash/crash_capture.c uses, and
// whether it was thrown, and the idle task's name
#ifndef CRASH_STUB_FREERTOS_H
#define CRASH_STUB_FREERTOS_H

#include <stdbool.h>

extern bool fake_irq_off;
#define taskDISABLE_INTERRUPTS()	(fake_irq_off = true)
#define configIDLE_TASK_NAME		"IDLE"

#endif
