// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 SoC B first stage (boot/README.md): drives B's transmitter off, then offers the boot ROM's handshake on UART3,
// B's link to A (boot/hrc7000/stage.c). B has no cable of its own: the host reaches this stage through ODR on A, which
// bridges the programming cable to the link (docs/spec/rx2-link.md 4). Without a host it goes on to the application.
//
// The window: ten offers at power-on, as on the other stages. When B's application wants the stub (RX2_STUB from A), it
// writes STAY_MAGIC at STAY_ADDR and jumps here: the stage then offers for up to a minute, so the host has time to
// answer through A. The word is cleared once the window is over either way, so the next start is a normal one.
#define STAGE_UART		0x14090000u	// UART3
// IO_DIPLEX0 bits 13 / 14: 1 = PTA13 / PTA14 are UART3 TXD / RXD
#define STAGE_UART_PINS()	wr(IO_DIPLEX0, rd(IO_DIPLEX0) | 0x6000u)
#define STAY_ADDR		0x0004FFF8u	// above the stage's stack (0x4F000 down) and every block it accepts
#define STAY_MAGIC		0x53544159u	// "STAY"
#define STAY_OFFERS		1200u		// about a minute at 50 ms
#define STAGE_WINDOW_MORE(offers)	((offers) < STAY_OFFERS && rd(STAY_ADDR) == STAY_MAGIC)

#include "../hrc7000/stage.c"

// B's transmitter pins (docs/spec/hardware-gd88.md): PTB20 / PTB21 TX enables, PTB6 / PTB3 TX tune, PTA17 / PTB10
// TX band switches; all low is the power-on state, transmitter off
#define TX_OFF_A	(1u << 17)
#define TX_OFF_B	((1u << 20) | (1u << 21) | (1u << 6) | (1u << 3) | (1u << 10))

void boot_main(void)
{
	wr(GPIOA, rd(GPIOA) & ~TX_OFF_A);	// level first, then output
	wr(GPIOA + 4, rd(GPIOA + 4) | TX_OFF_A);
	wr(GPIOB, rd(GPIOB) & ~TX_OFF_B);
	wr(GPIOB + 4, rd(GPIOB + 4) | TX_OFF_B);
	uart_setup();
	int got = handshake();
	wr(STAY_ADDR, 0);
	while (!got) {
		boot_resume();			// starts the application; returns only when there is none
		got = handshake();
	}
	for (;;) {
		frame();
	}
}
