// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F435/437 registers: only what the platform and its boards use, written from the AT32F435/437 reference manual
// (register names and offsets as the manual gives them) and checked against what the AT-D578UV's stock firmware writes
// (docs/spec/hardware-d578uv.md 10). The core is a Cortex-M4 with the FPU; CMSIS core_cm4.h supplies NVIC, SysTick, SCB
// and DWT. Most peripherals keep the STM32F4's register layout: the GPIO, timers, USART, DAC, EXINT and the watchdog.
// The ADC and the clock unit (CRM) are Artery's own.
#ifndef AT32F43X_H
#define AT32F43X_H

#include <stdint.h>

typedef enum {
	NonMaskableInt_IRQn = -14,
	HardFault_IRQn = -13,
	MemoryManagement_IRQn = -12,
	BusFault_IRQn = -11,
	UsageFault_IRQn = -10,
	SVCall_IRQn = -5,
	DebugMonitor_IRQn = -4,
	PendSV_IRQn = -2,
	SysTick_IRQn = -1,
	WWDT_IRQn = 0,
	PVM_IRQn = 1,
	TAMP_STAMP_IRQn = 2,
	ERTC_WKUP_IRQn = 3,
	FLASH_IRQn = 4,
	CRM_IRQn = 5,
	EXINT0_IRQn = 6,
	EXINT1_IRQn = 7,
	EXINT2_IRQn = 8,
	EXINT3_IRQn = 9,
	EXINT4_IRQn = 10,
	ADC1_2_3_IRQn = 18,
	EXINT9_5_IRQn = 23,
	TMR2_GLOBAL_IRQn = 28,
	TMR3_GLOBAL_IRQn = 29,
	TMR4_GLOBAL_IRQn = 30,
	USART1_IRQn = 37,
	USART2_IRQn = 38,
	USART3_IRQn = 39,
	EXINT15_10_IRQn = 40,
	OTGFS1_WKUP_IRQn = 42,
	TMR5_GLOBAL_IRQn = 50,
	UART4_IRQn = 52,
	UART5_IRQn = 53,
	TMR6_DAC_GLOBAL_IRQn = 54,
	TMR7_GLOBAL_IRQn = 55,
	OTGFS1_IRQn = 67,
	OTGFS2_IRQn = 77,
} IRQn_Type;

// Vector table entries after the 16 Cortex-M ones: every interrupt the AT32F435/437 has (the table reaches IRQ 110)
#define AT32_IRQ_COUNT		111

#define __CM4_REV		0x0001u
#define __MPU_PRESENT		1
#define __NVIC_PRIO_BITS	4
#define __Vendor_SysTickConfig	0
#define __FPU_PRESENT		1

#include "core_cm4.h"

#define AT32_REG(addr)		(*(volatile uint32_t *)(addr))

// Memory map
#define AT32_FLASH_BASE		0x08000000u
#define AT32_SRAM_BASE		0x20000000u

// Device signature: the flash size in KB and the 96-bit unique ID (the reference manual's device electronic signature)
#define AT32_FLASH_SIZE_KB	(*(volatile uint16_t *)0x1FFFF7E0u)
#define AT32_UID_BASE		0x1FFFF7E8u

// CRM: clock and reset management. The layout is the STM32F4's RCC, with Artery's PLL fields and two MISC registers
#define CRM_BASE		0x40023800u
#define CRM_CTRL		AT32_REG(CRM_BASE + 0x00u)
#define CRM_PLLCFG		AT32_REG(CRM_BASE + 0x04u)
#define CRM_CFG			AT32_REG(CRM_BASE + 0x08u)
#define CRM_CLKINT		AT32_REG(CRM_BASE + 0x0Cu)
#define CRM_AHBRST1		AT32_REG(CRM_BASE + 0x10u)
#define CRM_AHBRST2		AT32_REG(CRM_BASE + 0x14u)
#define CRM_APB1RST		AT32_REG(CRM_BASE + 0x20u)
#define CRM_APB2RST		AT32_REG(CRM_BASE + 0x24u)
#define CRM_AHBEN1		AT32_REG(CRM_BASE + 0x30u)
#define CRM_AHBEN2		AT32_REG(CRM_BASE + 0x34u)
#define CRM_APB1EN		AT32_REG(CRM_BASE + 0x40u)
#define CRM_APB2EN		AT32_REG(CRM_BASE + 0x44u)
#define CRM_CTRLSTS		AT32_REG(CRM_BASE + 0x74u)
#define CRM_MISC1		AT32_REG(CRM_BASE + 0xA0u)
#define CRM_MISC2		AT32_REG(CRM_BASE + 0xA4u)

