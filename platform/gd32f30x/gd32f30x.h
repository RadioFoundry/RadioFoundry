// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD32F30x registers: only what the platform and its boards use, written from the GD32F30x user manual (register
// names and offsets as the manual gives them). The interrupt numbers are the high-density and extra-density parts'
// (GD32F303xE/G/I/K). The core is a Cortex-M4 with the FPU; CMSIS core_cm4.h supplies NVIC, SysTick, SCB and DWT.
#ifndef GD32F30X_H
#define GD32F30X_H

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
	WWDGT_IRQn = 0,
	LVD_IRQn = 1,
	TAMPER_IRQn = 2,
	RTC_IRQn = 3,
	FMC_IRQn = 4,
	RCU_CTC_IRQn = 5,
	EXTI0_IRQn = 6,
	EXTI1_IRQn = 7,
	EXTI2_IRQn = 8,
	EXTI3_IRQn = 9,
	EXTI4_IRQn = 10,
	DMA0_Channel0_IRQn = 11,
	DMA0_Channel1_IRQn = 12,
	DMA0_Channel2_IRQn = 13,
	DMA0_Channel3_IRQn = 14,
	DMA0_Channel4_IRQn = 15,
	DMA0_Channel5_IRQn = 16,
	DMA0_Channel6_IRQn = 17,
	ADC0_1_IRQn = 18,
	USBD_HP_CAN0_TX_IRQn = 19,
	USBD_LP_CAN0_RX0_IRQn = 20,
	CAN0_RX1_IRQn = 21,
	CAN0_EWMC_IRQn = 22,
	EXTI5_9_IRQn = 23,
	TIMER0_BRK_IRQn = 24,
	TIMER0_UP_IRQn = 25,
	TIMER0_TRG_CMT_IRQn = 26,
	TIMER0_Channel_IRQn = 27,
	TIMER1_IRQn = 28,
	TIMER2_IRQn = 29,
	TIMER3_IRQn = 30,
	I2C0_EV_IRQn = 31,
	I2C0_ER_IRQn = 32,
	I2C1_EV_IRQn = 33,
	I2C1_ER_IRQn = 34,
	SPI0_IRQn = 35,
	SPI1_IRQn = 36,
	USART0_IRQn = 37,
	USART1_IRQn = 38,
	USART2_IRQn = 39,
	EXTI10_15_IRQn = 40,
	RTC_Alarm_IRQn = 41,
	USBD_WKUP_IRQn = 42,
	TIMER7_BRK_IRQn = 43,
	TIMER7_UP_IRQn = 44,
	TIMER7_TRG_CMT_IRQn = 45,
	TIMER7_Channel_IRQn = 46,
	ADC2_IRQn = 47,
	EXMC_IRQn = 48,
	SDIO_IRQn = 49,
	TIMER4_IRQn = 50,
	SPI2_IRQn = 51,
	UART3_IRQn = 52,
	UART4_IRQn = 53,
	TIMER5_IRQn = 54,
	TIMER6_IRQn = 55,
	DMA1_Channel0_IRQn = 56,
	DMA1_Channel1_IRQn = 57,
	DMA1_Channel2_IRQn = 58,
	DMA1_Channel3_4_IRQn = 59,
} IRQn_Type;

#define GD32_IRQ_COUNT		60

#define __CM4_REV		0x0001u
#define __MPU_PRESENT		0
#define __NVIC_PRIO_BITS	4
#define __Vendor_SysTickConfig	0
#define __FPU_PRESENT		1

#include "core_cm4.h"

#define GD32_REG(addr)		(*(volatile uint32_t *)(addr))

// Memory map
#define GD32_FLASH_BASE		0x08000000u
#define GD32_SRAM_BASE		0x20000000u
#define GD32_APB1_BASE		0x40000000u
#define GD32_APB2_BASE		0x40010000u
#define GD32_AHB_BASE		0x40018000u

// Device signature: flash and SRAM size in KB, and the 96-bit unique ID
#define GD32_FLASH_SIZE_KB	(*(volatile uint16_t *)0x1FFFF7E0u)
#define GD32_SRAM_SIZE_KB	(*(volatile uint16_t *)0x1FFFF7E2u)
#define GD32_UID_BASE		0x1FFFF7E8u

