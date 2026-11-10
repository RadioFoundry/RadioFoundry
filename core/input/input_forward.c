// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "config.h"
#include "input/input_forward.h"
#include "radio/radio.h"

void input_forward(const input_event_t *ev, bool ptt_blocked)
{
	if (ev->action == ACT_PTT && (ev->gesture == GESTURE_UP || (ev->gesture == GESTURE_DOWN && !ptt_blocked))) {
		radio_req_ptt(ev->gesture == GESTURE_DOWN);
#if CONFIG_FM_TX_TONES
	} else if (ev->action == ACT_TONE_BURST &&
			(ev->gesture == GESTURE_UP || (ev->gesture == GESTURE_DOWN && !ptt_blocked))) {
		radio_req_tone_burst_key(ev->gesture == GESTURE_DOWN);
#endif
	} else if (ev->action == ACT_MONITOR && (ev->gesture == GESTURE_DOWN || ev->gesture == GESTURE_UP)) {
		if (ev->gesture == GESTURE_UP && ev->value != 0) {
			radio_req_monitor_held_up();	// its hold function ran: not a press (DMR doesn't toggle)
		} else {
			radio_req_monitor(ev->gesture == GESTURE_DOWN);
		}
	}
}
