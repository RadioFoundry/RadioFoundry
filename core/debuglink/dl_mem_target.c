// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_MEM on the radio (dl_mem.h): volatile accesses of the asked width, and the platform's pin hook
// (hal_system_t.pin)
#include "config.h"
#include "debuglink/dl_mem.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_frame.h"

#if CONFIG_DEBUG_MEM
#include "board.h"
#include "hal_time.h"

static uint32_t rd(uint32_t addr, uint8_t width)
{
	uintptr_t a = (uintptr_t)addr;
	return width == 1u ? *(volatile uint8_t *)a : width == 2u ? *(volatile uint16_t *)a : *(volatile uint32_t *)a;
}

static void wr(uint32_t addr, uint8_t width, uint32_t v)
{
	uintptr_t a = (uintptr_t)addr;
	if (width == 1u) {
		*(volatile uint8_t *)a = (uint8_t)v;
	} else if (width == 2u) {
		*(volatile uint16_t *)a = (uint16_t)v;
	} else {
		*(volatile uint32_t *)a = v;
	}
}

size_t debuglink_cmd_mem(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	dl_mem_io_t io = { rd, wr, board.system != NULL ? board.system->pin : NULL };
	return dl_mem_cmd(&io, req, len, out, cap, hal_millis());
}
#else
size_t debuglink_cmd_mem(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}
#endif
