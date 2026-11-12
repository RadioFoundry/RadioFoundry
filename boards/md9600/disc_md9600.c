// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 receive audio from the discriminator on PC3 (RX_2T_5T, ADC123 IN13, docs/spec/hardware-md9600.md 19.9):
// voice band with the sub-audio removed. FM receive audio never passes through the HR_C6000 on this radio (6.1), so
// this is the MCU's only copy of it. Two readers share one capture: APRS receive (afsk_md9600.c) and the PCM stream's
// FM receive tap (c6000_md9600.c). Each keeps its own tail on the ring; the capture runs while either wants it.
//
// ADC3 converts continuously into a circular DMA buffer (DMA2 stream 1, channel 2); each half-transfer interrupt sums
// pairs of conversions, takes the DC away and appends the samples to the ring. ADC2 is the sub-audio decoder's
// (subtone_md9600.c); ADC3 otherwise makes the diagnostics page's one-off PA2 readings (md9600_adc3_qt), which wait
// while this runs. Start and stop in the radio task only.
#include "config.h"
#include "md9600.h"

#if MD9600_DISC
#include <stdbool.h>
#include "pins.h"
#include "platform.h"
#include "hal_time.h"

// ADC clock PCLK2 (72 MHz) / 8, 480 + 12 cycles a conversion: 18293 Hz. Sums of 2: 9146 Hz, 13 bits
#define DECIM			2u
#define HALF_OUT		32u				// samples a half buffer: an interrupt every 3.5 ms
#define HALF_RAW		(HALF_OUT * DECIM)
#define IRQ_PRIORITY		7				// no RTOS calls in the handler
#define ADC_CH			13u				// IN13, PC3
#define GAIN			4				// the summed 13-bit swing to most of 16 bits
#define DC_SHIFT		10				// DC tracker: about 0.11 s, far below the voice band

static volatile uint16_t raw[2u * HALF_RAW];
static int16_t ring[MD9600_DISC_RING];
static volatile uint32_t head;	// written by the interrupt
static int32_t dc;		// summed counts << DC_SHIFT
static bool dc_primed;
static uint8_t users;

void DMA2_Stream1_IRQHandler(void)
{
	uint32_t isr = DMA2->LISR;
	DMA2->LIFCR = DMA_LIFCR_CTCIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CFEIF1;
	if (!(isr & (DMA_LISR_HTIF1 | DMA_LISR_TCIF1))) {
		return;
	}
	const volatile uint16_t *p = &raw[(isr & DMA_LISR_TCIF1) ? HALF_RAW : 0u];
	uint32_t h = head;
	for (uint32_t o = 0; o < HALF_OUT; o++) {
		int32_t sum = 0;
		for (uint32_t k = 0; k < DECIM; k++) {
			sum += *p++;
		}
		if (!dc_primed) {
			dc_primed = true;
			dc = sum << DC_SHIFT;
		}
		dc += sum - (dc >> DC_SHIFT);
		int32_t s = (sum - (dc >> DC_SHIFT)) * GAIN;
		if (s > 32767) {
			s = 32767;
		} else if (s < -32768) {
			s = -32768;
		}
		ring[h % MD9600_DISC_RING] = (int16_t)s;
		h++;
	}
	head = h;	// a reader more than the ring behind loses the oldest
}

static void capture_start(void)
{
	stm32_pin_analog(PIN_PORT(PIN_DISC_AF), PIN_MASK(PIN_DISC_AF));
	RCC->APB2ENR |= RCC_APB2ENR_ADC3EN;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
	(void)RCC->APB2ENR;
	// The common prescaler (PCLK2 / 8) is the ADC1 scan's (adc_md9600.c), set at power-up

	DMA2_Stream1->CR = 0;
	while (DMA2_Stream1->CR & DMA_SxCR_EN) {
	}
	DMA2->LIFCR = DMA_LIFCR_CTCIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CFEIF1;
	DMA2_Stream1->PAR = (uint32_t)&ADC3->DR;
	DMA2_Stream1->M0AR = (uint32_t)raw;
	DMA2_Stream1->NDTR = 2u * HALF_RAW;
	DMA2_Stream1->FCR = 0;	// direct mode
	// Channel 2 (ADC3), peripheral to memory, 16-bit both sides, memory increment, circular, both interrupts
	DMA2_Stream1->CR = DMA_SxCR_CHSEL_1 | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
			   DMA_SxCR_HTIE | DMA_SxCR_TCIE;
	dc_primed = false;
	HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
	DMA2_Stream1->CR |= DMA_SxCR_EN;

	ADC3->CR2 = 0;
	ADC3->SR = 0;
	ADC3->CR1 = 0;		// 12-bit, one channel
	ADC3->SMPR1 = 7u << ADC_SMPR1_SMP13_Pos;	// 480 cycles
	ADC3->SQR1 = 0;
	ADC3->SQR3 = ADC_CH;
	ADC3->CR2 = ADC_CR2_CONT | ADC_CR2_DMA | ADC_CR2_DDS | ADC_CR2_ADON;
	hal_delay_us(5);	// ADC stabilization time
	ADC3->CR2 |= ADC_CR2_SWSTART;
}

static void capture_stop(void)
{
	HAL_NVIC_DisableIRQ(DMA2_Stream1_IRQn);
	ADC3->CR2 = 0;
	DMA2_Stream1->CR = 0;
	while (DMA2_Stream1->CR & DMA_SxCR_EN) {
	}
	// md9600_adc3_qt sets ADC3 up again for its one-off readings
	RCC->APB2ENR &= ~RCC_APB2ENR_ADC3EN;
}

void md9600_disc_start(uint8_t user)
{
	if (users == 0) {
		users = user;	// md9600_adc3_qt leaves ADC3 alone from here
		capture_start();
	}
	users |= user;
}

void md9600_disc_stop(uint8_t user)
{
	if (users == 0) {
		return;
	}
	users &= (uint8_t)~user;
	if (users == 0) {
		capture_stop();
	}
}

bool md9600_disc_running(void)
{
	return users != 0;
}

uint32_t md9600_disc_head(void)
{
	return head;
}

const volatile int16_t *md9600_disc_ring(void)
{
	return ring;
}
#endif
