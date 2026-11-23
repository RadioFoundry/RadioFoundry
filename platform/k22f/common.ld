/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* K22F application layout, INCLUDEd by each board's memory.ld after its MEMORY (FLASH, SRAM_L, SRAM_U). The image
 * starts with the vector table at ORIGIN(FLASH) (board.mk APP_BASE, where the radio's bootloader starts the
 * application; startup.c points VTOR at it).
 *
 * SRAM is two 64 KB halves that meet at 0x20000000; no object may straddle the boundary, so they are two regions.
 * SRAM_L (0x1FFF0000) holds .data and .bss; SRAM_U (0x20000000) holds what the core marks RFOUNDRY_CPU_RAM (.ramu_bss,
 * zeroed at start-up), the RTOS heap with every task stack (.ramu_noinit, RFOUNDRY_CPU_RAM_NOINIT) and the main stack
 * (start-up and interrupts) at the top of the board's SRAM_U region.
 *
 * The board's FLASH region ends where the codec starts: the flashing tools read the region from memory.ld
 * (tools/boards.py). */

ENTRY(Reset_Handler)

_estack = ORIGIN(SRAM_U) + LENGTH(SRAM_U);
_Min_Heap_Size = 0x400;		/* newlib's heap (nothing in RadioFoundry allocates from it) */
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
		*(.ramfunc) *(.ramfunc*)	/* code that runs from RAM: the flash controller's command loop (platform/k22f/ftfa.c) */
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
	/* The crash log's record (platform/k22f/crash_k22f.c): never cleared at start-up, so it outlives a reset */
	.crash_noinit (NOLOAD) : { . = ALIGN(4); *(.crash_noinit) *(.crash_noinit*) . = ALIGN(4); } >SRAM_U
	._main_stack (NOLOAD) : { . = ALIGN(8); . = . + _Min_Stack_Size; . = ALIGN(8); } >SRAM_U

	/DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
	.ARM.attributes 0 : { *(.ARM.attributes) }

	/* Belt and braces: everything loaded into flash ends where the board's FLASH region does, before the codec */
	ASSERT(_sidata + SIZEOF(.data) <= ORIGIN(FLASH) + LENGTH(FLASH), "the image reaches into the codec's flash")
}
