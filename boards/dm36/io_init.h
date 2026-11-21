// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef BOARD_IO_INIT_H
#define BOARD_IO_INIT_H

// hardware-dm36.md 4.1: latch output levels before applying the final directions.
static void board_io_init(void)
{
	IO_DIPLEX1 = 0xFFEFFE07u;
	hrc_pin_out(0x19, false);
	hrc_pin_out(0x1A, false);
	hrc_pin_out(0x1D, true);
	hrc_pin_out(0x38, false);
	hrc_pin_out(0x39, true);
	hrc_pin_out(0x3D, true);
	hrc_pin_out(0x52, false);
	hrc_pin_in(0x25);
	hrc_pin_in(0x29);
	hrc_pin_in(0x36);
	REG32(GPIOA_BASE + GPIO_DDR) = 0x270604E0u;
	REG32(GPIOB_BASE + GPIO_DDR) = 0x23A0D01Fu;
	REG32(GPIOC_BASE + GPIO_DDR) = 0x00047FC4u;
}
#endif
