/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* Baofeng DM-1801 Pro, HR_C7000 (CK803S), application behind the Baofeng boot loader (docs/spec/hardware-dm1801pro.md 2).
 *
 * Program flash (XIP, 1 MB at 0x03000000): the boot loader 0x03000000..0x0300C000 (never written), the application
 * from 0x0300C000 (board.mk APP_BASE), the codec slot 0x030E8000..0x03100000 (docs/spec/codec-dm1801pro.md 4: the
 * radio's own codec code and tables are merged there on the user's machine; never linked into).
 *
 * SRAM (288 KB at 0x10000): RadioFoundry's region from 0x10000, and at the top the codec window 0x43A00..0x58000 (83,456 B, the
 * 049 code). The modem SRAM window the codec's tables are copied to, 0x18000EF0..0x18003E68, is reserved.
 *
 * The layout is the platform's (platform/hrc7000/common.ld). */

__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
/* The codec window (platform/hrc7000/codec_slot.h: the slot's code runs at its start; the size is the largest code
 * among the board's codec maps; tools/boards.py reads these symbols for the codec tools) and the modem SRAM table
 * window, the union of the builds' stock table addresses */
__codec_run_lo = 0x00043A00;
__codec_run_hi = 0x00058000;
__codec_tab_lo = 0x18000EF0;
__codec_tab_hi = 0x18003E68;

MEMORY
{
	FLASH  (rx)  : ORIGIN = __app_start, LENGTH = __codec_slot - __app_start
	RAM    (rwx) : ORIGIN = 0x00010000, LENGTH = __codec_run_lo - 0x00010000	/* up to the codec window */
}

INCLUDE common.ld
