// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stdio.h>
#include "board.h"
#include "config.h"
#include "radio/radio_am.h"
static const hal_audio_tap_t tap = { .caps = AUDIO_TAP_RX_ENVELOPE };
const board_t board = { .audio_tap = &tap };
int main(void)
{
	int failures = 0;
	channel_t ch = { .mode = RADIO_MODE_FM, .rx_hz = 133475000u };
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
	CHECK(radio_am_display_channel(&ch) == (CONFIG_AM_ENVELOPE != 0));
	CHECK(!radio_am_capable() && !radio_am_channel(&ch)); // label must not select a missing native AM driver
	ch.rx_hz = 117999999u; CHECK(!radio_am_display_channel(&ch));
	ch.rx_hz = 136000000u; CHECK(!radio_am_display_channel(&ch));
	ch.rx_hz = 135999999u; CHECK(radio_am_display_channel(&ch) == (CONFIG_AM_ENVELOPE != 0));
	ch.rx_hz = 145500000u; CHECK(!radio_am_display_channel(&ch));
	ch.rx_hz = 453387500u; CHECK(!radio_am_display_channel(&ch));
	ch.rx_hz = 124100000u; ch.mode = RADIO_MODE_DMR; CHECK(!radio_am_display_channel(&ch));
	ch.mode = RADIO_MODE_P25; CHECK(!radio_am_display_channel(&ch));
	printf("envelopeLabelTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
