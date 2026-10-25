// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C6000 configuration and voice ports on STM32F4 GPIO and SPI (hrc_bus.h; docs/spec/hardware-dm1701.md 4.2, 4.3,
// hardware-md9600.md 4.1)
#include <stddef.h>
#include "hrc/hrc_bus.h"
#include "hal_time.h"
#include "FreeRTOS.h"

// About 110 ns at 72 MHz on top of the GPIO writes, keeping the clock near the 1-2 MHz the chip is known to take
static inline void bus_delay(void)
{
	__NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
}

static inline void line(GPIO_TypeDef *port, uint16_t pin, bool level)
{
	port->BSRR = level ? pin : (uint32_t)pin << 16;
}

// Clock idles high; MOSI set, clock low, sample MISO, clock high (the chip latches on the rising edge)
static uint8_t cfg_byte(const stm32_hrc_bus_cfg_t *c, uint8_t out)
{
	uint8_t in = 0;
	for (int i = 0; i < 8; i++) {
		line(c->port, c->mosi, (out & 0x80u) != 0);
		out = (uint8_t)(out << 1);
		line(c->port, c->clk, false);
		bus_delay();
		in = (uint8_t)((in << 1) | ((c->port->IDR & c->miso) ? 1u : 0u));
		line(c->port, c->clk, true);
		bus_delay();
	}
	return in;
}

// One transaction with interrupts up to the RTOS syscall priority masked (the HR_C6000 interrupts run at that level),
// so a modem ISR never lands inside it. Safe from tasks and from those ISRs; never blocks
static bool cfg_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	const stm32_hrc_bus_cfg_t *c = ctx;
	UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
	line(c->port, c->clk, true);
	line(c->port, c->cs, false);
	bus_delay();
	for (uint8_t i = 0; i < hdr_len; i++) {
		cfg_byte(c, hdr[i]);
	}
	for (uint16_t i = 0; i < len; i++) {
		uint8_t in = cfg_byte(c, tx != NULL ? tx[i] : 0xFF);
		if (rx != NULL) {
			rx[i] = in;
		}
	}
	bus_delay();
	line(c->port, c->cs, true);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
	return true;
}

// One page 3 frame on the voice port: 0x03 / 0x83, 0x00, then the data. Atomic with respect to the modem ISRs
static bool voice_xfer(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	const stm32_hrc_bus_cfg_t *c = ctx;
	const uint8_t hdr[2] = { write ? 0x03 : 0x83, 0x00 };
	UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
	line(c->voice_cs_port, c->voice_cs, false);
	bool ok = stm32_spi_xfer(c->voice_spi, hdr, NULL, sizeof hdr);
	if (ok) {
		ok = write ? stm32_spi_xfer(c->voice_spi, buf, NULL, len) : stm32_spi_xfer(c->voice_spi, NULL, buf, len);
	}
	line(c->voice_cs_port, c->voice_cs, true);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
	return ok;
}

void stm32_hrc_bus_init(const stm32_hrc_bus_cfg_t *cfg, hrc_bus_t *bus)
{
	stm32_pin_out(cfg->port, cfg->cs, true, false);
	stm32_pin_out(cfg->port, cfg->clk, true, false);
	stm32_pin_out(cfg->port, cfg->mosi, false, false);
	stm32_pin_in(cfg->port, cfg->miso, GPIO_PULLUP);
	stm32_pin_out(cfg->voice_cs_port, cfg->voice_cs, true, false);
	stm32_spi_master_init(cfg->voice_spi, true, false, cfg->voice_br);
	bus->transfer = cfg_transfer;
	bus->delay_ms = hal_delay_ms;
	bus->delay_us = hal_delay_us;
	bus->ctx = (void *)cfg;
	bus->voice = voice_xfer;
}
