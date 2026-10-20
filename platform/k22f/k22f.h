// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// NXP Kinetis K22F (MK22FN512, Cortex-M4F): the registers the platform drivers touch, by address and bit, with the
// names of the K22F reference manual (K22P121M120SF7RM). Only what ODR uses; add fields as drivers need them.
#ifndef K22F_H
#define K22F_H

#include <stdint.h>

// --- Interrupt numbers (reference manual 3.2.2.3, "Interrupt channel assignments") ---
typedef enum IRQn {
	NonMaskableInt_IRQn	= -14,
	HardFault_IRQn		= -13,
	MemoryManagement_IRQn	= -12,
	BusFault_IRQn		= -11,
	UsageFault_IRQn		= -10,
	SVCall_IRQn		= -5,
	DebugMonitor_IRQn	= -4,
	PendSV_IRQn		= -2,
	SysTick_IRQn		= -1,
	DMA0_IRQn		= 0,	// DMA channel n: n (0..15)
	DMA_Error_IRQn		= 16,
	FTF_IRQn		= 18,
	LVD_LVW_IRQn		= 20,
	LLWU_IRQn		= 21,
	WDOG_EWM_IRQn		= 22,
	I2C0_IRQn		= 24,
	I2C1_IRQn		= 25,
	SPI0_IRQn		= 26,
	SPI1_IRQn		= 27,
	I2S0_Tx_IRQn		= 28,
	I2S0_Rx_IRQn		= 29,
	UART0_RX_TX_IRQn	= 31,
	ADC0_IRQn		= 39,
	RTC_IRQn		= 46,
	PIT0_IRQn		= 48,	// PIT channel n: 48 + n (0..3)
	PDB0_IRQn		= 52,
	USB0_IRQn		= 53,
	DAC0_IRQn		= 56,
	MCG_IRQn		= 57,
	LPTMR0_IRQn		= 58,
	PORTA_IRQn		= 59,	// PORTx: 59 + port index (A..E)
	PORTB_IRQn		= 60,
	PORTC_IRQn		= 61,
	PORTD_IRQn		= 62,
	PORTE_IRQn		= 63,
	ADC1_IRQn		= 73,
} IRQn_Type;

#define K22F_IRQ_COUNT		86	// device vectors after the 16 core ones

// Cortex-M4 with FPU, no MPU, 4 priority bits
#define __CM4_REV		0x0001u
#define __MPU_PRESENT		0
#define __NVIC_PRIO_BITS	4
#define __Vendor_SysTickConfig	0
#define __FPU_PRESENT		1

#include "core_cm4.h"
#include "k22f_pin.h"

#define K22F_REG8(a)		(*(volatile uint8_t *)(uintptr_t)(a))
#define K22F_REG16(a)		(*(volatile uint16_t *)(uintptr_t)(a))
#define K22F_REG32(a)		(*(volatile uint32_t *)(uintptr_t)(a))

// --- Memory ---
#define K22F_FLASH_SIZE		0x00080000u	// 512 KB program flash
#define K22F_FLASH_SECTOR	0x800u		// 2 KB erase unit
#define K22F_SRAM_L		0x1FFF0000u	// 64 KB below 0x20000000 (code and system bus)
#define K22F_SRAM_U		0x20000000u	// 64 KB above
#define K22F_SRAM_END		0x20010000u

// --- SIM: clock gates, dividers, chip identity (chapter 12) ---
#define SIM_BASE		0x40047000u
#define SIM_SOPT1		K22F_REG32(SIM_BASE + 0x0000)
#define SIM_SOPT2		K22F_REG32(SIM_BASE + 0x1004)
#define SIM_SDID		K22F_REG32(SIM_BASE + 0x1024)
#define SIM_SCGC4		K22F_REG32(SIM_BASE + 0x1034)
#define SIM_SCGC5		K22F_REG32(SIM_BASE + 0x1038)
#define SIM_SCGC6		K22F_REG32(SIM_BASE + 0x103C)
#define SIM_SCGC7		K22F_REG32(SIM_BASE + 0x1040)
#define SIM_CLKDIV1		K22F_REG32(SIM_BASE + 0x1044)
#define SIM_CLKDIV2		K22F_REG32(SIM_BASE + 0x1048)
#define SIM_FCFG1		K22F_REG32(SIM_BASE + 0x104C)
#define SIM_UIDH		K22F_REG32(SIM_BASE + 0x1054)
#define SIM_UIDMH		K22F_REG32(SIM_BASE + 0x1058)
#define SIM_UIDML		K22F_REG32(SIM_BASE + 0x105C)
#define SIM_UIDL		K22F_REG32(SIM_BASE + 0x1060)

