// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef FREERTOS_BOARD_H
#define FREERTOS_BOARD_H
// The inherited 32.5 KiB less the 1536-byte GPS task (no GPS) and 2048 bytes once counted as a broadcast receive ring.
// The broadcast receiver takes nothing from the heap: that 2 KiB ring is APRS's FM receive ring (fm_rx_audio.c), so
// the spare is about 2.2 KiB at the most rather than the inherited 4.2 KiB; tasks.heap_min_free shows it on the radio
#define configTOTAL_HEAP_SIZE (29 * 1024 + (CONFIG_GPS ? 1536 : 0))
#endif
