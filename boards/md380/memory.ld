/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* TYT MD-380 / MD-390 / RT3, STM32F405VGT6, application behind the 48 KB Tytera bootloader (docs/spec/hardware-md380.md
 * 1.3, 13). The bootloader starts the application at 0x0800C000 like the other STM32 radios', so the vector table, stack rule and
 * start-up are the same (platform/stm32f4/common.ld).
 *
 * Codec (docs/spec/codec-placement.md, "STM32 radios"): the merge on the user's machine puts the stock D002.032
 * codec's main table window at its stock address 0x0808FC64..0x0809AC5B (sector 8), its four small table windows at
 * theirs (sectors 10 and 11), its code at 0x080E0000 (sector 11), and the descriptor that says where at 0x080FFF80.
 * The code runs in place.
 *
 * The application gets sectors 3-7 (__app_start, board.mk APP_BASE, up to 0x0807FFFF: 464 KB in one piece) and never
 * links into sectors 8-11 (sector 9 holds nothing yet). The flashing tools read the application region from this
 * script (tools/boards.py); it and the profile in tools/odrflash/odrflash/stockfw.py change together. */

MEMORY
{
	FLASH   (rx)  : ORIGIN = __app_start, LENGTH = 0x08080000 - __app_start	/* sectors 3-7 */
	RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 4	/* as on the other STM32 radios: SP below the top of RAM */
	CCMRAM  (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

INCLUDE common.ld