#define SIM_SOPT2_PLLFLLSEL(x)	((uint32_t)(x) << 16)	// 0 FLL, 1 PLL, 3 IRC48M
#define SIM_SOPT2_PLLFLLSEL_MASK	(3u << 16)
#define SIM_SCGC4_USBOTG	(1u << 18)
#define SIM_SCGC5_PORTA		(1u << 9)	// PORTA..PORTE: bits 9..13
#define SIM_SCGC5_PORTS		(0x1Fu << 9)
#define SIM_SCGC6_FTF		(1u << 0)
#define SIM_SCGC6_DMAMUX	(1u << 1)
#define SIM_SCGC6_ADC1		(1u << 7)
#define SIM_SCGC6_SPI0		(1u << 12)
#define SIM_SCGC6_SPI1		(1u << 13)
#define SIM_SCGC6_I2S		(1u << 15)
#define SIM_SCGC6_PIT		(1u << 23)
#define SIM_SCGC6_ADC0		(1u << 27)
#define SIM_SCGC6_RTC		(1u << 29)
#define SIM_SCGC6_DAC0		(1u << 31)
#define SIM_SCGC7_DMA		(1u << 1)
#define SIM_CLKDIV1_OUTDIV(core, bus, flexbus, flash)	\
	(((uint32_t)((core) - 1u) << 28) | ((uint32_t)((bus) - 1u) << 24) | ((uint32_t)((flexbus) - 1u) << 20) | \
	 ((uint32_t)((flash) - 1u) << 16))
#define SIM_FCFG1_PFSIZE(v)	(((v) >> 24) & 0xFu)

// --- MCG and OSC: clock generation (chapters 24, 25) ---
#define MCG_BASE		0x40064000u
#define MCG_C1			K22F_REG8(MCG_BASE + 0x0)
#define MCG_C2			K22F_REG8(MCG_BASE + 0x1)
#define MCG_C4			K22F_REG8(MCG_BASE + 0x3)
#define MCG_C5			K22F_REG8(MCG_BASE + 0x4)
#define MCG_C6			K22F_REG8(MCG_BASE + 0x5)
#define MCG_S			K22F_REG8(MCG_BASE + 0x6)
#define MCG_C7			K22F_REG8(MCG_BASE + 0xC)

#define MCG_C1_CLKS(x)		((uint8_t)((x) << 6))	// 0 FLL or PLL, 1 internal reference, 2 external reference
#define MCG_C1_CLKS_MASK	0xC0u
#define MCG_C1_FRDIV(x)		((uint8_t)((x) << 3))
#define MCG_C1_IREFS		0x04u
#define MCG_C1_IRCLKEN		0x02u
#define MCG_C2_RANGE(x)		((uint8_t)((x) << 4))	// 2 = very high frequency range (8-32 MHz)
#define MCG_C2_HGO		0x08u
#define MCG_C2_EREFS		0x04u	// 1 = crystal (oscillator on), 0 = external clock on EXTAL0
#define MCG_C5_PRDIV0(x)	((uint8_t)(x))	// divide by x + 1
#define MCG_C6_PLLS		0x40u
#define MCG_C6_VDIV0(x)		((uint8_t)(x))	// multiply by x + 24
#define MCG_S_LOCK0		0x40u
#define MCG_S_PLLST		0x20u
#define MCG_S_IREFST		0x10u
#define MCG_S_CLKST_MASK	0x0Cu
#define MCG_S_CLKST(x)		((uint8_t)((x) << 2))	// 0 FLL, 1 internal, 2 external, 3 PLL
#define MCG_S_OSCINIT0		0x02u

