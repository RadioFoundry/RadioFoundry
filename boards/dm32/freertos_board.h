// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FreeRTOS settings this radio changes from platform/hrc7000/FreeRTOSConfig.h (read before its defaults)
#ifndef FREERTOS_BOARD_H
#define FREERTOS_BOARD_H

// The RTOS heap holds every task stack and queue. Main RAM is this radio's limit (config.mk), and the platform's 40 KB
// leaves about 13 KB of it unused: the ten tasks' stacks are 22.8 KB (make stack-check), their control blocks about
// 1.1 KB, the FM receive ring 2 KB and the queues and locks about 1.3 KB, 27.3 KB in all, and a zone share send takes
// 1 KB for a moment (core/ui/ui_svc_zone.c). 34 KB keeps about 5.7 KB spare at the most, as much as the STM32 radios
// keep in theirs, and gives the rest to main RAM. The debug link's tasks view reports the least free since start
// (tasks.heap_min_free); tests/hil/test_task_stacks.py checks it
#define configTOTAL_HEAP_SIZE	(34 * 1024)

#endif