// RCU: reset and clock unit
#define RCU_BASE		0x40021000u
#define RCU_CTL			GD32_REG(RCU_BASE + 0x00u)
#define RCU_CFG0		GD32_REG(RCU_BASE + 0x04u)
#define RCU_INT			GD32_REG(RCU_BASE + 0x08u)
#define RCU_APB2RST		GD32_REG(RCU_BASE + 0x0Cu)
#define RCU_APB1RST		GD32_REG(RCU_BASE + 0x10u)
#define RCU_AHBEN		GD32_REG(RCU_BASE + 0x14u)
#define RCU_APB2EN		GD32_REG(RCU_BASE + 0x18u)
#define RCU_APB1EN		GD32_REG(RCU_BASE + 0x1Cu)
#define RCU_BDCTL		GD32_REG(RCU_BASE + 0x20u)
#define RCU_RSTSCK		GD32_REG(RCU_BASE + 0x24u)
#define RCU_AHBRST		GD32_REG(RCU_BASE + 0x28u)
#define RCU_CFG1		GD32_REG(RCU_BASE + 0x2Cu)

#define RCU_CTL_IRC8MEN		(1u << 0)
#define RCU_CTL_IRC8MSTB	(1u << 1)
#define RCU_CTL_HXTALEN		(1u << 16)
#define RCU_CTL_HXTALSTB	(1u << 17)
#define RCU_CTL_HXTALBPS	(1u << 18)
#define RCU_CTL_CKMEN		(1u << 19)
#define RCU_CTL_PLLEN		(1u << 24)
#define RCU_CTL_PLLSTB		(1u << 25)

#define RCU_CFG0_SCS_MASK	(3u << 0)
#define RCU_CFG0_SCS_IRC8M	(0u << 0)
#define RCU_CFG0_SCS_HXTAL	(1u << 0)
#define RCU_CFG0_SCS_PLL	(2u << 0)
#define RCU_CFG0_SCSS_MASK	(3u << 2)
#define RCU_CFG0_SCSS_PLL	(2u << 2)
#define RCU_CFG0_AHBPSC_MASK	(0xFu << 4)
#define RCU_CFG0_APB1PSC_MASK	(7u << 8)
#define RCU_CFG0_APB1PSC_DIV2	(4u << 8)
#define RCU_CFG0_APB2PSC_MASK	(7u << 11)
#define RCU_CFG0_ADCPSC_MASK	((3u << 14) | (1u << 28))
#define RCU_CFG0_ADCPSC_DIV6	(2u << 14)	// with ADCPSC[2] clear
#define RCU_CFG0_PLLSEL		(1u << 16)	// the PLL runs from HXTAL (through PREDV0), not IRC8M / 2
#define RCU_CFG0_PREDV0		(1u << 17)	// HXTAL / 2 into the PLL
#define RCU_CFG0_PLLMF_MASK	((0xFu << 18) | (1u << 27) | (1u << 30))
#define RCU_CFG0_USBDPSC_MASK	((3u << 22) | (1u << 29))
#define RCU_CFG0_USBDPSC_DIV2_5	(2u << 22)	// CK_PLL / 2.5: 48 MHz from 120 MHz

// The PLL multiplier field for x mul (2..63): PLLMF[3:0] at 18, PLLMF[4] at 27, PLLMF[5] at 30 (the user manual's
// table: x2..x16 are 0..14 with PLLMF[4] clear, x17..x32 are 0..15 with it set; x33..x63 set PLLMF[5])
static inline uint32_t rcu_pllmf(uint32_t mul)
{
	uint32_t v = mul < 17u ? mul - 2u : mul - 1u;
	return ((v & 0xFu) << 18) | (((v >> 4) & 1u) << 27) | (((v >> 5) & 1u) << 30);
}

