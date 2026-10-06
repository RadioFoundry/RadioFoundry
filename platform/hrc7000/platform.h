// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 (Dahua DH4570) SoC with a C-SKY CK803S core: register map and platform services for boards built on it
// (docs/spec/hardware-dm32.md 1-4). The modem / baseband registers are the driver's (drivers/modem/hrc7000).
#ifndef PLATFORM_HRC7000_H
#define PLATFORM_HRC7000_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_system.h"

#define REG32(a)		(*(volatile uint32_t *)(uintptr_t)(a))

// --- Clocks: APB peripherals run from sys / 2 (hardware-dm32.md 1.1) ---
#define HRC_CPU_HZ		168000000u
#define HRC_APB_HZ		84000000u
#define HRC_XTAL_HZ		24000000u

// --- System / modem block (hardware-dm32.md 1.1, 5.1) ---
#define SYS_BASE		0x11000000u
#define SYS_SOFT_RSTN		REG32(SYS_BASE + 0x000)
#define CLK_MGR_CTRL		REG32(SYS_BASE + 0x004)	// b0/b1 PLL resets, b2/b3 clock source, b30 switch done, b31 lock
#define CLK_MGR_PLL		REG32(SYS_BASE + 0x010)
#define CLK_MGR_DIV		REG32(SYS_BASE + 0x024)
#define CLK_MGR_USB_SDIO	REG32(SYS_BASE + 0x028)
#define CLK_MGR_GATES		REG32(SYS_BASE + 0x02C)
#define LCSFC_BAUDR		REG32(SYS_BASE + 0x030)
#define IO_DIPLEX0		REG32(SYS_BASE + 0x034)
#define IO_DIPLEX1		REG32(SYS_BASE + 0x038)
#define IO_DIPLEX2		REG32(SYS_BASE + 0x03C)
#define IOMGR_REN0		REG32(SYS_BASE + 0x040)
#define IOMGR_REN1		REG32(SYS_BASE + 0x044)
#define IOMGR_REN2		REG32(SYS_BASE + 0x048)
#define QUAD_ENABLE		REG32(SYS_BASE + 0x05C)

// --- APB peripherals (hardware-dm32.md 3) ---
#define TIMER_BASE		0x14000000u	// timer n (1..6) at + 0x14 * (n - 1)
#define WDT_BASE		0x14010000u
#define GPIOA_BASE		0x14020000u
#define UART0_BASE		0x14030000u
#define UART1_BASE		0x14040000u
#define SPI0_BASE		0x140A0000u
#define PWM_BASE		0x140C0000u
#define ADC_BASE		0x140D0000u
#define DAC_BASE		0x140F0000u
#define GPIOB_BASE		0x14100000u
#define GPIOC_BASE		0x14110000u
#define LCDC_BASE		0x12000000u	// i8080 LCD controller
#define PIC_BASE		0x17000000u
#define CACHE_BASE		0xE000F000u

// Timers (DesignWare style, count down from LOADCOUNT at APB)
#define TMR_LOAD(n)		REG32(TIMER_BASE + 0x14u * ((n) - 1u) + 0x00)
#define TMR_VALUE(n)		REG32(TIMER_BASE + 0x14u * ((n) - 1u) + 0x04)
#define TMR_CTRL(n)		REG32(TIMER_BASE + 0x14u * ((n) - 1u) + 0x08)
#define TMR_EOI(n)		REG32(TIMER_BASE + 0x14u * ((n) - 1u) + 0x0C)
#define TMR_INTSTAT(n)		REG32(TIMER_BASE + 0x14u * ((n) - 1u) + 0x10)
#define TMR_CTRL_EN		0x01u
#define TMR_CTRL_PERIODIC	0x02u	// reload from LOADCOUNT ("user defined" mode)
#define TMR_CTRL_INT_MASK	0x04u	// 1 = interrupt masked

// Timer allocation: 1 = RTOS tick, 2 = free-running microsecond clock (no interrupt), 3 = DMR PA one-shot (as
// stock; hardware-dm32.md 8.6), 4-6 free
#define TIMER_TICK		1
#define TIMER_CLOCK		2
#define TIMER_PA		3

// --- PIC: source s is CPU vector s + 32 (hardware-dm32.md 4) ---
#define PIC_MODE		REG32(PIC_BASE + 0x00)
#define PIC_PO			REG32(PIC_BASE + 0x04)
#define PIC_MASK		REG32(PIC_BASE + 0x08)
#define PIC_COW1		REG32(PIC_BASE + 0x10)	// end of interrupt: write 0x04
#define PIC_INT_ST		REG32(PIC_BASE + 0x44)
#define PIC_INT_ST_1		REG32(PIC_BASE + 0x48)
#define PIC_MODE_1		REG32(PIC_BASE + 0x60)
#define PIC_PO_1		REG32(PIC_BASE + 0x64)
#define PIC_MASK_1		REG32(PIC_BASE + 0x68)
#define PIC_EOI_VALUE		0x04u
#define PIC_SOURCES		64u

enum {
	IRQ_TIMER1 = 1, IRQ_TIMER2, IRQ_TIMER3, IRQ_TIMER4, IRQ_TIMER5, IRQ_TIMER6,
	IRQ_I2C1 = 7, IRQ_I2C0 = 8, IRQ_I2C2 = 9,
	IRQ_UART1 = 10, IRQ_UART2 = 11, IRQ_UART0 = 12, IRQ_UART3 = 13,
	IRQ_GPIOB = 14, IRQ_GPIOA = 15, IRQ_SPI0 = 16, IRQ_GPIOC = 17,
	IRQ_PWM = 18, IRQ_RTC = 19, IRQ_SDIO = 20, IRQ_USB = 21,
	IRQ_DAC = 25, IRQ_ADC = 26,
	IRQ_PCM_RD = 27,	// speaker PCM consumed (10 ms)
	IRQ_PCM_WR = 28,	// mic PCM buffer full (10 ms)
	IRQ_MODEM_SYS = 29,	// System_inter
	IRQ_MODEM_TS_TX = 30,	// Time_slot_inter_tx
	IRQ_MODEM_TS_RX = 31,	// Time_slot_inter_rx
	IRQ_MODEM_RF_TX = 32,	// Rf_tx_inter
	IRQ_MODEM_RF_RX = 33,	// Rf_rx_inter
};

