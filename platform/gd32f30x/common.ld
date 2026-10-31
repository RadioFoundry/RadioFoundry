/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* GD32F30x application layout, INCLUDEd by each board's memory.ld after its MEMORY (FLASH, RAM, KEPT). The image
 * starts with the vector table at ORIGIN(FLASH) (board.mk APP_BASE, where the vendor bootloader starts the
 * application). No codec in flash on these radios: the vocoder is in the DMR baseband. The flashing tools read the
 * region from memory.ld (tools/boards.py). KEPT is the 272 bytes at the top of SRAM, right after RAM, that a reset
 * keeps (.kept below). */

ENTRY(Reset_Handler)

/* The initial stack pointer: the top of the board's RAM region, which ends where KEPT starts */
_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0;	/* no C heap: _sbrk refuses (syscalls.c); FreeRTOS has its own */
_Min_Stack_Size = 0x800;	/* main stack: start-up and interrupts */

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

	._user_heap_stack :
	{
		. = ALIGN(8);
		PROVIDE(end = .);
		PROVIDE(_end = .);
		. = . + _Min_Heap_Size;
		. = . + _Min_Stack_Size;
		. = ALIGN(8);
	} >RAM

	/* Memory a reset keeps: the crash record and the restart cause's words (platform/gd32f30x/crash_gd32.c). KEPT is
	 * the top 272 bytes of SRAM, above the main stack; the start-up neither copies nor clears it (NOLOAD) */
	.kept (NOLOAD) : { KEEP(*(.kept)) } >KEPT

	/DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
	.ARM.attributes 0 : { *(.ARM.attributes) }

	ASSERT(ORIGIN(KEPT) == ORIGIN(RAM) + LENGTH(RAM), "KEPT must follow RAM: the main stack starts below it")
	ASSERT(_sidata + SIZEOF(.data) <= ORIGIN(FLASH) + LENGTH(FLASH), "the image is larger than the board's FLASH region")
}
