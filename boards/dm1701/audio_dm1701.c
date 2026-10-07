// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 PCM audio: I2S3 full duplex slave to the HR_C6000's built-in codec (docs/spec/hardware-dm1701.md 3.2, 4.4, 8).
//
// The HR_C6000 drives CK (PC10) and WS (PA15); SPI3 receives the microphone on PC12 (I2S3_SD), I2S3ext sends speaker
// PCM on PC11 (I2S3ext_SD). Philips standard, 16-bit data in 32-bit channels, 8 kHz, so every sample period has a left
// and a right halfword. Both directions run circular DMA over two 20 ms halves (DMA1 S0 ch0 RX, DMA1 S5 ch2 TX); they
// share the bit clock and start together, so the RX stream's half / full interrupts pace both. The interrupt only
// wakes the audio task, which calls the block callback (never from the ISR) and fills the TX half that has just been
// sent, one block (20 ms) of latency.
//
// Channels: the microphone is taken from the left channel (unverified which one the chip fills); speaker samples are
// written to both.
#include "dm1701.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "pins.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#if CONFIG_AUDIO_PROBE
#include <math.h>
#endif

#define AUDIO_TASK_PRIORITY	5	// above the radio task: a late block is an audible gap
#define AUDIO_TASK_STACK	1280	// words: the vocoder in the callback needs about 4 KB (vocoder_dm1701.c)
#define AUDIO_IRQ_PRIORITY	6	// spec 3.1; numerically >= 5 for FreeRTOS API calls

#define PIN_I2S_WS		GPIOA, GPIO_PIN_15	// AF6
#define PIN_I2S_CK		GPIOC, GPIO_PIN_10	// AF6
#define PIN_I2S_SD		GPIOC, GPIO_PIN_12	// AF6, HR_C6000 -> MCU
#define PIN_I2S_EXT_SD		GPIOC, GPIO_PIN_11	// AF5, MCU -> HR_C6000

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
#if CONFIG_AUDIO_PROBE
static int16_t probe_raw[2 * AUDIO_BLOCK_SAMPLES];	// the last block as received, left / right interleaved
#endif

// --- speaker routing ---

// The amp, its mute and the RX audio mux are shared with the FM path in radio_dm1701.c. A board-level
// dm1701_speaker_pcm() there (combining FM squelch and PCM requests) overrides this fallback, which saves the pins on
// and restores them on off.
void dm1701_speaker_pcm(bool on);

static uint32_t saved_odr_b, saved_odr_d;
static bool pcm_on;

__attribute__((weak)) void dm1701_speaker_pcm(bool on)
{
	const uint32_t mask_b = PIN_MASK(PIN_SPK_MUTE) | PIN_MASK(PIN_AMP_EN);
	const uint32_t mask_d = PIN_MASK(PIN_RX_AUDIO_MUX);
	if (on == pcm_on) {
		return;
	}
	pcm_on = on;
	if (on) {
		saved_odr_b = GPIOB->ODR & mask_b;
		saved_odr_d = GPIOD->ODR & mask_d;
		PIN_WRITE(PIN_RX_AUDIO_MUX, true);	// HR_C6000 line out (its DAC carries the I2S PCM) to the amp
		PIN_WRITE(PIN_AMP_EN, true);
		PIN_WRITE(PIN_SPK_MUTE, false);
	} else {
		GPIOB->BSRR = saved_odr_b | ((mask_b & ~saved_odr_b) << 16);
		GPIOD->BSRR = saved_odr_d | ((mask_d & ~saved_odr_d) << 16);
	}
}

