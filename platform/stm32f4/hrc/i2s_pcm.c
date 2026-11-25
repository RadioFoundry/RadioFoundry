// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PCM audio over I2S3 to an HR_C6000 / HR_C5000 codec (i2s_pcm.h; docs/spec/hardware-dm1701.md 3.2, 4.4, 8,
// hardware-md380.md 6.2, hardware-md9600.md 4.3).
//
// The chip drives CK (PC10) and WS (PA15); SPI3 receives the microphone on PC12 (I2S3_SD), I2S3ext sends speaker PCM on
// PC11 (I2S3ext_SD). Philips standard, 16-bit data in 32-bit channels, 8 kHz, so every sample period has a left and a
// right halfword. Both directions run circular DMA over two 20 ms halves (DMA1 S0 ch0 RX, DMA1 S5 ch2 TX); they share
// the bit clock and start together, so the RX stream's half / full interrupts pace both. The interrupt only wakes the
// audio task, which calls the block callback (never from the ISR) and fills the TX half that has just been sent, one
// block (20 ms) of latency.
//
// Channels: the microphone is taken from the left channel (unverified which one the chip fills); speaker samples are
// written to both.
//
// The prompt FIFO (cfg->prompt_fifo, CONFIG_AIR or CONFIG_BEEP): in FM mode I2S PCM doesn't reach the chip's line out
// (modem-hrc6000.md 8). While the FM copy holds the chip (a profile the correlator can't take, such as NXDN), and in FM
// receive while the PCM route is on (beeps; the board reports the chip, stm32_i2s_pcm_fm_chip), every speaker block
// also goes into a ring that a feeder task keeps the chip's 64-sample (8 ms) prompt FIFO topped up from. The I2S stream and the FIFO run
// on the chip's clock, so they never drift. The feeder runs at the top priority (with the RTOS timer task): the DMR
// glue's copy interrupt reads 512 bytes with interrupts masked every 32 ms, and a feeder below the audio task missed
// its turn there (8 ms gaps).
#include "hrc/i2s_pcm.h"

#if CONFIG_DMR	// the whole file: DMR only
#include <string.h>
#include "platform.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "debuglink/dlog.h"
#include "hrc/dmr_glue.h"
#include "hrc/pcm_ring.h"

// The two-argument pin form the boards' pins.h use
#define PIN_PORT(...)		PIN_PORT2_(__VA_ARGS__)
#define PIN_MASK(...)		PIN_MASK2_(__VA_ARGS__)
#define PIN_PORT2_(port, pin)	(port)
#define PIN_MASK2_(port, pin)	(pin)
#define PIN_READ(p)		stm32_pin_read(PIN_PORT(p), PIN_MASK(p))
// A DMA stream's memory address must be in main SRAM: the DMA controllers can't reach the CCM
#define DMA_RAM_OK(p)		configASSERT(((uintptr_t)(p) >> 24) == 0x20u)

#define AUDIO_TASK_PRIORITY	5	// above the radio task: a late block is an audible gap
#define AUDIO_TASK_STACK	1280	// words: the vocoder in the callback needs about 4 KB (stock_vocoder.c)
#define AUDIO_IRQ_PRIORITY	6	// numerically >= 4 for FreeRTOS API calls

#define PIN_I2S_WS		GPIOA, GPIO_PIN_15	// AF6
#define PIN_I2S_CK		GPIOC, GPIO_PIN_10	// AF6
#define PIN_I2S_SD		GPIOC, GPIO_PIN_12	// AF6, chip -> MCU
#define PIN_I2S_EXT_SD		GPIOC, GPIO_PIN_11	// AF5, MCU -> chip

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
static const stm32_i2s_pcm_cfg_t *cfg;

// The FIFO path: the FM copy (an air interface) and beeps in FM receive use it
#define I2S_FIFO	(CONFIG_AIR || CONFIG_BEEP)

#if I2S_FIFO
#define FIFO_TASK_PRIORITY	(configMAX_PRIORITIES - 1)
#define FIFO_TASK_STACK		176	// words: 704 B, the stack check's 336 deepest + 208 port + 128 margin, and a few
_Static_assert(AUDIO_BLOCK_SAMPLES % PCM_RING_CHUNK == 0, "a block is whole chunks");
_Static_assert(PCM_RING_CHUNK == HRC_PROMPT_CHUNK, "a ring chunk is one FIFO write");
static pcm_ring_t *ring;		// from the heap with the feeder, on the first FIFO use
static TaskHandle_t feeder;
static hrc_t *volatile fifo_chip;	// the chip whose FIFO the speaker blocks go to, NULL: none
static hrc_t *volatile fm_chip;		// the board's chip in FM receive (stm32_i2s_pcm_fm_chip), NULL: not
static volatile bool fifo_spk;		// the speaker's PCM route is on (stm32_i2s_pcm_speaker)
#define FIFO_PRIME	(2u * AUDIO_BLOCK_SAMPLES)	// in the ring before the first write: 40 ms in hand
_Static_assert(FIFO_PRIME + AUDIO_BLOCK_SAMPLES <= PCM_RING_SAMPLES, "a block fits on top of the priming");
static volatile uint32_t fifo_chunks, fifo_gaps, fifo_full;
// Writes a poll may make: the FIFO's depth in chunks, plus one for what drains meanwhile. A bound, so a chip that never
// reports full (reset, or the bit read wrong) can't keep the top-priority feeder in a loop that starves every task
#define FIFO_POLL_CHUNKS	((uint32_t)(HRC_PROMPT_DEPTH / HRC_PROMPT_CHUNK) + 1u)
#endif

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

