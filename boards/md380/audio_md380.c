// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 PCM audio: I2S3 full duplex slave to the HR_C5000's built-in codec (docs/spec/hardware-md380.md 6.2).
// The pins and format are the MD-380's own (the pins and AF numbers of its I2S3 set-up; 8 kHz, Philips, 16 bits in a
// 32-bit channel, clock idle low); which data line carries which direction is unconfirmed (bring-up stage 6).
//
// The HR_C5000 drives CK (PC10) and WS (PA15); SPI3 receives the microphone on PC12 (I2S3_SD), I2S3ext sends speaker
// PCM on PC11 (I2S3ext_SD). Philips standard, 16-bit data in 32-bit channels, 8 kHz, so every sample period has a left
// and a right halfword. Both directions run circular DMA over two 20 ms halves (DMA1 S0 ch0 RX, DMA1 S5 ch2 TX); they
// share the bit clock and start together, so the RX stream's half / full interrupts pace both. The interrupt only
// wakes the audio task, which calls the block callback (never from the ISR) and fills the TX half that has just been
// sent, one block (20 ms) of latency.
//
// Channels: the microphone is taken from the left channel (unverified which one the chip fills); speaker samples are
// written to both.
#include "md380.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "pins.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#define AUDIO_TASK_PRIORITY	5	// above the radio task: a late block is an audible gap
#define AUDIO_TASK_STACK	1280	// words: the vocoder in the callback needs about 4 KB (vocoder_md380.c)
#define AUDIO_IRQ_PRIORITY	6	// numerically >= 5 for FreeRTOS API calls

#define PIN_I2S_WS		GPIOA, GPIO_PIN_15	// AF6
#define PIN_I2S_CK		GPIOC, GPIO_PIN_10	// AF6
#define PIN_I2S_SD		GPIOC, GPIO_PIN_12	// AF6, HR_C5000 -> MCU (?)
#define PIN_I2S_EXT_SD		GPIOC, GPIO_PIN_11	// AF5, MCU -> HR_C5000 (?)

#define HALF_WORDS		(AUDIO_BLOCK_SAMPLES * 2)	// one 20 ms block, left + right
#define WS_TIMEOUT_MS		20u			// WS edges come every 62.5 us; none for 20 ms: no clock

// DMA buffers in main SRAM (CCM is not reachable by DMA)
static int16_t rx_buf[2 * HALF_WORDS] __attribute__((aligned(4)));
static int16_t tx_buf[2 * HALF_WORDS] __attribute__((aligned(4)));
static int16_t mic[AUDIO_BLOCK_SAMPLES];
static int16_t spk[AUDIO_BLOCK_SAMPLES];

static TaskHandle_t task;
static audio_block_cb_t block_cb;
static void *block_ctx;
static volatile bool running;
static volatile uint8_t pending;	// bit 0: half 0 received, bit 1: half 1
static volatile uint32_t overruns;	// blocks the task did not get to in time
static volatile uint32_t blocks;	// blocks processed (one per 20 ms while streaming)
static volatile uint32_t dropped;	// blocks skipped: both halves pending, the older one overwritten

// --- speaker routing (radio_md380.c owns the amplifier and the HR_C5000 audio path) ---

static void speaker_pcm(bool on)
{
	md380_speaker_pcm(on);
}

// --- I2S and DMA ---

static void dma_stop(void)
{
	DMA1_Stream0->CR &= ~DMA_SxCR_EN;
	DMA1_Stream5->CR &= ~DMA_SxCR_EN;
	while ((DMA1_Stream0->CR & DMA_SxCR_EN) || (DMA1_Stream5->CR & DMA_SxCR_EN)) {
	}
	DMA1->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0;
	DMA1->HIFCR = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;
}

#ifdef MD380_I2S_MASTER
// Bench knob (issue 3): the STM32 generates CK and WS (SPI3 master receive, I2S3ext slave transmit), for an HR_C5000
// that doesn't drive its I2S. Only when nothing else drives the two lines: each must follow a pull-up and a pull-down
#include "debuglink/dlog.h"
static bool i2s_master;

static bool line_free(GPIO_TypeDef *port, uint16_t pin)
{
	stm32_pin_in(port, pin, GPIO_PULLUP);
	hal_delay_us(50);
	bool up = (port->IDR & pin) != 0;
	stm32_pin_in(port, pin, GPIO_PULLDOWN);
	hal_delay_us(50);
	bool down = (port->IDR & pin) != 0;
	return up && !down;
}

