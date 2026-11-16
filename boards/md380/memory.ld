/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* TYT MD-380 / MD-390 / RT3, STM32F405VGT6, application behind the 48 KB Tytera bootloader (docs/spec/hardware-md380.md
 * 1.3, 13). The bootloader starts the application at 0x0800C000 like the other STM32 radios', so the vector table, stack rule and
 * start-up are the same (platform/stm32f4/common.ld).
 *
 * Codec (docs/spec/codec-placement.md, "STM32 radios" and "Layouts"): the merge on the user's machine puts the whole
 * stock codec in sector 11 (0x080E0000..0x080FFFFF): its code, its table windows, each moved as one block, and the
 * descriptor that says where at 0x080FFF80. The code runs in place.
 *
 * The application gets sectors 3-10 (__app_start, board.mk APP_BASE, up to 0x080DFFFF: 848 KB in one piece) and never
 * links into sector 11. Releases before this layout kept the codec in sectors 8-11 and gave the application 3-7; the
 * flashing tools move a radio's codec from there on the first install of this layout. They read the application
 * region from this script (tools/boards.py); it and the profiles in tools/rfoundry/rfoundry/stockfw.py change
 * together. */

MEMORY
{
	FLASH   (rx)  : ORIGIN = __app_start, LENGTH = 0x080E0000 - __app_start	/* sectors 3-10 */
	RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 4	/* as on the other STM32 radios: SP below the top of RAM */
	CCMRAM  (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

INCLUDE common.ld
