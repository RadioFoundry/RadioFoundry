// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 RF bus: the bit-banged HR_C5000 configuration port and the SKY72310 synthesizer bus, which share the data
// line (hardware-md380.md 4.2, 5.2). Kept apart from radio_md380.c so the host test (tests/md380RfbusTest.c) can
// run it against fake chips wired as the hardware is.
#include <stddef.h>
#include "rfbus_md380.h"
#include "pins.h"
#include "FreeRTOS.h"

// The two have their own clocks and chip selects but share the data line (PE5), so a transaction on either is one
// masked section

uint32_t md380_rfbus_lock(void)
{
	return portSET_INTERRUPT_MASK_FROM_ISR();
}

void md380_rfbus_unlock(uint32_t mask)
{
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

// About 110 ns at 72 MHz on top of the GPIO writes
static inline void bus_delay(void)
{
	__NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
}

// HR_C5000 byte: the clock idles low. Per bit: clock high, set MOSI, clock low (the chip samples MOSI here), wait, then
// sample MISO
static uint8_t bus_byte(uint8_t out)
{
	uint8_t in = 0;
	for (int i = 0; i < 8; i++) {
		PIN_WRITE(PIN_C5000_CLK, true);
		PIN_WRITE(PIN_C5000_MOSI, (out & 0x80u) != 0);
		out = (uint8_t)(out << 1);
		bus_delay();
		PIN_WRITE(PIN_C5000_CLK, false);
		// The chip drives the next bit after the falling edge: sampled at once, every read came one bit late
		// (0x10 read 0x40 after 0x80 was written, an LC read 00 .. 31 98 2D 2F for 00 .. 63 30 5A 5E, on air
		// 2026-10-18)
		bus_delay();
		in = (uint8_t)((in << 1) | (PIN_READ(PIN_C5000_MISO) ? 1u : 0u));
	}
	return in;
}

bool md380_rfbus_modem_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint32_t mask = md380_rfbus_lock();
	PIN_WRITE(PIN_C5000_CLK, false);
	PIN_WRITE(PIN_C5000_CS, false);
	bus_delay();
	for (uint8_t i = 0; i < hdr_len; i++) {
		bus_byte(hdr[i]);
	}
	for (uint16_t i = 0; i < len; i++) {
		uint8_t in = bus_byte(tx != NULL ? tx[i] : 0xFF);
		if (rx != NULL) {
			rx[i] = in;
		}
	}
	bus_delay();
	PIN_WRITE(PIN_C5000_CS, true);
	md380_rfbus_unlock(mask);
	return true;
}

// One 16-bit synthesizer word, MSB first, latched when its chip select goes high. The clock idles high: data is set
// while it is high, then it falls and rises again (the chip latches the bit on the rising edge)
bool md380_rfbus_pll_write(void *ctx, uint16_t word)
{
	uint32_t mask = md380_rfbus_lock();
	PIN_WRITE(PIN_PLL_CLK, true);
	bus_delay();
	PIN_WRITE(PIN_PLL_CS, false);
	bus_delay();
	for (int i = 0; i < 16; i++) {
		PIN_WRITE(PIN_PLL_DATA, (word & 0x8000u) != 0);
		word = (uint16_t)(word << 1);
		PIN_WRITE(PIN_PLL_CLK, false);
		bus_delay();
		PIN_WRITE(PIN_PLL_CLK, true);
		bus_delay();
	}
	PIN_WRITE(PIN_PLL_CS, true);
	md380_rfbus_unlock(mask);
	return true;
}

bool md380_rfbus_pll_locked(void *ctx)
{
	return PIN_READ(PIN_PLL_LD);
}

// VCO supply select: high is the receive VCO, low the transmit VCO
void md380_rfbus_vco_select(bool tx)
{
	PIN_WRITE(PIN_VCO_SEL, !tx);
}