#define CRM_CTRL_HICKEN		(1u << 0)
#define CRM_CTRL_HICKSTBL	(1u << 1)
#define CRM_CTRL_HEXTEN		(1u << 16)
#define CRM_CTRL_HEXTSTBL	(1u << 17)
#define CRM_CTRL_HEXTBYPS	(1u << 18)
#define CRM_CTRL_PLLEN		(1u << 24)
#define CRM_CTRL_PLLSTBL	(1u << 25)

// PLL: PLLCLK = source / MS x NS / FR. MS 1..15, NS up to 500, FR a power of two (code 0..5 = /1../32, the stock
// firmware's table). The stock firmware runs MS 1, NS 20, FR /2 from its 12 MHz crystal
#define CRM_PLLCFG_MS(v)	((uint32_t)(v) << 0)
#define CRM_PLLCFG_NS(v)	((uint32_t)(v) << 6)
#define CRM_PLLCFG_FR(code)	((uint32_t)(code) << 16)
#define CRM_PLLCFG_RCS_HEXT	(1u << 22)

#define CRM_CFG_SCLKSEL_MASK	(3u << 0)
#define CRM_CFG_SCLKSEL_PLL	(2u << 0)
#define CRM_CFG_SCLKSTS_MASK	(3u << 2)
#define CRM_CFG_SCLKSTS_PLL	(2u << 2)
#define CRM_CFG_AHBDIV_MASK	(0xFu << 4)
#define CRM_CFG_APB1DIV_MASK	(7u << 10)
#define CRM_CFG_APB1DIV_2	(4u << 10)
#define CRM_CFG_APB2DIV_MASK	(7u << 13)
#define CRM_CFG_APB2DIV_2	(4u << 13)

// MISC2: the system clock's automatic step while it switches to or from a fast clock
#define CRM_MISC2_AUTO_STEP_MASK	(3u << 4)
#define CRM_MISC2_AUTO_STEP_ON		(3u << 4)
// MISC1 bits 12..14: the stock firmware sets them, in the order 12, 14, 13, to run the USB from the 48 MHz HICK
// (what each bit does is not checked against the manual)
#define CRM_MISC1_USB_HICK_A		(1u << 12)
#define CRM_MISC1_USB_HICK_B		(1u << 14)
#define CRM_MISC1_USB_HICK_C		(1u << 13)

// ACC: the HICK's automatic trim from the USB start of frame, which keeps it at 48 MHz for the USB. C1..C3 are the
// stock firmware's window (7980, 8000, 8020); CTRL1 bit 1 then bit 0 starts it
#define ACC_BASE		0x40017400u
#define ACC_CTRL1		AT32_REG(ACC_BASE + 0x04u)
#define ACC_C1			AT32_REG(ACC_BASE + 0x0Cu)
#define ACC_C2			AT32_REG(ACC_BASE + 0x10u)
#define ACC_C3			AT32_REG(ACC_BASE + 0x14u)

#define CRM_AHBEN1_GPIOAEN	(1u << 0)	// GPIOA..GPIOH: bits 0..7
#define CRM_AHBEN1_DMA1EN	(1u << 22)
#define CRM_AHBEN1_DMA2EN	(1u << 24)
#define CRM_AHBEN2_OTGFS1EN	(1u << 7)
#define CRM_AHBRST2_OTGFS1RST	(1u << 7)
#define CRM_APB1EN_TMR2EN	(1u << 0)	// TMR2..TMR7: bits 0..5
#define CRM_APB1EN_WWDTEN	(1u << 11)
#define CRM_APB1EN_USART2EN	(1u << 17)
#define CRM_APB1EN_USART3EN	(1u << 18)
#define CRM_APB1EN_UART4EN	(1u << 19)
#define CRM_APB1EN_UART5EN	(1u << 20)
#define CRM_APB1EN_PWCEN	(1u << 28)
#define CRM_APB1EN_DACEN	(1u << 29)
#define CRM_APB2EN_USART1EN	(1u << 4)
#define CRM_APB2EN_USART6EN	(1u << 5)
#define CRM_APB2EN_ADC1EN	(1u << 8)
#define CRM_APB2EN_SCFGEN	(1u << 14)
#define CRM_APB2EN_ACCEN	(1u << 29)

