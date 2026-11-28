// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Hardware traits the core has code for that few boards share. A board that has one says BOARD_<TRAIT> := y in its
// board.mk (BOARD_NO_PCM in its config.mk, which config/features.mk reads too), and the Makefile passes each as
// RFOUNDRY_<TRAIT> = 0 or 1, so the other boards' images carry none of that code. The board still states the trait
// at run time (board_t.caps, its hooks); these only let the compiler drop the paths no board in the build can take.
// Not given (the host tests, the bench tools): on, so every path builds and runs there.
#ifndef HAL_TRAITS_H
#define HAL_TRAITS_H

// The DMR modem is a whole baseband the MCU talks to in packets (docs/spec/modem-sct.md): it keeps the slots, codes
// the voice and plays it itself (CAP_MODEM_VOICE). The hooks only such a modem fills: hal_dmr_modem_t's play_*,
// tx_release, slot_verified, clears_crc9 and cach_ticks, board_t.modem_codec and chip_reg_bytes,
// hal_radio_t.dmr_wake_listen. A modem driven by frame registers (HR_C6000, HR_C7000) fills none of them
#ifndef RFOUNDRY_PACKET_MODEM
#define RFOUNDRY_PACKET_MODEM	1
#endif

// No PCM path from the MCU to the speaker: beeps go through the radio's tone hook (hal_radio_t.beep_tone)
#ifndef RFOUNDRY_NO_PCM
#define RFOUNDRY_NO_PCM		1
#endif

// The air tap gives the derivative of the deviation (hal_baseband_t.rx_slope_hz): the soft modem integrates it
#ifndef RFOUNDRY_SLOPE_TAP
#define RFOUNDRY_SLOPE_TAP	1
#endif

// A second volume knob, side B's (hal_power_t.volume_b): the view and the volume bar show both
#ifndef RFOUNDRY_VOLUME_B
#define RFOUNDRY_VOLUME_B	1
#endif

// Temperature sensors the board lists (board_t.temp): the readings (core/app/temp.c), the debug link's sensors view and
// SNMP's rfoundryTempTable (docs/spec/temperature.md)
#ifndef RFOUNDRY_TEMP
#define RFOUNDRY_TEMP		1
#endif

#endif
