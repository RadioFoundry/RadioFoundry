// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The application: start-up task, then the radio and UI tasks
#include "board.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

static void startup_task(void *arg)
{
	(void)arg;
	board_init();
	board.display->init();
	board.display->set_backlight(100);
	board.input->init();
	board.radio->init();
	for (;;) {
		board.power->led(LED_GREEN, true);
		hal_delay_ms(500);
		board.power->led(LED_GREEN, false);
		hal_delay_ms(500);
	}
}

void app_start(void)
{
	xTaskCreate(startup_task, "start", 1024, NULL, 2, NULL);
}
