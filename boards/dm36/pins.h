// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Shared assignments: docs/spec/hardware-dm36.md 4. Unresolved nets stay unused.
#ifndef DM36_PINS_H
#define DM36_PINS_H
#include "../dm32/pins.h"

// The keypad on the shared matrix (hardware-dm36.md 5.2): [row 0x47..0x4B][column 0x31..0x34]; no Left/Right keys,
// no keys on the fifth row
#define BOARD_KEY_MAP { \
	KEY_STAR, KEY_3, KEY_2, KEY_1, \
	KEY_0, KEY_6, KEY_5, KEY_4, \
	KEY_HASH, KEY_9, KEY_8, KEY_7, \
	KEY_GREEN, KEY_UP, KEY_DOWN, KEY_RED, \
	KEYMATRIX_NONE, KEYMATRIX_NONE, KEYMATRIX_NONE, KEYMATRIX_NONE, \
}

// The side keys on their own inputs, low when pressed (hardware-dm36.md 5.2): SK1 is the boot key on 0x29; SK2 the
// other direct input, 0x25 (0x28 is the keypad backlight). io_init.h makes both inputs
#define PIN_SK1                  0x29u
#define PIN_SK2                  0x25u

// The tone path as stock drives it (hardware-dm36.md 9.1, from the stock image): 0x2F high while a tone plays and low
// when FM receive audio opens, the other way round from the shared default, so low gives the speaker the FD6818's audio
// (the switch's nets are not traced). 0x2E by the mode only, FM low and DMR high. In FM the FD6818's AF output is muted
// before a tone and stays muted until the squelch opens; without the mute a key beep played loud static (2026-11-16)
#undef SPK_SOURCE_FD6818
#define SPK_SOURCE_FD6818        0
#define BOARD_PATH_PIN_BY_MODE   1
#define BOARD_FM_AF_GATED        1

// DMR key-up: the PA comes up from its own drive without a boost, as stock writes one drive (hardware-dm36.md 8.3).
// UHF measured on the SDR (2026-11-16, lowest level): 0.4 dB overshoot, up 0.86 ms before the first symbol, where the
// shared boost gave 7.9 dB. VHF not measured yet
#define BOARD_KEYUP_VHF          1, 900
#define BOARD_KEYUP_UHF          1, 900

// Set-up B with the DM-36's DMR level into the HR_C7000 (0x48 = B2F8) and its air band entry
// (drivers/rf/fd6818/fd6818_setup_b.c)
#define BOARD_FD6818_VARIANT_B   fd6818_variant_b_dmr_b2f8

// shortcut: this handset's K = 774 gives 103 raw per volt; read factory K when another handset needs a different scale.
#undef BAT_RAW_PER_10V
#define BAT_RAW_PER_10V          1030u

// The volume/power knob on ADC0 (hardware-dm36.md 9.5): its raw reading at 0, 1/8, ... 8/8 of the travel, from a
// two-slope (audio) taper fitted by least squares to 18 even sweeps on the owner's radio (2026-11-16): the knee at
// 51.8 % of travel and raw 118, the top 676. The top point is a little under it so the end always reads 100 %
#define KNOB_CURVE { 0, 29, 57, 86, 114, 243, 387, 532, 672 }

#undef PIN_GPS_RXD
#undef PIN_GPS_POWER
#undef GPS_POWER_ON_LEVEL
// The broadcast receiver on the inherited PIN_BCAST_SDA / PIN_BCAST_SCL (0x19 / 0x1A), its enable on PTA29, low = on
// (hardware-dm36.md 4, 9.6)
#define PIN_BCAST_EN             0x1Du
#define BCAST_EN_ON_LEVEL        false
#define PIN_BT_ENABLE           0x30 // high enabled; held low at boot

// Board hooks keep the shared implementation independent of radio identity.
#define BOARD_IO_HEADER "io_init.h"
#define BOARD_VOLUME_HEADER "volume.h"
// The control nets have known initial states, but their electrical functions remain unverified.
// Battery ADC2/scaling, JTAG and RTC wiring remain unverified (sections 1 and 9.3).
#endif
