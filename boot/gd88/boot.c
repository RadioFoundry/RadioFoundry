// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC A first stage (boot/README.md): holds the power latch, drives the transmitter off and puts the serial
// output chain in its power-on state, then offers the boot ROM's handshake on UART1 (the programming cable; boot/hrc7000/stage.c). Without a host it goes
// on to the application.
//
// PTB2 is A's TX / RX line, high while transmitting (docs/spec/hardware-gd88.md). A soft reset can leave it high,
// mid-transmission, and the stage may wait here for a host, so it goes low first. The 64-bit serial output chain
// (PTB0 clock, PTB1 data, PTB3 latch: audio routing, the amplifier, the TX route) is shifted to all zero, its
// power-on state, and PTC20 goes low.
#define STAGE_UART		0x14040000u	// UART1
// IO_DIPLEX0 bits 9 / 10: 1 = PTA9 / PTA10 are GPIO, 0 = UART1 RXD / TXD
#define STAGE_UART_PINS()	wr(IO_DIPLEX0, rd(IO_DIPLEX0) & ~0x600u)

#include "../hrc7000/stage.c"

#define LATCH		(1u << 13)	// PTB13 keeps the radio on once the power key is let go
#define TX_LINE		(1u << 2)	// PTB2, high = transmit
#define CHAIN_CLK	(1u << 0)	// PTB0
#define CHAIN_DATA	(1u << 1)	// PTB1
#define CHAIN_LATCH	(1u << 3)	// PTB3, idle high
#define CHAIN_BITS	64u
#define RF_EN_C		(1u << 20)	// PTC20
#define CHAIN_SPINS	8u		// a few hundred ns per edge; the chain's registers need tens of ns

static void spin(uint32_t n)
{
	for (volatile uint32_t i = n; i != 0; i--) {
	}
}

static void chain_clear(void)
{
	uint32_t b = rd(GPIOB) & ~(CHAIN_CLK | CHAIN_DATA | CHAIN_LATCH);
	wr(GPIOB, b);				// latch low, data low, clock low
	wr(GPIOB + 4, rd(GPIOB + 4) | CHAIN_CLK | CHAIN_DATA | CHAIN_LATCH);
	for (uint32_t i = 0; i < CHAIN_BITS; i++) {
		spin(CHAIN_SPINS);
		wr(GPIOB, b | CHAIN_CLK);
		spin(CHAIN_SPINS);
		wr(GPIOB, b);
	}
	spin(CHAIN_SPINS);
	wr(GPIOB, b | CHAIN_LATCH);		// the zeros to the outputs
}

void boot_main(void)
{
	wr(GPIOB, rd(GPIOB) | LATCH);		// level first, then output: the pin never goes low
	wr(GPIOB + 4, rd(GPIOB + 4) | LATCH);
	wr(GPIOB, rd(GPIOB) & ~TX_LINE);
	wr(GPIOB + 4, rd(GPIOB + 4) | TX_LINE);
	wr(GPIOC, rd(GPIOC) & ~RF_EN_C);
	wr(GPIOC + 4, rd(GPIOC + 4) | RF_EN_C);
	chain_clear();
	uart_setup();
	while (!handshake()) {
		boot_resume();			// starts the application; returns only when there is none
	}
	for (;;) {
		frame();
	}
}
