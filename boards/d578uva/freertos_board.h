// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FreeRTOS settings for the AT-D578UV's Artery builds: the GD32 builds' (boards/d578uv/freertos_board.h). The heap
// there is sized for 96 KB of SRAM; this build has 128 KB (memory.ld) and keeps about the same heap, the rest left to
// features
#ifndef D578UVA_FREERTOS_BOARD_H
#define D578UVA_FREERTOS_BOARD_H

#include "../d578uv/freertos_board.h"

#if CONFIG_DMR && CONFIG_AIR
// P25 and NXDN: the modem task runs the air tap (boards/d578uv/modem_d578uv.c), and zone sharing's 1 KB send fits
// here (config.mk), unlike on the GD32 builds. The task stacks are about the GD32's, so 22.75 KB keeps about 2.2 KB
// spare beside a zone share send, above the 2 KB tests/hil/test_task_stacks.py asks for; the static RAM P25
// trunking and SNMP leave (config.mk) pays for it
#undef configTOTAL_HEAP_SIZE
#define configTOTAL_HEAP_SIZE	(23 * 1024 - 256)
#elif CONFIG_DMR
// 256 bytes less: this platform's GPS task stack is 384 bytes smaller than the GD32's (platform/at32f43x/gps_uart:
// hardware floating point), so the heap still has 128 bytes more spare than the GD32 sizing gives this build
#undef configTOTAL_HEAP_SIZE
#define configTOTAL_HEAP_SIZE	(22 * 1024 - 256)
#endif

#endif