typedef void (*hrc_irq_fn)(void);

// Point source s at fn (NULL = the default handler, which masks the source) and leave it masked
void hrc_irq_register(uint8_t source, hrc_irq_fn fn);
void hrc_irq_enable(uint8_t source, bool on);
// The four modem interrupts (sources 29-32) masked or unmasked as one group (gotchas 2.3: PIC_MASK bits 29-31 and
// PIC_MASK_1 bit 0, never vectors 93-95). Returns the previous mask bits for hrc_modem_irqs_restore
uint32_t hrc_modem_irqs_mask(void);
void hrc_modem_irqs_restore(uint32_t saved);

// Interrupt state of the CPU (PSR.IE)
static inline uint32_t hrc_irq_save(void)
{
	uint32_t psr;
	__asm__ volatile ("mfcr %0, psr\n\tpsrclr ie" : "=r"(psr) : : "memory");
	return psr;
}

static inline void hrc_irq_restore(uint32_t psr)
{
	__asm__ volatile ("mtcr %0, psr" : : "r"(psr) : "memory");
}

static inline bool hrc_in_isr(void)
{
	uint32_t psr;
	__asm__ volatile ("mfcr %0, psr" : "=r"(psr));
	return ((psr >> 16) & 0xFFu) != 0;	// PSR.VEC: the exception being served, 0 in thread mode
}

static inline void hrc_barrier(void)
{
	__asm__ volatile ("sync" : : : "memory");
}

// --- GPIO: pin index 0-95 = bank A 0-31, B 32-63, C 64-95 (hardware-dm32.md 3.9) ---
#define GPIO_DR			0x00u
#define GPIO_DDR		0x04u
#define GPIO_INTEN		0x30u
#define GPIO_INTMASK		0x34u
#define GPIO_INTTYPE		0x38u	// 1 = edge
#define GPIO_POLARITY		0x3Cu	// 1 = rising / high
#define GPIO_INTSTATUS		0x40u
#define GPIO_EOI		0x4Cu
#define GPIO_EXT		0x50u	// input levels
#define GPIO_BOTHEDGE		0x78u

static inline uint32_t hrc_gpio_bank(uint8_t pin)
{
	return pin < 32 ? GPIOA_BASE : pin < 64 ? GPIOB_BASE : GPIOC_BASE;
}

static inline uint32_t hrc_gpio_bit(uint8_t pin)
{
	return 1u << (pin & 31u);
}

void hrc_pin_out(uint8_t pin, bool level);
void hrc_pin_in(uint8_t pin);
void hrc_pin_write(uint8_t pin, bool level);	// read-modify-write of the bank's DR with interrupts off

static inline bool hrc_pin_read(uint8_t pin)
{
	return (REG32(hrc_gpio_bank(pin) + GPIO_EXT) & hrc_gpio_bit(pin)) != 0;
}

// Edge interrupt on an input pin (both edges when both, else rising / falling); the bank's handler acks with
// hrc_gpio_ack. Bank interrupts: A source 15, B 14, C 17
void hrc_pin_irq(uint8_t pin, bool on, bool both, bool rising);
uint32_t hrc_gpio_pending(uint32_t bank);
void hrc_gpio_ack(uint32_t bank, uint32_t bits);

// --- Start-up and services (clock.c, time.c) ---

// Watchdog off, PLL to 168 MHz unless the bootloader already runs on it, peripheral clocks on, flash cache.
// Runs from RAM (it reprograms the XIP flash controller)
void hrc_clock_init(void);
// Vector table in RAM, PIC to a known state (everything masked and cleared)
void hrc_irq_init(void);
void hrc_time_init(void);
// Timer 1 at configTICK_RATE_HZ for the RTOS (called by the port when the scheduler starts)
void hrc_tick_start(uint32_t hz);

// One-shot on a timer: fires once after us microseconds, then the timer stops (the handler reads EOI)
void hrc_timer_oneshot(uint8_t n, uint32_t us);
void hrc_timer_stop(uint8_t n);
// Clear a timer's interrupt (read EOI)
static inline void hrc_timer_ack(uint8_t n)
{
	(void)TMR_EOI(n);
}

// Free-running 84 MHz down-counter of TIMER_CLOCK, as an up count (wraps every 51 s)
static inline uint32_t hrc_cycles(void)
{
	return ~TMR_VALUE(TIMER_CLOCK);
}
uint32_t hrc_micros(void);

// Chip soft reset (QUAD_ENABLE = 0, SYS_SOFT_RSTN = 0; hardware-dm32.md 1.1). Does not return
void hrc_soft_reset(void) __attribute__((noreturn));

// Code that must not run from XIP flash (flash controller set-up, the modem slot interrupts): the linker script
// places .ramfunc in SRAM
#define HRC_RAMFUNC		__attribute__((section(".ramfunc"), noinline))

extern const hal_system_t hrc_system;

// The programming cable's UART as the board's host port (uart.c): UART0 unless the board sets HRC_HOST_UART=1
#include "hal_stream.h"
extern const hal_stream_t hrc_host_uart_stream;
void hrc_host_uart_init(void);
uint32_t hrc_host_uart_overruns(void);

#endif
