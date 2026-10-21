/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* The K22F flash stub (docs/spec/stub-k22f.md). ODR stages the image and copies it to SRAM_L's base before the jump
 * (platform/k22f/stub_entry.c, stub_handoff.h), so everything the image holds is linked to run there, entry first. The
 * image ends within the 8 KB staging limit; .bss and the stack use the rest of SRAM_L. SRAM_U is left alone: ODR's
 * task stack (which runs the copy) and the parked USB descriptor table are there, and nothing may straddle the
 * 0x20000000 boundary. */

ENTRY(k22stub_entry)

MEMORY
{
	RAM (rwx) : ORIGIN = 0x1FFF0000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_image_max = 8K;

SECTIONS
{
	.image :
	{
		KEEP(*(.entry))
		*(.text) *(.text*)
		*(.rodata) *(.rodata*)
		*(.data) *(.data*)
		. = ALIGN(4);
		_image_end = .;
	} >RAM

	.bss (NOLOAD) :
	{
		. = ALIGN(4);
		_sbss = .;
		*(.bss) *(.bss*) *(COMMON)
		. = ALIGN(4);
		_ebss = .;
	} >RAM

	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }

	ASSERT(_image_end - ORIGIN(RAM) <= _image_max, "the stub image is over 8 KB (ODR's staging buffer)")
	ASSERT(_ebss + 4K <= _estack, "no room left for the stack")
}
