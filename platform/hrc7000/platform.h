// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 (Dahua DH4570) SoC with a C-SKY CK803S core: register map and platform services for boards built on it
// (docs/spec/hardware-dm32.md 1-4). The modem / baseband registers are the driver's (drivers/modem/hrc7000).
#ifndef PLATFORM_HRC7000_H
#define PLATFORM_HRC7000_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hal_system.h"
#include "hal_flash.h"

#define REG32(a)		(*(volatile uint32_t *)(uintptr_t)(a))

// --- Clocks: HRC_CPU_HZ and the APB clocks (clock_hz.h, which FreeRTOSConfig.h reads too) ---
#include "clock_hz.h"
#define HRC_XTAL_HZ		24000000u
#include "clock_math.h"

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
#define IO_DIPLEX2_DAC_A	(1u << 27)	// 0 = the DAC channel on its pin (PTC23 / 24 / 25)
#define IO_DIPLEX2_DAC_B	(1u << 28)
#define IO_DIPLEX2_DAC_C	(1u << 29)
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
#define UART2_BASE		0x14050000u
#define UART3_BASE		0x14090000u	// the GD-88's link between its two SoCs (hardware-gd88.md 1)
#define SPI0_BASE		0x140A0000u
#define SPI2_BASE		0x140E0000u
#define PWM_BASE		0x140C0000u
#define ADC_BASE		0x140D0000u
#define DAC_BASE		0x140F0000u
#define GPIOB_BASE		0x14100000u
#define GPIOC_BASE		0x14110000u
#define LCDC_BASE		0x12000000u	// i8080 LCD controller
#define PIC_BASE		0x17000000u
#define CACHE_BASE		0xE000F000u

// DAC (3 channels, 12 bit; hardware-dm32.md 3.8). Its pins: IO_DIPLEX2_DAC_*
#define DAC_PD_CTRL		REG32(DAC_BASE + 0x00)	// bit n = 1 powers channel n down
#define DAC_PD_MODE_EN		REG32(DAC_BASE + 0x04)
#define DAC_DATA_A		REG32(DAC_BASE + 0x08)
#define DAC_DATA_B		REG32(DAC_BASE + 0x0C)
#define DAC_DATA_C		REG32(DAC_BASE + 0x10)
#define DAC_A_OFF		(1u << 0)
#define DAC_B_OFF		(1u << 1)
#define DAC_C_OFF		(1u << 2)
#define DAC_MAX			4095u

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
// stock; hardware-dm32.md 8.6), 4 = AFSK, 5 = the board's own, 6 free
#define TIMER_TICK		1
#define TIMER_CLOCK		2
#define TIMER_PA		3
#define TIMER_AFSK		4	// APRS AFSK transmit: tops up the prompt FIFO (hrc_afsk.c)
#define TIMER_BOARD		5	// a board's one-shot (DB25-D: the waits of its DMR burst sequence, dmr_db25d.c)

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

// --- SPI master (ssi.c): SPI0_BASE or SPI2_BASE ---

// 8-bit frames, mode 0, SCLK = APB / baudr (even, 2 or more). The pins' IO mux and the GPIO chip select are the board's
void hrc_ssi_init(uint32_t base, uint16_t baudr);
// Around each transfer, with the board's GPIO chip select
void hrc_ssi_select(uint32_t base, bool active);
// Polled, one byte at a time. tx NULL sends 0xFF, rx NULL discards. False if the controller stops answering
bool hrc_ssi_xfer(uint32_t base, const uint8_t *tx, uint8_t *rx, uint32_t len);

// --- Data flash on SPI0 (flash_spi0.c) ---

typedef struct {
	uint8_t cs_pin;			// GPIO pin index of the chip select, low = selected
	uint32_t write_floor;		// first byte the firmware may write or erase
	const hal_flash_map_t *map;	// the core's areas
} hrc_flash_cfg_t;

extern hal_flash_t hrc_flash_spi0;
void hrc_flash_spi0_configure(const hrc_flash_cfg_t *cfg);	// before hrc_flash_spi0.init
uint32_t hrc_flash_spi0_jedec(void);
// The chip UID that INFO_CHIP and REBOOT's token use (system.c): the board's data flash unique ID, read once at its init
// (flash_spi0.c does it for the SPI NOR). An ID of one repeated byte is ignored
void hrc_chip_set_uid(const uint8_t *id, uint32_t len);
// The task holding the data flash lock and for how long (ms), "-" when free: a stuck flash user shows here
const char *hrc_flash_spi0_holder(uint32_t *held_ms);

// --- ADC (adc.c): channels 0..7 (A..H) ---

void hrc_adc_init(void);
uint16_t hrc_adc_read(uint8_t ch);	// 10 bits, 0 if the conversion doesn't finish
uint16_t hrc_adc_last(uint8_t ch);	// the last reading, for diagnostics

// --- Start-up and services (clock.c, time.c) ---

