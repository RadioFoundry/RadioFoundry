/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* TYT MD-9600 / RT90, STM32F405VGT6, application behind the 48 KB TYT bootloader (docs/spec/hardware-md9600.md 1.3).
 * The bootloader starts the application at 0x0800C000 like the other STM32F4 radios' bootloaders, so the vector table, stack
 * rule and start-up are the same (platform/stm32f4/common.ld).
 *
 * Codec (docs/spec/codec-md9600.md "Hosting", docs/spec/codec-placement.md "Layouts"): the merge on the user's machine
 * puts the stock build's table window, moved as one block, at the start of sector 10 (0x080C0000 plus its stock address
 * modulo 16), the code at the start of sector 11 (0x080E0000) and the descriptor that says where at 0x080FFF80
 * (boards/md9600/vocoder_md9600.c).
 *
 * The application gets sectors 3-9 (__app_start, board.mk APP_BASE, up to 0x080BFFFF: 720 KB in one piece) and never
 * links into sectors 10-11, so an update can keep the codec without an erase. Releases before this layout kept the
 * tables in sector 9 and gave the application 3-8; the flashing tools move a radio's codec from there on the first
 * install of this layout. They read the application region from this script (tools/boards.py). */

MEMORY
{
	FLASH   (rx)  : ORIGIN = __app_start, LENGTH = 0x080C0000 - __app_start	/* sectors 3-9 */
	RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 4	/* as on the other STM32F4 radios: SP below the top of RAM */
	CCMRAM  (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

INCLUDE common.ld
