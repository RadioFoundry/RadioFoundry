/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Radioddity GD-77, MK22FN512 (512 KB flash, 128 KB SRAM), application behind the radio's bootloader
 * (docs/spec/hardware-gd77.md 6.1).
 *
 * Program flash: the bootloader owns 0x00000..0x03FFF and is never written. The application starts at 0x04000 with its
 * vector table first. The last 2 KB sector, 0x7F800..0x7FFFF, holds radio data that must survive and is never linked
 * into, so the application region ends at 0x7F800.
 *
 * SRAM is two 64 KB halves that meet at 0x20000000; no object may straddle the boundary, so they are two regions.
 * SRAM_L (0x1FFF0000) holds .data and .bss; SRAM_U (0x20000000) holds what the core marks ODR_CPU_RAM (.ramu_bss,
 * zeroed at start-up), the RTOS heap with every task stack (.ramu_noinit, ODR_CPU_RAM_NOINIT) and the main stack
 * (start-up and interrupts) at the top. The stack starts at 0x2000FEF0: the top 272 bytes of SRAM stay unused, a
 * margin for anything the bootloader keeps there (unverified). */

ENTRY(Reset_Handler)

_estack = 0x2000FEF0;
_Min_Heap_Size = 0x400;		/* newlib's heap (nothing in ODR allocates from it) */
_Min_Stack_Size = 0x800;	/* main stack: start-up and interrupts */

MEMORY
{
	FLASH   (rx)  : ORIGIN = 0x00004000, LENGTH = 0x0007F800 - 0x00004000
	SRAM_L  (xrw) : ORIGIN = 0x1FFF0000, LENGTH = 64K
	SRAM_U  (rw)  : ORIGIN = 0x20000000, LENGTH = 0x2000FEF0 - 0x20000000
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
	} >SRAM_L AT> FLASH

	.bss :
	{
		. = ALIGN(4);
		_sbss = .;
		__bss_start__ = _sbss;
		*(.bss) *(.bss*) *(COMMON)
		. = ALIGN(4);
		_ebss = .;
		__bss_end__ = _ebss;
	} >SRAM_L

	._user_heap :
	{
		. = ALIGN(8);
		PROVIDE(end = .);
		PROVIDE(_end = .);
		. = . + _Min_Heap_Size;
		. = ALIGN(8);
	} >SRAM_L

	.ramu_bss (NOLOAD) : { . = ALIGN(4); _sramu_bss = .; *(.ramu_bss) *(.ramu_bss*) . = ALIGN(4); _eramu_bss = .; } >SRAM_U
	.ramu_noinit (NOLOAD) : { . = ALIGN(8); *(.ramu_noinit) *(.ramu_noinit*) . = ALIGN(8); } >SRAM_U
	._main_stack (NOLOAD) : { . = ALIGN(8); . = . + _Min_Stack_Size; . = ALIGN(8); } >SRAM_U

	/DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
	.ARM.attributes 0 : { *(.ARM.attributes) }

	/* The image ends before the radio's data sector */
	ASSERT(_sidata + SIZEOF(.data) <= 0x0007F800, "the image reaches into the last flash sector (0x7F800)")
}
