// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F43x start-up: the vector table and the reset handler. The image starts with the table at the board's APP_BASE,
// where the vendor bootloader jumps to it; the reset handler points VTOR at it, turns the FPU on, copies .data, clears
// .bss, runs the constructors (the boards' .preinit_array entries among them) and calls main()
#include <stdint.h>
#include "at32f43x.h"

extern uint32_t _sidata[], _sdata[], _edata[], _sbss[], _ebss[], _estack[];
extern void __libc_init_array(void);
int main(void);
void Reset_Handler(void);

void Default_Handler(void);
void Default_Handler(void)
{
	for (;;) {
	}
}

// Cortex-M exceptions, then the interrupts some driver here may take (at32f43x.h names them). A handler nobody defines
// falls to Default_Handler, and so does every interrupt the table doesn't name
#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK_ALIAS;
void HardFault_Handler(void) WEAK_ALIAS;
void MemManage_Handler(void) WEAK_ALIAS;
void BusFault_Handler(void) WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void) WEAK_ALIAS;
void DebugMon_Handler(void) WEAK_ALIAS;
void PendSV_Handler(void) WEAK_ALIAS;
void SysTick_Handler(void) WEAK_ALIAS;
void EXINT0_IRQHandler(void) WEAK_ALIAS;
void EXINT1_IRQHandler(void) WEAK_ALIAS;
void EXINT2_IRQHandler(void) WEAK_ALIAS;
void EXINT3_IRQHandler(void) WEAK_ALIAS;
void EXINT4_IRQHandler(void) WEAK_ALIAS;
void EXINT9_5_IRQHandler(void) WEAK_ALIAS;
void EXINT15_10_IRQHandler(void) WEAK_ALIAS;
void TMR2_GLOBAL_IRQHandler(void) WEAK_ALIAS;
void TMR3_GLOBAL_IRQHandler(void) WEAK_ALIAS;
void TMR4_GLOBAL_IRQHandler(void) WEAK_ALIAS;
void TMR5_GLOBAL_IRQHandler(void) WEAK_ALIAS;
void TMR6_DAC_GLOBAL_IRQHandler(void) WEAK_ALIAS;
void TMR7_GLOBAL_IRQHandler(void) WEAK_ALIAS;
void USART1_IRQHandler(void) WEAK_ALIAS;
void USART2_IRQHandler(void) WEAK_ALIAS;
void USART3_IRQHandler(void) WEAK_ALIAS;
void UART4_IRQHandler(void) WEAK_ALIAS;
void UART5_IRQHandler(void) WEAK_ALIAS;
void OTGFS1_IRQHandler(void) WEAK_ALIAS;
void OTGFS2_IRQHandler(void) WEAK_ALIAS;

typedef void (*vector_t)(void);

#define IRQ(n)	[16 + (n)]

// Every entry Default_Handler first, then the named ones over it: GCC's range initializer, whose later entries
// replace earlier ones on purpose
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used)) const vector_t at32_vectors[16 + AT32_IRQ_COUNT] = {
	[2 ... 16 + AT32_IRQ_COUNT - 1] = Default_Handler,
	[0] = (vector_t)(uintptr_t)_estack,
	[1] = Reset_Handler,
	[2] = NMI_Handler,
	[3] = HardFault_Handler,
	[4] = MemManage_Handler,
	[5] = BusFault_Handler,
	[6] = UsageFault_Handler,
	[7] = 0, [8] = 0, [9] = 0, [10] = 0,
	[11] = SVC_Handler,
	[12] = DebugMon_Handler,
	[13] = 0,
	[14] = PendSV_Handler,
	[15] = SysTick_Handler,
	IRQ(EXINT0_IRQn) = EXINT0_IRQHandler,
	IRQ(EXINT1_IRQn) = EXINT1_IRQHandler,
	IRQ(EXINT2_IRQn) = EXINT2_IRQHandler,
	IRQ(EXINT3_IRQn) = EXINT3_IRQHandler,
	IRQ(EXINT4_IRQn) = EXINT4_IRQHandler,
	IRQ(EXINT9_5_IRQn) = EXINT9_5_IRQHandler,
	IRQ(EXINT15_10_IRQn) = EXINT15_10_IRQHandler,
	IRQ(TMR2_GLOBAL_IRQn) = TMR2_GLOBAL_IRQHandler,
	IRQ(TMR3_GLOBAL_IRQn) = TMR3_GLOBAL_IRQHandler,
	IRQ(TMR4_GLOBAL_IRQn) = TMR4_GLOBAL_IRQHandler,
	IRQ(TMR5_GLOBAL_IRQn) = TMR5_GLOBAL_IRQHandler,
	IRQ(TMR6_DAC_GLOBAL_IRQn) = TMR6_DAC_GLOBAL_IRQHandler,
	IRQ(TMR7_GLOBAL_IRQn) = TMR7_GLOBAL_IRQHandler,
	IRQ(USART1_IRQn) = USART1_IRQHandler,
	IRQ(USART2_IRQn) = USART2_IRQHandler,
	IRQ(USART3_IRQn) = USART3_IRQHandler,
	IRQ(UART4_IRQn) = UART4_IRQHandler,
	IRQ(UART5_IRQn) = UART5_IRQHandler,
	IRQ(OTGFS1_IRQn) = OTGFS1_IRQHandler,
	IRQ(OTGFS2_IRQn) = OTGFS2_IRQHandler,
};
#pragma GCC diagnostic pop

__attribute__((noreturn)) void Reset_Handler(void)
{
	// The bootloader jumps here with its own vector table and maybe its interrupts still set up
	__disable_irq();
	SysTick->CTRL = 0;
	for (uint32_t i = 0; i < (AT32_IRQ_COUNT + 31u) / 32u; i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}
	SCB->VTOR = (uint32_t)at32_vectors;
	SCB->CPACR |= (3u << 20) | (3u << 22);	// CP10 and CP11: the FPU, before any code that may use it
	__DSB();
	__ISB();

	for (uint32_t *s = _sidata, *d = _sdata; d < _edata;) {
		*d++ = *s++;
	}
	for (uint32_t *d = _sbss; d < _ebss;) {
		*d++ = 0;
	}
	__libc_init_array();
	__enable_irq();	// PRIMASK as after a reset: the RTOS masks by BASEPRI
	(void)main();
	for (;;) {
	}
}
