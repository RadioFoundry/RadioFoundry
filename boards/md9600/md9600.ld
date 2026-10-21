/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* TYT MD-9600 / RT90, STM32F405VGT6, application behind the 48 KB TYT bootloader at 0x0800C000, as the DM-1701 and the
 * MD-380 (docs/spec/hardware-md9600.md 1.3), so the vector table, stack rule and start-up are theirs.
 *
 * The application gets sectors 3-7 (0x0800C000..0x0807FFFF, 464 KB in one piece) and never links into sectors 8-11,
 * the MD-380's layout: those whole sectors stay free for the stock codec, so a later update can keep it without an
 * erase (docs/spec/codec-placement.md). The codec goes there as docs/spec/codec-md9600.md "Hosting" says: the table
 * window stays at its stock address in sector 9, the code moves to the start of sector 11 and the descriptor sits at the
 * end of sector 11 (boards/md9600/vocoder_md9600.c). Sectors 8 and 10 stay free. */

ENTRY(Reset_Handler)

_estack = 0x2001FFFC;		/* hardware-dm1701.md 18 (1): the AnyRoad loader rejects SP = top of RAM; kept here (?) */
_Min_Heap_Size = 0x1000;
_Min_Stack_Size = 0x800;	/* main stack: start-up and interrupts */

MEMORY
{
	FLASH   (rx)  : ORIGIN = 0x0800C000, LENGTH = 0x08080000 - 0x0800C000	/* sectors 3-7 */
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
	 * entry, like .bss; .ccmram (ODR_CPU_RAM_NOINIT: the RTOS heap with every task stack) is left as it is */
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

	/* Belt and braces for the sector rule above: everything loaded into flash ends before codec sector 8 */
	ASSERT(_sidata + SIZEOF(.data) <= 0x08080000, "the image reaches into codec sector 8 (0x08080000)")
}
