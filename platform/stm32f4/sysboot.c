// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// STM32F4 system services (hal_system.h): chip identity and option bytes, memory reads, reboot, and the software
// entry to the MCU's ROM bootloader (docs/spec/hardware-dm1701.md "Software entry to a bootloader").
//
// ROM DFU entry: the request stores a flag in an RTC backup register (kept through a system reset, unlike RAM, which
// the stock bootloader runs in before the application) and resets. The stock bootloader then starts the application
// as usual, and stm32_sysboot_check(), first thing in main(), finds the flag, clears it (so a failed entry boots
// normally next time), puts the board in its reset-time pin state (board_sysboot_prepare: the power latch stays
// high), returns clocks and peripherals to their reset state, maps system memory at 0 and jumps to the ROM's reset
// vector.
#include <string.h>
#include "config.h"
#include "platform.h"
#include "hal_system.h"

#define SYSBOOT_MAGIC		0xB007DF11u	// in RTC_BKP19R: enter the ROM bootloader on the next start
#define SYSMEM_BASE		0x1FFF0000u
#define SYSMEM_END		0x1FFF7800u
#define UID_BASE_ADDR		0x1FFF7A10u
#define FLASH_SIZE_ADDR		0x1FFF7A22u
#define SPIN_LIMIT		1000000u

__attribute__((weak)) void board_sysboot_prepare(void)
{
}

static bool chip(hal_chip_t *c)
{
	c->idcode = DBGMCU->IDCODE;
	c->flash_kb = *(volatile const uint16_t *)FLASH_SIZE_ADDR;
	c->uid[0] = *(volatile const uint32_t *)(UID_BASE_ADDR + 0u);
	c->uid[1] = *(volatile const uint32_t *)(UID_BASE_ADDR + 4u);
	c->uid[2] = *(volatile const uint32_t *)(UID_BASE_ADDR + 8u);
	c->optcr = FLASH->OPTCR;
	return true;
}

static void read(uint32_t addr, void *dst, size_t n)
{
	memcpy(dst, (const void *)(uintptr_t)addr, n);
}

static void backup_write(uint32_t v)
{
	RCC->APB1ENR |= RCC_APB1ENR_PWREN;
	(void)RCC->APB1ENR;
	PWR->CR |= PWR_CR_DBP;
	RTC->BKP19R = v;
	(void)RTC->BKP19R;
}

static bool reboot(uint8_t mode)
{
	if (mode == HAL_REBOOT_ROM_DFU) {
		backup_write(SYSBOOT_MAGIC);
		if (RTC->BKP19R != SYSBOOT_MAGIC) {
			return false;
		}
	} else if (mode != HAL_REBOOT_NORMAL) {
		return false;
	}
	__disable_irq();
	NVIC_SystemReset();
	return false;	// not reached
}

const hal_system_t stm32_system = {
	.chip = chip,
	.read = read,
	.reboot = reboot,
#if CONFIG_STUB_ENTRY
	.stub_region = stm32_stub_region,	// stub_entry.c
	.enter_stub = stm32_enter_stub,
	.stub_run_addr = 0x20000000u,		// the staging buffer is copied to the SRAM base at entry
#endif
};

static void spin_until(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
	for (uint32_t i = 0; i < SPIN_LIMIT && (*reg & mask) != want; i++) {
	}
}

void stm32_sysboot_check(void)
{
	RCC->APB1ENR |= RCC_APB1ENR_PWREN;
	(void)RCC->APB1ENR;
	if (RTC->BKP19R != SYSBOOT_MAGIC) {
		return;
	}
	backup_write(0);
	PWR->CR &= ~PWR_CR_DBP;
	if (RTC->BKP19R != 0) {
		return;	// can't clear it: entering anyway would loop into the ROM on every start
	}

	board_sysboot_prepare();

	__disable_irq();
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;
	for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}

	// Clocks back to HSI 16 MHz, HSE and the PLLs off and at their reset values (the stock bootloader leaves
	// HSE/PLL running; the ROM sets its own clocks up)
	RCC->CR |= RCC_CR_HSION;
	spin_until(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY);
	RCC->CFGR = 0;
	spin_until(&RCC->CFGR, RCC_CFGR_SWS, 0);
	RCC->CR &= ~(RCC_CR_PLLON | RCC_CR_PLLI2SON | RCC_CR_CSSON);
	spin_until(&RCC->CR, RCC_CR_PLLRDY | RCC_CR_PLLI2SRDY, 0);
	RCC->CR &= ~RCC_CR_HSEON;
	spin_until(&RCC->CR, RCC_CR_HSERDY, 0);
	RCC->CR &= ~RCC_CR_HSEBYP;
	RCC->PLLCFGR = 0x24003010u;
	RCC->PLLI2SCFGR = 0x20003000u;
	RCC->CIR = 0x00FF0000u;	// clear the ready flags, interrupts off
	RCC->CIR = 0;
	FLASH->ACR = 0;		// 0 wait states is right for 16 MHz; caches off

	// Peripherals to their reset state, except the GPIO ports (the board's pin levels above, the power latch)
	RCC->AHB1RSTR = RCC_AHB1RSTR_DMA1RST | RCC_AHB1RSTR_DMA2RST | RCC_AHB1RSTR_OTGHRST | RCC_AHB1RSTR_CRCRST;
	RCC->AHB2RSTR = 0xFFFFFFFFu;
	RCC->AHB3RSTR = 0xFFFFFFFFu;
	RCC->APB1RSTR = 0xFFFFFFFFu;
	RCC->APB2RSTR = 0xFFFFFFFFu;
	RCC->AHB1RSTR = 0;
	RCC->AHB2RSTR = 0;
	RCC->AHB3RSTR = 0;
	RCC->APB1RSTR = 0;
	RCC->APB2RSTR = 0;
	RCC->AHB1ENR &= 0x000001FFu;	// GPIOA..GPIOI only
	RCC->AHB2ENR = 0;
	RCC->AHB3ENR = 0;
	RCC->APB1ENR = 0;
	RCC->APB2ENR = RCC_APB2ENR_SYSCFGEN;
	(void)RCC->APB2ENR;

	uint32_t sp = *(volatile const uint32_t *)SYSMEM_BASE;
	uint32_t pc = *(volatile const uint32_t *)(SYSMEM_BASE + 4u);
	if (sp < 0x20000000u || sp > 0x20020000u || pc < SYSMEM_BASE || pc >= SYSMEM_END) {
		NVIC_SystemReset();	// not a ROM vector table: boot normally (the flag is gone)
	}
	SYSCFG->MEMRMP = SYSCFG_MEMRMP_MEM_MODE_0;	// system flash at 0
	SCB->VTOR = 0;
	__DSB();
	__ISB();
	__enable_irq();		// the reset state; nothing can interrupt now (NVIC cleared, SysTick off)
	__asm volatile("msr msp, %0\n\tbx %1" : : "r"(sp), "r"(pc) : "memory");
	__builtin_unreachable();
}
