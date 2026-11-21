/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* STM32F4 application layout, INCLUDEd by each board's memory.ld after its MEMORY (FLASH, RAM, CCMRAM). The image
 * starts with the vector table at ORIGIN(FLASH) (board.mk APP_BASE, where the bootloader starts the application).
 *
 * The board's FLASH region ends where the codec's first flash sector starts (docs/spec/codec-placement.md, "STM32
 * radios"): whole sectors stay free of our code, so the flashing tool can update the firmware without erasing the
 * codec. The flashing tools read the region from memory.ld (tools/boards.py). */

ENTRY(Reset_Handler)

/* The initial stack pointer: the top of the board's RAM region. One stock bootloader rejects SP = top of RAM
 * (hardware-dm1701.md 18 (1)), so the regions end 4 bytes short */
_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0x1000;
_Min_Stack_Size = 0x800;	/* main stack: start-up and interrupts (at least this much RAM stays free for it) */
_Min_Task_Heap = 24K;		/* the RTOS heap: every task stack (about 20 KB with DMR), queues */
_Ccm_Spare = 256;		/* CCM the heap leaves free, so the build's memory report shows what the statics use */

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

	/* CCM: CPU only (no DMA can reach it); ccm.c sets it up before main. .ccmdata (RFOUNDRY_CPU_DATA) is copied from
	 * flash like .data, .ccmbss (RFOUNDRY_CPU_RAM) zeroed like .bss; .ccmram (RFOUNDRY_CPU_RAM_NOINIT: the codec state)
	 * is left as it is and must be written before it's read. The RTOS heap, with every task stack, is the rest */
	_siccmdata = LOADADDR(.ccmdata);
	.ccmdata : { . = ALIGN(4); _sccmdata = .; *(.ccmdata) *(.ccmdata*) . = ALIGN(4); _eccmdata = .; } >CCMRAM AT> FLASH
	/* the end of the .bin (the last section copied into it): the support info's image hash runs __app_base .. here */
	__image_end = LOADADDR(.ccmdata) + SIZEOF(.ccmdata);
	.ccmbss (NOLOAD) : { . = ALIGN(4); _sccmbss = .; *(.ccmbss) *(.ccmbss*) . = ALIGN(4); _eccmbss = .; } >CCMRAM
	.ccmram (NOLOAD) : { . = ALIGN(8); *(.ccmram) *(.ccmram*) . = ALIGN(4); } >CCMRAM
	.ccmheap (NOLOAD) :
	{
		. = ALIGN(8);
		_sccmheap = .;
		. = MAX(., ORIGIN(CCMRAM) + LENGTH(CCMRAM) - _Ccm_Spare);
		_eccmheap = .;
	} >CCMRAM
	ASSERT(_eccmheap - _sccmheap >= _Min_Task_Heap, "CCM: less than _Min_Task_Heap left for the RTOS heap (task stacks)")

	._user_heap_stack :
	{
		. = ALIGN(8);
		PROVIDE(end = .);
		PROVIDE(_end = .);
		/* Nothing uses the heap (no _sbrk): the main stack may grow down to here, its canary words (irq_stack.c) */
		__main_stack_bottom = .;
		. = . + _Min_Heap_Size;
		. = . + _Min_Stack_Size;
		. = ALIGN(8);
	} >RAM

	/DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
	.ARM.attributes 0 : { *(.ARM.attributes) }

	/* Belt and braces for the sector rule above: everything loaded into flash ends before the codec's first sector */
	ASSERT(_sidata + SIZEOF(.data) <= ORIGIN(FLASH) + LENGTH(FLASH), "the image reaches into the codec's flash sectors")
}
