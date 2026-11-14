// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FreeRTOS settings this radio changes from platform/gd32f30x/FreeRTOSConfig.h (read before its defaults)
#ifndef FREERTOS_BOARD_H
#define FREERTOS_BOARD_H

#include "config.h"

#if CONFIG_DMR
// The RTOS heap holds every task stack and queue, and main RAM (96 KB, no CCM) is this radio's limit with DMR on. The
// ten tasks' stacks are about 16.7 KB (the GPS task's 1.4 KB with CONFIG_GPS), their control blocks about 0.9 KB, the
// queues and locks about 1.3 KB, and a zone share send takes 1 KB for a moment: about 19.9 KB. 22 KB keeps about
// 2.1 KB spare (3.1 KB with zone sharing off, as it is with DMR on), above the 2 KB
// tests/hil/test_task_stacks.py asks for. Estimated from the source, not yet measured on the radio: the debug link's
// tasks view reports the least free since start (tasks.heap_min_free)
#define configTOTAL_HEAP_SIZE	(22 * 1024)
#endif

#endif
