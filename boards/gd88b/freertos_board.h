// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// RTOS settings of the GD-88's SoC B where they differ from platform/hrc7000/FreeRTOSConfig.h. B runs no radio core
// (app_gd88b.c): its tasks are the receiver, DMR and audio, and its watchdog feed is app_gd88b.c's idle hook
#ifndef FREERTOS_BOARD_H
#define FREERTOS_BOARD_H

#define configTOTAL_HEAP_SIZE			(20 * 1024)	// task stacks and queues: the receiver, DMR and audio tasks

#endif
