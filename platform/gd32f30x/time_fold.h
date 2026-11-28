// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD32F30x clocks' arithmetic (time.c): the DWT cycle counter's progress folded into running microsecond and
// millisecond counts with 32-bit divides only. A 64-bit count divided on each read is a libgcc call
// (__aeabi_uldivmod, about 2 us at 120 MHz), and hal_micros runs in the DMR burst interrupt. Pure, so the host tests
// run it.
#ifndef GD32_TIME_FOLD_H
#define GD32_TIME_FOLD_H

#include <stdint.h>

typedef struct {
	uint32_t last;		// the cycle counter at the last fold
	uint32_t cyc;		// cycles not yet a whole microsecond
	uint32_t us;		// microseconds (wraps after 71 minutes, as hal_micros)
	uint32_t us_ms;		// microseconds not yet a whole millisecond
	uint32_t ms;		// milliseconds (wraps after 49 days, as hal_millis)
} time_fold_t;

static inline void time_fold_init(time_fold_t *t, uint32_t now)
{
	*t = (time_fold_t){ .last = now };
}

// The counter read now, at per_us cycles a microsecond. Called at least once per counter wrap (35 s at 120 MHz)
static inline void time_fold(time_fold_t *t, uint32_t now, uint32_t per_us)
{
	uint32_t d = now - t->last;
	t->last = now;
	// cyc < per_us before, so the sum fits unless d is near a whole wrap, which the tick's fold rules out
	uint32_t c = t->cyc + d;
	uint32_t du = c / per_us;
	t->cyc = c - du * per_us;
	t->us += du;
	uint32_t u = t->us_ms + du;
	uint32_t dm = u / 1000u;
	t->us_ms = u - dm * 1000u;
	t->ms += dm;
}

#endif
