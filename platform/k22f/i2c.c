// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// I2C0 as a polled master (reference manual chapter 52) behind the generic i2c_bus_t: whole transactions to a 7-bit
// address, a write, or a write and a repeated START into a read. Every wait is bounded; a stuck bus reports an error
// instead of hanging the caller. The pins and their mux are the board's (it calls this once they carry I2C0).
#include "platform.h"
#include "periph_math.h"
#include "hal_time.h"

#define BYTE_WAIT_US	1000u	// one byte at 100 kHz takes 90 us
#define BUSY_WAIT_US	2000u

static uint32_t errors;

static bool wait_iicif(void)
{
	uint32_t start = hal_micros();
	while ((I2C0_S & I2C_S_IICIF) == 0) {
		if (hal_micros() - start > BYTE_WAIT_US) {
			return false;
		}
	}
	I2C0_S = I2C_S_IICIF;
	return true;
}

// One byte out; false on a timeout, lost arbitration or no acknowledge
static k22f_status_t put(uint8_t b)
{
	I2C0_D = b;
	if (!wait_iicif()) {
		return K22F_ERR_TIMEOUT;
	}
	if (I2C0_S & I2C_S_ARBL) {
		I2C0_S = I2C_S_ARBL;
		return K22F_ERR_BUS;
	}
	return (I2C0_S & I2C_S_RXAK) ? K22F_ERR_NACK : K22F_OK;
}

static void stop(void)
{
	I2C0_C1 = (uint8_t)(I2C0_C1 & ~(I2C_C1_MST | I2C_C1_TX | I2C_C1_TXAK));
	uint32_t start = hal_micros();
	while ((I2C0_S & I2C_S_BUSY) && hal_micros() - start < BUSY_WAIT_US) {
	}
}

static k22f_status_t start(uint8_t addr_rw, bool repeated)
{
	if (repeated) {
		I2C0_C1 = (uint8_t)(I2C0_C1 | I2C_C1_RSTA | I2C_C1_TX);
	} else {
		uint32_t t = hal_micros();
		while (I2C0_S & I2C_S_BUSY) {
			if (hal_micros() - t > BUSY_WAIT_US) {
				return K22F_ERR_BUS;
			}
		}
		I2C0_C1 = (uint8_t)(I2C0_C1 | I2C_C1_TX);
		I2C0_C1 = (uint8_t)(I2C0_C1 | I2C_C1_MST);	// START
	}
	return put(addr_rw);
}

static k22f_status_t read_bytes(uint8_t *r, size_t rn)
{
	// Receive: the next byte is NACKed when it is the last one; reading D starts the next byte, so the last byte is
	// read only after the STOP
	I2C0_C1 = (uint8_t)((I2C0_C1 & ~I2C_C1_TX) | (rn == 1u ? I2C_C1_TXAK : 0u));
	(void)I2C0_D;	// dummy read: starts the first byte
	for (size_t i = 0; i < rn; i++) {
		if (!wait_iicif()) {
			return K22F_ERR_TIMEOUT;
		}
		if (i + 1u == rn) {
			stop();
		} else if (i + 2u == rn) {
			I2C0_C1 = (uint8_t)(I2C0_C1 | I2C_C1_TXAK);
		}
		r[i] = I2C0_D;
	}
	return K22F_OK;
}

static k22f_status_t transaction(uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	k22f_status_t st = K22F_OK;
	if (wn > 0u || rn == 0u) {
		st = start((uint8_t)(addr7 << 1), false);
		for (size_t i = 0; st == K22F_OK && i < wn; i++) {
			st = put(w[i]);
		}
		if (st == K22F_OK && rn > 0u) {
			st = start((uint8_t)((addr7 << 1) | 1u), true);
		}
	} else {
		st = start((uint8_t)((addr7 << 1) | 1u), false);
	}
	if (st == K22F_OK && rn > 0u) {
		st = read_bytes(r, rn);		// sends the STOP itself when it gets to the end
		if (st != K22F_OK) {
			stop();
		}
	} else {
		stop();
	}
	if (st != K22F_OK) {
		errors++;
	}
	return st;
}

k22f_status_t k22f_i2c0_init(uint32_t max_hz)
{
	SIM_SCGC4 |= SIM_SCGC4_I2C0;
	I2C0_C1 = 0;
	uint32_t hz;
	uint8_t f = k22f_i2c_f(k22f_bus_clock(), max_hz, &hz);
	if (f == 0xFFu) {
		return K22F_ERR_ARG;
	}
	I2C0_F = f;
	I2C0_FLT = 2;	// glitch filter: pulses up to 2 bus clocks are ignored
	I2C0_S = I2C_S_IICIF | I2C_S_ARBL;
	I2C0_C1 = I2C_C1_IICEN;
	return K22F_OK;
}

uint32_t k22f_i2c0_errors(void)
{
	return errors;
}

static bool bus_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t n)
{
	(void)ctx;
	return transaction(addr7, data, n, NULL, 0) == K22F_OK;
}

static bool bus_write_read(void *ctx, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	(void)ctx;
	return transaction(addr7, w, wn, r, rn) == K22F_OK;
}

const i2c_bus_t k22f_i2c0_bus = { .write = bus_write, .write_read = bus_write_read };
