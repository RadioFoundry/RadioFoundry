/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* The AT32F43x flash stub (docs/spec/stub-at32f43x.md). RadioFoundry stages the image and copies it to the SRAM base
 * before the jump (platform/at32f43x/stub_entry.c), so everything the image holds is linked to run there, entry first.
 * The image is at most 8 KB (AT32STUB_IMAGE_MAX); .bss and the stack stay in the first 32 KB, which every AT32F43x
 * part has and which ends well below the kept RAM at the top of SRAM (the restart-cause word the stub's RESET marks). */

ENTRY(at32stub_entry)

MEMORY
{
	RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_image_max = 8K;

SECTIONS
{
	.image :
	{
		KEEP(*(.entry))
		/* The image header at a fixed offset (AT32STUB_HDR_OFFSET in at32stub_header.h): the entry code is 12 bytes,
		 * so the location only moves forward; ld refuses an entry that grew past it */
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
	ASSERT(_image_end - ORIGIN(RAM) <= _image_max, "the stub image is over 8 KB (AT32STUB_IMAGE_MAX)")
	ASSERT(_ebss + 4K <= _estack, "no room left for the stack")
}
