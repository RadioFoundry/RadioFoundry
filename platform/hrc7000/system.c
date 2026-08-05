// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// hal_system_t for the HR_C7000: memory reads and reboot. No chip identity registers are known (the eFuse behind SPI
// master 3 is not mapped out), so chip() reports nothing.
#include <string.h>
#include "platform.h"

#define SOFT_RESET_SPINS	1000000u

static bool chip(hal_chip_t *c)
{
	(void)c;
	return false;
}

static void rd(uint32_t addr, void *dst, size_t n)
{
	memcpy(dst, (const void *)(uintptr_t)addr, n);
}

HRC_RAMFUNC void hrc_soft_reset(void)
{
	__asm__ volatile ("psrclr ie");
	QUAD_ENABLE = 0;	// XIP controller back to its reset mode, so the ROM and bootloader can read flash
	SYS_SOFT_RSTN = 0;
	for (volatile uint32_t i = 0; i < SOFT_RESET_SPINS; i++) {
	}
	for (;;) {	// did not reset: the battery has to come out
	}
}

// Only a normal reset: the ROM serial loader needs an FTDI cable and Dahua's tool (hardware-dm32.md 13.3), and the
// Baofeng update mode needs PTT + SK1 held at power-on, which software can't do
static bool reboot(uint8_t mode)
{
	if (mode != HAL_REBOOT_NORMAL) {
		return false;
	}
	hrc_soft_reset();
}

const hal_system_t hrc_system = { .chip = chip, .read = rd, .reboot = reboot };