// Bench knob (cfg->master_if_free): the STM32 generates CK and WS (SPI3 master receive, I2S3ext slave transmit), for a
// chip that doesn't drive its I2S. Only when nothing else drives the two lines: each must follow a pull-up and a pull-down
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

static void hw_setup(void)
{
	__HAL_RCC_SPI3_CLK_ENABLE();
	__HAL_RCC_DMA1_CLK_ENABLE();
	if (cfg->master_if_free) {
		bool ck_free = line_free(PIN_PORT(PIN_I2S_CK), PIN_MASK(PIN_I2S_CK));
		bool ws_free = line_free(PIN_PORT(PIN_I2S_WS), PIN_MASK(PIN_I2S_WS));
		i2s_master = ck_free && ws_free;
		dlog("i2s: ck %s, ws %s: %s", ck_free ? "free" : "driven", ws_free ? "free" : "driven",
			i2s_master ? "STM32 master" : "slave (a line is driven)");
		if (i2s_master) {
			plli2s_on();
		}
	}
	stm32_pin_af(PIN_PORT(PIN_I2S_WS), PIN_MASK(PIN_I2S_WS), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_CK), PIN_MASK(PIN_I2S_CK), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_SD), PIN_MASK(PIN_I2S_SD), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_EXT_SD), PIN_MASK(PIN_I2S_EXT_SD), GPIO_AF5_I2S3ext, false, GPIO_NOPULL);

	// Philips, 16-bit data in a 32-bit channel, the board's clock polarity. Main: slave receive; ext: slave transmit
	const uint32_t fmt = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_CHLEN | (cfg->ckpol ? SPI_I2SCFGR_CKPOL : 0u);
	SPI3->I2SCFGR = 0;
	I2S3ext->I2SCFGR = 0;
	SPI3->I2SPR = 2;	// unused in slave mode; reset value
	SPI3->I2SCFGR = fmt | SPI_I2SCFGR_I2SCFG_0;	// 01 = slave receive
	if (i2s_master) {
		SPI3->I2SPR = 50u;					// I2SDIV 50, ODD 0, MCKOE 0
		SPI3->I2SCFGR = fmt | SPI_I2SCFGR_I2SCFG_1 | SPI_I2SCFGR_I2SCFG_0;	// 11 = master receive
	}
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

	// Enable right after a WS rising edge (low, high, low, high) so both channels line up. Slave I2S in Philips mode
	// must be enabled while WS is high. Interrupts off for the short window between the edge and enable
	if (i2s_master) {
		I2S3ext->I2SCFGR |= SPI_I2SCFGR_I2SE;	// the slave transmitter first, then the master starts the clock
		SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;
		return true;
	}
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
	if (cfg->raw_block != NULL) {
		cfg->raw_block(in);	// both channels, before the callback looks at them
	}
	audio_block_cb_t cb = block_cb;
	if (cb != NULL) {
		cb(mic, spk, block_ctx);
	} else {
		memset(spk, 0, sizeof spk);
	}
#if I2S_FIFO
	// Silence while the PCM route is off: the FIFO goes past the switch that keeps the stream off the amplifier, which
	// stays on for the hang after a call, so what the stream holds then (not a call) would play
	if (fifo_chip != NULL && !pcm_ring_push(ring, fifo_spk ? spk : NULL, AUDIO_BLOCK_SAMPLES)) {
		fifo_full++;
	}
#endif
	// This half has just gone out (TX runs in step with RX), so it is free until the DMA comes back in 20 ms
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		out[2 * i] = spk[i];
		out[2 * i + 1] = spk[i];
	}
}

#if I2S_FIFO
// The chip in a mode whose line out the FIFO reaches: the FM copy's, else the board's in FM receive. The feeder writes
// page 3 only while this is still its chip: back in DMR, page 3 is the vocoder's
static hrc_t *fifo_mode_chip(void)
{
	hrc_t *copy = stm32_hrc_copy_chip();
	return copy != NULL ? copy : fm_chip;
}

// Waits for FIFO_PRIME in the ring before the first write, so the FIFO starts with 40 ms in hand (20 ms left a few gaps a
// call); after that a chunk the ring hasn't got is a gap (silence written)
static void feeder_task(void *arg)
{
	(void)arg;
	int16_t buf[PCM_RING_CHUNK];
	bool primed = false;
	for (;;) {
		hrc_t *chip = fifo_chip;
		if (chip == NULL) {
			primed = false;
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			continue;
		}
		if (!primed) {
			primed = pcm_ring_level(ring) >= FIFO_PRIME;
		}
		// Stops as soon as the chip leaves the mode too: the prompt bit stays on until the audio task's next block
		for (uint32_t n = 0; n < FIFO_POLL_CHUNKS && primed && fifo_chip == chip && fifo_mode_chip() == chip
				     && hrc_prompt_room(chip); n++) {
			if (!pcm_ring_pop(ring, buf)) {
				fifo_gaps++;
			}
			(void)hrc_prompt_write(chip, buf);
			fifo_chunks++;
		}
		vTaskDelay(1);	// the FIFO holds 8 ms: polled every 1 ms, as the chip's manual does
	}
}

