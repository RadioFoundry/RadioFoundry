/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* TYT MD-UV380, STM32F405VG, application behind the stock bootloader (docs/spec/hardware-mduv380.md 11, 12; the layout
 * of hardware-dm1701.md 1.4, 1.5). Codec (docs/spec/codec-placement.md, "STM32 radios"; docs/spec/codec-mduv380.md): the
 * merge on the user's machine puts the codec's tables at their address in the TYT image (sectors 10 and 11), its code
 * right after them and the descriptor that says where at 0x080FFF80. The code runs in place.
 *
 * The application gets sectors 3-9 (__app_start, board.mk APP_BASE, up to 0x080BFFFF: 720 KB in one piece) and never
 * links into sectors 10 and 11. The layout is the platform's (platform/stm32f4/common.ld). */

MEMORY
{
	FLASH   (rx)  : ORIGIN = __app_start, LENGTH = 0x080C0000 - __app_start	/* sectors 3-9 */
	RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 4	/* the bootloader rejects SP = top of RAM (hardware-dm1701.md 1.5) */
	CCMRAM  (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

INCLUDE common.ld