// CTRLSTS: the reset source flags, cleared together by RSTFC
#define CRM_CTRLSTS_RSTFC	(1u << 24)
#define CRM_CTRLSTS_NRSTF	(1u << 26)	// the NRST pin
#define CRM_CTRLSTS_PORRSTF	(1u << 27)	// power on
#define CRM_CTRLSTS_SWRSTF	(1u << 28)	// software (SYSRESETREQ)
#define CRM_CTRLSTS_WDTRSTF	(1u << 29)	// watchdog
#define CRM_CTRLSTS_WWDTRSTF	(1u << 30)
#define CRM_CTRLSTS_LPRSTF	(1u << 31)

// PWC: the core voltage regulator's output (LDOOV), raised before the core runs fast
#define PWC_BASE		0x40007000u
#define PWC_LDOOV		AT32_REG(PWC_BASE + 0x10u)
#define PWC_LDOOV_MASK		(7u << 0)

// FLASH: the flash controller. DIVR divides the system clock for the flash interface
#define FLASH_BASE		0x40023C00u
#define FLASH_DIVR		AT32_REG(FLASH_BASE + 0x60u)
#define FLASH_DIVR_MASK		(3u << 0)

// GPIO: the STM32F4's layout. CFGR two bits a pin (0 input, 1 output, 2 multiplexed, 3 analog), OMODE (1 open drain),
// ODRVR the drive strength, PULL two bits a pin (0 none, 1 up, 2 down), SCR sets the low half and clears the high
// half, CLR clears, MUXL / MUXH four bits a pin: the multiplexed function
typedef struct {
	volatile uint32_t CFGR;
	volatile uint32_t OMODE;
	volatile uint32_t ODRVR;
	volatile uint32_t PULL;
	volatile uint32_t IDT;
	volatile uint32_t ODT;
	volatile uint32_t SCR;
	volatile uint32_t WPR;
	volatile uint32_t MUXL;
	volatile uint32_t MUXH;
	volatile uint32_t CLR;
	uint32_t reserved[4];
	volatile uint32_t HDRV;
} at32_gpio_t;

#define GPIOA			((at32_gpio_t *)0x40020000u)
#define GPIOB			((at32_gpio_t *)0x40020400u)
#define GPIOC			((at32_gpio_t *)0x40020800u)
#define GPIOD			((at32_gpio_t *)0x40020C00u)
#define GPIOE			((at32_gpio_t *)0x40021000u)
#define GPIOF			((at32_gpio_t *)0x40021400u)
#define GPIOG			((at32_gpio_t *)0x40021800u)
#define GPIOH			((at32_gpio_t *)0x40021C00u)
#define GPIO_PORT_INDEX(p)	(((uint32_t)(p) - 0x40020000u) / 0x400u)

// SCFG: the EXINT line sources, four bits a line (the port index)
#define SCFG_BASE		0x40013800u
#define SCFG_EXINTC(n)		AT32_REG(SCFG_BASE + 0x08u + 4u * (n))	// lines 4n..4n+3

// EXINT: the STM32's EXTI layout
#define EXINT_BASE		0x40013C00u
#define EXINT_INTEN		AT32_REG(EXINT_BASE + 0x00u)
#define EXINT_EVTEN		AT32_REG(EXINT_BASE + 0x04u)
#define EXINT_POLCFG1		AT32_REG(EXINT_BASE + 0x08u)	// rising edge
#define EXINT_POLCFG2		AT32_REG(EXINT_BASE + 0x0Cu)	// falling edge
#define EXINT_SWTRG		AT32_REG(EXINT_BASE + 0x10u)
#define EXINT_INTSTS		AT32_REG(EXINT_BASE + 0x14u)	// write 1 to clear

// TMR2..TMR7 on APB1: the STM32's general-purpose timer layout. Their clock is twice APB1 when APB1 is divided (it
// always is here), so the core clock
#define TMR_BASE(n)		(0x40000000u + 0x400u * ((n) - 2u))

// USART: STS, DT, BAUDR, CTRL1..3 (the STM32F1's layout; BAUDR is the plain clock / baud divider)
typedef struct {
	volatile uint32_t STS;
	volatile uint32_t DT;
	volatile uint32_t BAUDR;
	volatile uint32_t CTRL1;
	volatile uint32_t CTRL2;
	volatile uint32_t CTRL3;
	volatile uint32_t GDIV;
} at32_usart_t;

