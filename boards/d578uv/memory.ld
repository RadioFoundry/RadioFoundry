/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* AnyTone AT-D578UV, GD32F303VK (3 MB flash, 96 KB SRAM), application behind the vendor bootloader
 * (docs/spec/hardware-d578uv.md 2). The bootloader holds 0x08000000..0x08005FFF and starts the image at 0x08006000
 * (board.mk APP_BASE). The stock application ends below 0x08100000; RadioFoundry's image stays below the modem boot
 * block slot (0x080FF000..0x08100000), where the boot block the DMR baseband needs is merged from the user's stock
 * file on their machine (hardware-d578uv.md 4.3). The layout is the platform's (platform/gd32f30x/common.ld). */

/* The registry and the flashing tools know the slot as the codec slot (tools/boards.py): like a codec, the block is
 * the radio maker's and merged on the user's machine (tools/modem_boot.py) */
__codec_slot = 0x080FF000;
__codec_slot_end = 0x08100000;

MEMORY
{
	FLASH  (rx)  : ORIGIN = __app_start, LENGTH = __codec_slot - __app_start
	RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 96K - 272
	/* The crash record and the restart cause's words, kept across a reset (platform/gd32f30x/common.ld) */
	KEPT   (rw)  : ORIGIN = 0x20000000 + 96K - 272, LENGTH = 272
}

INCLUDE common.ld
