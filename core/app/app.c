// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The application: the start-up task brings up the board and starts the core tasks, then stays on as housekeeping
// (power switch watch).
//
// Tasks (FreeRTOS, configMAX_PRIORITIES 7, heap 28 KB on the DM-1701 shared by stacks and queues; stack sizes in
// words):
//   timer service   6   256   FreeRTOS
//   start / house   5   512   this file: start-up, then a 100 ms power switch poll (sleeps nearly always)
//   audio           5   1280  board (DMR): PCM blocks, the vocoder runs here (created at the first DMR call)
//   dmr             5   512   board (DMR): modem events to the DMR call stack's queue or the hotspot's
//   radio           4   768   core/radio: requests, 25 ms status poll, squelch, PTT, TX time-out; 10 ms DMR call
//                             stack poll (512 without DMR); the voice sink / source and the hotspot radio ops
//   input           3   256   core/input: 10 ms key scan, gestures, PTT / monitor straight to the radio core
//   host            3   512   core/hostlink: the host port (USB CDC or a UART): MMDVM hotspot, CPS
//   usb             3   256   drivers/usb: TinyUSB device stack (tud_task)
//   ui              2   -     core/ui: chosen there (keep it at or below 2 and within about 1024 words)
//   idle            0   128   FreeRTOS
// The radio task is above input so a PTT edge is acted on as soon as it is queued; input is above the UI so a busy
// redraw never delays a key scan.
#include "board.h"
#include "hal_time.h"
#include "codeplug/codeplug.h"
#include "radio/radio.h"
#include "input/input.h"
#include "ui/ui.h"
#include "config.h"
#if CONFIG_HOSTLINK
#include "hostlink/hostlink.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

#define START_TASK_PRIORITY	5
#define START_TASK_STACK	512	// words
#define HOUSEKEEPING_MS		100
#define POWER_OFF_TX_MS		200	// after the PTT release, before the latch drops

static void power_off(void)
{
	radio_req_ptt(false);		// stop any transmission first
	hal_delay_ms(POWER_OFF_TX_MS);
	board.display->set_backlight(0);
	board.power->latch(false);
	for (;;) {			// the supply goes away; nothing more to do
		hal_delay_ms(1000);
	}
}

static void startup_task(void *arg)
{
	(void)arg;
	board_init();
	board.display->init();
	board.display->set_backlight(100);
	board.input->init();
	codeplug_init();
	board.radio->init();
	radio_core_start();
	input_start();
	ui_start();
#if CONFIG_HOSTLINK
	hostlink_start();
#endif	// after the settings: the USB personality depends on them

	for (;;) {
		hal_delay_ms(HOUSEKEEPING_MS);
		if (!board.power->switch_on()) {
			power_off();
		}
	}
}

void app_start(void)
{
	xTaskCreate(startup_task, "start", START_TASK_STACK, NULL, START_TASK_PRIORITY, NULL);
}
