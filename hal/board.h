// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The board descriptor: everything the core may know about the radio it runs on.
// Core code reads this at run time; it never tests a radio define.
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_display.h"
#include "hal_input.h"
#include "hal_radio.h"
#include "hal_power.h"
#include "hal_flash.h"
#include "hal_dmr_modem.h"
#include "hal_audio.h"
#include "hal_audio_tap.h"
#include "hal_vocoder.h"
#include "hal_stream.h"
#include "hal_system.h"
#include "hal_cps.h"
#include "hal_netif.h"
#include "hal_bcast.h"
#include "hal_afsk.h"

typedef struct {
	uint32_t lo_hz;
	uint32_t hi_hz;
	bool tx;		// transmit allowed in this range
} band_t;

enum {
	CAP_KEYPAD	= 1u << 0,
	CAP_ENCODER	= 1u << 1,
	CAP_GPS		= 1u << 2,
	CAP_DMR		= 1u << 3,
	CAP_VOLUME_POT	= 1u << 4,
	// The channel receiver demodulates AM in the air band (108-137 MHz, receive only; radio/radio_am.h). The band
	// table must hold an RX-only band covering it, and board.radio->rx_am must be set
	CAP_AM_RX	= 1u << 5,
	// A separate broadcast FM receiver (board.bcast, hal_bcast.h)
	CAP_BCAST_FM	= 1u << 6,
};

typedef struct {
	const char *name;
	uint32_t caps;
	const band_t *bands;
	uint8_t band_count;
	uint8_t power_count;
	const char *const *power_names;	// short labels for the status line, power_count entries
	uint8_t power_default;
	const hal_display_t *display;
	const hal_input_t *input;
	const keymap_entry_t *keymap;
	uint8_t keymap_len;
	const hal_radio_t *radio;
	const hal_power_t *power;
	const hal_flash_t *flash;
	const hal_dmr_modem_t *dmr;	// NULL on FM-only radios
	const hal_audio_t *audio;
	const hal_vocoder_t *vocoder;
	const hal_stream_t *host_port;	// the link to a host computer (MMDVM hotspot, CPS); NULL if none
	// Optional: line i of a board diagnostics page (raw readings for bench checks); false past the last line
	bool (*diag_line)(uint8_t i, char *buf, uint8_t len);
	// Optional: zero the counters behind diag_line (the debug link's RESET_COUNTERS)
	void (*diag_reset)(void);
	// Optional: chip identity, raw memory reads, reboot modes (hal_system.h)
	const hal_system_t *system;
	// Battery warnings (the radio core's battery monitor): below battery_low_mv the radio warns, below
	// battery_critical_mv it refuses to transmit (and may switch off). 0 = no warning of that kind
	uint16_t battery_low_mv;
	uint16_t battery_critical_mv;
	// Optional: the open-firmware CPS shim's view of the radio (hal_cps.h)
	const hal_cps_t *cps;
	// Optional: the USB network personality (hal_netif.h); NULL when not built
	const hal_netif_t *netif;
	// Optional: taps on the audio path (hal_audio_tap.h); NULL when the board has none
	const hal_audio_tap_t *audio_tap;
	// Optional: the broadcast FM receiver (hal_bcast.h); NULL when the board has none or it isn't built
	const hal_bcast_t *bcast;
	// Optional: APRS AFSK through the FM transmitter, and received audio for the demodulator (hal_afsk.h); NULL when
	// not built or the radio can't
	const hal_afsk_t *afsk;
	// Optional, with CAP_GPS: whether the GPS module is there, for radios sold with and without one on the same board
	// (the MD-380 / MD-380G): true once the module has been heard. NULL = CAP_GPS alone says
	bool (*gps_present)(void);
} board_t;

extern const board_t board;

// The radio has a GPS receiver now: CAP_GPS and, where the board detects the module at run time, its answer
static inline bool board_has_gps(void)
{
	return (board.caps & CAP_GPS) != 0 && (board.gps_present == NULL || board.gps_present());
}

// Clocks, pins, anything that must happen before the RTOS starts. Called from the platform's main()
void board_early_init(void);
// Start-up after the scheduler runs (drivers that sleep), from the first task
void board_init(void);

#endif