// PLLI2S from the 2 MHz PLL input (HSE 8 MHz / M 4): N 128 = 256 MHz, R 5 = 51.2 MHz; with I2SDIV 50 and 32-bit
// channels Fs = 51.2 MHz / (64 x 100) = 8 kHz
static void plli2s_on(void)
{
	RCC->CR &= ~RCC_CR_PLLI2SON;
	while (RCC->CR & RCC_CR_PLLI2SRDY) {
	}
	RCC->PLLI2SCFGR = (5u << RCC_PLLI2SCFGR_PLLI2SR_Pos) | (128u << RCC_PLLI2SCFGR_PLLI2SN_Pos);
	RCC->CFGR &= ~RCC_CFGR_I2SSRC;
	RCC->CR |= RCC_CR_PLLI2SON;
	uint32_t t0 = hal_millis();
	while (!(RCC->CR & RCC_CR_PLLI2SRDY) && (uint32_t)(hal_millis() - t0) < 10u) {
	}
}
#endif

static void hw_setup(void)
{
	__HAL_RCC_SPI3_CLK_ENABLE();
	__HAL_RCC_DMA1_CLK_ENABLE();
#ifdef MD380_I2S_MASTER
	bool ck_free = line_free(PIN_PORT(PIN_I2S_CK), PIN_MASK(PIN_I2S_CK));
	bool ws_free = line_free(PIN_PORT(PIN_I2S_WS), PIN_MASK(PIN_I2S_WS));
	i2s_master = ck_free && ws_free;
	dlog("i2s: ck %s, ws %s: %s", ck_free ? "free" : "driven", ws_free ? "free" : "driven",
		i2s_master ? "STM32 master" : "slave (a line is driven)");
	if (i2s_master) {
		plli2s_on();
	}
#endif
	stm32_pin_af(PIN_PORT(PIN_I2S_WS), PIN_MASK(PIN_I2S_WS), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_CK), PIN_MASK(PIN_I2S_CK), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_SD), PIN_MASK(PIN_I2S_SD), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_EXT_SD), PIN_MASK(PIN_I2S_EXT_SD), GPIO_AF5_I2S3ext, false, GPIO_NOPULL);

	// Philips, 16-bit data in a 32-bit channel, clock idle low (the MD-380's own I2S set-up; the DM-1701's is idle
	// high: -DMD380_I2S_CKPOL=1). Main: slave receive; ext: slave transmit
#ifndef MD380_I2S_CKPOL
#define MD380_I2S_CKPOL	0
#endif
	const uint32_t fmt = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_CHLEN | (MD380_I2S_CKPOL ? SPI_I2SCFGR_CKPOL : 0u);
	SPI3->I2SCFGR = 0;
	I2S3ext->I2SCFGR = 0;
	SPI3->I2SPR = 2;	// unused in slave mode; reset value
	SPI3->I2SCFGR = fmt | SPI_I2SCFGR_I2SCFG_0;	// 01 = slave receive
#ifdef MD380_I2S_MASTER
	if (i2s_master) {
		SPI3->I2SPR = 50u;					// I2SDIV 50, ODD 0, MCKOE 0
		SPI3->I2SCFGR = fmt | SPI_I2SCFGR_I2SCFG_1 | SPI_I2SCFGR_I2SCFG_0;	// 11 = master receive
	}
#endif
	I2S3ext->I2SCFGR = fmt;				// 00 = slave transmit
	SPI3->CR2 = SPI_CR2_RXDMAEN;
	I2S3ext->CR2 = SPI_CR2_TXDMAEN;

	HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, AUDIO_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
}

static bool ws_wait(bool level)
{
	uint32_t t0 = hal_millis();	// TIM14 (priority 3) keeps counting inside RTOS critical sections
	while (PIN_READ(PIN_I2S_WS) != level) {
		if ((uint32_t)(hal_millis() - t0) > WS_TIMEOUT_MS) {
			return false;
		}
	}
	return true;
}

