/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* Radioddity GD-88 SoC B, the second receiver, with DMR (CONFIG_DMR=y; without it config.mk selects
 * boards/gd88/fm_only.ld), HR_C7000 (CK803S), application behind B's IAP loader and RadioFoundry's first stage
 * (docs/spec/hardware-gd88.md 2, 8, 10).
 *
 * Program flash (XIP, 2 MB at 0x03000000): the loader below 0x0300C000 (never written), RadioFoundry's first stage
 * 0x0300C000..0x0300D000 (board.mk APP_BASE, BOOT_SIZE), the application from 0x0300D000, the codec slot
 * 0x030E8000..0x03100000 (hardware-gd88.md 8: the codec is merged there on the user's machine; never linked into).
 *
 * SRAM (288 KB at 0x10000): RadioFoundry's region from 0x10000, and at the top the codec window 0x44E00..0x58000 (the 2022-10
 * code, the largest of B's builds). B's stub comes through its first stage (boot/gd88b), never
 * through a runtime stub entry. The modem SRAM window the codec tables are copied to is reserved:
 * 0x18000B40..0x18004324, the union of B's builds (0x18000B40..0x18003B10) and A's (0x18001204..0x18004324),
 * because B runs A's codec, packed as on A, with its tables at A's stock address (board.toml [codec] from,
 * hardware-gd88.md 8). RadioFoundry's only other use of modem SRAM is the crash record at its top (0x18007EF0).
 *
 * The layout is the platform's (platform/hrc7000/common.ld). */

__codec_slot = 0x030E8000;
__codec_slot_end = 0x03100000;
/* The codec window (platform/hrc7000/codec_slot.h: the slot's code runs at its start; the size is the largest code
 * among the board's codec maps; tools/boards.py reads these symbols for the codec tools) and the modem SRAM table
 * window, the union of A's and B's builds' stock table addresses (B takes A's codec too) */
__codec_run_lo = 0x00044E00;
__codec_run_hi = 0x00058000;
__codec_tab_lo = 0x18000B40;
__codec_tab_hi = 0x18004324;

MEMORY
{
	FLASH  (rx)  : ORIGIN = __app_start, LENGTH = __codec_slot - __app_start
	RAM    (rwx) : ORIGIN = 0x00010000, LENGTH = __codec_run_lo - 0x00010000	/* up to the codec window */
}

INCLUDE common.ld
