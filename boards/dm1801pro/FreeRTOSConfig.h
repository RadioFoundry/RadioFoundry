// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// FreeRTOS for the DM-1801 Pro (CK803S at 168 MHz; one interrupt level, no priorities: platform/hrc7000/freertos)
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

#define configUSE_PREEMPTION			1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION	0
#define configCPU_CLOCK_HZ			168000000u
#define configTICK_RATE_HZ			1000
#define configMAX_PRIORITIES			7
#define configMINIMAL_STACK_SIZE		128
#define configTOTAL_HEAP_SIZE			(40 * 1024)	// task stacks and queues
#define configMAX_TASK_NAME_LEN			12
#define configUSE_16_BIT_TICKS			0
#define configIDLE_SHOULD_YIELD			1
#define configUSE_MUTEXES			1
#define configUSE_RECURSIVE_MUTEXES		1
#define configUSE_COUNTING_SEMAPHORES		1
#define configQUEUE_REGISTRY_SIZE		0
#define configUSE_TASK_NOTIFICATIONS		1
#define configSUPPORT_STATIC_ALLOCATION		0
#define configSUPPORT_DYNAMIC_ALLOCATION	1
#define configUSE_IDLE_HOOK			0
#define configUSE_TICK_HOOK			1
#define configCHECK_FOR_STACK_OVERFLOW		2
#define configUSE_MALLOC_FAILED_HOOK		1
#define configUSE_TIMERS			1
#define configTIMER_TASK_PRIORITY		(configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH		8
#define configTIMER_TASK_STACK_DEPTH		256
#define configUSE_TRACE_FACILITY		0
#define configUSE_STATS_FORMATTING_FUNCTIONS	0

#define INCLUDE_vTaskPrioritySet		1
#define INCLUDE_uxTaskPriorityGet		1
#define INCLUDE_vTaskDelete			1
#define INCLUDE_vTaskSuspend			1
#define INCLUDE_vTaskDelayUntil			1
#define INCLUDE_vTaskDelay			1
#define INCLUDE_xTaskGetSchedulerState		1
#define INCLUDE_xTaskGetCurrentTaskHandle	1
#define INCLUDE_uxTaskGetStackHighWaterMark	1

#define configASSERT(x)				do { if (!(x)) { __asm__ volatile ("psrclr ie"); for (;;) {} } } while (0)

#endif
