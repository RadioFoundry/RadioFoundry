// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Two STM32F4 peripheral helpers only this board uses so far: the synthesizers' and IF receivers' three-wire bus on
// GPIO (wire3_md9600.c) and the ADC1 scan (adc_md9600.c). They move to platform/stm32f4 when a second radio needs them.
// Firmware only: the host tests include md9600.h, not this file.
#ifndef PERIPH_MD9600_H
#define PERIPH_MD9600_H

#include "platform.h"

// A write-only three-wire bus bit-banged on GPIO, the transport behind drivers/bus/wire3.h: pass a stm32_wire3_t as
// the bus context. The pins must already be outputs at their idle levels (chip selects high)
typedef struct {
	GPIO_TypeDef *clk_port, *data_port;
	uint16_t clk_pin, data_pin;
	GPIO_TypeDef *cs_port[2];
	uint16_t cs_pin[2];
	uint8_t cs_count;
	uint16_t half_period_ns;	// clock high and low time
} stm32_wire3_t;
bool stm32_wire3_write(void *ctx, uint8_t cs, uint32_t word, uint8_t bits);

// ADC1 continuous scan of n channels (rank order) into buffer, n_scans scans deep, through DMA2 Stream0. buffer must
// be in main SRAM (DMA). False when it isn't, or n is 0 or over 16
bool stm32_adc1_scan_start(const uint8_t *channels, uint8_t n, volatile uint16_t *buffer, uint16_t n_scans);
// The average of one rank over the buffered scans (raw 12-bit); restarts the converter after an overrun
uint16_t stm32_adc1_scan_avg(uint8_t rank);

#endif
