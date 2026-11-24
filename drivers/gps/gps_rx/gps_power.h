// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When a GPS module is powered, the part every board shares (hal_gps_t.enable): the module is powered and listened
// to at start-up whatever Settings > Location > GPS says, so the radio knows whether it has one. Once it has been heard
// it follows the setting: the board's GPS task switches its supply and receive path within one poll. Never heard: off
// for good. Generic and pure: no UART, no RTOS. The setting may come from any task; everything else is the GPS task's.
#ifndef GPS_POWER_H
#define GPS_POWER_H

#include <stdbool.h>

typedef struct {
	volatile bool off_wanted;	// the setting is off (gps_power_set); zero = on, so it holds from before init
	bool present;			// a module was heard
	bool on;			// powered and listened to: the probe at start-up, then the setting
	bool gone;			// nothing was heard: never switched on again
} gps_power_t;

// Start of the probe: the board has just powered the module and its receive path. Keeps the setting
void gps_power_init(gps_power_t *p);
// Settings > Location > GPS, from any task: takes effect at the GPS task's next gps_power_step
void gps_power_set(gps_power_t *p, bool on);
// The probe heard a module
void gps_power_found(gps_power_t *p);
// The probe heard nothing: the board has switched everything off
void gps_power_absent(gps_power_t *p);
// After each poll: true when the board must switch the module and its receive path to p->on now
bool gps_power_step(gps_power_t *p);

#endif
