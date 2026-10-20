// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// FreeRTOS for the STM32F405 radios (docs/spec/hardware-dm1701.md 3.1: kernel at the lowest priority, max syscall 5)
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
extern uint32_t SystemCoreClock;

#define configUSE_PREEMPTION			1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION	1
#define configCPU_CLOCK_HZ			(SystemCoreClock)
#define configTICK_RATE_HZ			1000
#define configMAX_PRIORITIES			7
#if CONFIG_CRASH_LOG
#define configMINIMAL_STACK_SIZE		144	// the idle task: a configASSERT in the kernel captures on it (make stack-check)
#else
#define configMINIMAL_STACK_SIZE		128
#endif
#define configAPPLICATION_ALLOCATED_HEAP	1		// ucHeap in the CCM (board.c): every task stack is there
#define configTOTAL_HEAP_SIZE			(28 * 1024)	// task stacks (about 20 KB with DMR: audio 5 KB, radio 3 KB), queues
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

// Every task's handle for the debug link's tasks view, its least free stack (core/app/task_stack.c)
#if CONFIG_TASK_STACKS && !defined(__ASSEMBLER__)
#include "app/task_stack.h"
#define traceTASK_CREATE(tcb)			task_stack_created(tcb)
#define traceTASK_DELETE(tcb)			task_stack_deleted(tcb)
#endif

#define INCLUDE_vTaskPrioritySet		1
#define INCLUDE_uxTaskPriorityGet		1
#define INCLUDE_vTaskDelete			1
#define INCLUDE_vTaskSuspend			1
#define INCLUDE_vTaskDelayUntil			1
#define INCLUDE_vTaskDelay			1
#define INCLUDE_xTaskGetSchedulerState		1
#define INCLUDE_xTaskGetCurrentTaskHandle	1
#define INCLUDE_uxTaskGetStackHighWaterMark	1

#define configPRIO_BITS				4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY		15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY	5
#define configKERNEL_INTERRUPT_PRIORITY		(configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY	(configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

// A failed assertion: with the crash log, kept (the line, the caller) and the radio restarts (core/crash/crash.h)
#if CONFIG_CRASH_LOG && !defined(__ASSEMBLER__)
void crash_assert(unsigned line) __attribute__((noreturn));
#define configASSERT(x)				do { if (!(x)) { crash_assert(__LINE__); } } while (0)
#else
#define configASSERT(x)				do { if (!(x)) { __asm volatile ("cpsid i"); for (;;) {} } } while (0)
#endif

#define vPortSVCHandler				SVC_Handler
#define xPortPendSVHandler			PendSV_Handler
#define xPortSysTickHandler			SysTick_Handler

#endif
