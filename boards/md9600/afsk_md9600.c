// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 APRS AFSK (hal_afsk.h, docs/spec/aprs.md 5).
//
// Transmit (CONFIG_MD9600_TX): the HR_C6000's tone generator in place of the microphone, as for the 1750 Hz burst,
// retuned at every symbol boundary by the TIM6 interrupt (priority 5, the RTOS syscall level: each configuration
// port transaction masks it, so a retune never lands inside one). A retune is two table entries, four register
// writes, about 40 us of an 833 us symbol. Unverified on air: the generator's phase across a retune and the
// deviation per level.
//
// Receive: the discriminator audio on PC3
// (RX_2T_5T, ADC123 IN13, docs/spec/hardware-md9600.md 19.9), voice band with the sub-audio removed. While the core
// listens, ADC3 converts it continuously into a circular DMA buffer (DMA2 stream 1, channel 2); each half-transfer
// interrupt sums pairs of conversions, takes the DC away and queues the samples for rx_read. ADC2 is the sub-audio
// decoder's (subtone_md9600.c); ADC3 otherwise makes the diagnostics page's one-off PA2 readings (md9600_adc3_qt),
// which wait while this runs.
#include "config.h"

#if CONFIG_APRS && (CONFIG_APRS_RX || CONFIG_MD9600_TX)
#include <stdbool.h>
#include "md9600.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"

#if CONFIG_MD9600_TX
#define TX_IRQ_PRIORITY		5	// the RTOS syscall level (no RTOS calls in the handler)

static const uint8_t *volatile tx_tones;
static volatile uint16_t tx_nbits;
static volatile uint16_t tx_pos;	// next symbol
static uint16_t tx_mark, tx_space;
static volatile uint16_t tx_hz;
static volatile bool tx_running;
static volatile uint32_t tx_missed;	// retunes that failed: a corrupt frame

static bool tx_symbol(uint16_t i)
{
	return ((tx_tones[i >> 3] >> (i & 7u)) & 1u) != 0;
}

static void tx_timer_stop(void)
{
	TIM6->CR1 = 0;
	TIM6->DIER = 0;
	HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
	TIM6->SR = 0;
}

static void tx_timer_start(uint16_t baud)
{
	__HAL_RCC_TIM6_CLK_ENABLE();
	uint32_t pclk = HAL_RCC_GetPCLK1Freq();
	uint32_t clk = ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) ? pclk : 2u * pclk;	// APB1 timer clock
	TIM6->CR1 = 0;
	TIM6->PSC = 1;					// 72 MHz / 2
	TIM6->ARR = clk / 2u / baud - 1u;		// 29999 at 1200 baud
	TIM6->CNT = 0;
	TIM6->EGR = TIM_EGR_UG;				// load PSC
	TIM6->SR = 0;
	TIM6->DIER = TIM_DIER_UIE;
	HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TX_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
	TIM6->CR1 = TIM_CR1_CEN;
}

void TIM6_DAC_IRQHandler(void)
{
	if ((TIM6->SR & TIM_SR_UIF) == 0) {
		return;
	}
	TIM6->SR = (uint16_t)~TIM_SR_UIF;
	uint16_t i = tx_pos;
	if (i >= tx_nbits) {
		tx_timer_stop();	// the last symbol has had its full time
		tx_running = false;
		return;
	}
	uint16_t hz = tx_symbol(i) ? tx_mark : tx_space;
	if (hz != tx_hz) {
		if (md9600_c6000_afsk_tone(hz)) {
			tx_hz = hz;
		} else {
			tx_missed++;
		}
	}
	tx_pos = (uint16_t)(i + 1u);
}

static bool tx_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	if (tx_running || n == 0 || baud == 0) {
		return false;
	}
	tx_tones = t;
	tx_nbits = n;
	tx_mark = mark;
	tx_space = space;
	tx_hz = tx_symbol(0) ? mark : space;
	if (!md9600_c6000_afsk_begin(tx_hz, level)) {	// the first symbol's tone, at task level
		return false;
	}
	tx_pos = 1;
	tx_running = true;
	tx_timer_start(baud);
	return true;
}

static bool tx_busy(void)
{
	return tx_running;
}

static void tx_stop(void)
{
	tx_timer_stop();
	tx_running = false;
	(void)md9600_c6000_tx_pair(0, 0);	// the microphone back
}
#endif

#if CONFIG_APRS_RX

// ADC clock PCLK2 (72 MHz) / 8, 480 + 12 cycles a conversion: 18293 Hz. Sums of 2: 9146 Hz, 13 bits
#define ADC_RATE_HZ		(72000000u / 8u / 492u)
#define DECIM			2u
#define RATE_HZ			((ADC_RATE_HZ + DECIM / 2u) / DECIM)
#define HALF_OUT		32u				// samples a half buffer: an interrupt every 3.5 ms
#define HALF_RAW		(HALF_OUT * DECIM)
#define RING			1024u				// 112 ms at 9146 Hz (the core reads every poll)
#define IRQ_PRIORITY		7				// no RTOS calls in the handler
#define ADC_CH			13u				// IN13, PC3
#define GAIN			4				// the summed 13-bit swing to most of 16 bits
#define DC_SHIFT		10				// DC tracker: about 0.11 s, far below the 1200 Hz mark tone

static volatile uint16_t raw[2u * HALF_RAW];
static int16_t ring[RING];
static volatile uint32_t head;	// written by the interrupt
static uint32_t tail;		// read by the radio task
static int32_t dc;		// summed counts << DC_SHIFT
static bool dc_primed;
static volatile bool running;

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
		ring[h % RING] = (int16_t)s;
		h++;
	}
	head = h;	// a reader more than RING behind loses the oldest (rx_read skips ahead)
}

static bool rx_start(void)
{
	stm32_pin_analog(PIN_PORT(PIN_DISC_AF), PIN_MASK(PIN_DISC_AF));
	RCC->APB2ENR |= RCC_APB2ENR_ADC3EN;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
	(void)RCC->APB2ENR;
	// The common prescaler (PCLK2 / 8) is the ADC1 scan's (adc_md9600.c), set at power-up
	running = true;		// md9600_adc3_qt leaves ADC3 alone from here

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
	head = 0;
	tail = 0;
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
	return true;
}

static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	uint32_t h = head;
	if (h - tail > RING) {
		tail = h - RING;	// fell behind: the oldest are gone
	}
	uint16_t n = 0;
	while (n < max && tail != h) {
		buf[n++] = ring[tail % RING];
		tail++;
	}
	return n;
}

static void rx_stop(void)
{
	HAL_NVIC_DisableIRQ(DMA2_Stream1_IRQn);
	ADC3->CR2 = 0;
	DMA2_Stream1->CR = 0;
	while (DMA2_Stream1->CR & DMA_SxCR_EN) {
	}
	// md9600_adc3_qt sets ADC3 up again for its one-off readings
	RCC->APB2ENR &= ~RCC_APB2ENR_ADC3EN;
	running = false;
}

bool md9600_afsk_rx_running(void)
{
	return running;
}

#endif	// CONFIG_APRS_RX

const hal_afsk_t md9600_afsk = {
#if CONFIG_MD9600_TX
	.tx_start = tx_start,
	.tx_busy = tx_busy,
	.tx_stop = tx_stop,
#endif
#if CONFIG_APRS_RX
	.rx_rate_hz = RATE_HZ,
	.rx_start = rx_start,
	.rx_read = rx_read,
	.rx_stop = rx_stop,
#endif
};
#endif
