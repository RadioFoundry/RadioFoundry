// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// SPI master on the registers, byte at a time (platform.h): no HAL time base or DMA needed, so it works before the
// scheduler, and tx / rx may be NULL. Shared by the boards' SPI flash and LCD glue
#include "platform.h"

#define XFER_SPINS		100000u		// per byte: a byte takes 1.8 us at 4.5 MHz, 0.4 us at 18 MHz

void stm32_spi_master_init(SPI_TypeDef *spi, bool cpol, bool cpha, uint8_t br)
{
	if (spi == SPI1) {
		__HAL_RCC_SPI1_CLK_ENABLE();
	} else if (spi == SPI2) {
		__HAL_RCC_SPI2_CLK_ENABLE();
	} else {
		__HAL_RCC_SPI3_CLK_ENABLE();
	}
	spi->CR1 = 0;
	// Master, software chip select held inactive (SSM, SSI), 8-bit, MSB first, the baud rate divider 2^(br + 1)
	spi->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | ((uint32_t)(br & 7u) << SPI_CR1_BR_Pos) |
		   (cpol ? SPI_CR1_CPOL : 0u) | (cpha ? SPI_CR1_CPHA : 0u);
	spi->CR2 = 0;
	spi->CR1 |= SPI_CR1_SPE;
}

bool stm32_spi_xfer(SPI_TypeDef *r, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		uint32_t spins = XFER_SPINS;
		while ((r->SR & SPI_SR_TXE) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		*(volatile uint8_t *)&r->DR = tx != NULL ? tx[i] : 0xFF;
		spins = XFER_SPINS;
		while ((r->SR & SPI_SR_RXNE) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		uint8_t b = *(volatile uint8_t *)&r->DR;
		if (rx != NULL) {
			rx[i] = b;
		}
	}
	uint32_t spins = XFER_SPINS;
	while ((r->SR & SPI_SR_BSY) != 0) {
		if (--spins == 0) {
			return false;
		}
	}
	return true;
}
