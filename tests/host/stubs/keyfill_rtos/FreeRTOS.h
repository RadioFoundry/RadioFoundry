// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for FreeRTOS.h, for keyFillTest only: the heap calls core/ui/ui_svc_keyfill.c makes, which the test
// defines (it watches what is allocated and freed)
#ifndef KEYFILL_STUB_FREERTOS_H
#define KEYFILL_STUB_FREERTOS_H

#include <stddef.h>

typedef long BaseType_t;
void *pvPortMalloc(size_t n);
void vPortFree(void *p);

#endif