static bool hw_start(void)
{
	dma_stop();
	SPI3->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	I2S3ext->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	memset(tx_buf, 0, sizeof tx_buf);
	pending = 0;

	// RX: SPI3 DR -> rx_buf, half and full interrupts
	DMA1_Stream0->PAR = (uint32_t)&SPI3->DR;
	DMA_RAM_OK(rx_buf);
	DMA1_Stream0->M0AR = (uint32_t)rx_buf;
	DMA1_Stream0->NDTR = 2 * HALF_WORDS;
	DMA1_Stream0->FCR = 0;	// direct mode
	DMA1_Stream0->CR = (0u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC |
			   DMA_SxCR_CIRC | DMA_SxCR_HTIE | DMA_SxCR_TCIE;	// P->M, priority low
	// TX: tx_buf -> I2S3ext DR
	DMA1_Stream5->PAR = (uint32_t)&I2S3ext->DR;
	DMA_RAM_OK(tx_buf);
	DMA1_Stream5->M0AR = (uint32_t)tx_buf;
	DMA1_Stream5->NDTR = 2 * HALF_WORDS;
	DMA1_Stream5->FCR = 0;
	DMA1_Stream5->CR = (2u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC |
			   DMA_SxCR_CIRC | DMA_SxCR_DIR_0;	// M->P
	DMA1_Stream0->CR |= DMA_SxCR_EN;
	DMA1_Stream5->CR |= DMA_SxCR_EN;

	// As the DM-1701: enable right after a WS rising edge (low, high, low, high) so both channels line up. Slave I2S in
	// Philips mode must be enabled while WS is high. Interrupts off for the short window between the edge and enable
#ifdef MD380_I2S_MASTER
	if (i2s_master) {
		I2S3ext->I2SCFGR |= SPI_I2SCFGR_I2SE;	// the slave transmitter first, then the master starts the clock
		SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;
		return true;
	}
#endif
	if (!ws_wait(false) || !ws_wait(true) || !ws_wait(false)) {
		dma_stop();
		return false;
	}
	taskENTER_CRITICAL();
	bool ok = ws_wait(true);
	if (ok) {
		I2S3ext->I2SCFGR |= SPI_I2SCFGR_I2SE;
		SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;
	}
	taskEXIT_CRITICAL();
	if (!ok) {
		dma_stop();
	}
	return ok;
}

static void hw_stop(void)
{
	SPI3->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	I2S3ext->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	dma_stop();
}

void DMA1_Stream0_IRQHandler(void)
{
	uint32_t isr = DMA1->LISR;
	DMA1->LIFCR = isr & (DMA_LISR_HTIF0 | DMA_LISR_TCIF0 | DMA_LISR_TEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_FEIF0);
	uint8_t done = 0;
	if (isr & DMA_LISR_HTIF0) {
		done |= 1;
	}
	if (isr & DMA_LISR_TCIF0) {
		done |= 2;
	}
	if (done == 0 || task == NULL) {
		return;
	}
	if (pending & done) {
		overruns++;	// the task hasn't taken the previous one
	}
	pending |= done;
	BaseType_t woken = pdFALSE;
	vTaskNotifyGiveFromISR(task, &woken);
	portYIELD_FROM_ISR(woken);
}

static void process(uint8_t half)
{
	const int16_t *in = &rx_buf[half * HALF_WORDS];
	int16_t *out = &tx_buf[half * HALF_WORDS];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mic[i] = in[2 * i];
	}
	audio_block_cb_t cb = block_cb;
	if (cb != NULL) {
		cb(mic, spk, block_ctx);
	} else {
		memset(spk, 0, sizeof spk);
	}
	// This half has just gone out (TX runs in step with RX), so it is free until the DMA comes back in 20 ms
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		out[2 * i] = spk[i];
		out[2 * i + 1] = spk[i];
	}
}

static void audio_task(void *arg)
{
	(void)arg;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		for (;;) {
			taskENTER_CRITICAL();
			uint8_t p = pending;
			uint8_t half;
			if (p == 3) {
				// Both pending: we are a block late and the DMA is already overwriting the older half. Take the
				// one it is not writing and drop the other
				half = (DMA1_Stream0->NDTR > HALF_WORDS) ? 1 : 0;
				pending = 0;
				dropped++;
			} else {
				half = (p & 1) ? 0 : 1;
				pending = 0;
			}
			taskEXIT_CRITICAL();
			if (p == 0 || !running) {
				break;
			}
			process(half);
			blocks++;
		}
	}
}

// --- hal_audio_t ---

static bool start(audio_block_cb_t cb, void *ctx)
{
	if (task == NULL) {
		hw_setup();
		if (xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY, &task) != pdPASS) {
			task = NULL;
			return false;
		}
	}
	block_ctx = ctx;
	block_cb = cb;
	if (running) {
		return true;	// already streaming: just the new callback
	}
	running = true;
	md380_i2s_clock(true);	// the chip's set-ups leave CK and WS off (hardware-md380.md 4.7)
	if (!hw_start()) {
		// No I2S clock from the HR_C5000 (powered down, or its codec not running): blocks the DMA caught before the
		// clock check gave up must not reach the callback
		running = false;
		pending = 0;
		block_cb = NULL;
		md380_i2s_clock(false);
		return false;
	}
	return true;
}

// Safe to call from the block callback itself (the audio task): streaming stops, the task stays for the next start()
static void stop(void)
{
	running = false;
	hw_stop();
	pending = 0;
	block_cb = NULL;
	md380_i2s_clock(false);
}

uint32_t md380_audio_overruns(void)
{
	return overruns;
}

void md380_audio_stats(md380_audio_stats_t *out)
{
	out->blocks = blocks;
	out->overruns = overruns;
	out->dropped = dropped;
}

void md380_audio_reset_stats(void)
{
	taskENTER_CRITICAL();
	blocks = 0;
	overruns = 0;
	dropped = 0;
	taskEXIT_CRITICAL();
}

const hal_audio_t md380_audio = { .start = start, .stop = stop, .speaker_pcm = speaker_pcm };


#endif	// CONFIG_DMR
