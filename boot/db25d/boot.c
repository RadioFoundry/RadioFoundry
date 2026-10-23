// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D first stage (boot/README.md): holds the power latch and drives the transmitter off, then answers on UART1 (the K1 programming cable) the way
// the HR_C7000 serial boot ROM answers on UART0, so stub/stubtool.py loads a stub here exactly as on the other
// HR_C7000 boards. Without a host it goes on to the application. Runs from XIP flash and never touches the flash controller.
//
// ROM protocol (stubtool's start(); frames are 02 op sum8(payload) 03 + payload):
//   radio: 02 24 00 03, repeated, until the host answers 02 14 00 03
//   host:  op 0x18, payload u32 BE address + 00 00 00 40 + 256 bytes: stored, acked 02 05 00 03
//   host:  op 0x1B, payload u32 BE address: acked 02 05 00 03, then the stage jumps there
// A frame with a bad sum, an unknown op, an address out of range or a gap of RX_SPINS is dropped without an ack: the
// host sends it again.
//
// BOOT_WINDOW (boot.S): 0 offers the handshake forever; n offers it n times, about 50 ms apart, then boot_resume()
// starts the application. On that path only the power latch and the transmitter pins are changed, to the levels the
// application drives them to itself.
#include <stdint.h>

#ifndef BOOT_HOST_TEST	// boards/db25d/tests/bootDb25dTest.c supplies these
static inline uint32_t rd(uint32_t a)
{
	return *(volatile uint32_t *)(uintptr_t)a;
}

#define LATCH		(1u << 13)	// PTB13 keeps the radio on once the power key is let go
// The transmitter's pins (hardware-db25d.md 3.2, boards/db25d/pins.h): PTA26 UHF PA select, PTA27 TX supply A, PTB2 TX
// supply B, PTB3 VHF PA select, all low = off; PTA23 high = receive. A soft reset can leave them as they were, and the
// stage may wait here for a host, so they go off before anything that takes time (docs/spec/crash-log.md 8)
#define TX_OFF_A	((1u << 26) | (1u << 27))
#define RX_ON_A		(1u << 23)
#define TX_OFF_B	((1u << 2) | (1u << 3))

void boot_main(void)
{
	wr(GPIOB, rd(GPIOB) | LATCH);		// level first, then output: the pin never goes low
	wr(GPIOB + 4, rd(GPIOB + 4) | LATCH);
	wr(GPIOA, (rd(GPIOA) & ~TX_OFF_A) | RX_ON_A);	// the same for the transmitter pins
	wr(GPIOA + 4, rd(GPIOA + 4) | TX_OFF_A | RX_ON_A);
	wr(GPIOB, rd(GPIOB) & ~TX_OFF_B);
	wr(GPIOB + 4, rd(GPIOB + 4) | TX_OFF_B);
	uart_setup();
	while (!handshake()) {
		boot_resume();			// starts the application; returns only when there is none
	}
	for (;;) {
		frame();
	}
}