// Watchdog off, PLL to 168 MHz unless the bootloader already runs on it, peripheral clocks on, flash cache.
// Runs from RAM (it reprograms the XIP flash controller)
void hrc_clock_init(void);
// Back to the clocks the loader left (a no-op when the application never changed them). From RAM, interrupts off,
// nothing running from XIP flash. The caller reprograms the UART divisor afterwards (hrc_host_uart_settle)
void hrc_clock_restore_loader(void);
// Vector table in RAM, PIC to a known state (everything masked and cleared)
void hrc_irq_init(void);
void hrc_time_init(void);
// LCD backlight on PWM channel ch, output on PTA15 + ch (backlight.c). cfg is the CFG value before load and enable,
// rpt the repeat count (0: not written), period in PWM clocks (24 MHz)
typedef struct {
	uint8_t ch;
	uint16_t cfg;
	uint16_t rpt;
	uint32_t period;
} hrc_backlight_t;
// 0 off, 100 fully on, otherwise PWM duty in percent
void hrc_backlight_pwm(const hrc_backlight_t *bl, uint8_t percent);
// PTA15 / PWM0 with the DM-32 port's settings
void hrc_backlight(uint8_t percent);

// Timer 1 at configTICK_RATE_HZ for the RTOS (called by the port when the scheduler starts)
void hrc_tick_start(uint32_t hz);

// One-shot on a timer: fires once after us microseconds, then the timer stops (the handler reads EOI)
void hrc_timer_oneshot(uint8_t n, uint32_t us);
void hrc_timer_stop(uint8_t n);
// Periodic: an interrupt every us microseconds until hrc_timer_stop (the handler reads EOI)
void hrc_timer_periodic(uint8_t n, uint32_t us);
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

// Chip soft reset (QUAD_ENABLE = 0, LCSFC_BAUDR = 4, SYS_SOFT_RSTN = 0; hardware-dm32.md 1.1). Does not return
void hrc_soft_reset(void) __attribute__((noreturn));

// Runtime RAM-monitor (stub) entry (CONFIG_STUB_ENTRY, docs/spec/hardware-dm32.md 17.2). The host loads the stub here
// and ODR jumps to it without a power cycle. The span is the codec window at the top of SRAM, which ODR never links
// anything into (boards/dm32/memory.ld: 0x43A00..0x58000); the codec is parked before the load, so the whole window is free. Base is
// 4 KB aligned inside it. The universal stub copies itself to 0x30000 (ODR's RAM, dead by then), clear of this span. A
// board with another layout sets its own span in board.mk or config.mk (the DB25-D)
#ifndef HRC_STUB_BASE
#define HRC_STUB_BASE		0x00044000u
#define HRC_STUB_SIZE		0x00014000u	// 80 KB, to the end of SRAM
#endif

// Crash log (CONFIG_CRASH_LOG, crash_hrc.c): the record lives at the top of the SRAM shared with the modem, which the
// codec's tables (from 0x18000800, at most to 0x18004F90 in any stock image) and the microphone PCM (0x18000000)
// don't reach (hardware-dm32.md 2: free for software). Main SRAM is the bootloader's and the start-up's
#define HRC_CRASH_STORE		0x18007F00u	// 256 bytes to the end, 0x18008000
// hrc_fault's capture: the frame is hrc_fault_entry's (r0..r13, r15, EPSR, EPC); does not return
void hrc_crash_fault(const uint32_t *frame, uint32_t vector) __attribute__((noreturn));

// Code that must not run from XIP flash (flash controller set-up, the modem slot interrupts): the linker script
// places .ramfunc in SRAM
#define HRC_RAMFUNC		__attribute__((section(".ramfunc"), noinline))

// --- Transmitter off (board_rf_safe in board.h, docs/spec/crash-log.md 8) ---

// A pin forced to an output at a level: DR, then DDR, so it never glitches. Plain read-modify-writes without
// hrc_pin_out's interrupt save (the callers run with interrupts off), inlined so the boards' RAM-resident hook runs
// nothing from XIP flash
static inline __attribute__((always_inline)) void hrc_pin_force(uint8_t pin, bool level)
{
	uint32_t bank = pin < 32 ? GPIOA_BASE : pin < 64 ? GPIOB_BASE : GPIOC_BASE;
	uint32_t bit = 1u << (pin & 31u);
	uint32_t dr = REG32(bank + GPIO_DR);
	REG32(bank + GPIO_DR) = level ? (dr | bit) : (dr & ~bit);
	REG32(bank + GPIO_DDR) |= bit;
}
// The fault and reset paths: board_rf_safe, then the modem's own transmit state off (FM PTT, the DMR slot engine's
// TX/RX control). RAM code, interrupts off. Not at start-up, where the modem may still be held in reset
void hrc_rf_safe(void);
// CONFIG_WATCHDOG (watchdog.c): before the stub entry a running watchdog gets 32 s and a feed, as the STM32 IWDG; the
// stub feeds it from then on. RAM code, interrupts off
void hrc_watchdog_stretch(void);

extern const hal_system_t hrc_system;

