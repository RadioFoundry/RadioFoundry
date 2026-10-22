// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The external SPI NOR flash, for the flash stub. The wiring comes from the board record (f4stub.h): the SPI block,
// SCK / MISO / MOSI and their alternate function, the chip select, the SPI mode, and the other chip selects on the
// same bus, which are held high (a display on the flash's bus). The bus is set up from scratch, since ODR may
// have left it mid-use. Commands are the common SPI NOR set (JEDEC 0x9F, read 0x03 with 3 address bytes, write enable
// 0x06, status 0x05, 4 KB sector erase 0x20, page program 0x02, release from power-down 0xAB), so parts up to 16 MB
// work, in mode 0 or 3.
#include <stddef.h>
#include "stm32f4xx.h"
#include "f4stub_spi.h"
#include "f4stub_usb.h"

#define XFER_SPINS	100000u		// per byte: far longer than a byte takes at any prescaler
#define PAGE		256u
#define ERASE_MS	1000u		// a 4 KB sector: 400 ms maximum on the W25Q128 data sheet
#define PROGRAM_MS	10u		// a page: 3 ms maximum

static SPI_TypeDef *spi;
static GPIO_TypeDef *cs_port;
static uint16_t cs_mask;
static bool up;

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

static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
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

// One command: tx bytes out, then n bytes in (or n bytes of data out when out is set), chip select around it
static bool cmd(const uint8_t *tx, uint32_t ntx, uint8_t *in, const uint8_t *out, uint32_t n)
{
	cs(true);
	bool ok = xfer(tx, NULL, ntx) && (n == 0 || xfer(out, in, n));
	uint32_t spins = XFER_SPINS;
	while ((spi->SR & SPI_SR_BSY) != 0 && --spins > 0) {
	}
	cs(false);
	return ok;
}

static bool cmd_addr(uint8_t op, uint32_t addr, uint8_t *in, const uint8_t *out, uint32_t n)
{
	uint8_t tx[4] = { op, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
	return cmd(tx, 4, in, out, n);
}

static bool wait_ready(uint32_t ms)
{
	uint32_t t0 = stub_ms();
	for (;;) {
		uint8_t op = 0x05, st = 0xFF;
		if (!cmd(&op, 1, &st, NULL, 1)) {
			return false;
		}
		if ((st & 1u) == 0) {
			return true;
		}
		if (stub_ms() - t0 > ms) {
			return false;
		}
		usb_poll();
	}
}

static bool write_enable(void)
{
	uint8_t op = 0x06, st = 0;
	if (!cmd(&op, 1, NULL, NULL, 0)) {
		return false;
	}
	op = 0x05;
	return cmd(&op, 1, &st, NULL, 1) && (st & 2u) != 0;
}

uint32_t f4spi_init(const f4_board_t *b, uint32_t pclk_hz, uint8_t id[3])
{
	id[0] = id[1] = id[2] = 0;
	up = false;
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

	uint8_t wake = 0xAB, jedec = 0x9F;
	(void)cmd(&wake, 1, NULL, NULL, 0);
	stub_delay_ms(1);		// tRES1: 3 us
	if (!cmd(&jedec, 1, id, NULL, 3)) {
		return 0;
	}
	if ((id[0] == 0x00 && id[1] == 0x00) || (id[0] == 0xFF && id[1] == 0xFF) || id[2] < 0x10u) {
		return 0;
	}
	up = true;
	(void)wait_ready(ERASE_MS);	// ODR may have handed over in the middle of an erase
	return id[2] >= 0x18u ? 0x1000000u : 1u << id[2];	// 3 address bytes reach 16 MB
}

bool f4spi_read(uint32_t addr, uint8_t *dst, uint32_t n)
{
	return up && cmd_addr(0x03, addr, dst, NULL, n);
}

bool f4spi_erase(uint32_t addr)
{
	return up && wait_ready(ERASE_MS) && write_enable() && cmd_addr(0x20, addr, NULL, NULL, 0) && wait_ready(ERASE_MS);
}

bool f4spi_program(uint32_t addr, const uint8_t *data, uint32_t n)
{
	if (!up) {
		return false;
	}
	while (n > 0) {
		uint32_t k = PAGE - addr % PAGE;
		if (k > n) {
			k = n;
		}
		if (!wait_ready(PROGRAM_MS) || !write_enable() || !cmd_addr(0x02, addr, NULL, data, k) ||
		    !wait_ready(PROGRAM_MS)) {
			return false;
		}
		addr += k;
		data += k;
		n -= k;
	}
	return true;
}
