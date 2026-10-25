/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2017 C-SKY Microsystems Co., Ltd. All rights reserved.
 * Copyright (c) 2026 The RadioFoundry Authors
 *
 * FreeRTOS port for the C-SKY CK803S in the HR_C7000, adapted from FreeRTOS-Kernel
 * portable/ThirdParty/CDK/T-HEAD_CK802 (MIT). Changes: no CSI headers; the context switch requested from an interrupt
 * is done once at the end of the common interrupt entry (portasm.S) instead of calling vTaskSwitchContext inside the
 * handler; portEND_SWITCHING_ISR never calls the task-level yield. CK803S without high registers, DSP or FPU: the
 * context is r0-r13, r15, PSR and PC (17 words).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 * to permit persons to whom the Software is furnished to do so, subject to the following conditions: the above
 * copyright notice and this permission notice shall be included in all copies or substantial portions of the
 * Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define portCHAR		char
#define portFLOAT		float
#define portDOUBLE		double
#define portLONG		long
#define portSHORT		short
#define portSTACK_TYPE		uint32_t
#define portBASE_TYPE		long

typedef portSTACK_TYPE StackType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;

#if (configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_16_BITS)
typedef uint16_t TickType_t;
#define portMAX_DELAY		(TickType_t)0xffff
#elif (configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_32_BITS)
typedef uint32_t TickType_t;
#define portMAX_DELAY		(TickType_t)0xffffffffUL
#define portTICK_TYPE_IS_ATOMIC	1
#else
#error configTICK_TYPE_WIDTH_IN_BITS set to unsupported tick type width.
#endif

#define portBYTE_ALIGNMENT	8
#define portSTACK_GROWTH	(-1)
#define portTICK_PERIOD_MS	((TickType_t)1000 / configTICK_RATE_HZ)

static inline void vPortEnableInterrupt(void)
{
	__asm__ volatile ("psrset ie" : : : "memory");
}

static inline void vPortDisableInterrupt(void)
{
	__asm__ volatile ("psrclr ie" : : : "memory");
}

static inline uint32_t ulPortSaveLocalPSR(void)
{
	uint32_t psr;
	__asm__ volatile ("mfcr %0, psr\n\tpsrclr ie" : "=r"(psr) : : "memory");
	return psr;
}

static inline void vPortRestoreLocalPSR(uint32_t psr)
{
	__asm__ volatile ("mtcr %0, psr" : : "r"(psr) : "memory");
}

extern void vPortYield(void);
extern void vPortEnterCritical(void);
extern void vPortExitCritical(void);
extern volatile uint32_t ulCriticalNesting;
extern volatile uint32_t ulPortPendingYield;	// a task-level yield asked for inside a critical section
extern volatile uint32_t ulPortYieldRequired;	// an interrupt handler asked for a context switch at its end

#define portDISABLE_INTERRUPTS()		vPortDisableInterrupt()
#define portENABLE_INTERRUPTS()			vPortEnableInterrupt()
#define portENTER_CRITICAL()			vPortEnterCritical()
#define portEXIT_CRITICAL()			vPortExitCritical()
#define portSET_INTERRUPT_MASK_FROM_ISR()	ulPortSaveLocalPSR()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(a)	vPortRestoreLocalPSR(a)

#define portNOP()				__asm__ volatile ("nop")
#define portMEMORY_BARRIER()			__asm__ volatile ("" : : : "memory")

#define portYIELD()					\
	do {						\
		if (ulCriticalNesting == 0) {		\
			vPortYield();			\
		} else {				\
			ulPortPendingYield = 1;		\
		}					\
	} while (0)

// From an interrupt handler: switch at the end of the common interrupt entry
#define portYIELD_FROM_ISR(x)		do { if ((x) != pdFALSE) { ulPortYieldRequired = 1; } } while (0)
#define portEND_SWITCHING_ISR(x)	portYIELD_FROM_ISR(x)

#define portTASK_FUNCTION_PROTO(vFunction, pvParameters)	void vFunction(void *pvParameters) __attribute__((noreturn))
#define portTASK_FUNCTION(vFunction, pvParameters)		void vFunction(void *pvParameters)

#ifdef __cplusplus
}
#endif

#endif
