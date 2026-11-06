// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD32F30x start-up: the vector table and the reset handler. The image starts with the table at the board's
// APP_BASE, where the vendor bootloader jumps to it; the reset handler points VTOR at it, turns the FPU on, copies
// .data, clears .bss, runs the constructors (the boards' .preinit_array entries among them) and calls main()
#include <stdint.h>
#include "gd32f30x.h"

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

// Cortex-M exceptions, then the GD32F30x interrupts. A handler a driver doesn't define falls to Default_Handler
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
void WWDGT_IRQHandler(void) WEAK_ALIAS;
void LVD_IRQHandler(void) WEAK_ALIAS;
void TAMPER_IRQHandler(void) WEAK_ALIAS;
void RTC_IRQHandler(void) WEAK_ALIAS;
void FMC_IRQHandler(void) WEAK_ALIAS;
void RCU_CTC_IRQHandler(void) WEAK_ALIAS;
void EXTI0_IRQHandler(void) WEAK_ALIAS;
void EXTI1_IRQHandler(void) WEAK_ALIAS;
void EXTI2_IRQHandler(void) WEAK_ALIAS;
void EXTI3_IRQHandler(void) WEAK_ALIAS;
void EXTI4_IRQHandler(void) WEAK_ALIAS;
void DMA0_Channel0_IRQHandler(void) WEAK_ALIAS;
void DMA0_Channel1_IRQHandler(void) WEAK_ALIAS;
void DMA0_Channel2_IRQHandler(void) WEAK_ALIAS;
void DMA0_Channel3_IRQHandler(void) WEAK_ALIAS;
void DMA0_Channel4_IRQHandler(void) WEAK_ALIAS;
void DMA0_Channel5_IRQHandler(void) WEAK_ALIAS;
void DMA0_Channel6_IRQHandler(void) WEAK_ALIAS;
void ADC0_1_IRQHandler(void) WEAK_ALIAS;
void USBD_HP_CAN0_TX_IRQHandler(void) WEAK_ALIAS;
void USBD_LP_CAN0_RX0_IRQHandler(void) WEAK_ALIAS;
void CAN0_RX1_IRQHandler(void) WEAK_ALIAS;
void CAN0_EWMC_IRQHandler(void) WEAK_ALIAS;
void EXTI5_9_IRQHandler(void) WEAK_ALIAS;
void TIMER0_BRK_IRQHandler(void) WEAK_ALIAS;
void TIMER0_UP_IRQHandler(void) WEAK_ALIAS;
void TIMER0_TRG_CMT_IRQHandler(void) WEAK_ALIAS;
void TIMER0_Channel_IRQHandler(void) WEAK_ALIAS;
void TIMER1_IRQHandler(void) WEAK_ALIAS;
void TIMER2_IRQHandler(void) WEAK_ALIAS;
void TIMER3_IRQHandler(void) WEAK_ALIAS;
void I2C0_EV_IRQHandler(void) WEAK_ALIAS;
void I2C0_ER_IRQHandler(void) WEAK_ALIAS;
void I2C1_EV_IRQHandler(void) WEAK_ALIAS;
void I2C1_ER_IRQHandler(void) WEAK_ALIAS;
void SPI0_IRQHandler(void) WEAK_ALIAS;
void SPI1_IRQHandler(void) WEAK_ALIAS;
void USART0_IRQHandler(void) WEAK_ALIAS;
void USART1_IRQHandler(void) WEAK_ALIAS;
void USART2_IRQHandler(void) WEAK_ALIAS;
void EXTI10_15_IRQHandler(void) WEAK_ALIAS;
void RTC_Alarm_IRQHandler(void) WEAK_ALIAS;
void USBD_WKUP_IRQHandler(void) WEAK_ALIAS;
void TIMER7_BRK_IRQHandler(void) WEAK_ALIAS;
void TIMER7_UP_IRQHandler(void) WEAK_ALIAS;
void TIMER7_TRG_CMT_IRQHandler(void) WEAK_ALIAS;
void TIMER7_Channel_IRQHandler(void) WEAK_ALIAS;
void ADC2_IRQHandler(void) WEAK_ALIAS;
void EXMC_IRQHandler(void) WEAK_ALIAS;
void SDIO_IRQHandler(void) WEAK_ALIAS;
void TIMER4_IRQHandler(void) WEAK_ALIAS;
void SPI2_IRQHandler(void) WEAK_ALIAS;
void UART3_IRQHandler(void) WEAK_ALIAS;
void UART4_IRQHandler(void) WEAK_ALIAS;
void TIMER5_IRQHandler(void) WEAK_ALIAS;
void TIMER6_IRQHandler(void) WEAK_ALIAS;
void DMA1_Channel0_IRQHandler(void) WEAK_ALIAS;
void DMA1_Channel1_IRQHandler(void) WEAK_ALIAS;
void DMA1_Channel2_IRQHandler(void) WEAK_ALIAS;
void DMA1_Channel3_4_IRQHandler(void) WEAK_ALIAS;

