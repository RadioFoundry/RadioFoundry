/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The OpenDigitalRadio Authors */
/* Baofeng DM-1701, STM32F405VG, application behind the stock bootloader (docs/spec/hardware-dm1701.md 1.4, 1.5).
 * Codec (docs/spec/codec-placement.md, "STM32 radios"): the merge on the user's machine puts the stock V02.06 CSV
 * codec's tables at their stock address 0x080D19F0..0x080E3A72 (sectors 10 and 11), its code right after them at
 * 0x080E3A74, and the descriptor that says where at 0x080FFF80. The code runs in place.
 *
 * Whole flash sectors are kept free of our code, so the flashing tool can update the firmware without erasing the
 * codec: the application gets sectors 3-9 (0x0800C000..0x080BFFFF, 720 KB in one piece) and never links into sectors
 * 10 and 11. */

ENTRY(Reset_Handler)

_estack = 0x2001FFFC;		/* the bootloader rejects SP = top of RAM */
_Min_Heap_Size = 0x1000;
_Min_Stack_Size = 0x800;	/* main stack: start-up and interrupts */

MEMORY
{
	FLASH   (rx)  : ORIGIN = 0x0800C000, LENGTH = 0x080C0000 - 0x0800C000	/* sectors 3-9 */
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

	/* Belt and braces for the sector rule above: everything loaded into flash ends before codec sector 10 */
	ASSERT(_sidata + SIZEOF(.data) <= 0x080C0000, "the image reaches into codec sector 10 (0x080C0000)")
}
