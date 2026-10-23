// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C7000 first stages' common part (boot/README.md): it answers on a UART the way the HR_C7000 serial boot ROM
// answers on UART0, so stub/stubtool.py loads a stub through it exactly as on a radio whose boot ROM
// answers on its cable. Runs from XIP
// flash and never touches the flash controller. A radio's boot.c defines STAGE_UART, STAGE_UART_PINS() and its own
// boot_main(), then includes this file.
//
// ROM protocol (stubtool's start(); frames are 02 op sum8(payload) 03 + payload):
//   radio: 02 24 00 03, repeated, until the host answers 02 14 00 03
//   host:  op 0x18, payload u32 BE address + 00 00 00 40 + 256 bytes: stored, acked 02 05 00 03
//   host:  op 0x1B, payload u32 BE address: acked 02 05 00 03, then the stage jumps there
// A frame with a bad sum, an unknown op, an address out of range or a gap of RX_SPINS is dropped without an ack: the
// host sends it again.
//
// BOOT_WINDOW (entry.S): 0 offers the handshake forever; n offers it n times, about 50 ms apart, then boot_resume()
// starts the application.
#include <stdint.h>

#ifndef BOOT_HOST_TEST	// tests/host/bootDb25dTest.c supplies these
static inline uint32_t rd(uint32_t a)
{
	return *(volatile uint32_t *)(uintptr_t)a;
}

static inline void wr(uint32_t a, uint32_t v)
{
	*(volatile uint32_t *)(uintptr_t)a = v;
}

static inline void ram_wr(uint32_t a, uint8_t v)
{
	*(volatile uint8_t *)(uintptr_t)a = v;
}

static inline void jump(uint32_t a)
{
	((void (*)(void))(uintptr_t)a)();
}
#endif

#ifndef BOOT_WINDOW
#define BOOT_WINDOW	0
#endif
// A radio's stage can keep offering past BOOT_WINDOW (offers so far): a second SoC when its application asked for
// it
#ifndef STAGE_WINDOW_MORE
#define STAGE_WINDOW_MORE(offers)	0
#endif

#define SYS		0x11000000u
#define CLK_GATES	(SYS + 0x2C)
#define IO_DIPLEX0	(SYS + 0x34)	// pin functions; STAGE_UART_PINS() gives the stage's UART its pins
#define WDT		0x14010000u
#define UART		STAGE_UART
#define U_RBR		0x00u		// THR on write, DLL with DLAB
#define U_IER		0x04u		// DLH with DLAB
#define U_FCR		0x08u
#define U_LCR		0x0Cu
#define U_LSR		0x14u		// bit 0 data ready, bit 6 transmitter empty
#define U_USR		0x7Cu		// bit 1 TX FIFO not full
#define GPIOA		0x14020000u	// DR +0, DDR +4
#define GPIOB		0x14100000u
#define GPIOC		0x14110000u

#define UART_DIV	19u		// 36 MHz APB / 16 / 115200, as the IAP loader sets it
#define RAM_HI		0x00050000u	// start addresses below the stub's stack top
#define LOAD_HI		0x0004E000u	// blocks below this: this stage's stack starts at 0x4F000 (boot.S)
#define HS_SPINS	200000u		// about 50 ms between handshakes at the loader's 72 MHz
#define RX_SPINS	4000000u
#define TX_SPINS	100000u

void boot_resume(void);
void boot_main(void);

// -1 when the UART never took the byte (not clocked, not set up): the caller gives up on the cable
static int tx(uint32_t b)
{
	for (uint32_t n = TX_SPINS; !(rd(UART + U_USR) & 2u); n--) {
		if (n == 0) {
			return -1;
		}
	}
	wr(UART + U_RBR, b & 0xFFu);
	return 0;
}

static int tx4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	return tx(a) | tx(b) | tx(c) | tx(d);
}

static int rx(void)
{
	for (uint32_t n = RX_SPINS; n != 0; n--) {
		if (rd(UART + U_LSR) & 1u) {
			return (int)(rd(UART + U_RBR) & 0xFFu);
		}
	}
	return -1;
}

// The loader sets the UART up only in its update mode; on a normal power-on it starts the application with the UART
// untouched. Set up as the loader does it, unless it already is (8N1 in LCR), so the loader's divisor stays
static void uart_setup(void)
{
	if ((rd(UART + U_LCR) & 3u) == 3u) {
		return;
	}
	wr(CLK_GATES, 0xFFFFFFFFu);
	STAGE_UART_PINS();
	wr(UART + U_LCR, 0x80u);
	wr(UART + U_RBR, UART_DIV & 0xFFu);
	wr(UART + U_IER, UART_DIV >> 8);
	wr(UART + U_LCR, 0x03u);
	wr(UART + U_IER, 0);
	wr(UART + U_FCR, 0x07u);
}

// Offer the handshake until the host answers 02 14 00 03 (1), the window runs out or the UART is dead (0)
static int handshake(void)
{
	static const uint8_t answer[4] = { 0x02, 0x14, 0x00, 0x03 };
	uint32_t got = 0;
	for (uint32_t offers = 0; BOOT_WINDOW == 0 || offers < BOOT_WINDOW || STAGE_WINDOW_MORE(offers); offers++) {
		if (tx4(0x02, 0x24, 0x00, 0x03)) {
			return 0;
		}
		for (uint32_t n = HS_SPINS; n != 0; n--) {
			if (rd(UART + U_LSR) & 1u) {
				uint32_t b = rd(UART + U_RBR) & 0xFFu;
				got = b == answer[got] ? got + 1u : (b == answer[0]);
				if (got == 4u) {
					return 1;
				}
			}
		}
	}
	return 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// One frame. A good start command doesn't return
static void frame(void)
{
	uint8_t buf[8 + 256];
	int b;
	while ((b = rx()) != 0x02) {
		if (b < 0) {
			return;
		}
	}
	int op = rx(), chk = rx(), etx = rx();
	uint32_t len = op == 0x18 ? sizeof buf : op == 0x1B ? 4u : 0u;
	if (chk < 0 || etx != 0x03 || len == 0) {
		return;
	}
	uint32_t sum = 0;
	for (uint32_t i = 0; i < len; i++) {
		if ((b = rx()) < 0) {
			return;
		}
		buf[i] = (uint8_t)b;
		sum += (uint32_t)b;
	}
	uint32_t addr = be32(buf);
	if ((sum & 0xFFu) != (uint32_t)chk || addr >= RAM_HI || (addr & 3u)) {
		return;
	}
	if (op == 0x18) {
		if (addr + 256u > LOAD_HI) {
			return;
		}
		for (uint32_t i = 0; i < 256u; i++) {
			ram_wr(addr + i, buf[8 + i]);
		}
		tx4(0x02, 0x05, 0x00, 0x03);
		return;
	}
	tx4(0x02, 0x05, 0x00, 0x03);
	for (uint32_t n = TX_SPINS; n != 0 && !(rd(UART + U_LSR) & 0x40u); n--) {	// the ack is out before the stub runs
	}
	wr(WDT + 0x00, 0x5ADA7200u);	// watchdog off: the loader may have started it, and the stub never feeds it
	wr(WDT + 0x10, 0);
	wr(WDT + 0x00, 0);
	jump(addr);
}

