// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The storage locks on FreeRTOS: recursive mutexes, created on first use (storage_init, from the start-up task
// before the other tasks exist). Before the scheduler runs the lock is a no-op.
//
// A long run of writes (a zone import, in the start task's storage job at priority 5) takes and gives the lock once
// per record. A give only makes a lower-priority waiter ready; the writer keeps the CPU and takes the lock again
// before the waiter runs, so the waiter (the host link's SNMP poll, the UI reading a channel name) would wait for the
// whole run. The last give therefore yields a tick when someone is waiting.
#include "storage/storage.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static SemaphoreHandle_t lock;
static SemaphoreHandle_t quick;
static volatile uint32_t waiting;	// tasks in storage_lock's take
static uint32_t depth;			// the holder's recursion depth (only the holder touches it)

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
		taskENTER_CRITICAL();
		waiting++;
		taskEXIT_CRITICAL();
		xSemaphoreTakeRecursive(lock, portMAX_DELAY);
		taskENTER_CRITICAL();
		waiting--;
		taskEXIT_CRITICAL();
		depth++;
	}
}

void storage_unlock(void)
{
	if (running() && lock != NULL) {
		bool last = --depth == 0;
		xSemaphoreGiveRecursive(lock);
		if (last && waiting != 0) {
			vTaskDelay(1);	// the waiter takes it now (see the top)
		}
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