static void speaker_pcm(bool on)
{
	dm1701_speaker_pcm(on);
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

static void hw_setup(void)
{
	__HAL_RCC_SPI3_CLK_ENABLE();
	__HAL_RCC_DMA1_CLK_ENABLE();
	stm32_pin_af(PIN_PORT(PIN_I2S_WS), PIN_MASK(PIN_I2S_WS), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_CK), PIN_MASK(PIN_I2S_CK), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_SD), PIN_MASK(PIN_I2S_SD), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_EXT_SD), PIN_MASK(PIN_I2S_EXT_SD), GPIO_AF5_I2S3ext, false, GPIO_NOPULL);

	// Philips, CKPOL high (spec 4.4), 16-bit data in a 32-bit channel. Main: slave receive; ext: slave transmit
	const uint32_t fmt = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_CKPOL | SPI_I2SCFGR_CHLEN;
	SPI3->I2SCFGR = 0;
	I2S3ext->I2SCFGR = 0;
	SPI3->I2SPR = 2;	// unused in slave mode; reset value
	SPI3->I2SCFGR = fmt | SPI_I2SCFGR_I2SCFG_0;	// 01 = slave receive
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

	// Spec 4.4: enable right after a WS rising edge (low, high, low, high) so both channels line up. Slave I2S in
	// Philips mode must be enabled while WS is high. Interrupts off for the short window between the edge and enable
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
#if CONFIG_AUDIO_PROBE
	memcpy(probe_raw, in, sizeof probe_raw);	// both channels, before the callback looks at them
#endif
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
	if (!hw_start()) {
		running = false;
		return false;	// no I2S clock from the HR_C6000 (powered down, or its codec not running)
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
}

uint32_t dm1701_audio_overruns(void)
{
	return overruns;
}

void dm1701_audio_stats(dm1701_audio_stats_t *out)
{
	out->blocks = blocks;
	out->overruns = overruns;
	out->dropped = dropped;
}

void dm1701_audio_reset_stats(void)
{
	taskENTER_CRITICAL();
	blocks = 0;
	overruns = 0;
	dropped = 0;
	taskEXIT_CRITICAL();
}

const hal_audio_t dm1701_audio = { .start = start, .stop = stop, .speaker_pcm = speaker_pcm };

#if CONFIG_AUDIO_PROBE
// The probe's view of the stream (hal_audio_tap.h). Called from the probe's block callback, i.e. the audio task right
// after process() copied the block, or from the debug link task (a torn block is fine for a diagnostic)
static bool probe_last_block(int16_t *left, int16_t *right, uint32_t *count)
{
	if (!running) {
		return false;
	}
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		left[i] = probe_raw[2 * i];
		right[i] = probe_raw[2 * i + 1];
	}
	*count = blocks;
	return true;
}

// PC8 (the stock beep PWM, TIM8_CH3 on AF3): a 50 % square wave; off puts it back as the low output board.c makes it
static void probe_pin_tone(uint16_t hz)
{
	TIM8->CR1 = 0;
	TIM8->DIER = 0;
	DMA2_Stream2->CR = 0;				// the PWM audio's DMA, if it ran
	if (hz == 0) {
		stm32_pin_out(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), false, false);
		return;
	}
	__HAL_RCC_TIM8_CLK_ENABLE();
	TIM8->CR1 = 0;
	TIM8->PSC = 72 - 1;				// APB2 timer clock 72 MHz -> 1 MHz
	TIM8->ARR = 1000000u / hz - 1u;
	TIM8->CCR3 = 500000u / hz;
	TIM8->CCMR2 = 6u << TIM_CCMR2_OC3M_Pos;		// PWM mode 1
	TIM8->CCER = TIM_CCER_CC3E;
	TIM8->BDTR = TIM_BDTR_MOE;			// TIM8 is an advanced timer: main output enable
	TIM8->EGR = TIM_EGR_UG;
	TIM8->CR1 = TIM_CR1_CEN;
	stm32_pin_af(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), GPIO_AF3_TIM8, false, GPIO_NOPULL);
}

// PWM audio on PC8: TIM8 CH3, its compare request on DMA2 stream 2 channel 0 rewriting CCR3 from a circular table each
// carrier period. The table holds whole periods of the tone (as near hz as its length allows)
#define PWM_TABLE_MAX		1024u
static uint16_t pwm_table[PWM_TABLE_MAX];

