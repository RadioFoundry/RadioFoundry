// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dlog(): printf-style log lines into a small RAM ring that the debug link reads (GET_LOG, docs/spec/debuglink.md).
// Cheap: formats with dl_fmt (no libc printf) into a line on the caller's stack (DLOG_LINE_MAX), then copies it into
// the ring under a short critical section. From tasks only, never from an interrupt. Each line gets the ms clock.
// When the ring is full the oldest lines are dropped (counted). Built without CONFIG_DLOG (the debug link, or a second
// SoC's server, core/config.h) it compiles to nothing, so firmware code may call it unconditionally.
#ifndef DLOG_H
#define DLOG_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"

#define DLOG_LINE_MAX	80	// characters per line, longer lines are cut
#define DLOG_RING_BYTES	2048

#if CONFIG_DLOG
void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Take whole lines, oldest first, as "<ms> text\n", into buf (at most cap bytes). Returns the bytes taken; *more is
// set when lines remain. *dropped: lines lost to overflow since the last read (then zeroed)
size_t dlog_read(char *buf, size_t cap, uint8_t *more, uint32_t *dropped);
#else
#define dlog(...)	((void)0)
#endif

#endif
