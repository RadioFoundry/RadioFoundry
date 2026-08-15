// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The application: the start-up task brings up the board and starts the core tasks, then stays on as housekeeping
// (power switch watch).
//
// Tasks (FreeRTOS, configMAX_PRIORITIES 7, heap 28 KB on the DM-1701 shared by stacks and queues; stack sizes in
// words):
//   timer service   6   256   FreeRTOS
//   start / house   5   512   this file: start-up, then a 100 ms power switch poll and the settings saves
//                             (sleeps nearly always)
//   audio           5   1280  board (DMR): PCM blocks, the vocoder runs here (created at the first DMR call)
//   dmr             5   512   board (DMR): modem events to the DMR call stack's queue or the hotspot's
//   radio           4   768   core/radio: requests, 25 ms status poll, squelch, PTT, TX time-out; 10 ms DMR call
//                             stack poll (512 without DMR); the voice sink / source and the hotspot radio ops
//   input           3   256   core/input: 10 ms key scan, gestures, PTT / monitor straight to the radio core
//   host            3   512   core/hostlink: the host port (USB CDC): MMDVM hotspot, CPS
//   usb             3   256   drivers/usb: TinyUSB device stack (tud_task)
//   ui              2   -     core/ui: chosen there (keep it at or below 2 and within about 1024 words)
//   idle            0   128   FreeRTOS
// The radio task is above input so a PTT edge is acted on as soon as it is queued; input is above the UI so a busy
// redraw never delays a key scan.
#include "board.h"
#include "hal_time.h"
#include "codeplug/codeplug.h"
#include "storage/storage.h"
#include "settings/settings.h"
#include "radio/radio.h"
#include "input/input.h"
#include "ui/ui.h"
#include "config.h"
#if CONFIG_USB
#include "hostlink/hostlink.h"
#endif
#if CONFIG_CPS_LEGACY
#include "cps/cps_legacy.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

#define START_TASK_PRIORITY	5
#define START_TASK_STACK	512	// words
#define HOUSEKEEPING_MS		100
#define POWER_OFF_TX_MS		200	// after the PTT release, before the latch drops
#define POWER_OFF_DEREG_MS	3000	// at most this long for the ARS deregistration (gotcha 7.4)

static void power_off(void)
{
	radio_req_ptt(false);		// stop any transmission first
	radio_req_power_off();		// then what has to go out before the supply drops (ARS deregistration)
	hal_delay_ms(POWER_OFF_TX_MS);
	for (unsigned t = 0; t < POWER_OFF_DEREG_MS && radio_power_off_busy(); t += 50) {
		hal_delay_ms(50);
	}
	settings_flush();		// unsaved changes (the debounce hadn't run out yet)
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
	storage_init();			// finishes a codeplug sector rewrite a power cut interrupted
	settings_init();
	codeplug_init();
	board.radio->init();
	radio_core_start();
	input_start();
	ui_start();
#if CONFIG_USB
	settings_t st;
	settings_read(&st);
	hostlink_set_hotspot_allowed(st.hotspot_allowed != 0);
#if CONFIG_CPS_LEGACY
	cps_legacy_start();		// registers the CPS handler with the host link
#endif
	hostlink_start();
#endif	// after the settings: the USB personality depends on them

	for (;;) {
		hal_delay_ms(HOUSEKEEPING_MS);
		settings_poll(hal_millis());	// saves here, so no other task waits for the flash
		if (!board.power->switch_on() || radio_power_off_wanted()) {
			power_off();	// the knob, or the radio core after a long critical battery (battery_off_s)
		}
	}
}

void app_start(void)
{
	xTaskCreate(startup_task, "start", START_TASK_STACK, NULL, START_TASK_PRIORITY, NULL);
}
