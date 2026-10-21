// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// K22F start-up: the vector table at the start of the application image, and the reset entry. The radio's bootloader
// starts the application at its vector table; nothing here assumes what it left set, so the reset entry sets the stack,
// switches the watchdog off, points VTOR at this table, turns the FPU on and lays out RAM before main()
#include "platform.h"

extern uint32_t _estack[];			// the linker script: top of the main stack
extern uint32_t _sidata[], _sdata[], _edata[];	// .data: load address, run span
extern uint32_t _sbss[], _ebss[];		// .bss
extern uint32_t _sramu_bss[], _eramu_bss[];	// ODR_CPU_RAM in the upper SRAM, zeroed like .bss

int main(void);
void __libc_init_array(void);
void Reset_Handler(void);
void reset_c(void) __attribute__((noreturn, used));

void Default_Handler(void)
{
	__disable_irq();
	for (;;) {
	}
}

#define WEAK __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK;
void HardFault_Handler(void) WEAK;
void MemManage_Handler(void) WEAK;
void BusFault_Handler(void) WEAK;
void UsageFault_Handler(void) WEAK;
void SVC_Handler(void) WEAK;
void DebugMon_Handler(void) WEAK;
void PendSV_Handler(void) WEAK;
void SysTick_Handler(void) WEAK;
void DMA0_IRQHandler(void) WEAK;
void DMA1_IRQHandler(void) WEAK;
void DMA2_IRQHandler(void) WEAK;
void DMA3_IRQHandler(void) WEAK;
void DMA_Error_IRQHandler(void) WEAK;
void SPI0_IRQHandler(void) WEAK;
void SPI1_IRQHandler(void) WEAK;
void I2S0_Tx_IRQHandler(void) WEAK;
void I2S0_Rx_IRQHandler(void) WEAK;
void ADC0_IRQHandler(void) WEAK;
void PIT0_IRQHandler(void) WEAK;
void PIT1_IRQHandler(void) WEAK;
void PIT2_IRQHandler(void) WEAK;
void PIT3_IRQHandler(void) WEAK;
void USB0_IRQHandler(void) WEAK;
void DAC0_IRQHandler(void) WEAK;
void PORTA_IRQHandler(void) WEAK;
void PORTB_IRQHandler(void) WEAK;
void PORTC_IRQHandler(void) WEAK;
void PORTD_IRQHandler(void) WEAK;
void PORTE_IRQHandler(void) WEAK;

typedef void (*vector_t)(void);

__attribute__((section(".isr_vector"), used))
const vector_t k22f_vectors[16 + K22F_IRQ_COUNT] = {
	(vector_t)(uintptr_t)_estack, Reset_Handler, NMI_Handler, HardFault_Handler,
	MemManage_Handler, BusFault_Handler, UsageFault_Handler, 0,
	0, 0, 0, SVC_Handler,
	DebugMon_Handler, 0, PendSV_Handler, SysTick_Handler,
	// IRQ 0..85 (k22f.h); the ones without a name of their own stop in Default_Handler
	DMA0_IRQHandler,
	DMA1_IRQHandler,
	DMA2_IRQHandler,
	DMA3_IRQHandler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,	// IRQ 4-15
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	DMA_Error_IRQHandler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,	// IRQ 17-25
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	Default_Handler,
	SPI0_IRQHandler,
	SPI1_IRQHandler,
	I2S0_Tx_IRQHandler,
	I2S0_Rx_IRQHandler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,	// IRQ 30-38
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	Default_Handler,
	ADC0_IRQHandler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,	// IRQ 40-47
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	PIT0_IRQHandler,
	PIT1_IRQHandler,
	PIT2_IRQHandler,
	PIT3_IRQHandler,
	Default_Handler,	// IRQ 52
	USB0_IRQHandler,
	Default_Handler, Default_Handler,	// IRQ 54-55
	DAC0_IRQHandler,
	Default_Handler, Default_Handler,	// IRQ 57-58
	PORTA_IRQHandler,
	PORTB_IRQHandler,
	PORTC_IRQHandler,
	PORTD_IRQHandler,
	PORTE_IRQHandler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,	// IRQ 64-85
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	Default_Handler, Default_Handler, Default_Handler, Default_Handler,
	Default_Handler, Default_Handler,
};
_Static_assert(sizeof k22f_vectors == 4u * (16u + K22F_IRQ_COUNT), "vector table size");

// The stack first: the bootloader may jump here with its own stack pointer
__attribute__((naked)) void Reset_Handler(void)
{
	__asm volatile(
		"ldr	r0, =_estack\n"
		"msr	msp, r0\n"
		"b	reset_c\n");
}

// Read after RAM is laid out (a value stored before the .bss loop would be cleared by it); the registers keep the
// last reset's sources until the next reset
static uint16_t reset_cause;

uint16_t k22f_reset_cause(void)
{
	return reset_cause;
}

void reset_c(void)
{
	__disable_irq();
	// The watchdog: unlocked with the two keys in a row, then off (it can only be changed shortly after an unlock)
	WDOG_UNLOCK = WDOG_UNLOCK_KEY1;
	WDOG_UNLOCK = WDOG_UNLOCK_KEY2;
	__NOP();
	__NOP();
	WDOG_STCTRLH = WDOG_STCTRLH_RESERVED | WDOG_STCTRLH_WAITEN | WDOG_STCTRLH_STOPEN | WDOG_STCTRLH_ALLOWUPDATE |
		       WDOG_STCTRLH_CLKSRC;
	SCB->VTOR = (uint32_t)(uintptr_t)k22f_vectors;
	SCB->CPACR |= (3u << 20) | (3u << 22);		// CP10, CP11: the FPU, full access
	__DSB();
	__ISB();

	for (uint32_t *s = _sidata, *d = _sdata; d < _edata;) {
		*d++ = *s++;
	}
	for (uint32_t *d = _sbss; d < _ebss;) {
		*d++ = 0;
	}
	for (uint32_t *d = _sramu_bss; d < _eramu_bss;) {
		*d++ = 0;
	}
	reset_cause = (uint16_t)(RCM_SRS0 | ((uint16_t)RCM_SRS1 << 8));
	__libc_init_array();
	__enable_irq();
	(void)main();
	for (;;) {
	}
}
