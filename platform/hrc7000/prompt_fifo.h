// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 prompt FIFO's push for the platform's users: speaker PCM (audio_pcm.c) and APRS AFSK (hrc_afsk.c)
// (docs/spec/modem-hrc7000.md 8.4).
//
// The FIFO is two halves of one 32-sample chunk each, and AUDIO_CONTROL b31 clears as the modem moves on to play the
// other half. A chunk written in the first microseconds after that lands in the wrong half: from then on each chunk
// plays one chunk early or late, a jump at every 4 ms boundary (broadband clicks at 250 Hz), until a later push
// happens to line the halves up again. Whether a push falls there depended on how the pusher's own timing lined up
// with the modem's, so the fault came and went for seconds at a time. Pushed 0.1-3 ms after the bit cleared, every
// chunk played in order (measured). So a push waits HRC_PROMPT_SETTLE_US after it saw the bit clear: only a push that
// saw it clear writes, and the bit can only have cleared before that look
#ifndef HRC_PROMPT_FIFO_H
#define HRC_PROMPT_FIFO_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_time.h"
#include "modem/hrc7000/hrc7000.h"

#define HRC_PROMPT_SETTLE_US	200u	// twice the shortest wait measured clean; 5 % of the CPU at 250 chunks a second

// One chunk (C7_PROMPT_CHUNK samples) when the FIFO has room; false when it is full. Waits HRC_PROMPT_SETTLE_US
// before writing. Interrupt or task context
static inline bool hrc_prompt_push(hrc7000_t *m, const int16_t *s)
{
	if ((hrc7000_rd(m, C7_AUDIO_CONTROL) & C7_AUDIO_RING_FULL) != 0) {
		return false;
	}
	hal_delay_us(HRC_PROMPT_SETTLE_US);
	return hrc7000_prompt_push(m, s);
}

#endif
