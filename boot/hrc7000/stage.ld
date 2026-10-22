/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* HR_C7000 first stage (entry.S, boot/<radio>/boot.c): runs in place from the 4 KB sector at 0x0300C000, where the
 * IAP loader jumps. No data and no bss: nothing is copied to RAM before it runs. */
ENTRY(boot_entry)
MEMORY { FLASH (rx) : ORIGIN = 0x0300C000, LENGTH = 0x1000 }
SECTIONS
{
	.text : { KEEP(*(.entry)) *(.text*) *(.rodata*) } > FLASH
	/DISCARD/ : { *(.data*) *(.bss*) *(.comment) }
}
