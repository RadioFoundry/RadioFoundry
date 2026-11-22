// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for task.h, for keyFillTest only: the scheduler suspend the emergency wipe's hook takes, which the
// test defines (it checks the hook resumes what it suspends)
#ifndef KEYFILL_STUB_TASK_H
#define KEYFILL_STUB_TASK_H

#include "FreeRTOS.h"

void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);

#endif
