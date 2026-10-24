/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Radioddity GD-77, MK22FN512 (512 KB flash, 128 KB SRAM), application behind the radio's bootloader
 * (docs/spec/hardware-gd77.md 6.1). The sections are the platform's (platform/k22f/common.ld).
 *
 * Program flash: the bootloader owns 0x00000..0x03FFF and is never written. The application starts at 0x04000
 * (__app_start, board.mk APP_BASE) with its vector table first. The last 2 KB sector, 0x7F800..0x7FFFF, holds radio
 * data that must survive and is never written. The application ends at 0x55D00: above it the flash is the stock
 * AMBE+2 codec's, which the merge on the user's machine writes (docs/spec/codec-gd77.md): its code at
 * 0x55D00..0x67C57, its table window somewhere in 0x67C58..0x7B5F7 (it depends on the stock build) and a descriptor
 * at 0x7B700. The application never links past 0x55D00.
 *
 * The main stack starts at 0x2000FEF0: the top 272 bytes of SRAM stay unused, a margin for anything the bootloader
 * keeps there (unverified).
 *
 * __codec_slot..__codec_slot_end: the codec's area, which an image merged with the codec fills up to the descriptor's
 * end (0x7B780) and whose sectors the flashing tools erase only to write a codec (tools/boards.py). */

__codec_slot = 0x00055D00;
__codec_slot_end = 0x0007B800;

MEMORY
{
	FLASH   (rx)  : ORIGIN = __app_start, LENGTH = __codec_slot - __app_start
	SRAM_L  (xrw) : ORIGIN = 0x1FFF0000, LENGTH = 64K
	SRAM_U  (rw)  : ORIGIN = 0x20000000, LENGTH = 0x2000FEF0 - 0x20000000
}

INCLUDE common.ld
