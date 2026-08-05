// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GPIO by pin index 0-95 (docs/spec/hardware-dm32.md 3.9). DesignWare APB GPIO: DR / DDR are whole-bank registers, so
// every change is a read-modify-write with interrupts off (the modem interrupts switch PA and LNA pins).
#include "platform.h"

static void bank_rmw(uint32_t reg, uint32_t bit, bool set)
{
	uint32_t psr = hrc_irq_save();
	uint32_t v = REG32(reg);
	REG32(reg) = set ? (v | bit) : (v & ~bit);
	hrc_irq_restore(psr);
}

void hrc_pin_write(uint8_t pin, bool level)
{
	bank_rmw(hrc_gpio_bank(pin) + GPIO_DR, hrc_gpio_bit(pin), level);
}

void hrc_pin_out(uint8_t pin, bool level)
{
	hrc_pin_write(pin, level);	// level first, so the pin never glitches when it becomes an output
	bank_rmw(hrc_gpio_bank(pin) + GPIO_DDR, hrc_gpio_bit(pin), true);
}

void hrc_pin_in(uint8_t pin)
{
	uint32_t bank = hrc_gpio_bank(pin), bit = hrc_gpio_bit(pin);
	bank_rmw(bank + GPIO_INTEN, bit, false);	// inputs get no interrupt unless asked for (spec 16 item 9)
	bank_rmw(bank + GPIO_DDR, bit, false);
}

void hrc_pin_irq(uint8_t pin, bool on, bool both, bool rising)
{
	uint32_t bank = hrc_gpio_bank(pin), bit = hrc_gpio_bit(pin);
	bank_rmw(bank + GPIO_INTEN, bit, false);
	if (!on) {
		return;
	}
	bank_rmw(bank + GPIO_INTTYPE, bit, true);	// edge
	bank_rmw(bank + GPIO_BOTHEDGE, bit, both);
	bank_rmw(bank + GPIO_POLARITY, bit, rising);
	bank_rmw(bank + GPIO_INTMASK, bit, false);
	REG32(bank + GPIO_EOI) = bit;
	bank_rmw(bank + GPIO_INTEN, bit, true);
}

uint32_t hrc_gpio_pending(uint32_t bank)
{
	return REG32(bank + GPIO_INTSTATUS);
}

void hrc_gpio_ack(uint32_t bank, uint32_t bits)
{
	REG32(bank + GPIO_EOI) = bits;
}