// The programming cable's UART as the board's host port (uart.c): UART0 unless the board sets HRC_HOST_UART=1
#include "hal_stream.h"
extern const hal_stream_t hrc_host_uart_stream;
void hrc_host_uart_init(void);
// The hand-over to the runtime stub: drain the transmitter while the clocks are still ours, restore the loader's clocks
// (hrc_clock_restore_loader), then set the divisor for the loader's APB clock. Both run from RAM
void hrc_host_uart_drain(void);
void hrc_host_uart_loader_div(void);
// Weak, called from the receive interrupt for every character (error: the UART flagged it)
void hrc_host_uart_rx_tap(uint8_t b, bool error);
void hrc_host_uart_put_polled(const char *s);	// interrupts off, no RTOS: the fault trace (irq.c)
uint32_t hrc_host_uart_overruns(void);	// bytes lost because the receive ring was full
uint32_t hrc_host_uart_line_errors(void);	// characters the UART flagged (its FIFO overran, framing, parity)
// When the last byte without a line error arrived (hal_millis()); false if none has yet. A board whose PTT shares the
// serial line (drivers/input/sharedptt) reads it to tell host data from a press
bool hrc_host_uart_last_rx(uint32_t *ms);
// Host port traffic for the diagnostics page: when the host link stops answering, these tell a dead receiver (rx
// stops), a stuck transmitter (tx_stalls) and a stuck host link task (waits stops) apart
typedef struct {
	uint32_t rx_bytes, tx_bytes, tx_stalls, waits;
	uint8_t ier;			// the interrupt enables as the UART has them now (0x05 expected)
} hrc_uart_traffic_t;
void hrc_host_uart_traffic(hrc_uart_traffic_t *t);

// Any of the SoC's UARTs (UART0_BASE, UART1_BASE, ...; uart.c). setup: baud 8N1 from the APB clock, FIFOs on, receive
// trigger at one character, interrupts off. rx_irq: the received-data and line-status interrupts (IER = 0x05) on or
// off. rx_isr, for the UART's interrupt handler: every waiting character (64 at most) to rx, flagged when the UART
// marked it (FIFO overrun, parity, framing, break), then the line-status and busy-detect interrupts cleared; returns
// how many came
void hrc_uart_setup(uint32_t base, uint32_t baud);
void hrc_uart_rx_irq(uint32_t base, bool on);
void hrc_uart_rx_clear(uint32_t base);	// drain the RX FIFO, clear line status and busy-detect
uint32_t hrc_uart_rx_isr(uint32_t base, void (*rx)(uint8_t c, bool flagged));
bool hrc_uart_poll_rx(uint32_t base, uint8_t *c);	// a character waiting: taken. No interrupts needed
bool hrc_uart_poll_tx(uint32_t base, uint8_t c);	// false: the TX FIFO is full

// A link to another SoC on a UART (link_uart.c): the GD-88's A and B on UART3 (hardware-gd88.md 1). Receive by
// interrupt into a ring, transmit by polling the TX FIFO; one writer at a time (the caller serializes). The board muxes
// the pins first. read takes what has come (never waits); wait blocks the calling task until a byte is there or
// timeout_ms passes (true: bytes waiting)
typedef struct {
	uint32_t rx_bytes, tx_bytes;
	uint32_t overruns;	// bytes lost because the ring was full
	uint32_t line_errors;	// characters the UART flagged
	uint32_t tx_stalls;	// writes cut short: the TX FIFO stayed full
} hrc_link_uart_stats_t;
void hrc_link_uart_init(uint32_t base, uint8_t irq, uint32_t baud);
size_t hrc_link_uart_read(uint8_t *buf, size_t max);
bool hrc_link_uart_wait(uint32_t timeout_ms);
size_t hrc_link_uart_write(const uint8_t *buf, size_t len);
void hrc_link_uart_stats(hrc_link_uart_stats_t *st);

// A GPS module's NMEA output on one of the UARTs, 9600 8N1, receive only (gps_hrc.c; hardware-dm32.md 12, hardware-db25d.md
// 3.1). The module is powered and listened for for 5 s at start-up; one checksum-valid sentence makes it present (and
// every sentence then goes to the APRS position source, CONFIG_APRS), nothing heard powers it off and stops the UART
typedef struct {
	uint32_t uart_base;	// UART0_BASE ... UART2_BASE
	uint8_t irq;		// the UART's interrupt source (IRQ_UARTn)
	uint8_t power_pin;	// GPIO that switches the module's supply
	bool power_on_level;	// the level on power_pin that powers the module
	uint32_t rxd_mux_clear;	// IO_DIPLEX0 bits cleared to give the RXD pad to the UART; 0 = the board's io init has
} hrc_gps_cfg_t;
void hrc_gps_init(const hrc_gps_cfg_t *cfg);
void hrc_gps_enable(bool on);	// Settings > Location > GPS (board_t.gps_enable): the module's power, once it has been heard
bool hrc_gps_present(void);	// the module has been heard: board_t.gps_present
void hrc_gps_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes);	// valid / bad sentences, bytes received
bool hrc_gps_running(void);	// the module is powered and listened to (diagnostics)

#endif
