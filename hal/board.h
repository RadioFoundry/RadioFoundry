// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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
#include "hal_rx2.h"
#include "hal_temp.h"

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
	// The radio has no battery (a mobile on a fixed supply). The core shows no battery icon, percentage, menu item or
	// warning, never refuses to transmit for a battery, and reports no battery level. Unset: a battery radio
	CAP_NO_BATTERY	= 1u << 7,
	// Beeps can't sound while the radio transmits DMR (they share the modulator's audio path and are held off it). The
	// talk permit then plays before the first burst: DMR voice keys after a short hold in which the radio still
	// receives and the tone sounds (dmr_call_config_t.permit_hold_ms)
	CAP_BEEP_NOT_ON_AIR = 1u << 8,
	// The DMR modem encodes and plays the voice itself (a baseband with its own vocoder and codec path): voice bits
	// never reach the MCU, so DMR voice needs no board.vocoder, and what works on the voice bits (privacy, recording,
	// a hotspot's voice) has nothing to work on. The modem still takes and reports bursts in their slots
	CAP_MODEM_VOICE	= 1u << 9,
};

// One band's power level names, where a board's levels give different powers per band (board_t.power_bands)
typedef struct {
	uint32_t lo_hz, hi_hz;		// the transmit frequencies it covers
	const char *const *names;	// power_count labels, lowest first
} board_power_band_t;

typedef struct {
	const char *name;
	uint32_t caps;
	const band_t *bands;
	uint8_t band_count;
	uint8_t power_count;
	const char *const *power_names;	// short labels for the status line, power_count entries
	// Optional: the labels per band, where a level's power differs between bands (a top level of 45 W on one band and
	// 50 W on the other). Each band has power_count levels, so a stored level is the same step on every band. A
	// frequency in no entry gets power_names. Read through board_power_name()
	const board_power_band_t *power_bands;
	uint8_t power_band_count;
	uint8_t power_default;
	const hal_display_t *display;
	const hal_input_t *input;
	const keymap_entry_t *keymap;
	uint8_t keymap_len;
	// The keys the user may reassign (Settings > Keys, the codeplug's quick-key slots; core/input/keyassign.h), as key
	// map bits in role order, up to KEYASSIGN_ROLES: every key with a function of its own (side keys, numbered SK1,
	// SK2, ... from the top, then programmable and function keys), not the keys the menus need (select, back, QUICK,
	// CONTEXT, digits, PTT). side_key_names: each one's name as printed on the radio, for Settings ("SK1", "P1")
	const uint8_t *side_keys;
	const char *const *side_key_names;
	uint8_t side_key_count;
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
	// (a G model beside the plain one): true once the module has been heard. NULL = CAP_GPS alone says
	bool (*gps_present)(void);
	// Optional, with CAP_GPS: Settings > Location > GPS. Power the module and its receive path on or off now. Called
	// with the stored value at start-up and on each change; the module is still probed at start-up whatever the
	// setting says, so board_has_gps() is known. NULL = the module is always on
	void (*gps_enable)(bool on);
	// Optional: a second receiver, so dual watch hears both channels at once (hal_rx2.h); NULL on one-receiver radios
	const hal_rx2_t *rx2;
	// Optional: the temperature sensors and the fan (hal_temp.h); NULL on a radio with none the firmware reads
	const hal_temp_t *temp;
	// Optional: bench access to the radio's RF chip registers (the debug link's CHIP_REG, docs/spec/debuglink.md
	// 3.24). chip and page are the board's numbering (its hardware page lists them), chip below 0xFF (the SoC's).
	// Writes val when write, then sets *val to the register as read back (a write-only register: the value written).
	// False for a chip, page or register it doesn't offer. NULL: none
	bool (*chip_reg)(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write);
} board_t;

extern const board_t board;

// The radio runs from a battery (every handheld). False for a mobile on a fixed supply (CAP_NO_BATTERY)
static inline bool board_has_battery(void)
{
	return (board.caps & CAP_NO_BATTERY) == 0;
}

// hz is in one of the radio's bands (board.bands), and one that may transmit when need_tx
static inline bool board_in_band(uint32_t hz, bool need_tx)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		if (hz >= b->lo_hz && hz <= b->hi_hz && (b->tx || !need_tx)) {
			return true;
		}
	}
	return false;
}

// The transmit band hz is in (an index into board.bands), or -1
static inline int board_tx_band(uint32_t hz)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		if (b->tx && hz >= b->lo_hz && hz <= b->hi_hz) {
			return i;
		}
	}
	return -1;
}

// A repeater offset carries to a new RX frequency only within one transmit band: RX before and after and the TX it
// gives all in it (a UHF offset means nothing on VHF, even where TX would land in a band)
// A power level's label at a transmit frequency (power_bands, else power_names); "?" for no such level
static inline const char *board_power_name(uint8_t level, uint32_t tx_hz)
{
	if (level >= board.power_count) {
		return "?";
	}
	for (uint8_t i = 0; i < board.power_band_count; i++) {
		const board_power_band_t *b = &board.power_bands[i];
		if (tx_hz >= b->lo_hz && tx_hz <= b->hi_hz) {
			return b->names[level];
		}
	}
	return board.power_names != NULL ? board.power_names[level] : "?";
}

static inline bool board_offset_carries(uint32_t old_rx, uint32_t new_rx, uint32_t new_tx)
{
	int band = board_tx_band(new_rx);
	return band >= 0 && board_tx_band(old_rx) == band && board_tx_band(new_tx) == band;
}

// The voltage the radio measures: its battery, its DC input (no battery, hal_power_t.supply_mv set), or neither
typedef enum { BOARD_VOLTS_NONE, BOARD_VOLTS_BATTERY, BOARD_VOLTS_SUPPLY } board_volts_t;

static inline board_volts_t board_volts(void)
{
	if (board_has_battery())
		return BOARD_VOLTS_BATTERY;
	return board.power != NULL && board.power->supply_mv != NULL ? BOARD_VOLTS_SUPPLY : BOARD_VOLTS_NONE;
}

// Settings > Location > GPS took effect (a board without the hook has nothing to switch)
static inline void board_gps_enable(bool on)
{
	if (board.gps_enable != NULL) {
		board.gps_enable(on);
	}
}

// The radio has a GPS receiver now: CAP_GPS and, where the board detects the module at run time, its answer
static inline bool board_has_gps(void)
{
	return (board.caps & CAP_GPS) != 0 && (board.gps_present == NULL || board.gps_present());
}

// Clocks, pins, anything that must happen before the RTOS starts. Called from the platform's main()
void board_early_init(void);
// Transmitter off, now (docs/spec/crash-log.md 8): the TX supplies, the PA selects and keying, the PA drive DAC to 0,
// the modem's TX state where plain register writes stop it, and the RX/TX switch to receive where it is a plain pin.
// Called from fault handlers, the crash capture, before every software reset or jump out of the application, and
// first thing in the platform's main(), before the clocks are set up. So it runs with interrupts off and without the
// RTOS: register and GPIO writes only, no locks, no flash or NAND, no waits; it may run any number of times and
// leaves everything else (the power latch, the display) as it is. On the HR_C7000 it is RAM code (HRC_RAMFUNC)
void board_rf_safe(void);
// Start-up after the scheduler runs (drivers that sleep), from the first task
void board_init(void);

#endif
