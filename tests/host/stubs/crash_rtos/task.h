// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for task.h, for crashCaptureTest only: the running task's handle and name
#ifndef CRASH_STUB_TASK_H
#define CRASH_STUB_TASK_H

typedef void *TaskHandle_t;

TaskHandle_t xTaskGetCurrentTaskHandle(void);
char *pcTaskGetName(TaskHandle_t t);

#endif
