// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// core/radio/radio_ysf.c as it ships, built for ysfRadioTest with one seam: its reads of the engine's receiver go
// through ysf_seam_receiver, which the test defines. On the radio the debug link's task can be preempted by the radio
// task between two statements of radio_ysf_stats; on the host nothing can preempt it, so the test runs the radio task's
// part (a stop and D-STAR taking the engine area) from inside that read instead.
//
// The engine header is included first, so its accessor keeps its own name for the test to call; the module's own
// include of it is then skipped by the guard, and its calls are the only ones the macro renames.
#include "ysf/ysf_engine.h"

const ysf_rx_t *ysf_seam_receiver(const ysf_engine_t *e);
#define ysf_engine_receiver ysf_seam_receiver
#include "../../core/radio/radio_ysf.c"
