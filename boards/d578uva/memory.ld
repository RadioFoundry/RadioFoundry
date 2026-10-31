/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* AnyTone AT-D578UV, Artery builds: AT32F435 (more than 1 MB of flash), application behind the vendor bootloader at
 * 0x08006000 as on the GD32 builds (docs/spec/hardware-d578uv.md 10). The flash layout is boards/d578uv/memory.ld's:
 * the image stays below the modem boot block slot (0x080FF000..0x08100000), where the boot block the DMR baseband
 * needs is merged from the user's stock file (hardware-d578uv.md 4.3). The layout is the platform's
 * (platform/at32f43x/common.ld). */

/* The modem boot slot, known to the registry and the flashing tools as the codec slot (tools/boards.py) */
__codec_slot = 0x080FF000;
__codec_slot_end = 0x08100000;

/* SRAM: the part's option byte EOPB0 splits its 512 KB between SRAM and the flash's zero-wait area, leaving at least
 * 128 KB of SRAM in every setting. The stock firmware reads no option byte and uses under 96 KB, so the setting on
 * a radio is unknown: the image takes the 128 KB every setting has */
MEMORY
{
	FLASH  (rx)  : ORIGIN = __app_start, LENGTH = __codec_slot - __app_start
	RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 272
	/* The crash record and the restart cause's words, kept across a reset (platform/at32f43x/common.ld) */
	KEPT   (rw)  : ORIGIN = 0x20000000 + 128K - 272, LENGTH = 272
}

INCLUDE common.ld