#define OSC_BASE		0x40065000u
#define OSC_CR			K22F_REG8(OSC_BASE + 0x0)
#define OSC_CR_ERCLKEN		0x80u

// --- SMC, PMC, RCM: power modes and reset sources (chapters 15-17) ---
#define SMC_BASE		0x4007E000u
#define SMC_PMPROT		K22F_REG8(SMC_BASE + 0x0)
#define SMC_PMCTRL		K22F_REG8(SMC_BASE + 0x1)
#define SMC_PMSTAT		K22F_REG8(SMC_BASE + 0x3)
#define SMC_PMPROT_AHSRUN	0x80u
#define SMC_PMCTRL_RUNM_RUN	0x00u
#define SMC_PMCTRL_RUNM_HSRUN	0x60u
#define SMC_PMSTAT_RUN		0x01u
#define SMC_PMSTAT_HSRUN	0x80u

#define PMC_BASE		0x4007D000u
#define PMC_REGSC		K22F_REG8(PMC_BASE + 0x2)
#define PMC_REGSC_ACKISO	0x08u

#define RCM_BASE		0x4007F000u
#define RCM_SRS0		K22F_REG8(RCM_BASE + 0x0)
#define RCM_SRS1		K22F_REG8(RCM_BASE + 0x1)

// --- WDOG (chapter 23) ---
#define WDOG_BASE		0x40052000u
#define WDOG_STCTRLH		K22F_REG16(WDOG_BASE + 0x0)
#define WDOG_TOVALH		K22F_REG16(WDOG_BASE + 0x4)
#define WDOG_TOVALL		K22F_REG16(WDOG_BASE + 0x6)
#define WDOG_REFRESH		K22F_REG16(WDOG_BASE + 0xC)
#define WDOG_UNLOCK		K22F_REG16(WDOG_BASE + 0xE)
#define WDOG_UNLOCK_KEY1	0xC520u
#define WDOG_UNLOCK_KEY2	0xD928u
#define WDOG_REFRESH_KEY1	0xA602u
#define WDOG_REFRESH_KEY2	0xB480u
#define WDOG_STCTRLH_WDOGEN	0x0001u
#define WDOG_STCTRLH_CLKSRC	0x0002u
#define WDOG_STCTRLH_ALLOWUPDATE	0x0010u
#define WDOG_STCTRLH_STOPEN	0x0040u
#define WDOG_STCTRLH_WAITEN	0x0080u
#define WDOG_STCTRLH_RESERVED	0x0100u	// reads as 1; written as 1

// --- PORT and GPIO (chapters 11, 55) ---
#define PORT_BASE(port)		(0x40049000u + 0x1000u * (uint32_t)(port))	// port 0..4 = A..E
#define PORT_PCR(port, pin)	K22F_REG32(PORT_BASE(port) + 4u * (uint32_t)(pin))
#define PORT_ISFR(port)		K22F_REG32(PORT_BASE(port) + 0xA0u)
// PCR fields: k22f_pin.h

#define GPIO_BASE(port)		(0x400FF000u + 0x40u * (uint32_t)(port))
#define GPIO_PDOR(port)		K22F_REG32(GPIO_BASE(port) + 0x00u)
#define GPIO_PSOR(port)		K22F_REG32(GPIO_BASE(port) + 0x04u)
#define GPIO_PCOR(port)		K22F_REG32(GPIO_BASE(port) + 0x08u)
#define GPIO_PTOR(port)		K22F_REG32(GPIO_BASE(port) + 0x0Cu)
#define GPIO_PDIR(port)		K22F_REG32(GPIO_BASE(port) + 0x10u)
#define GPIO_PDDR(port)		K22F_REG32(GPIO_BASE(port) + 0x14u)

