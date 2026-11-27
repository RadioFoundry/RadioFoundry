/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* APP_BASE is 0x0300C000; the 48 KiB stock bootloader stays outside FLASH.
 * Codec windows (hardware-dm36.md 2.1): the tables are copied to their stock address, 0x18000EFC..0x18003E74, so the
 * table window reaches that end; the code runs in the inherited run window. */
__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
__codec_run_lo = 0x00043A00;
__codec_run_hi = 0x00058000;
__codec_tab_lo = 0x18000E48;
__codec_tab_hi = 0x18003E74;

MEMORY
{
	FLASH  (rx)  : ORIGIN = __app_start, LENGTH = __codec_slot - __app_start
	RAM    (rwx) : ORIGIN = 0x00010000, LENGTH = __codec_run_lo - 0x00010000	/* up to the codec window */
}

/* Keep cold state in main RAM until the modem SRAM is checked. */
REGION_ALIAS("MODEM_BSS", RAM);

INCLUDE common.ld
