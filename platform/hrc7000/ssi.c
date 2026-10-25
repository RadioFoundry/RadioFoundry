// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DesignWare SSI master (SPI0 0x140A0000, SPI2 0x140E0000; hardware-dm32.md 3.2): 8-bit frames, mode 0, polled one byte
// at a time, no FIFO depth assumed. Chip selects are GPIOs the board drives, so SER only gates the clock.
#include <stddef.h>
#include "platform.h"

#define SSI_CTRLR0(b)		REG32((b) + 0x00)
#define SSI_CTRLR1(b)		REG32((b) + 0x04)
#define SSI_SSIENR(b)		REG32((b) + 0x08)
#define SSI_SER(b)		REG32((b) + 0x10)
#define SSI_BAUDR(b)		REG32((b) + 0x14)
#define SSI_TXFTLR(b)		REG32((b) + 0x18)
#define SSI_RXFTLR(b)		REG32((b) + 0x1C)
#define SSI_SR(b)		REG32((b) + 0x28)
#define SSI_IMR(b)		REG32((b) + 0x2C)
#define SSI_DMACR(b)		REG32((b) + 0x4C)
#define SSI_DR(b)		REG32((b) + 0x60)
#define SSI_SR_BUSY		0x01u
#define SSI_SR_TFNF		0x02u	// TX FIFO not full
#define SSI_SR_TFE		0x04u	// TX FIFO empty
#define CTRLR0_8BIT_MODE0	0x07u	// 8-bit frames, mode 0, transmit and receive

#define SPINS			100000u

void hrc_ssi_init(uint32_t base, uint16_t baudr)
{
	SSI_SSIENR(base) = 0;
	SSI_CTRLR0(base) = CTRLR0_8BIT_MODE0;
	SSI_CTRLR1(base) = 0;
	SSI_BAUDR(base) = baudr;
	SSI_TXFTLR(base) = 0;
	SSI_RXFTLR(base) = 0;
	SSI_IMR(base) = 0;
	SSI_DMACR(base) = 0;
	SSI_SER(base) = 0;
	SSI_SSIENR(base) = 1;
}

void hrc_ssi_select(uint32_t base, bool active)
{
	SSI_SER(base) = active ? 1u : 0u;
}

static bool wait_sr(uint32_t base, uint32_t mask, uint32_t want)
{
	for (uint32_t n = SPINS; n != 0; n--) {
		if ((SSI_SR(base) & mask) == want) {
			return true;
		}
	}
	return false;
}

bool hrc_ssi_xfer(uint32_t base, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		if (!wait_sr(base, SSI_SR_TFNF, SSI_SR_TFNF)) {
			return false;
		}
		SSI_DR(base) = tx != NULL ? tx[i] : 0xFFu;
		if (!wait_sr(base, SSI_SR_TFE, SSI_SR_TFE) || !wait_sr(base, SSI_SR_BUSY, 0)) {
			return false;
		}
		uint8_t b = (uint8_t)SSI_DR(base);
		if (rx != NULL) {
			rx[i] = b;
		}
	}
	return true;
}
