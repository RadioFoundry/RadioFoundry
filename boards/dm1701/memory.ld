/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* Baofeng DM-1701, STM32F405VG, application behind the stock bootloader (docs/spec/hardware-dm1701.md 1.4, 1.5).
 * Codec (docs/spec/codec-placement.md, "STM32 radios"): the merge on the user's machine puts the stock V02.06 CSV
 * codec's tables at their stock address 0x080D19F0..0x080E3A72 (sectors 10 and 11), its code right after them at
 * 0x080E3A74, and the descriptor that says where at 0x080FFF80. The code runs in place. V02.01 to V02.03 CSV put the
 * same codec 0x40 lower (tables from 0x080D19B0, code at 0x080E3A34), in the same two sectors.
 *
 * The application gets sectors 3-9 (__app_start, board.mk APP_BASE, up to 0x080BFFFF: 720 KB in one piece) and never
 * links into sectors 10 and 11. The layout is the platform's (platform/stm32f4/common.ld). */

MEMORY
{
	FLASH   (rx)  : ORIGIN = __app_start, LENGTH = 0x080C0000 - __app_start	/* sectors 3-9 */
	RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 4	/* the bootloader rejects SP = top of RAM */
	CCMRAM  (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

INCLUDE common.ld
