// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// I2S0 (the SAI, reference manual chapter 54) as a slave of the codec, with one DMA ring each way. The codec drives
// the bit clock and frame sync into the receiver's pins; the transmitter runs synchronous to the receiver, so one
// pair of clock pins serves both. DMA channel 0 drains the receive FIFO into rx_ring, channel 1 fills the transmit FIFO
// from tx_ring, both 16 bits at a time from the data registers' low half (a 16-bit sample right-justified in a 32-bit
// word by the first-bit setting). The half and major loop interrupts tell the stream's owner which half is free.
// Written from the manual and not yet run on a radio (docs/spec/hardware-gd77.md 8).
#include "platform.h"

#define DMA_RX		0u
#define DMA_TX		1u
#define RX_WATERMARK	0u	// a DMA request as soon as one word is in the receive FIFO
#define TX_WATERMARK	4u	// and while the 8-word transmit FIFO holds 4 or fewer
#define AUDIO_IRQ_PRIO	6u	// below configMAX_SYSCALL_INTERRUPT_PRIORITY (5): the callback may use FromISR calls

static const k22f_i2s_stream_t *stream;
static volatile uint32_t errors;

static uint32_t cr2(bool sync)
{
	return I2S_CR2_SYNC(sync ? 1u : 0u) | I2S_CR2_BCP;	// clocks from outside (BCD 0), sampled on the rising edge
}

static uint32_t cr4(const k22f_i2s_fmt_t *f)
{
	return I2S_CR4_FRSZ(f->words) | I2S_CR4_SYWD(f->word_bits) | I2S_CR4_MF | (f->early_fs ? I2S_CR4_FSE : 0u) |
	       (f->fs_active_low ? I2S_CR4_FSP : 0u);	// FSD 0: frame sync from outside
}

static uint32_t cr5(const k22f_i2s_fmt_t *f)
{
	return I2S_CR5_WNW(f->word_bits) | I2S_CR5_W0W(f->word_bits) | I2S_CR5_FBT(f->first_bit);
}

static void dma_ring(uint8_t ch, bool rx, int16_t *ring, uint16_t n)
{
	uint32_t ring_bytes = (uint32_t)n * 2u;
	DMA_CERQ = ch;
	DMAMUX_CHCFG(ch) = 0;
	if (rx) {
		DMA_TCD_SADDR(ch) = I2S0_RDR0_ADDR;
		DMA_TCD_SOFF(ch) = 0;
		DMA_TCD_SLAST(ch) = 0;
		DMA_TCD_DADDR(ch) = (uint32_t)(uintptr_t)ring;
		DMA_TCD_DOFF(ch) = 2;
		DMA_TCD_DLASTSGA(ch) = (uint32_t)(-(int32_t)ring_bytes);
	} else {
		DMA_TCD_SADDR(ch) = (uint32_t)(uintptr_t)ring;
		DMA_TCD_SOFF(ch) = 2;
		DMA_TCD_SLAST(ch) = (uint32_t)(-(int32_t)ring_bytes);
		DMA_TCD_DADDR(ch) = I2S0_TDR0_ADDR;
		DMA_TCD_DOFF(ch) = 0;
		DMA_TCD_DLASTSGA(ch) = 0;
	}
	DMA_TCD_ATTR(ch) = DMA_ATTR_SIZE_16;
	DMA_TCD_NBYTES(ch) = 2;
	DMA_TCD_CITER(ch) = n;
	DMA_TCD_BITER(ch) = n;
	DMA_TCD_CSR(ch) = DMA_CSR_INTHALF | DMA_CSR_INTMAJOR;
	DMAMUX_CHCFG(ch) = (uint8_t)(DMAMUX_ENBL | (rx ? DMAMUX_SRC_I2S0_RX : DMAMUX_SRC_I2S0_TX));
	NVIC_SetPriority((IRQn_Type)(DMA0_IRQn + ch), AUDIO_IRQ_PRIO);
	NVIC_EnableIRQ((IRQn_Type)(DMA0_IRQn + ch));
	DMA_SERQ = ch;
}

