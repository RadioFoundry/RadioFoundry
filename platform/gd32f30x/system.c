// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD32F30x system services (hal_system.h): chip identity, memory reads, reboot. No ROM bootloader entry from software
// yet: the vendor bootloader's own update mode is the way back (the board's hardware page)
#include <string.h>
#include "config.h"
#include "platform.h"
#include "board.h"
#include "hal_system.h"
#if CONFIG_CRASH_LOG
#include "hal_crash.h"
#endif

static bool chip(hal_chip_t *c)
{
	memset(c, 0, sizeof(*c));
	c->flash_kb = GD32_FLASH_SIZE_KB;
	c->uid[0] = *(volatile const uint32_t *)(GD32_UID_BASE + 0u);
	c->uid[1] = *(volatile const uint32_t *)(GD32_UID_BASE + 4u);
	c->uid[2] = *(volatile const uint32_t *)(GD32_UID_BASE + 8u);
	c->family = HAL_CHIP_GD32F30X;
	return true;
}

static void read(uint32_t addr, void *dst, size_t n)
{
	memcpy(dst, (const void *)(uintptr_t)addr, n);
}

static bool reboot(uint8_t mode)
{
	if (mode != HAL_REBOOT_NORMAL) {
		return false;
	}
#if CONFIG_CRASH_LOG
	hal_crash_soft_mark();
#endif
	__disable_irq();
	board_rf_safe();
	NVIC_SystemReset();
	return false;
}

const hal_system_t gd32_system = {
	.chip = chip,
	.read = read,
	.reboot = reboot,
#if CONFIG_STUB_ENTRY
	// stub_entry.c: no span of its own (stub_region, enter_stub NULL), staged in the frame buffer instead
	.stub_run_addr = 0x20000000u,			// the SRAM base, GD32STUB_RUN_ADDR
	.stub_image_ok = gd32_stub_image_ok,
	.enter_stub_staged = gd32_enter_stub_staged,
#endif
};