// --- PIT (chapter 41) ---
#define PIT_BASE		0x40037000u
#define PIT_MCR			K22F_REG32(PIT_BASE + 0x000)
#define PIT_LDVAL(n)		K22F_REG32(PIT_BASE + 0x100u + 0x10u * (uint32_t)(n))
#define PIT_CVAL(n)		K22F_REG32(PIT_BASE + 0x104u + 0x10u * (uint32_t)(n))
#define PIT_TCTRL(n)		K22F_REG32(PIT_BASE + 0x108u + 0x10u * (uint32_t)(n))
#define PIT_TFLG(n)		K22F_REG32(PIT_BASE + 0x10Cu + 0x10u * (uint32_t)(n))
#define PIT_MCR_MDIS		(1u << 1)
#define PIT_MCR_FRZ		(1u << 0)
#define PIT_TCTRL_TEN		(1u << 0)
#define PIT_TCTRL_TIE		(1u << 1)
#define PIT_TFLG_TIF		(1u << 0)

// --- DSPI (SPI0, SPI1; chapter 51) ---
#define SPI_BASE(n)		(0x4002C000u + 0x1000u * (uint32_t)(n))
#define SPI_MCR(n)		K22F_REG32(SPI_BASE(n) + 0x00u)
#define SPI_TCR(n)		K22F_REG32(SPI_BASE(n) + 0x08u)
#define SPI_CTAR(n, k)		K22F_REG32(SPI_BASE(n) + 0x0Cu + 4u * (uint32_t)(k))
#define SPI_SR(n)		K22F_REG32(SPI_BASE(n) + 0x2Cu)
#define SPI_RSER(n)		K22F_REG32(SPI_BASE(n) + 0x30u)
#define SPI_PUSHR(n)		K22F_REG32(SPI_BASE(n) + 0x34u)
#define SPI_POPR(n)		K22F_REG32(SPI_BASE(n) + 0x38u)
#define SPI_MCR_MSTR		(1u << 31)
#define SPI_MCR_ROOE		(1u << 24)
#define SPI_MCR_PCSIS(x)	((uint32_t)(x) << 16)	// chip select inactive levels, 1 = high
#define SPI_MCR_MDIS		(1u << 14)
#define SPI_MCR_DIS_TXF		(1u << 13)
#define SPI_MCR_DIS_RXF		(1u << 12)
#define SPI_MCR_CLR_TXF		(1u << 11)
#define SPI_MCR_CLR_RXF		(1u << 10)
#define SPI_MCR_HALT		(1u << 0)
#define SPI_CTAR_DBR		(1u << 31)
#define SPI_CTAR_FMSZ(bits)	((uint32_t)((bits) - 1u) << 27)
#define SPI_CTAR_CPOL		(1u << 26)
#define SPI_CTAR_CPHA		(1u << 25)
#define SPI_CTAR_LSBFE		(1u << 24)
#define SPI_CTAR_PCSSCK(x)	((uint32_t)(x) << 22)
#define SPI_CTAR_PASC(x)	((uint32_t)(x) << 20)
#define SPI_CTAR_PDT(x)		((uint32_t)(x) << 18)
#define SPI_CTAR_PBR(x)		((uint32_t)(x) << 16)	// 0 /2, 1 /3, 2 /5, 3 /7
#define SPI_CTAR_CSSCK(x)	((uint32_t)(x) << 12)
#define SPI_CTAR_ASC(x)		((uint32_t)(x) << 8)
#define SPI_CTAR_DT(x)		((uint32_t)(x) << 4)
#define SPI_CTAR_BR(x)		((uint32_t)(x) << 0)	// 0 /2, 1 /4, 2 /6, 3 /8, then powers of two from 4 /16
#define SPI_SR_TCF		(1u << 31)
#define SPI_SR_EOQF		(1u << 28)
#define SPI_SR_TFUF		(1u << 27)
#define SPI_SR_TFFF		(1u << 25)
#define SPI_SR_RFOF		(1u << 19)
#define SPI_SR_RFDF		(1u << 17)
#define SPI_PUSHR_CONT		(1u << 31)
#define SPI_PUSHR_CTAS(x)	((uint32_t)(x) << 28)
#define SPI_PUSHR_PCS(x)	((uint32_t)(x) << 16)

