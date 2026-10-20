// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// NXP Kinetis K22F platform services for the boards built on it (the GD-77 family; docs/spec/hardware-gd77.md 2)
#ifndef PLATFORM_K22F_H
#define PLATFORM_K22F_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "k22f.h"
#include "clock_math.h"
#include "hal_system.h"

// What a platform call that isn't finished says, instead of failing silently
typedef enum {
	K22F_OK = 0,
	K22F_ERR_TIMEOUT,
	K22F_ERR_ARG,
	K22F_ERR_NOT_IMPLEMENTED,
} k22f_status_t;

// --- Clocks (clock.c) ---
extern uint32_t SystemCoreClock;	// the core clock now (FreeRTOS configCPU_CLOCK_HZ)
// Switch from whatever the loader left to the PLL plan in c. Returns K22F_OK, K22F_ERR_ARG when the plan breaks a
// limit (clock_math.h), or K22F_ERR_TIMEOUT when the external clock or the PLL never came up: the radio then runs from
// the internal FLL (K22F_FEI_HZ) and the board decides what still works
k22f_status_t k22f_clock_init(const k22f_clock_t *c);
uint32_t k22f_bus_clock(void);		// the bus clock now (PIT, DSPI, I2S register clock)
bool k22f_clock_on_pll(void);		// false after a fallback

// --- Pins (gpio.c, k22f_pin.h) ---
// Port clocks on; every entry's PCR, and for GPIO outputs the level before the direction, so no pin glitches
void k22f_pins_apply(const k22f_pin_cfg_t *table, size_t n);
void k22f_pin_config(uint8_t pin, uint32_t flags);
void k22f_pin_dir(uint8_t pin, bool output);
uint32_t k22f_port_irq_pending(uint8_t port);
void k22f_port_irq_ack(uint8_t port, uint32_t bits);

// Set / clear / read go through the GPIO set, clear and input registers: no read-modify-write, safe from interrupts
static inline void k22f_pin_write(uint8_t pin, bool level)
{
	if (level) {
		GPIO_PSOR(k22f_pin_port(pin)) = k22f_pin_bit(pin);
	} else {
		GPIO_PCOR(k22f_pin_port(pin)) = k22f_pin_bit(pin);
	}
}

static inline bool k22f_pin_read(uint8_t pin)
{
	return (GPIO_PDIR(k22f_pin_port(pin)) & k22f_pin_bit(pin)) != 0;
}

// --- Time (time.c): the DWT cycle counter, valid before the scheduler and across clock changes ---
void k22f_time_init(void);

// --- DSPI master, polled, 8-bit frames; the chip select is the board's GPIO (dspi.c) ---
typedef struct {
	uint8_t inst;		// 0 = SPI0, 1 = SPI1
	bool cpol, cpha;	// SPI mode
	uint8_t pbr, br;	// CTAR prescaler and scaler codes (clock_math.h k22f_dspi_hz)
	bool dbr;		// double baud rate
} k22f_dspi_cfg_t;
void k22f_dspi_init(const k22f_dspi_cfg_t *c);
// Full duplex, len bytes. tx NULL sends 0xFF, rx NULL discards. False on a timeout (the module stopped answering)
bool k22f_dspi_xfer(uint8_t inst, const uint8_t *tx, uint8_t *rx, uint32_t len);

// --- I2S0 as a slave with DMA rings (i2s.c) ---
// Both directions take the bit clock and frame sync from the codec on the receive pins (RX_BCLK, RX_FS); transmit runs
// synchronous to receive. Each frame is words x word_bits, the sample left aligned at first_bit (MSB first)
typedef struct {
	uint8_t words;		// words per frame (2 = left / right)
	uint8_t word_bits;	// bits per word (32)
	uint8_t first_bit;	// index of the sample's MSB in the word (15 = a 16-bit sample in the low half)
	bool early_fs;		// frame sync one bit early (I2S style)
	bool fs_active_low;
} k22f_i2s_fmt_t;
// One ring each way: frames x words 16-bit samples, interleaved. half(rx, second_half) runs in the DMA interrupt when a
// receive half is full or a transmit half has been sent, at half the ring's period
typedef struct {
	k22f_i2s_fmt_t fmt;
	int16_t *rx_ring, *tx_ring;
	uint16_t ring_samples;	// per ring, a multiple of 2 x words
	void (*half)(bool rx, bool second_half);
} k22f_i2s_stream_t;
k22f_status_t k22f_i2s_start(const k22f_i2s_stream_t *s);
void k22f_i2s_stop(void);
uint32_t k22f_i2s_errors(void);		// FIFO and sync errors seen

// --- PIT (pit.c): periodic callbacks from the timer interrupt ---
k22f_status_t k22f_pit_start(uint8_t ch, uint32_t period_us, void (*fn)(void), uint8_t irq_prio);
void k22f_pit_stop(uint8_t ch);

// --- ADC (adc.c): 12-bit single-ended, polled, hardware average of 8 ---
k22f_status_t k22f_adc_init(uint8_t inst);
// 0..4095, or 0xFFFF when the conversion never finished
uint16_t k22f_adc_read(uint8_t inst, uint8_t channel);

// --- DAC0 (dac.c): 12-bit, VDDA reference, software trigger ---
void k22f_dac_init(void);
void k22f_dac_write(uint16_t value);

// --- Program flash self-programming (ftfa.c): not implemented yet; both say so ---
k22f_status_t k22f_flash_erase_sector(uint32_t addr);
k22f_status_t k22f_flash_program(uint32_t addr, const void *data, uint32_t len);

// --- System (system.c) ---
extern const hal_system_t k22f_system;
uint8_t k22f_reset_cause(void);		// RCM_SRS0 at start-up (bit 7 power-on, 6 pin, 5 watchdog, 1 low voltage)
void k22f_reset(void) __attribute__((noreturn));

// Interrupts off / back as they were
static inline uint32_t k22f_irq_save(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	return primask;
}

static inline void k22f_irq_restore(uint32_t primask)
{
	__set_PRIMASK(primask);
}

#endif