#define RCU_AHBEN_DMA0EN	(1u << 0)
#define RCU_AHBEN_DMA1EN	(1u << 1)
#define RCU_APB2EN_AFEN		(1u << 0)
#define RCU_APB2EN_PAEN		(1u << 2)	// PA..PG: bits 2..8
#define RCU_APB2EN_ADC0EN	(1u << 9)
#define RCU_APB2EN_TIMER0EN	(1u << 11)
#define RCU_APB2EN_USART0EN	(1u << 14)
#define RCU_APB1EN_TIMER1EN	(1u << 0)	// TIMER1..TIMER6: bits 0..5
#define RCU_APB1EN_TIMER3EN	(1u << 2)
#define RCU_APB1EN_TIMER4EN	(1u << 3)
#define RCU_APB1EN_WWDGTEN	(1u << 11)
#define RCU_APB1EN_USART1EN	(1u << 17)
#define RCU_APB1EN_USART2EN	(1u << 18)
#define RCU_APB1EN_UART3EN	(1u << 19)
#define RCU_APB1EN_UART4EN	(1u << 20)
#define RCU_APB1EN_USBDEN	(1u << 23)
#define RCU_APB1EN_PMUEN	(1u << 28)
#define RCU_APB1EN_DACEN	(1u << 29)
#define RCU_APB1RST_USBDRST	(1u << 23)
#define RCU_BDCTL_BKPRST	(1u << 16)

// RSTSCK: the reset source flags, cleared together by RSTFC
#define RCU_RSTSCK_RSTFC	(1u << 24)
#define RCU_RSTSCK_EPRSTF	(1u << 26)	// the NRST pin
#define RCU_RSTSCK_PORRSTF	(1u << 27)	// power on
#define RCU_RSTSCK_SWRSTF	(1u << 28)	// software (SYSRESETREQ)
#define RCU_RSTSCK_FWDGTRSTF	(1u << 29)	// free watchdog
#define RCU_RSTSCK_WWDGTRSTF	(1u << 30)
#define RCU_RSTSCK_LPRSTF	(1u << 31)

// PMU: the high-driver mode the core needs above 108 MHz
#define PMU_BASE		0x40007000u
#define PMU_CTL			GD32_REG(PMU_BASE + 0x00u)
#define PMU_CS			GD32_REG(PMU_BASE + 0x04u)
#define PMU_CTL_BKPWEN		(1u << 8)	// backup domain writes allowed
#define PMU_CTL_LDOVS_MASK	(3u << 14)
#define PMU_CTL_LDOVS_HIGH	(3u << 14)
#define PMU_CTL_HDEN		(1u << 16)
#define PMU_CTL_HDS		(1u << 17)
#define PMU_CS_HDRF		(1u << 16)
#define PMU_CS_HDSRF		(1u << 17)

// Backup registers: 42 half-words in the backup domain, kept across a system reset (not a power cut)
#define BKP_BASE		0x40006C00u
#define BKP_DATA(n)		(*(volatile uint32_t *)(BKP_BASE + ((n) < 10u ? 0x04u + 4u * (n) : 0x40u + 4u * ((n) - 10u))))

// FMC: the flash controller (the wait states are automatic on the GD32F30x: the code area runs with no waits)
#define FMC_BASE		0x40022000u

// GPIO: per pin four bits in CTL0 (pins 0-7) and CTL1 (8-15): MD[1:0] (0 input, 1 output 10 MHz, 2 output 2 MHz, 3
// output 50 MHz) and CTL[1:0] (input: 0 analog, 1 floating, 2 pull up or down by OCTL; output: 0 push-pull,
// 1 open drain, 2 alternate push-pull, 3 alternate open drain). SPD (output, MD 3): 120 MHz edges
typedef struct {
	volatile uint32_t CTL0;
	volatile uint32_t CTL1;
	volatile uint32_t ISTAT;
	volatile uint32_t OCTL;
	volatile uint32_t BOP;		// low half sets, high half clears
	volatile uint32_t BC;		// clears
	volatile uint32_t LOCK;
	uint32_t reserved[8];
	volatile uint32_t SPD;
} gd32_gpio_t;

#define GPIOA			((gd32_gpio_t *)0x40010800u)
#define GPIOB			((gd32_gpio_t *)0x40010C00u)
#define GPIOC			((gd32_gpio_t *)0x40011000u)
#define GPIOD			((gd32_gpio_t *)0x40011400u)
#define GPIOE			((gd32_gpio_t *)0x40011800u)
#define GPIOF			((gd32_gpio_t *)0x40011C00u)
#define GPIOG			((gd32_gpio_t *)0x40012000u)
#define GPIO_PORT_INDEX(p)	(((uint32_t)(p) - 0x40010800u) / 0x400u)

