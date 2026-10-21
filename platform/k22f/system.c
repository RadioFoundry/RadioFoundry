// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The MCU for the debug link and the core (hal_system.h): chip identity from the SIM, memory reads, reset. The K22F
// has no ROM bootloader to reboot into, so HAL_REBOOT_ROM_DFU is refused; there is no runtime stub entry yet
#include <string.h>
#include "platform.h"

// SIM_FCFG1 PFSIZE codes to KB (reference manual 12.2.16); 0 for a code this part family doesn't use
static uint16_t flash_kb(uint32_t pfsize)
{
	switch (pfsize) {
	case 0x7u: return 128u;
	case 0x9u: return 256u;
	case 0xBu: return 512u;
	default: return 0u;
	}
}

static bool chip(hal_chip_t *c)
{
	c->idcode = SIM_SDID;
	c->flash_kb = flash_kb(SIM_FCFG1_PFSIZE(SIM_FCFG1));
	c->uid[0] = SIM_UIDL;
	c->uid[1] = SIM_UIDML;
	c->uid[2] = SIM_UIDMH;
	c->optcr = (uint32_t)FTFA_FSEC | ((uint32_t)FTFA_FOPT << 8);	// flash security and option bytes
	return true;
}

static void read_mem(uint32_t addr, void *dst, size_t n)
{
	memcpy(dst, (const void *)(uintptr_t)addr, n);
}

void k22f_reset(void)
{
	__disable_irq();
	NVIC_SystemReset();
}

static bool reboot(uint8_t mode)
{
	if (mode != HAL_REBOOT_NORMAL) {
		return false;
	}
	k22f_reset();
}

const hal_system_t k22f_system = {
	.chip = chip,
	.read = read_mem,
	.reboot = reboot,
};
