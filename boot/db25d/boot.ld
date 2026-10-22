/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* DB25-D first stage (boot.S, boot.c): runs in place from its 4 KB sector at the image address (board.mk APP_BASE), where the IAP loader jumps.
 * No data and no bss: nothing is copied to RAM before it runs. */
ENTRY(boot_entry)
MEMORY { FLASH (rx) : ORIGIN = __app_base, LENGTH = __boot_size }
SECTIONS
{
	.text : { KEEP(*(.entry)) *(.text*) *(.rodata*) } > FLASH
	/DISCARD/ : { *(.data*) *(.bss*) *(.comment) }
}