// AFIO: remaps and the EXTI line sources
#define AFIO_BASE		0x40010000u
#define AFIO_EC			GD32_REG(AFIO_BASE + 0x00u)
#define AFIO_PCF0		GD32_REG(AFIO_BASE + 0x04u)
#define AFIO_EXTISS(n)		GD32_REG(AFIO_BASE + 0x08u + 4u * (n))	// lines 4n..4n+3, four bits each
#define AFIO_PCF1		GD32_REG(AFIO_BASE + 0x1Cu)
#define AFIO_PCF0_SWJ_MASK	(7u << 24)
#define AFIO_PCF0_SWJ_NONJTRST	(1u << 24)	// full SWD and JTAG without NJTRST
#define AFIO_PCF0_SWJ_SWDP	(2u << 24)	// SWD only: PA15, PB3 and PB4 are GPIO
#define AFIO_PCF0_SWJ_OFF	(4u << 24)	// SWD off too: PA13 and PA14 are GPIO

// EXTI
#define EXTI_BASE		0x40010400u
#define EXTI_INTEN		GD32_REG(EXTI_BASE + 0x00u)
#define EXTI_EVEN		GD32_REG(EXTI_BASE + 0x04u)
#define EXTI_RTEN		GD32_REG(EXTI_BASE + 0x08u)
#define EXTI_FTEN		GD32_REG(EXTI_BASE + 0x0Cu)
#define EXTI_SWIEV		GD32_REG(EXTI_BASE + 0x10u)
#define EXTI_PD			GD32_REG(EXTI_BASE + 0x14u)

// TIMER1..TIMER6 on APB1: the update-event registers, and channel 3's compare output (TIMER1..TIMER4). Their clock is twice APB1 when APB1 is divided (it always
// is here: gd32_apb1_hz), so the core clock
#define TIMER_BASE(n)		(0x40000000u + 0x400u * ((n) - 1u))
#define TIMER_CTL0(b)		GD32_REG((b) + 0x00u)
#define TIMER_DMAINTEN(b)	GD32_REG((b) + 0x0Cu)
#define TIMER_INTF(b)		GD32_REG((b) + 0x10u)	// flags cleared by writing 0
#define TIMER_SWEVG(b)		GD32_REG((b) + 0x14u)
#define TIMER_CHCTL1(b)		GD32_REG((b) + 0x1Cu)
#define TIMER_CHCTL2(b)		GD32_REG((b) + 0x20u)
#define TIMER_CNT(b)		GD32_REG((b) + 0x24u)
#define TIMER_PSC(b)		GD32_REG((b) + 0x28u)
#define TIMER_CAR(b)		GD32_REG((b) + 0x2Cu)
#define TIMER_CH3CV(b)		GD32_REG((b) + 0x40u)
#define TIMER_CTL0_CEN		(1u << 0)
#define TIMER_CTL0_UPS		(1u << 2)	// only an overflow raises the update interrupt, not SWEVG
#define TIMER_CTL0_SPM		(1u << 3)	// single pulse: the counter stops at the update
#define TIMER_CTL0_ARSE		(1u << 7)	// CAR is buffered
#define TIMER_CHCTL1_CH3_PWM1	(7u << 12)	// CH3 compare mode 111: PWM mode 1
#define TIMER_CHCTL1_CH3_PRELOAD	(1u << 11)
#define TIMER_CHCTL2_CH3EN	(1u << 12)
#define TIMER_DMAINTEN_UPIE	(1u << 0)
#define TIMER_SWEVG_UPG		(1u << 0)	// loads PSC and CAR

// USART: STAT0, DATA, BAUD, CTL0..2 (the layout of the STM32F1's USART)
typedef struct {
	volatile uint32_t STAT0;
	volatile uint32_t DATA;
	volatile uint32_t BAUD;
	volatile uint32_t CTL0;
	volatile uint32_t CTL1;
	volatile uint32_t CTL2;
	volatile uint32_t GP;
} gd32_usart_t;

