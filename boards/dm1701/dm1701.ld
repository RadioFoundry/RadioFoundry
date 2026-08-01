/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Baofeng DM-1701, STM32F405VG, application behind the stock bootloader (docs/spec/hardware-dm1701.md 1.4, 1.5).
 * Codec profile: stock DM-1701 V02.06 CSV runs in place (docs/spec/codec-dm1701.md "Recommended hosting"):
 * code window 0x08074714..0x08084930 and table window 0x080D19F0..0x080E4240 are reserved, never linked into. */

ENTRY(Reset_Handler)

_estack = 0x2001FFFC;		/* the bootloader rejects SP = top of RAM */
_Min_Heap_Size = 0x1000;
_Min_Stack_Size = 0x800;	/* main stack: start-up and interrupts */

MEMORY
{
	FLASH   (rx)  : ORIGIN = 0x0800C000, LENGTH = 0x08074714 - 0x0800C000
	FLASH2  (r)   : ORIGIN = 0x08084930, LENGTH = 0x080D19F0 - 0x08084930
	RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 128K - 4
	CCMRAM  (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

SECTIONS
{
	.isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH

	.text :
	{
		. = ALIGN(4);
		*(.text) *(.text*) *(.glue_7) *(.glue_7t) *(.eh_frame)
		KEEP(*(.init)) KEEP(*(.fini))
		. = ALIGN(4);
		_etext = .;
	} >FLASH

	.rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH

	/* Large constant data (fonts, tables) may be placed in the space between the codec windows */
	.rodata2 : { . = ALIGN(4); *(.rodata2) *(.rodata2*) . = ALIGN(4); } >FLASH2

	.ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
	.ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

	.preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
	.init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
	.fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH

	_sidata = LOADADDR(.data);
	.data :
	{
		. = ALIGN(4);
		_sdata = .;
		*(.data) *(.data*)
		. = ALIGN(4);
		_edata = .;
	} >RAM AT> FLASH

	.bss :
	{
		. = ALIGN(4);
		_sbss = .;
		__bss_start__ = _sbss;
		*(.bss) *(.bss*) *(COMMON)
		. = ALIGN(4);
		_ebss = .;
		__bss_end__ = _ebss;
	} >RAM

	/* CCM: CPU only (no DMA), not zeroed or initialized by the start-up code: users clear it themselves */
	.ccmram (NOLOAD) : { . = ALIGN(4); *(.ccmram) *(.ccmram*) . = ALIGN(4); } >CCMRAM

	._user_heap_stack :
	{
		. = ALIGN(8);
		PROVIDE(end = .);
		PROVIDE(_end = .);
		. = . + _Min_Heap_Size;
		. = . + _Min_Stack_Size;
		. = ALIGN(8);
	} >RAM

	/DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
	.ARM.attributes 0 : { *(.ARM.attributes) }

	/* The .bin is one contiguous stream from 0x0800C000; data placed beyond the codec window needs the codec merged
	 * (or padding) first, so keep it empty until that path exists */
	ASSERT(SIZEOF(.rodata2) == 0, ".rodata2 is not packed into the image yet")
}