// --- I2S0 / SAI (chapter 54) ---
#define I2S0_BASE		0x4002F000u
#define I2S0_TCSR		K22F_REG32(I2S0_BASE + 0x000)
#define I2S0_TCR1		K22F_REG32(I2S0_BASE + 0x004)
#define I2S0_TCR2		K22F_REG32(I2S0_BASE + 0x008)
#define I2S0_TCR3		K22F_REG32(I2S0_BASE + 0x00C)
#define I2S0_TCR4		K22F_REG32(I2S0_BASE + 0x010)
#define I2S0_TCR5		K22F_REG32(I2S0_BASE + 0x014)
#define I2S0_TDR0_ADDR		(I2S0_BASE + 0x020)
#define I2S0_TMR		K22F_REG32(I2S0_BASE + 0x060)
#define I2S0_RCSR		K22F_REG32(I2S0_BASE + 0x080)
#define I2S0_RCR1		K22F_REG32(I2S0_BASE + 0x084)
#define I2S0_RCR2		K22F_REG32(I2S0_BASE + 0x088)
#define I2S0_RCR3		K22F_REG32(I2S0_BASE + 0x08C)
#define I2S0_RCR4		K22F_REG32(I2S0_BASE + 0x090)
#define I2S0_RCR5		K22F_REG32(I2S0_BASE + 0x094)
#define I2S0_RDR0_ADDR		(I2S0_BASE + 0x0A0)
#define I2S0_RMR		K22F_REG32(I2S0_BASE + 0x0E0)
#define I2S_CSR_TE		(1u << 31)	// TE / RE
#define I2S_CSR_FR		(1u << 25)	// FIFO reset
#define I2S_CSR_SR		(1u << 24)	// software reset
#define I2S_CSR_WSF		(1u << 20)
#define I2S_CSR_SEF		(1u << 19)
#define I2S_CSR_FEF		(1u << 18)
#define I2S_CSR_SEIE		(1u << 11)
#define I2S_CSR_FEIE		(1u << 10)
#define I2S_CSR_FRDE		(1u << 0)	// DMA request on the FIFO watermark
#define I2S_CR2_SYNC(x)		((uint32_t)(x) << 30)	// 1 = synchronous with the other direction
#define I2S_CR2_BCP		(1u << 25)	// bit clock active low (sample on the rising edge)
#define I2S_CR2_BCD		(1u << 24)	// 1 = bit clock generated here
#define I2S_CR3_CE		(1u << 16)	// channel 0 enable
#define I2S_CR4_FRSZ(words)	((uint32_t)((words) - 1u) << 16)
#define I2S_CR4_SYWD(bits)	((uint32_t)((bits) - 1u) << 8)
#define I2S_CR4_MF		(1u << 4)	// MSB first
#define I2S_CR4_FSE		(1u << 3)	// frame sync one bit early (I2S)
#define I2S_CR4_FSP		(1u << 1)	// frame sync active low
#define I2S_CR4_FSD		(1u << 0)	// 1 = frame sync generated here
#define I2S_CR5_WNW(bits)	((uint32_t)((bits) - 1u) << 24)
#define I2S_CR5_W0W(bits)	((uint32_t)((bits) - 1u) << 16)
#define I2S_CR5_FBT(bit)	((uint32_t)(bit) << 8)