#define USART0			((gd32_usart_t *)0x40013800u)	// APB2
#define USART1			((gd32_usart_t *)0x40004400u)	// APB1
#define USART2			((gd32_usart_t *)0x40004800u)
#define UART3			((gd32_usart_t *)0x40004C00u)
#define UART4			((gd32_usart_t *)0x40005000u)
#define USART_STAT0_PERR	(1u << 0)
#define USART_STAT0_FERR	(1u << 1)
#define USART_STAT0_NERR	(1u << 2)
#define USART_STAT0_ORERR	(1u << 3)
#define USART_STAT0_IDLEF	(1u << 4)
#define USART_STAT0_RBNE	(1u << 5)
#define USART_STAT0_TC		(1u << 6)
#define USART_STAT0_TBE		(1u << 7)
#define USART_CTL0_REN		(1u << 2)
#define USART_CTL0_TEN		(1u << 3)
#define USART_CTL0_RBNEIE	(1u << 5)
#define USART_CTL0_TBEIE	(1u << 7)
#define USART_CTL0_UEN		(1u << 13)

// FWDGT: the free watchdog, on the 40 kHz IRC40K
#define FWDGT_BASE		0x40003000u
#define FWDGT_CTL		GD32_REG(FWDGT_BASE + 0x00u)
#define FWDGT_PSC		GD32_REG(FWDGT_BASE + 0x04u)
#define FWDGT_RLD		GD32_REG(FWDGT_BASE + 0x08u)
#define FWDGT_STAT		GD32_REG(FWDGT_BASE + 0x0Cu)

// DBG: peripherals that stop while a debugger halts the core
#define DBG_CTL0		GD32_REG(0xE0042004u)
#define DBG_CTL0_FWDGT_HOLD	(1u << 8)

// DAC: two 12-bit channels on PA4 (DAC0) and PA5 (DAC1)
#define DAC_BASE		0x40007400u
#define DAC_CTL			GD32_REG(DAC_BASE + 0x00u)
#define DAC0_R12DH		GD32_REG(DAC_BASE + 0x08u)
#define DAC1_R12DH		GD32_REG(DAC_BASE + 0x14u)
#define DAC_CTL_DEN0		(1u << 0)
#define DAC_CTL_DBOFF0		(1u << 1)
#define DAC_CTL_DEN1		(1u << 16)

// ADC0: regular sequence, the layout of the STM32F1's ADC
#define ADC0_BASE		0x40012400u
#define ADC_STAT(b)		GD32_REG((b) + 0x00u)
#define ADC_CTL0(b)		GD32_REG((b) + 0x04u)
#define ADC_CTL1(b)		GD32_REG((b) + 0x08u)
#define ADC_SAMPT0(b)		GD32_REG((b) + 0x0Cu)	// channels 10..17
#define ADC_SAMPT1(b)		GD32_REG((b) + 0x10u)	// channels 0..9
#define ADC_RSQ0(b)		GD32_REG((b) + 0x2Cu)
#define ADC_RSQ1(b)		GD32_REG((b) + 0x30u)
#define ADC_RSQ2(b)		GD32_REG((b) + 0x34u)
#define ADC_RDATA(b)		GD32_REG((b) + 0x4Cu)
#define ADC_STAT_EOC		(1u << 1)
#define ADC_CTL1_ADCON		(1u << 0)
#define ADC_CTL1_RSTCLB		(1u << 3)
#define ADC_CTL1_CLB		(1u << 2)
#define ADC_CTL1_ETERC		(1u << 20)
#define ADC_CTL1_ETSRC_SW	(7u << 17)
#define ADC_CTL1_SWRCST		(1u << 22)
// The inserted group: one channel on its own trigger and data register (adc.c's sampling channel)
#define ADC_ISQ(b)		GD32_REG((b) + 0x38u)	// IL [21:20]; with IL 0 the one conversion is ISQ3's channel [19:15]
#define ADC_IDATA0(b)		GD32_REG((b) + 0x3Cu)
#define ADC_CTL1_ETSIC_SW	(7u << 12)	// the inserted trigger is SWICST
#define ADC_CTL1_ETEIC		(1u << 15)
#define ADC_CTL1_SWICST		(1u << 21)

// USBD: the full-speed device core and its 512-byte packet buffer (the STM32F103's USB device, register for register)
#define USBD_BASE		0x40005C00u
#define USBD_PMA_BASE		0x40006000u

#endif
