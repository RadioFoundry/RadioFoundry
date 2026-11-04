// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000's READ_MEM windows (docs/spec/debuglink.md 3.10), no hardware access (host tested). Memory reads have no
// side effects: main SRAM, the SRAM shared with the modem, the program flash through XIP. Register blocks only where
// every register is plain read-write or read-only, with no read-to-clear bit and no FIFO (hardware-dm32.md 3, 4.1):
// the system block's clock and IO mux registers, the three GPIO banks (DesignWare APB GPIO: EOI is write-only) and
// the PIC (its request record clears on a write of 1, not on a read). Never the timers or watchdog (their EOI
// registers clear on read), UARTs, SPI, I2C, the modem registers or its buffer RAM (FIFOs, read-to-clear status).
#ifndef HRC_MEM_WINDOW_H
#define HRC_MEM_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_system.h"

// The program flash the XIP window maps: 1 MB unless the board says (board.mk)
#ifndef HRC_XIP_FLASH_SIZE
#define HRC_XIP_FLASH_SIZE	0x100000u
#endif

static inline bool hrc_mem_window_for(unsigned i, hal_mem_window_t *w, uint32_t flash_size)
{
	static const hal_mem_window_t fixed[] = {
		{ 0x00010000u, 0x00057FFFu, false },	// main SRAM, 288 KB: ours, the codec window, the stub window
		{ 0x18000000u, 0x18007FFFu, false },	// SRAM shared with the modem: PCM buffers, the codec's tables
		{ 0x11000000u, 0x1100005Fu, true },	// system block: reset, clock manager, IO mux, XIP mode
		{ 0x14020000u, 0x1402007Fu, true },	// GPIO A
		{ 0x14100000u, 0x1410007Fu, true },	// GPIO B
		{ 0x14110000u, 0x1411007Fu, true },	// GPIO C
		// PIC, only the registers hardware-dm32.md 4.1 documents (what the gaps hold is unknown): mode, polarity,
		// mask, vector, EOI, priorities; fast select; the request record; the _1 set
		{ 0x17000000u, 0x17000033u, true },
		{ 0x1700003Cu, 0x1700003Fu, true },
		{ 0x17000044u, 0x1700004Bu, true },
		{ 0x17000060u, 0x1700008Fu, true },
	};
	if (i == 0) {
		*w = (hal_mem_window_t){ 0x03000000u, 0x03000000u + flash_size - 1u, false };	// program flash (XIP)
		return true;
	}
	if (i - 1u < sizeof(fixed) / sizeof(fixed[0])) {
		*w = fixed[i - 1u];
		return true;
	}
	return false;
}

#endif
