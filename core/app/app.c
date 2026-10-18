// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The application: the start-up task brings up the board and starts the core tasks, then stays on as housekeeping
// (power switch watch).
//
// Tasks (FreeRTOS, configMAX_PRIORITIES 7, heap 28 KB on the DM-1701 shared by stacks and queues; stack sizes in
// words):
//   timer service   6   256   FreeRTOS
//   start / house   5   512   this file: start-up, then a 100 ms power switch poll, the settings saves and the
//                             UI's codeplug writes (storage_job_poll; sleeps nearly always)
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
#include "storage/storage.h"
#include "settings/settings.h"
#include "radio/radio.h"
#include "input/input.h"
#include "ui/ui.h"
#include "ui/ui_svc.h"
#include "config.h"
#if CONFIG_HOSTLINK
#include "hostlink/hostlink.h"
#endif
#if CONFIG_TFTP
#include "net/tftp_codeplug.h"
#endif
#if CONFIG_CPS_OPENFW
#include "cps/cps_openfw.h"
#endif
#if CONFIG_CPS_FAST
#include "cps/cps_fast.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

#define START_TASK_PRIORITY	5
// words: the radio core's set-up runs here at boot (radio_sm_init, 584 bytes of frame on the DB25-D, then the APRS
// identity load or a DMR VFO's contact search down to the flash driver); 512 left the DB25-D 88 bytes (make stack-check)
#define START_TASK_STACK	640
#define HOUSEKEEPING_MS		100
#define POWER_OFF_TX_MS		200	// after the PTT release, before the latch drops
#define POWER_OFF_DEREG_MS	3000	// at most this long for the ARS deregistration (gotcha 7.4)
#define POWER_OFF_STAY_MS	2000	// auto power off: still running this long after the latch dropped = the knob holds it

// knob: the user switched off (or the battery is flat), so the supply is going away whatever happens; otherwise this
// is the UI's auto power off, and the knob may still hold the supply: then the radio carries on and says so
static void power_off(bool knob)
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
	for (uint32_t t = 0; knob || t < POWER_OFF_STAY_MS; t += 100) {	// the supply goes away; nothing more to do
		hal_delay_ms(100);
	}
	board.power->latch(true);	// still here: the knob is on
	radio_req_power_off_cancel();
	input_inject_wake();		// the backlight comes back for the UI's banner
	ui_event_t ev = { .kind = UI_EV_POWER_OFF_FAILED };
	ui_notify(&ev);			// "Couldn't switch off"; the UI waits a whole period again
}

static void startup_task(void *arg)
{
	(void)arg;
	board_init();
	board.display->init();
	board.display->set_backlight(100);
	board.input->init();
	storage_init();			// finishes a codeplug sector rewrite a power cut interrupted
#if CONFIG_CPS_FAST
	cps_fast_resume();		// and a fast codeplug protocol commit (docs/spec/cps-fast.md 7)
#endif
#if CONFIG_TFTP
	tftp_codeplug_resume();		// and a TFTP codeplug write's commit (before the codeplug is read)
#endif
	settings_init();
	codeplug_init();
	board.radio->init();
	radio_core_start();
	input_start();
	ui_start();
#if CONFIG_HOSTLINK
	settings_t st;
	settings_read(&st);
	hostlink_set_hotspot_allowed(st.hotspot_allowed != 0);
#if CONFIG_TFTP
	tftp_codeplug_allow_write((st.net_flags & SETTINGS_NET_TFTP_NO_WRITE) == 0);
#endif
#if CONFIG_CPS_OPENFW
	cps_openfw_start();		// registers the CPS handler with the host link
#endif
	hostlink_start();
#endif	// after the settings: the host link personality depends on them

	for (;;) {
		hal_delay_ms(HOUSEKEEPING_MS);
		settings_poll(hal_millis());	// saves here, so no other task waits for the flash
		storage_job_poll();		// and the codeplug writes the UI asks for (save to channel, factory reset)
		if (!board.power->switch_on() || radio_power_off_wanted()) {
			power_off(true);	// the knob, or the radio core after a long critical battery (battery_off_s)
		} else if (ui_app_power_off_due()) {
			power_off(false);	// Settings > Power > Auto power off
		}
	}
}

void app_start(void)
{
	xTaskCreate(startup_task, "start", START_TASK_STACK, NULL, START_TASK_PRIORITY, NULL);
}
