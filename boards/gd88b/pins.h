// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC B pin map (docs/spec/hardware-gd88.md 10). Pin = port * 32 + bit (PTA 0x00, PTB 0x20, PTC 0x40). The RF
// chips' bus is A's (common/gd88_rf.h). Every pin here is low at start-up except PIN_AUD_VHF and PIN_AUD_UHF's pair
#ifndef GD88B_PINS_H
#define GD88B_PINS_H

// The receive band: UHF = PTB8 high, PTB7 low; VHF the reverse
#define PIN_RX_BAND_UHF		0x28	// PTB8
#define PIN_RX_BAND_VHF		0x27	// PTB7
// The RX front end of each band, high = on. Shared: A's receiver needs its band's too (paths_gd88b.c)
#define PIN_RX_FRONT_UHF	0x2F	// PTB15
#define PIN_RX_FRONT_VHF	0x2E	// PTB14
// The RX path of each receiver, high = on, off while transmitting. Settled on the radio: A needs PTB1 and B needs PTB0
// (B's reading of a bench signal rises 53 dB with it), and neither cares about the other's. PTB13 and PTB5, the rest
// of the original firmware's mode pairs (PTB1 + PTB13, PTB0 + PTB5), make no difference to either: held low
#define PIN_RX_A		0x21	// PTB1
#define PIN_RX_B		0x20	// PTB0
#define PIN_PARK_B13		0x2D
#define PIN_PARK_B5		0x25
// The audio source to A: mode 0 = PTC18 high, PTB4 low; mode 1 the reverse; both low = none. While A transmits the
// original firmware holds mode 0 ("path 0", A's chip), and so does pa_gd88b.c.
// TODO(gd88): unknown, needs a radio: which mode is the main chip's AF and which the modem's line out. Mode 0 is taken
// as the chip's AF (FM)
#define PIN_AUD_SRC0		0x52	// PTC18
#define PIN_AUD_SRC1		0x24	// PTB4
// The audio band pair: VHF = PTB18 high, PTB22 low; UHF the reverse. Both high at start-up. Set to A's TX band while A
// transmits
#define PIN_AUD_VHF		0x32	// PTB18
#define PIN_AUD_UHF		0x36	// PTB22
// The transmitter's switches. The PA is A's (pa_gd88b.c): PA select, 200 us, PA supply, then the drive (DAC B), in
// A's TX band. TX path UHF / VHF belongs to B's own transmitter, which isn't used: held low
#define PIN_TX_PATH_UHF		0x11	// PTA17
#define PIN_TX_PATH_VHF		0x2A	// PTB10
#define PIN_PA_SEL_UHF		0x26	// PTB6
#define PIN_PA_SEL_VHF		0x23	// PTB3
#define PIN_PA_SUP_UHF		0x34	// PTB20
#define PIN_PA_SUP_VHF		0x35	// PTB21
// A's TX line, an input: high while A transmits (A's PTB2). B keys the PA on its edges
#define PIN_TX_LINE		0x22	// PTB2
// TODO(gd88): unknown, needs a radio: PTB11 and PTB12's roles. Low from start-up on
#define PIN_PARK_B11		0x2B
#define PIN_PARK_B12		0x2C
// UART1's pads, PTA9 (RXD) and PTA10 (TXD), share the programming cable's lines with A's (A's PTA9 is also the K1
// PTT line). B never drives them: GPIO inputs from start-up on, as the original firmware leaves them
#define PIN_CABLE_RXD		0x09	// PTA9
#define PIN_CABLE_TXD		0x0A	// PTA10
#define DIPLEX0_UART1		((1u << 9) | (1u << 10))	// IO_DIPLEX0: 1 = GPIO, 0 = UART1
// The status LEDs, high = on. A's status LEDs: A switches them over the link (RX2_LED).
// TODO(gd88): unknown, needs a radio: their colors. LED 1 lights while A transmits, LED 2 while it receives
#define PIN_LED1		0x10	// PTA16
#define PIN_LED2		0x31	// PTB17

#endif
