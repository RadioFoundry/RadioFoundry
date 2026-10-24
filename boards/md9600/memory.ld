/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* TYT MD-9600 / RT90, STM32F405VGT6, application behind the 48 KB TYT bootloader (docs/spec/hardware-md9600.md 1.3).
 * The bootloader starts the application at 0x0800C000 like the other STM32F4 radios' bootloaders, so the vector table, stack
 * rule and start-up are the same (platform/stm32f4/common.ld).
 *
 * Codec (docs/spec/codec-md9600.md "Hosting"): the merge on the user's machine keeps the stock build's table window at
 * its stock address in sector 9 (06.09 at 0x080AB13C, 16.45 at 0x080AC2BC), puts the code at the start of sector 11
 * (0x080E0000) and the descriptor that says where at 0x080FFF80 (boards/md9600/vocoder_md9600.c). Sectors 8 and 10
 * stay free.
 *
 * The application gets sectors 3-7 (__app_start, board.mk APP_BASE, up to 0x0807FFFF: 464 KB in one piece) and never
 * links into sectors 8-11, so an update can keep the codec without an erase. The flashing tools read the application
 * region from this script (tools/boards.py). */

MEMORY
{
	FLASH   (rx)  : ORIGIN = __app_start, LENGTH = 0x08080000 - __app_start	/* sectors 3-7 */
	RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 4	/* as on the other STM32F4 radios: SP below the top of RAM */
	CCMRAM  (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

INCLUDE common.ld
