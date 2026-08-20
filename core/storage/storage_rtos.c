// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The storage locks on FreeRTOS: recursive mutexes, created on first use (storage_init, from the start-up task
// before the other tasks exist). Before the scheduler runs the lock is a no-op
#include "storage/storage.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static SemaphoreHandle_t lock;
static SemaphoreHandle_t quick;

static bool running(void)
{
	return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

void storage_lock(void)
{
	if (!running()) {
		return;
	}
	if (lock == NULL) {
		lock = xSemaphoreCreateRecursiveMutex();	// first call: storage_init from the start-up task, alone then
	}
	if (lock != NULL) {
		xSemaphoreTakeRecursive(lock, portMAX_DELAY);
	}
}

void storage_unlock(void)
{
	if (running() && lock != NULL) {
		xSemaphoreGiveRecursive(lock);
	}
}

void storage_quick_lock(void)
{
	if (!running()) {
		return;
	}
	if (quick == NULL) {
		quick = xSemaphoreCreateRecursiveMutex();	// first call: settings_init from the start-up task
	}
	if (quick != NULL) {
		xSemaphoreTakeRecursive(quick, portMAX_DELAY);
	}
}

void storage_quick_unlock(void)
{
	if (running() && quick != NULL) {
		xSemaphoreGiveRecursive(quick);
	}
}
