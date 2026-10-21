// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DSPI master (reference manual chapter 51), polled, 8-bit frames, FIFOs off: one byte out, one byte in. The chip
// select is either a GPIO the caller drives, so a transaction of any length stays selected without the CONT bit, or
// one of the module's PCS outputs: then every byte but the transaction's last carries CONT, which keeps it selected
// between bytes, and the CTAR delays give the select setup, hold and idle times
#include "platform.h"
#include "periph_math.h"

#define BYTE_WAIT_LOOPS	20000u	// far above one byte at the slowest rate used (about 100 us at 100 kHz)

void k22f_dspi_init(const k22f_dspi_cfg_t *c)
{
	uint8_t n = c->inst;
	SIM_SCGC6 |= n == 0 ? SIM_SCGC6_SPI0 : SIM_SCGC6_SPI1;
	SPI_MCR(n) = SPI_MCR_MSTR | SPI_MCR_ROOE | SPI_MCR_PCSIS(0x3F) | SPI_MCR_DIS_TXF | SPI_MCR_DIS_RXF |
		     SPI_MCR_CLR_TXF | SPI_MCR_CLR_RXF | SPI_MCR_HALT;
	uint32_t delays = 0;
	uint8_t pre, scale;
	if (c->cs_ns != 0u && k22f_dspi_delay(k22f_bus_clock(), c->cs_ns, &pre, &scale)) {
		delays = SPI_CTAR_PCSSCK(pre) | SPI_CTAR_CSSCK(scale) | SPI_CTAR_PASC(pre) | SPI_CTAR_ASC(scale) |
			 SPI_CTAR_PDT(pre) | SPI_CTAR_DT(scale);
	}
	SPI_CTAR(n, 0) = SPI_CTAR_FMSZ(8) | (c->cpol ? SPI_CTAR_CPOL : 0u) | (c->cpha ? SPI_CTAR_CPHA : 0u) |
			 SPI_CTAR_PBR(c->pbr & 3u) | SPI_CTAR_BR(c->br & 15u) | (c->dbr ? SPI_CTAR_DBR : 0u) | delays;
	SPI_SR(n) = SPI_SR_TCF | SPI_SR_EOQF | SPI_SR_TFUF | SPI_SR_TFFF | SPI_SR_RFOF | SPI_SR_RFDF;	// write 1 to clear
	SPI_MCR(n) &= ~SPI_MCR_HALT;
}

// cmd: the PUSHR command half for every byte; cont_last: whether the last byte keeps the chip select too
static bool xfer(uint8_t inst, uint32_t cmd, bool cont_last, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		uint32_t c = (i + 1u < len || cont_last) ? cmd : (cmd & ~SPI_PUSHR_CONT);
		SPI_PUSHR(inst) = c | (tx != NULL ? tx[i] : 0xFFu);
		uint32_t w = 0;
		while ((SPI_SR(inst) & SPI_SR_RFDF) == 0) {
			if (++w > BYTE_WAIT_LOOPS) {
				return false;
			}
		}
		uint8_t b = (uint8_t)SPI_POPR(inst);
		SPI_SR(inst) = SPI_SR_RFDF | SPI_SR_TCF;
		if (rx != NULL) {
			rx[i] = b;
		}
	}
	return true;
}

bool k22f_dspi_xfer(uint8_t inst, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	return xfer(inst, SPI_PUSHR_CTAS(0), false, tx, rx, len);
}

bool k22f_dspi_xfer_pcs(uint8_t inst, uint8_t pcs, const uint8_t *tx, uint8_t *rx, uint32_t len, bool end)
{
	if (pcs > 5u) {
		return false;
	}
	return xfer(inst, SPI_PUSHR_CTAS(0) | SPI_PUSHR_CONT | SPI_PUSHR_PCS(1u << pcs), !end, tx, rx, len);
}
