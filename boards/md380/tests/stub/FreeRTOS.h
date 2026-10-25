// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS.h, for md380RfbusTest only: the interrupt mask macros rfbus_md380.c uses
#ifndef MD380_STUB_FREERTOS_H
#define MD380_STUB_FREERTOS_H

#include <stdint.h>

extern int fake_irq_depth;
#define portSET_INTERRUPT_MASK_FROM_ISR()	(fake_irq_depth++, (uint32_t)0x50)
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(m)	((void)(m), fake_irq_depth--)

#endif