// The FIFO off. fifo_chip first: the feeder runs above any caller, and one let in between would write page 3 with the
// prompt bit already clear
static void fifo_release(void)
{
	hrc_t *chip = fifo_chip;
	fifo_chip = NULL;
	(void)hrc_prompt_on(chip, false);
}

// Per block: the speaker to the prompt FIFO too while the FM copy holds the chip, or while a beep plays in FM receive
// (pcm_fifo_chip). A beep's route stays on for its hold after the tone, so the ring has played the tone out by the
// time the route's end turns the FIFO off
static void fifo_follow(void)
{
	hrc_t *chip = cfg->prompt_fifo ? pcm_fifo_chip(stm32_hrc_copy_chip(), fm_chip, fifo_spk) : NULL;
	if (chip == fifo_chip) {
		return;
	}
	if (chip != NULL && feeder == NULL) {
		ring = pvPortMalloc(sizeof *ring);
		if (ring == NULL || xTaskCreate(feeder_task, "fifo", FIFO_TASK_STACK, NULL, FIFO_TASK_PRIORITY,
						&feeder) != pdPASS) {
			vPortFree(ring);
			ring = NULL;
			feeder = NULL;
			static bool told;	// tried again each block; said once
			if (!told) {
				told = true;
				dlog("i2s: prompt FIFO: no memory, speaker silent in the FM copy");
			}
			return;
		}
	}
	if (fifo_chip != NULL) {
		fifo_release();
		vTaskDelay(2);	// the feeder sees it stopped before the ring is reset below
	}
	if (chip != NULL) {
		pcm_ring_reset(ring);
		(void)hrc_prompt_on(chip, true);
		fifo_chip = chip;
		xTaskNotifyGive(feeder);
	}
}

void stm32_i2s_pcm_speaker(bool on)
{
	fifo_spk = on;
}

void stm32_i2s_pcm_fm_chip(hrc_t *chip)
{
	fm_chip = chip;
}

static void fifo_off(void)
{
	if (fifo_chip != NULL) {
		fifo_release();
		dlog("i2s: prompt FIFO off");
	}
}
#else
void stm32_i2s_pcm_speaker(bool on)
{
	(void)on;
}

void stm32_i2s_pcm_fm_chip(hrc_t *chip)
{
	(void)chip;
}

static void fifo_follow(void) {}
static void fifo_off(void) {}
#endif

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
			fifo_follow();
			process(half);
			blocks++;
		}
	}
}

// --- hal_audio_t ---

static void clock(bool on)
{
	if (cfg->clock != NULL) {
		cfg->clock(on);
	}
}

bool stm32_i2s_pcm_start(const stm32_i2s_pcm_cfg_t *c, audio_block_cb_t cb, void *ctx)
{
	if (task == NULL) {
		cfg = c;
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
	clock(true);
	if (!hw_start()) {
		// No I2S clock from the chip (powered down, or its codec not running): blocks the DMA caught before the
		// clock check gave up must not reach the callback
		running = false;
		pending = 0;
		block_cb = NULL;
		clock(false);
		return false;
	}
	return true;
}

void stm32_i2s_pcm_stop(void)
{
	running = false;
	if (task == NULL) {
		return;		// never started: nothing set up
	}
	fifo_off();
	hw_stop();
	pending = 0;
	block_cb = NULL;
	clock(false);
}

bool stm32_i2s_pcm_running(void)
{
	return running;
}

void stm32_i2s_pcm_stats(stm32_i2s_pcm_stats_t *out)
{
	out->blocks = blocks;
	out->overruns = overruns;
	out->dropped = dropped;
}

void stm32_i2s_pcm_stream_stats(audio_stream_stats_t *out)
{
	out->blocks = blocks;
	out->late = overruns;
	out->dropped = dropped;
#if I2S_FIFO
	out->fifo_chunks = fifo_chunks;
	out->fifo_gaps = fifo_gaps;
	out->fifo_full = fifo_full;
	out->fifo_fm = fm_chip != NULL && (cfg == NULL || cfg->prompt_fifo);	// before the first start: the board's word
#else
	out->fifo_chunks = out->fifo_gaps = out->fifo_full = 0;
	out->fifo_fm = false;
#endif
}

void stm32_i2s_pcm_reset_stats(void)
{
	taskENTER_CRITICAL();
	blocks = 0;
	overruns = 0;
	dropped = 0;
#if I2S_FIFO
	fifo_chunks = 0;
	fifo_gaps = 0;
	fifo_full = 0;
#endif
	taskEXIT_CRITICAL();
}

#endif	// CONFIG_DMR