#define USART1			((at32_usart_t *)0x40011000u)	// APB2
#define USART2			((at32_usart_t *)0x40004400u)	// APB1
#define USART3			((at32_usart_t *)0x40004800u)
#define UART4			((at32_usart_t *)0x40004C00u)
#define UART5			((at32_usart_t *)0x40005000u)
#define USART_STS_PERR		(1u << 0)
#define USART_STS_FERR		(1u << 1)
#define USART_STS_NERR		(1u << 2)
#define USART_STS_ROERR		(1u << 3)
#define USART_STS_RDBF		(1u << 5)
#define USART_STS_TDC		(1u << 6)
#define USART_STS_TDBE		(1u << 7)
#define USART_CTRL1_REN		(1u << 2)
#define USART_CTRL1_TEN		(1u << 3)
#define USART_CTRL1_RDBFIEN	(1u << 5)
#define USART_CTRL1_UEN		(1u << 13)

// WDT: the watchdog on the 40 kHz LICK, the STM32's IWDG layout and keys
#define WDT_BASE		0x40003000u
#define WDT_CMD			AT32_REG(WDT_BASE + 0x00u)
#define WDT_DIV			AT32_REG(WDT_BASE + 0x04u)
#define WDT_RLD			AT32_REG(WDT_BASE + 0x08u)
#define WDT_STS			AT32_REG(WDT_BASE + 0x0Cu)

// DEBUG: peripherals that stop while a debugger halts the core
#define DEBUG_APB1_PAUSE	AT32_REG(0xE0042008u)
#define DEBUG_APB1_PAUSE_WDT	(1u << 12)

// DAC: two 12-bit channels on PA4 (DAC1) and PA5 (DAC2), the STM32's layout
#define DAC_BASE		0x40007400u
#define DAC_CTRL		AT32_REG(DAC_BASE + 0x00u)
#define DAC_D1DTH12R		AT32_REG(DAC_BASE + 0x08u)
#define DAC_D2DTH12R		AT32_REG(DAC_BASE + 0x14u)
#define DAC_CTRL_D1EN		(1u << 0)
#define DAC_CTRL_D2EN		(1u << 16)

// ADC1 and the common block: Artery's ADC (not the STM32F1's), regular ("ordinary") sequence by software trigger
#define ADC1_BASE		0x40012000u
#define ADC_STS(b)		AT32_REG((b) + 0x00u)
#define ADC_CTRL1(b)		AT32_REG((b) + 0x04u)
#define ADC_CTRL2(b)		AT32_REG((b) + 0x08u)
#define ADC_SPT1(b)		AT32_REG((b) + 0x0Cu)	// channels 10..18, three bits each
#define ADC_SPT2(b)		AT32_REG((b) + 0x10u)	// channels 0..9
#define ADC_OSQ1(b)		AT32_REG((b) + 0x2Cu)	// the sequence length, OCLEN [23:20]
#define ADC_OSQ3(b)		AT32_REG((b) + 0x34u)	// the first conversion's channel, OSN1 [4:0]
#define ADC_ODT(b)		AT32_REG((b) + 0x4Cu)
#define ADC_STS_OCCE		(1u << 1)	// a regular conversion ended (cleared by reading ODT)
#define ADC_STS_RDY		(1u << 6)	// the ADC is ready after ADCEN
#define ADC_CTRL2_ADCEN		(1u << 0)
#define ADC_CTRL2_ADCAL		(1u << 2)
#define ADC_CTRL2_ADCALINIT	(1u << 3)
#define ADC_CTRL2_OCSWTRG	(1u << 30)	// start the regular sequence
// The preempted group: one channel on its own trigger and data register (adc.c's sampling channel)
#define ADC_PSQ(b)		AT32_REG((b) + 0x38u)	// PCLEN [21:20]; with PCLEN 0 the one conversion is PSN4's channel [19:15]
#define ADC_PDT1(b)		AT32_REG((b) + 0x3Cu)
#define ADC_CTRL2_PCSWTRG	(1u << 22)	// start the preempted group (its external trigger stays off: PCETE 0)
#define ADCCOM_CCTRL		AT32_REG(0x40012304u)
#define ADCCOM_CCTRL_ADCDIV_MASK	(0xFu << 16)
#define ADCCOM_CCTRL_ADCDIV_4	(2u << 16)	// HCLK / 4: 30 MHz at 120 MHz (the ADC runs up to 80 MHz)

// OTGFS1: the Synopsys DWC2 full-speed core (TinyUSB's dwc2 driver) on PA11 / PA12. GCCFG is its general control
#define OTGFS1_BASE		0x50000000u
#define OTGFS1_GCCFG		AT32_REG(OTGFS1_BASE + 0x38u)
#define OTGFS_GCCFG_PWRDOWN	(1u << 16)	// the transceiver on
#define OTGFS_GCCFG_VBUSIG	(1u << 21)	// ignore VBUS: the session is always valid

#endif
