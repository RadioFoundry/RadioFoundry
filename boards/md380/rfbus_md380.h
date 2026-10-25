// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MD-380's bit-banged RF bus: HR_C5000 configuration port and SKY72310 synthesizer bus (rfbus_md380.c)
#ifndef RFBUS_MD380_H
#define RFBUS_MD380_H

#include <stdint.h>
#include <stdbool.h>

// The two chips share the data line: every transaction on either is one masked section (interrupts up to the RTOS
// syscall level off), so a modem ISR never lands inside one
uint32_t md380_rfbus_lock(void);
void md380_rfbus_unlock(uint32_t mask);

// HR_C5000 transfer (hrc_bus_t.transfer): CS low, hdr_len header bytes, len data bytes, CS high
bool md380_rfbus_modem_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx,
				uint16_t len);
// SKY72310 (sky72310_bus_t): one 16-bit word, and the lock detect line
bool md380_rfbus_pll_write(void *ctx, uint16_t word);
bool md380_rfbus_pll_locked(void *ctx);
// VCO supply: the transmit VCO when tx, else the receive VCO
void md380_rfbus_vco_select(bool tx);

#endif