typedef void (*vector_t)(void);

__attribute__((section(".isr_vector"), used)) const vector_t gd32_vectors[16 + GD32_IRQ_COUNT] = {
	(vector_t)(uintptr_t)_estack,
	Reset_Handler,
	NMI_Handler,
	HardFault_Handler,
	MemManage_Handler,
	BusFault_Handler,
	UsageFault_Handler,
	0, 0, 0, 0,
	SVC_Handler,
	DebugMon_Handler,
	0,
	PendSV_Handler,
	SysTick_Handler,
	WWDGT_IRQHandler,
	LVD_IRQHandler,
	TAMPER_IRQHandler,
	RTC_IRQHandler,
	FMC_IRQHandler,
	RCU_CTC_IRQHandler,
	EXTI0_IRQHandler,
	EXTI1_IRQHandler,
	EXTI2_IRQHandler,
	EXTI3_IRQHandler,
	EXTI4_IRQHandler,
	DMA0_Channel0_IRQHandler,
	DMA0_Channel1_IRQHandler,
	DMA0_Channel2_IRQHandler,
	DMA0_Channel3_IRQHandler,
	DMA0_Channel4_IRQHandler,
	DMA0_Channel5_IRQHandler,
	DMA0_Channel6_IRQHandler,
	ADC0_1_IRQHandler,
	USBD_HP_CAN0_TX_IRQHandler,
	USBD_LP_CAN0_RX0_IRQHandler,
	CAN0_RX1_IRQHandler,
	CAN0_EWMC_IRQHandler,
	EXTI5_9_IRQHandler,
	TIMER0_BRK_IRQHandler,
	TIMER0_UP_IRQHandler,
	TIMER0_TRG_CMT_IRQHandler,
	TIMER0_Channel_IRQHandler,
	TIMER1_IRQHandler,
	TIMER2_IRQHandler,
	TIMER3_IRQHandler,
	I2C0_EV_IRQHandler,
	I2C0_ER_IRQHandler,
	I2C1_EV_IRQHandler,
	I2C1_ER_IRQHandler,
	SPI0_IRQHandler,
	SPI1_IRQHandler,
	USART0_IRQHandler,
	USART1_IRQHandler,
	USART2_IRQHandler,
	EXTI10_15_IRQHandler,
	RTC_Alarm_IRQHandler,
	USBD_WKUP_IRQHandler,
	TIMER7_BRK_IRQHandler,
	TIMER7_UP_IRQHandler,
	TIMER7_TRG_CMT_IRQHandler,
	TIMER7_Channel_IRQHandler,
	ADC2_IRQHandler,
	EXMC_IRQHandler,
	SDIO_IRQHandler,
	TIMER4_IRQHandler,
	SPI2_IRQHandler,
	UART3_IRQHandler,
	UART4_IRQHandler,
	TIMER5_IRQHandler,
	TIMER6_IRQHandler,
	DMA1_Channel0_IRQHandler,
	DMA1_Channel1_IRQHandler,
	DMA1_Channel2_IRQHandler,
	DMA1_Channel3_4_IRQHandler,
};

__attribute__((noreturn)) void Reset_Handler(void)
{
	// The bootloader jumps here with its own vector table and maybe its interrupts still set up
	__disable_irq();
	SysTick->CTRL = 0;
	for (uint32_t i = 0; i < (GD32_IRQ_COUNT + 31u) / 32u; i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}
	SCB->VTOR = (uint32_t)gd32_vectors;
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
