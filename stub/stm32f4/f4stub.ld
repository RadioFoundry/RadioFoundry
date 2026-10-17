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
