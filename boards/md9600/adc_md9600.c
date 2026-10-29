// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ADC1 continuous scan into a circular DMA buffer (periph_md9600.h): DMA2 Stream0 channel 0 keeps the last scans of
// every rank, so a reading is a short average with no interrupt and no locking. 480-cycle sampling at PCLK2 / 8 on
// every rank: slow sources (supply, RSSI, noise, keypad ladders), about 0.4 ms per eight-channel scan at 72 MHz
#include "periph_md9600.h"

static volatile uint16_t *buf;
static uint8_t ranks;
static uint16_t scans;

static void start(void)
{
	DMA2_Stream0->CR = 0;
	while (DMA2_Stream0->CR & DMA_SxCR_EN) {
	}
	DMA2->LIFCR = DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0;
	DMA2_Stream0->PAR = (uint32_t)&ADC1->DR;
	DMA2_Stream0->M0AR = (uint32_t)buf;
	DMA2_Stream0->NDTR = (uint32_t)ranks * scans;
	DMA2_Stream0->FCR = 0;	// direct mode
	// Channel 0 (ADC1), peripheral to memory, 16-bit both sides, memory increment, circular, low priority
	DMA2_Stream0->CR = DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC;
	DMA2_Stream0->CR |= DMA_SxCR_EN;

	ADC1->CR2 &= ~ADC_CR2_ADON;
	ADC1->SR = 0;
	ADC1->CR2 = ADC_CR2_CONT | ADC_CR2_DMA | ADC_CR2_DDS | ADC_CR2_ADON;
	for (volatile uint32_t i = 0; i < 400u; i++) {	// stabilization, a few microseconds
	}
	ADC1->CR2 |= ADC_CR2_SWSTART;
}

bool stm32_adc1_scan_start(const uint8_t *channels, uint8_t n, volatile uint16_t *buffer, uint16_t n_scans)
{
	if (n == 0 || n > 16 || n_scans == 0 || ((uintptr_t)buffer >> 24) != 0x20u) {
		return false;	// the DMA can't reach the CCM
	}
	buf = buffer;
	ranks = n;
	scans = n_scans;
	RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
	(void)RCC->AHB1ENR;
	ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0 | ADC_CCR_ADCPRE_1 | ADC_CCR_TSVREFE;	// PCLK2 / 8
	ADC1->CR1 = ADC_CR1_SCAN;	// 12-bit
	ADC1->SMPR1 = 0x07FFFFFFu;	// 480 cycles on every channel
	ADC1->SMPR2 = 0x3FFFFFFFu;
	uint32_t sq[3] = { 0, 0, 0 };
	for (uint8_t i = 0; i < n; i++) {
		sq[i / 6u] |= (uint32_t)(channels[i] & 0x1Fu) << (5u * (i % 6u));
	}
	ADC1->SQR3 = sq[0];
	ADC1->SQR2 = sq[1];
	ADC1->SQR1 = sq[2] | ((uint32_t)(n - 1u) << ADC_SQR1_L_Pos);
	start();
	return true;
}

uint16_t stm32_adc1_scan_avg(uint8_t rank)
{
	if (buf == NULL || rank >= ranks) {
		return 0;
	}
	// An overrun stops the converter: start again (the buffer still holds the last good scans)
	if ((ADC1->SR & ADC_SR_OVR) || !(DMA2_Stream0->CR & DMA_SxCR_EN)) {
		start();
	}
	uint32_t sum = 0;
	for (uint32_t i = rank; i < (uint32_t)ranks * scans; i += ranks) {
		sum += buf[i];
	}
	return (uint16_t)(sum / scans);
}
