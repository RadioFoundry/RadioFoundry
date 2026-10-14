/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Baofeng DM-1701, STM32F405VG, application behind the stock bootloader (docs/spec/hardware-dm1701.md 1.4, 1.5).
 * Codec profile: stock DM-1701 V02.06 CSV runs in place (docs/spec/codec-dm1701.md "Recommended hosting"):
 * code window 0x08074714..0x08084930 and table window 0x080D19F0..0x080E4240.
 *
 * Whole flash sectors, not just the windows, are kept free of our code, so the flashing tool can update the firmware
 * without erasing the codec (docs/spec/hardware-dm1701.md 1.4): the code window spans sectors 7 and 8
 * (0x08060000..0x0809FFFF), the table window sectors 10 and 11 (0x080C0000..0x080FFFFF). The application gets
 * sectors 3-6 (0x0800C000..0x0805FFFF, 336 KB). Sector 9 (0x080A0000..0x080BFFFF, 128 KB) is FLASH2 and holds all
 * read-only data (.rodata2, below), which frees that much of the 336 KB for code. The build writes it as a second
 * image, odr_dm1701_rodata2.bin, and the release tools and odrflash write it together with the application
 * (docs/dev/release.md "Sector 9"). Never link into sectors 7, 8, 10 or 11. */

ENTRY(Reset_Handler)

_estack = 0x2001FFFC;		/* the bootloader rejects SP = top of RAM */
_Min_Heap_Size = 0x1000;
_Min_Stack_Size = 0x800;	/* main stack: start-up and interrupts */

MEMORY
{
	FLASH   (rx)  : ORIGIN = 0x0800C000, LENGTH = 0x08060000 - 0x0800C000	/* sectors 3-6 */
	FLASH2  (r)   : ORIGIN = 0x080A0000, LENGTH = 0x080C0000 - 0x080A0000	/* sector 9 */
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

	/* All constant data (fonts, tables, string literals) lives in sector 9, between the codec sectors. Flash is memory
	 * mapped, so a read from it costs the same as one from sectors 3-6 and nothing needs a copy. The firmware never
	 * erases or programs internal flash, so nothing can write sector 9 while it is read */
	.rodata2 : { . = ALIGN(4); *(.rodata2) *(.rodata2.*) *(.rodata) *(.rodata.*) . = ALIGN(4); } >FLASH2

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

	/* CCM: CPU only (no DMA can reach it). .ccmbss (ODR_CPU_RAM) is zeroed before main by board.c's .preinit_array
	 * entry, like .bss; .ccmram (ODR_CPU_RAM_NOINIT: the RTOS heap with every task stack, the codec state) is left
	 * as it is and must be written before it's read */
	.ccmbss (NOLOAD) : { . = ALIGN(4); _sccmbss = .; *(.ccmbss) *(.ccmbss*) . = ALIGN(4); _eccmbss = .; } >CCMRAM
	.ccmram (NOLOAD) : { . = ALIGN(8); *(.ccmram) *(.ccmram*) . = ALIGN(4); } >CCMRAM

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

	/* The application .bin holds what is loaded into FLASH, the _rodata2.bin what is loaded into FLASH2 (Makefile) */
	ASSERT(SIZEOF(.rodata2) <= LENGTH(FLASH2), "constant data does not fit sector 9")
	/* Belt and braces for the sector rule above: everything loaded into flash ends before sector 7 */
	ASSERT(_sidata + SIZEOF(.data) <= 0x08060000, "the image reaches into codec sector 7 (0x08060000)")
}