static uint32_t probe_pin_pwm(uint16_t top, uint16_t hz, uint16_t amp_permille)
{
	DMA2_Stream2->CR = 0;
	while (DMA2_Stream2->CR & DMA_SxCR_EN) {
	}
	if (hz == 0 || top < 15u) {
		probe_pin_tone(0);
		return 0;
	}
	uint32_t fs = 72000000u / (top + 1u);
	uint32_t k = (PWM_TABLE_MAX * hz) / fs;		// whole tone periods that fit the table
	if (k == 0) {
		k = 1;
	}
	uint32_t n = (fs * k + hz / 2u) / hz;
	if (n > PWM_TABLE_MAX || n < 4u) {
		return 0;
	}
	float half = (float)(top + 1u) / 2.0f, a = half * (float)(amp_permille > 1000u ? 1000u : amp_permille) / 1000.0f;
	for (uint32_t i = 0; i < n; i++) {
		int32_t v = (int32_t)(half + a * sinf(6.2831853f * (float)(i * k) / (float)n) + 0.5f);
		pwm_table[i] = (uint16_t)(v < 0 ? 0 : v > (int32_t)top ? top : v);
	}
	__HAL_RCC_TIM8_CLK_ENABLE();
	__HAL_RCC_DMA2_CLK_ENABLE();
	TIM8->CR1 = 0;
	TIM8->DIER = 0;
	TIM8->PSC = 0;					// 72 MHz counts
	TIM8->ARR = top;
	TIM8->CCR3 = pwm_table[0];
	TIM8->CCMR2 = (6u << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;	// PWM mode 1, preload: a new duty each period
	TIM8->CCER = TIM_CCER_CC3E;
	TIM8->BDTR = TIM_BDTR_MOE;
	TIM8->EGR = TIM_EGR_UG;
	DMA2->LIFCR = 0x3Fu << 16;			// stream 2's flags
	DMA2_Stream2->PAR = (uint32_t)&TIM8->CCR3;
	DMA2_Stream2->M0AR = (uint32_t)pwm_table;
	DMA2_Stream2->NDTR = n;
	DMA2_Stream2->FCR = 0;
	DMA2_Stream2->CR = (0u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 |
			   DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_DIR_0;
	DMA2_Stream2->CR |= DMA_SxCR_EN;
	TIM8->DIER = TIM_DIER_CC3DE;
	TIM8->CR1 = TIM_CR1_CEN;
	stm32_pin_af(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), GPIO_AF3_TIM8, false, GPIO_NOPULL);
	uint32_t ndtr = DMA2_Stream2->NDTR;
	uint32_t t0 = hal_millis();
	while (DMA2_Stream2->NDTR == ndtr && hal_millis() - t0 < 2u) {
	}
	return DMA2_Stream2->NDTR != ndtr ? fs : 0;	// the DMA must be moving
}

// Sample GPIOA-E's inputs back to back for ms; output pins read their own level, so a diff of two scans is the answer
static uint8_t probe_pin_scan(uint16_t ms, uint16_t *changed, uint8_t max)
{
	GPIO_TypeDef *const ports[] = { GPIOA, GPIOB, GPIOC, GPIOD, GPIOE };
	uint8_t n = max < 5 ? max : 5;
	uint16_t first[5];
	for (uint8_t i = 0; i < n; i++) {
		first[i] = (uint16_t)ports[i]->IDR;
		changed[i] = 0;
	}
	uint32_t t0 = hal_millis();
	while (hal_millis() - t0 < ms) {
		for (uint8_t i = 0; i < n; i++) {
			changed[i] |= (uint16_t)(ports[i]->IDR ^ first[i]);
		}
	}
	return n;
}

const hal_audio_tap_t dm1701_audio_tap = {
	.caps = AUDIO_TAP_DMR,
	.modem_reg = dm1701_modem_reg,
	.modem_ram = dm1701_modem_ram,
	.modem_burst = dm1701_modem_burst,
	.last_block = probe_last_block,
	.pin_tone = probe_pin_tone,
	.pin_scan = probe_pin_scan,
	.pin_pwm = probe_pin_pwm,
};
#endif

#endif	// CONFIG_DMR