k22f_status_t k22f_i2s_start(const k22f_i2s_stream_t *s)
{
	const k22f_i2s_fmt_t *f = &s->fmt;
	if (f->words < 1u || f->words > 32u || f->word_bits < 8u || f->word_bits > 32u || f->first_bit >= f->word_bits ||
	    s->rx_ring == NULL || s->tx_ring == NULL || s->ring_samples == 0 || s->ring_samples % (2u * f->words) != 0) {
		return K22F_ERR_ARG;
	}
	k22f_i2s_stop();
	stream = s;
	SIM_SCGC6 |= SIM_SCGC6_I2S | SIM_SCGC6_DMAMUX;
	SIM_SCGC7 |= SIM_SCGC7_DMA;

	I2S0_TCSR = I2S_CSR_SR | I2S_CSR_FR;
	I2S0_RCSR = I2S_CSR_SR | I2S_CSR_FR;
	I2S0_TCSR = 0;
	I2S0_RCSR = 0;
	I2S0_RCR1 = RX_WATERMARK;
	I2S0_TCR1 = TX_WATERMARK;
	I2S0_RCR2 = cr2(false);
	I2S0_TCR2 = cr2(true);
	I2S0_RCR3 = I2S_CR3_CE;
	I2S0_TCR3 = I2S_CR3_CE;
	I2S0_RCR4 = cr4(f);
	I2S0_TCR4 = cr4(f);
	I2S0_RCR5 = cr5(f);
	I2S0_TCR5 = cr5(f);
	I2S0_RMR = 0;
	I2S0_TMR = 0;

	dma_ring(DMA_RX, true, s->rx_ring, s->ring_samples);
	dma_ring(DMA_TX, false, s->tx_ring, s->ring_samples);
	NVIC_SetPriority(I2S0_Tx_IRQn, AUDIO_IRQ_PRIO);
	NVIC_SetPriority(I2S0_Rx_IRQn, AUDIO_IRQ_PRIO);
	NVIC_EnableIRQ(I2S0_Tx_IRQn);
	NVIC_EnableIRQ(I2S0_Rx_IRQn);
	// The synchronous side first, then the side whose clocks it follows
	I2S0_TCSR = I2S_CSR_TE | I2S_CSR_SEIE | I2S_CSR_FEIE | I2S_CSR_FRDE;
	I2S0_RCSR = I2S_CSR_TE | I2S_CSR_SEIE | I2S_CSR_FEIE | I2S_CSR_FRDE;
	return K22F_OK;
}

void k22f_i2s_stop(void)
{
	if (stream == NULL) {
		return;
	}
	I2S0_RCSR &= ~I2S_CSR_TE;
	I2S0_TCSR &= ~I2S_CSR_TE;
	DMA_CERQ = DMA_RX;
	DMA_CERQ = DMA_TX;
	NVIC_DisableIRQ((IRQn_Type)(DMA0_IRQn + DMA_RX));
	NVIC_DisableIRQ((IRQn_Type)(DMA0_IRQn + DMA_TX));
	NVIC_DisableIRQ(I2S0_Tx_IRQn);
	NVIC_DisableIRQ(I2S0_Rx_IRQn);
	stream = NULL;
}

uint32_t k22f_i2s_errors(void)
{
	return errors;
}

// After the half interrupt the current count is half the start count; after the major loop it has been reloaded
static void dma_done(uint8_t ch, bool rx)
{
	DMA_CINT = ch;
	if (stream != NULL && stream->half != NULL) {
		stream->half(rx, DMA_TCD_CITER(ch) == DMA_TCD_BITER(ch));
	}
}

void DMA0_IRQHandler(void) { dma_done(DMA_RX, true); }
void DMA1_IRQHandler(void) { dma_done(DMA_TX, false); }

// A FIFO error (overrun or underrun) or a frame sync out of place: count it, clear the flags, reset the FIFO
static void sai_error(volatile uint32_t *csr)
{
	uint32_t v = *csr;
	if (v & (I2S_CSR_FEF | I2S_CSR_SEF | I2S_CSR_WSF)) {
		errors++;
		*csr = v | I2S_CSR_FEF | I2S_CSR_SEF | I2S_CSR_WSF | ((v & I2S_CSR_FEF) ? I2S_CSR_FR : 0u);
	}
}

void I2S0_Tx_IRQHandler(void) { sai_error(&I2S0_TCSR); }
void I2S0_Rx_IRQHandler(void) { sai_error(&I2S0_RCSR); }
