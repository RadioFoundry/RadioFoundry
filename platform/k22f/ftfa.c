// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Program flash self-programming: not implemented yet. The K22F's flash controller runs a command while the same
// flash block can't be read, so the command loop has to run from RAM with interrupts off, and the last 2 KB sector of
// the GD-77's flash holds radio data that must survive (docs/spec/hardware-gd77.md 6). Until that is done and checked
// on a radio, both calls refuse.
#include "platform.h"

k22f_status_t k22f_flash_erase_sector(uint32_t addr)
{
	(void)addr;
	return K22F_ERR_NOT_IMPLEMENTED;
}

k22f_status_t k22f_flash_program(uint32_t addr, const void *data, uint32_t len)
{
	(void)addr;
	(void)data;
	(void)len;
	return K22F_ERR_NOT_IMPLEMENTED;
}
