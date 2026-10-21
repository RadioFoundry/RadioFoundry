// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The external SPI NOR flash's bus, for the flash stub. The wiring comes from the board record (f4stub.h): the SPI
// block, SCK / MISO / MOSI and their alternate function, the chip select, the SPI mode, and the other chip selects on
// the same bus, which are held high (the MD-9600 shares SPI2 with its LCD). The bus is set up from scratch, since ODR
// may have left it mid-use. The chip's commands are shared with the other stubs (stub/common/stubnor.c); mode 0 or 3.
#include <stddef.h>
#include "stm32f4xx.h"
#include "f4stub_spi.h"
#include "stubnor.h"

#define XFER_SPINS	100000u		// per byte: far longer than a byte takes at any prescaler

static SPI_TypeDef *spi;
static GPIO_TypeDef *cs_port;
static uint16_t cs_mask;

static GPIO_TypeDef *port_of(uint8_t pin)
{
	return (GPIO_TypeDef *)(GPIOA_BASE + (pin >> 4) * (GPIOB_BASE - GPIOA_BASE));
}

static void port_clock(uint8_t pin)
{
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN << (pin >> 4);
	(void)RCC->AHB1ENR;
}

// A push-pull output driven high (a chip select at rest)
static void out_high(uint8_t pin)
{
	if (pin == F4_PIN_NONE) {
		return;
	}
	unsigned n = pin & 15u;
	GPIO_TypeDef *g = port_of(pin);
	port_clock(pin);
	g->BSRR = 1u << n;
	g->OTYPER &= ~(1u << n);
	g->MODER = (g->MODER & ~(3u << (2u * n))) | (1u << (2u * n));
}

static void alt(uint8_t pin, uint8_t af)
{
	unsigned n = pin & 15u;
	GPIO_TypeDef *g = port_of(pin);
	port_clock(pin);
	g->AFR[n >> 3] = (g->AFR[n >> 3] & ~(0xFu << (4u * (n & 7u)))) | ((uint32_t)af << (4u * (n & 7u)));
	g->OTYPER &= ~(1u << n);
	g->OSPEEDR = (g->OSPEEDR & ~(3u << (2u * n))) | (2u << (2u * n));
	g->PUPDR &= ~(3u << (2u * n));
	g->MODER = (g->MODER & ~(3u << (2u * n))) | (2u << (2u * n));
}

static void cs(bool active)
{
	cs_port->BSRR = active ? ((uint32_t)cs_mask << 16) : cs_mask;
}

bool nor_hw_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		uint32_t spins = XFER_SPINS;
		while ((spi->SR & SPI_SR_TXE) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		*(volatile uint8_t *)&spi->DR = tx != NULL ? tx[i] : 0xFFu;
		spins = XFER_SPINS;
		while ((spi->SR & SPI_SR_RXNE) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		uint8_t b = *(volatile uint8_t *)&spi->DR;
		if (rx != NULL) {
			rx[i] = b;
		}
	}
	return true;
}

void nor_hw_select(bool on)
{
	if (!on) {
		uint32_t spins = XFER_SPINS;
		while ((spi->SR & SPI_SR_BSY) != 0 && --spins > 0) {
		}
	}
	cs(on);
}

uint32_t f4spi_init(const f4_board_t *b, uint32_t pclk_hz, uint8_t id[3])
{
	id[0] = id[1] = id[2] = 0;
	if (b->spi_bus == 0) {
		return 0;
	}
	if (b->spi_bus == 1u) {
		spi = SPI1;
		RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
		(void)RCC->APB2ENR;
	} else {
		spi = b->spi_bus == 2u ? SPI2 : SPI3;
		RCC->APB1ENR |= b->spi_bus == 2u ? RCC_APB1ENR_SPI2EN : RCC_APB1ENR_SPI3EN;
		(void)RCC->APB1ENR;
	}
	// Every chip select on the bus high before the clock line moves
	cs_port = port_of(b->cs);
	cs_mask = (uint16_t)(1u << (b->cs & 15u));
	out_high(b->cs);
	out_high(b->idle[0]);
	out_high(b->idle[1]);
	alt(b->sck, b->af);
	alt(b->miso, b->af);
	alt(b->mosi, b->af);
	// The slowest of the prescalers at or under the record's rate: PCLK / 2^(br + 1)
	uint32_t br = 0;
	while (br < 7u && pclk_hz / (2u << br) > b->spi_hz) {
		br++;
	}
	spi->CR1 = 0;
	spi->CR2 = 0;		// ODR may have left DMA requests on
	spi->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (br << SPI_CR1_BR_Pos) | b->spi_mode;
	spi->CR1 |= SPI_CR1_SPE;
	(void)spi->DR;

	return stubnor_probe(id);
}
