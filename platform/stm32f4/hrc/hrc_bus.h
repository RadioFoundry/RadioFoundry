// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C6000's two MCU ports on the STM32F4 radios with one: the configuration port bit-banged on four GPIO
// lines of one port, and the voice port on an SPI peripheral with its own chip select (docs/spec/modem-hrc6000.md 1,
// 1.1). hrc_bus.c gives the hrc_bus_t the chip driver runs on. Both transfers mask interrupts up to the RTOS syscall
// level, where the chip's own interrupts run, so a modem ISR never lands inside one: safe from tasks and those ISRs.
#ifndef STM32_HRC_BUS_H
#define STM32_HRC_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"
#include "modem/hrc_common/hrc.h"

typedef struct {
	GPIO_TypeDef *port;	// the configuration port's four lines
	uint16_t cs, clk, mosi, miso;	// pin masks on port (MOSI: MCU to chip)
	SPI_TypeDef *voice_spi;	// voice port: SPI master, clock idle high, data on the first edge
	uint8_t voice_br;	// its clock: PCLK / 2^(br + 1)
	GPIO_TypeDef *voice_cs_port;
	uint16_t voice_cs;
} stm32_hrc_bus_cfg_t;

// Set the lines up (chip selects high, the clock at its idle level, the voice SPI on) and fill bus for hrc6000_attach.
// The voice SPI's pins must already be on their alternate function. cfg and bus stay valid for the bus's life
void stm32_hrc_bus_init(const stm32_hrc_bus_cfg_t *cfg, hrc_bus_t *bus);

#endif
