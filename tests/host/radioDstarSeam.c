// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// core/radio/radio_dstar.c as it ships, built for radioDstarTest with one seam: its memset calls go through
// dstar_seam_memset, which the test defines and which zeroes the bytes as memset does. On the radio the audio task and
// the transmit interrupt preempt the debug link's task part way through a write; on the host nothing preempts it, so
// the test runs their part from inside the write instead.
//
// <string.h> is included first, so the module's own include of it is skipped and only its calls are renamed. Some C
// libraries define memset as a macro (fortified builds); the seam replaces that too.
#include <stddef.h>
#include <string.h>

void *dstar_seam_memset(void *dst, int c, size_t n);
#undef memset
#define memset dstar_seam_memset
#include "../../core/radio/radio_dstar.c"
