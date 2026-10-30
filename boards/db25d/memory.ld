/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* Radioddity DB25-D with DMR (CONFIG_DMR=y; without it config.mk selects fm_only.ld), HR_C7000 (CK803S),
 * application behind the vendor IAP loader and RadioFoundry's first stage (docs/spec/hardware-db25d.md 2, 7).
 *
 * Program flash (XIP, 2 MB at 0x03000000): the loader 0x03000000..0x0300C000 (never written), RadioFoundry's first stage
 * 0x0300C000..0x0300D000 (board.mk APP_BASE, BOOT_SIZE), the application from 0x0300D000, the codec slot
 * 0x030E8000..0x03100000 (hardware-db25d.md 8: the stock codec code and tables are merged there on the user's machine;
 * never linked into).
 *
 * SRAM (288 KB at 0x10000): RadioFoundry's region from 0x10000, and at the top the codec window 0x44E00..0x58000 (78,336 B: the
 * 020 code, 78,172 B, the largest of the supported builds). The runtime stub entry (CONFIG_STUB_ENTRY) loads the stub
 * into the codec window (HRC_STUB_BASE in config.mk) after the codec is parked. The modem SRAM window the codec's
 * tables are copied to, 0x180010B4..0x18004360 (the four 018 builds and 020 put the same tables at five addresses, the
 * 2023-07-06 build's the lowest; the tables don't move with the code, hardware-db25d.md 8.4), is reserved.
 *
 * The layout is the platform's (platform/hrc7000/common.ld). */

__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
/* The codec window (platform/hrc7000/codec_slot.h: the slot's code runs at its start; the size is the largest code
 * among the board's codec maps; tools/boards.py reads these symbols for the codec tools) and the modem SRAM table
 * window, the union of the builds' stock table addresses */
__codec_run_lo = 0x00044E00;
__codec_run_hi = 0x00058000;
__codec_tab_lo = 0x180010B4;
__codec_tab_hi = 0x18004360;

MEMORY
{
	FLASH  (rx)  : ORIGIN = __app_start, LENGTH = __codec_slot - __app_start
	RAM    (rwx) : ORIGIN = 0x00010000, LENGTH = __codec_run_lo - 0x00010000	/* up to the codec window */
}

INCLUDE common.ld