// --- eDMA and DMAMUX (chapters 21, 22) ---
#define DMA_BASE		0x40008000u
#define DMA_CR			K22F_REG32(DMA_BASE + 0x000)
#define DMA_ES			K22F_REG32(DMA_BASE + 0x004)
#define DMA_ERQ			K22F_REG32(DMA_BASE + 0x00C)
#define DMA_CERQ		K22F_REG8(DMA_BASE + 0x01A)
#define DMA_SERQ		K22F_REG8(DMA_BASE + 0x01B)
#define DMA_CDNE		K22F_REG8(DMA_BASE + 0x01C)
#define DMA_CERR		K22F_REG8(DMA_BASE + 0x01E)
#define DMA_CINT		K22F_REG8(DMA_BASE + 0x01F)
#define DMA_INT			K22F_REG32(DMA_BASE + 0x024)
#define DMA_TCD(n)		(DMA_BASE + 0x1000u + 0x20u * (uint32_t)(n))
#define DMA_TCD_SADDR(n)	K22F_REG32(DMA_TCD(n) + 0x00u)
#define DMA_TCD_SOFF(n)		K22F_REG16(DMA_TCD(n) + 0x04u)
#define DMA_TCD_ATTR(n)		K22F_REG16(DMA_TCD(n) + 0x06u)
#define DMA_TCD_NBYTES(n)	K22F_REG32(DMA_TCD(n) + 0x08u)
#define DMA_TCD_SLAST(n)	K22F_REG32(DMA_TCD(n) + 0x0Cu)
#define DMA_TCD_DADDR(n)	K22F_REG32(DMA_TCD(n) + 0x10u)
#define DMA_TCD_DOFF(n)		K22F_REG16(DMA_TCD(n) + 0x14u)
#define DMA_TCD_CITER(n)	K22F_REG16(DMA_TCD(n) + 0x16u)
#define DMA_TCD_DLASTSGA(n)	K22F_REG32(DMA_TCD(n) + 0x18u)
#define DMA_TCD_CSR(n)		K22F_REG16(DMA_TCD(n) + 0x1Cu)
#define DMA_TCD_BITER(n)	K22F_REG16(DMA_TCD(n) + 0x1Eu)
#define DMA_ATTR_SIZE_16	0x0101u	// SSIZE = DSIZE = 16 bit
#define DMA_CSR_INTMAJOR	(1u << 1)
#define DMA_CSR_INTHALF		(1u << 2)
#define DMA_CSR_DONE		(1u << 7)

#define DMAMUX_BASE		0x40021000u
#define DMAMUX_CHCFG(n)		K22F_REG8(DMAMUX_BASE + (uint32_t)(n))
#define DMAMUX_ENBL		0x80u
#define DMAMUX_SRC_I2S0_RX	12u
#define DMAMUX_SRC_I2S0_TX	13u

// --- ADC0 / ADC1 (chapter 35) ---
#define ADC_BASE(n)		((n) == 0 ? 0x4003B000u : 0x40027000u)
#define ADC_SC1A(n)		K22F_REG32(ADC_BASE(n) + 0x00u)
#define ADC_CFG1(n)		K22F_REG32(ADC_BASE(n) + 0x08u)
#define ADC_CFG2(n)		K22F_REG32(ADC_BASE(n) + 0x0Cu)
#define ADC_RA(n)		K22F_REG32(ADC_BASE(n) + 0x10u)
#define ADC_SC2(n)		K22F_REG32(ADC_BASE(n) + 0x20u)
#define ADC_SC3(n)		K22F_REG32(ADC_BASE(n) + 0x24u)
#define ADC_PG(n)		K22F_REG32(ADC_BASE(n) + 0x2Cu)
#define ADC_MG(n)		K22F_REG32(ADC_BASE(n) + 0x30u)
#define ADC_CLP(n, off)		K22F_REG32(ADC_BASE(n) + (off))	// CLPD 0x34 .. CLP0 0x4C, CLMD 0x54 .. CLM0 0x6C
#define ADC_SC1_COCO		(1u << 7)
#define ADC_SC1_ADCH(x)		((uint32_t)(x) & 0x1Fu)
#define ADC_CFG1_ADIV(x)	((uint32_t)(x) << 5)
#define ADC_CFG1_ADLSMP		(1u << 4)
#define ADC_CFG1_MODE_12	(1u << 2)
#define ADC_SC3_CAL		(1u << 7)
#define ADC_SC3_CALF		(1u << 6)
#define ADC_SC3_AVGE		(1u << 2)
#define ADC_SC3_AVGS(x)		((uint32_t)(x) & 3u)	// 4, 8, 16, 32 samples

// --- DAC0 (chapter 37) ---
#define DAC0_BASE		0x4003F000u
#define DAC0_DAT0L		K22F_REG8(DAC0_BASE + 0x00)
#define DAC0_DAT0H		K22F_REG8(DAC0_BASE + 0x01)
#define DAC0_C0			K22F_REG8(DAC0_BASE + 0x21)
#define DAC0_C1			K22F_REG8(DAC0_BASE + 0x22)
#define DAC_C0_DACEN		0x80u
#define DAC_C0_DACRFS		0x40u	// reference 2 (VDDA)
#define DAC_C0_DACTRGSEL	0x20u	// software trigger

#endif
