/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* The STM32F4 flash stub (docs/spec/stub-stm32f4.md). ODR stages the image and copies it to the SRAM base before the
 * jump (platform/stm32f4/stub_entry.c), so everything the image holds is linked to run there, entry first. The
 * image ends within STUB_STAGE_SIZE (8 KB); .bss and the stack use the rest of SRAM, which ODR no longer needs. */

ENTRY(f4stub_entry)

MEMORY
{
	RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_image_max = 8K;

SECTIONS
{
	.image :
	{
		KEEP(*(.entry))
		/* The image header at a fixed offset (F4STUB_HDR_OFFSET in f4stub_header.h): the entry code is 12 bytes, so
		 * the location only moves forward; ld refuses an entry that grew past it */
		. = 0x10;
		_stub_header = .;
		KEEP(*(.stub_header))
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

	ASSERT(_stub_header == ORIGIN(RAM) + 0x10, "the image header is not at offset 0x10")
	ASSERT(_image_end - ORIGIN(RAM) <= _image_max, "the stub image is over 8 KB (ODR's staging buffer)")
	ASSERT(_ebss + 4K <= _estack, "no room left for the stack")
}
