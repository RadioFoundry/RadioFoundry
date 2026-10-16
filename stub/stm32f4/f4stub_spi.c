// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The external SPI NOR flash, for the flash stub. Both radios the stub serves wire it the same way: SPI1 on PB3 (SCK),
// PB4 (MISO), PB5 (MOSI), alternate function 5, chip select PD7, mode 0 (hardware-dm1701.md 2, hardware-md380.md 12).
// The bus is set up from scratch, since ODR may have left it mid-use. Commands are the common SPI NOR set (JEDEC
// 0x9F, read 0x03 with 3 address bytes, write enable 0x06, status 0x05, 4 KB sector erase 0x20, page program 0x02,
// release from power-down 0xAB), so parts up to 16 MB work.
#include <stddef.h>
#include "stm32f4xx.h"
#include "f4stub_spi.h"
#include "f4stub_usb.h"

#define SPI_MAX_HZ	4500000u	// what ODR runs this bus at (boards/dm1701/flash_dm1701.c)
#define XFER_SPINS	100000u		// per byte: far longer than a byte takes at any prescaler
#define PAGE		256u
#define ERASE_MS	1000u		// a 4 KB sector: 400 ms maximum on the W25Q128 data sheet
#define PROGRAM_MS	10u		// a page: 3 ms maximum
#define CS_PIN		7u

static bool up;

static void cs(bool active)
{
	GPIOD->BSRR = active ? (1u << (CS_PIN + 16u)) : (1u << CS_PIN);
}

static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		uint32_t spins = XFER_SPINS;
		while ((SPI1->SR & SPI_SR_TXE) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		*(volatile uint8_t *)&SPI1->DR = tx != NULL ? tx[i] : 0xFFu;
		spins = XFER_SPINS;
		while ((SPI1->SR & SPI_SR_RXNE) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		uint8_t b = *(volatile uint8_t *)&SPI1->DR;
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
	while ((SPI1->SR & SPI_SR_BSY) != 0 && --spins > 0) {
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

uint32_t f4spi_init(uint32_t pclk2_hz, uint8_t id[3])
{
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIODEN;
	RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
	(void)RCC->APB2ENR;
	cs(false);
	GPIOD->MODER = (GPIOD->MODER & ~(3u << (2u * CS_PIN))) | (1u << (2u * CS_PIN));
	GPIOD->OTYPER &= ~(1u << CS_PIN);
	for (unsigned pin = 3; pin <= 5; pin++) {
		GPIOB->MODER = (GPIOB->MODER & ~(3u << (2u * pin))) | (2u << (2u * pin));
		GPIOB->OTYPER &= ~(1u << pin);
		GPIOB->OSPEEDR |= 2u << (2u * pin);
		GPIOB->PUPDR &= ~(3u << (2u * pin));
		GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFu << (4u * pin))) | (5u << (4u * pin));
	}
	// The slowest of the prescalers at or under SPI_MAX_HZ: PCLK2 / 2^(br + 1)
	uint32_t br = 0;
	while (br < 7u && pclk2_hz / (2u << br) > SPI_MAX_HZ) {
		br++;
	}
	SPI1->CR1 = 0;
	SPI1->CR2 = 0;
	SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (br << SPI_CR1_BR_Pos);
	SPI1->CR1 |= SPI_CR1_SPE;
	(void)SPI1->DR;

	uint8_t wake = 0xAB, jedec = 0x9F;
	id[0] = id[1] = id[2] = 0;
	(void)cmd(&wake, 1, NULL, NULL, 0);
	stub_delay_ms(1);		// tRES1: 3 us
	if (!cmd(&jedec, 1, id, NULL, 3)) {
		return 0;
	}
	if ((id[0] == 0x00 && id[1] == 0x00) || (id[0] == 0xFF && id[1] == 0xFF) || id[2] < 0x10u) {
		return 0;
	}
	up = true;
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
