/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* Retevis RT10, HR_C7000 (CK803S), started by the boot ROM (docs/spec/hardware-rt10.md 2).
 *
 * Program flash (XIP, 2 MB at 0x03000000): the boot ROM's flag sector at 0x03000000 (its entry word names
 * 0x0300C000), 0x03001000..0x0300C000 unused, the application from 0x0300C000 (board.mk APP_BASE), the codec slot
 * 0x030E8000..0x03100000 as on the other HR_C7000 radios (never linked into).
 *
 * SRAM (288 KB at 0x10000): RadioFoundry's region from 0x10000 and the codec window 0x43A00..0x58000 at the top. The
 * runtime stub (CONFIG_STUB_ENTRY) loads into that window (platform.h HRC_STUB_BASE). The modem SRAM window the stock
 * build copies its codec tables to, 0x1800420C..0x18007184 (hardware-rt10.md 8), is reserved.
 *
 * The layout is the platform's (platform/hrc7000/common.ld). */

__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
__codec_run_lo = 0x00043A00;
__codec_run_hi = 0x00058000;
__codec_tab_lo = 0x1800420C;
__codec_tab_hi = 0x18007184;

MEMORY
{
	FLASH  (rx)  : ORIGIN = __app_start, LENGTH = __codec_slot - __app_start
	RAM    (rwx) : ORIGIN = 0x00010000, LENGTH = __codec_run_lo - 0x00010000	/* up to the codec window */
}

INCLUDE common.ld
