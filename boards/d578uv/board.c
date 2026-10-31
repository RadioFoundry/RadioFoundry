// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AnyTone AT-D578UV: board descriptor, clocks and reset-time pin levels, key map (docs/spec/hardware-d578uv.md)
#include "board.h"
#include "config.h"
#include "d578uv.h"
#include "pins.h"
#include "mcu_io.h"
#if CONFIG_DMR
#include "modem/sct/sct_dmr.h"
#endif
#if CONFIG_GPS
#include "gps_uart/gps_uart.h"
#endif
#if CONFIG_USB
#include "usb/usb_device.h"
#include "usb/usb_ncm.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
#if CONFIG_AM_RX
	{ 108000000, 136000000, false },	// the air band, AM receive only (docs/spec/hardware-d578uv.md 5.4)
#endif
#if CONFIG_BCAST_FM
	{ 87500000, 108000000, false },		// the broadcast band, on the KTMicro receiver (hardware-d578uv.md 5.4)
#endif
};

static const char *const power_names[] = { "L", "M", "H", "T" };

// Up, down, confirm and exit as the stock menus use them (the stock firmware locks the keys on exit held; here that is
// the core's long QUICK, menu held, and a long exit goes home). The stock firmware's other front keys are programmable
// (P2..P6 and PUSH, defaults Tx Power, TalkAround, Speaker Mode, APRS Set, Switch ChType and none), so they get the
// core's other actions on a press and the user actions on a long press. The serial microphone's keypad: the digits
// (with their DTMF), '*' and '#' as list down and up, A/B, and PA..PD with the stock defaults (Key Menu, Zone Up, Zone
// Down, Key Exit) and DTMF A..D while PTT is held on FM
static const keymap_entry_t keymap[] = {
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ KEY_MENU, ACT_SELECT, ACT_QUICK, false, false, 0 },
	{ KEY_EXIT, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_C15, ACT_CONTEXT, ACT_USER_1, false, false, 0 },
	{ KEY_C17, ACT_AB, ACT_USER_2, false, false, 0 },
	{ KEY_C18, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_C19, ACT_USER_3, ACT_USER_3, false, false, 0 },
	{ KEY_C1E, ACT_TALK_TARGET, ACT_USER_4, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
	// The microphone's Up and Down act as the front Up and Down (on the home screen they step the channel)
	{ KEY_MIC_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_MIC_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ KEY_MIC_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false, 0 },
	{ KEY_MIC_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false, 0 },
	{ KEY_MIC_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false, 0 },
	{ KEY_MIC_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false, 0 },
	{ KEY_MIC_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false, 0 },
	{ KEY_MIC_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false, 0 },
	{ KEY_MIC_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false, 0 },
	{ KEY_MIC_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false, 0 },
	{ KEY_MIC_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false, 0 },
	{ KEY_MIC_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false, 0 },
	{ KEY_MIC_STAR, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, '*' },
	{ KEY_MIC_HASH, ACT_NAV_UP, ACT_NAV_UP, false, true, '#' },
	{ KEY_MIC_AB, ACT_AB, ACT_AB, false, false, 0 },
	{ KEY_MIC_PA, ACT_SELECT, ACT_SELECT, false, false, 'A' },
	{ KEY_MIC_PB, ACT_ZONE_UP, ACT_ZONE_UP, false, false, 'B' },
	{ KEY_MIC_PC, ACT_ZONE_DOWN, ACT_ZONE_DOWN, false, false, 'C' },
	{ KEY_MIC_PD, ACT_BACK, ACT_BACK, false, false, 'D' },
};

const board_t board = {
	.name = "AT-D578UV",
#if CONFIG_DMR
	.caps = CAP_ENCODER | CAP_NO_BATTERY | CAP_DMR | CAP_MODEM_VOICE | (CONFIG_GPS ? CAP_GPS : 0) |
		(CONFIG_AM_RX ? CAP_AM_RX : 0) | (CONFIG_BCAST_FM ? CAP_BCAST_FM : 0),
	.dmr = &sct_dmr_modem,	// start() fails until the baseband has booted: FM only without the boot block
#else
	.caps = CAP_ENCODER | CAP_NO_BATTERY | (CONFIG_GPS ? CAP_GPS : 0) | (CONFIG_AM_RX ? CAP_AM_RX : 0) |
		(CONFIG_BCAST_FM ? CAP_BCAST_FM : 0),
#endif
#if CONFIG_GPS
	.gps_enable = mcu_gps_enable,
	.gps_present = mcu_gps_present,	// CAP_GPS: the module is fitted, but say so only once it has been heard
#endif
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,
	.display = &d578uv_display,
	.input = &d578uv_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.radio = &d578uv_radio,
#if CONFIG_DUAL_WATCH
	.rx2 = &d578uv_rx2,
#endif
#if CONFIG_BCAST_FM
	.bcast = &d578uv_bcast,
#endif
#if CONFIG_APRS
	.afsk = &d578uv_afsk,
#endif
	.power = &d578uv_power,
#if CONFIG_USB
	.host_port = &usb_cdc_stream,	// the debug link, CPS and the hotspot's MMDVM on the USB serial port
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// PPP on the USB serial port, the network adapter under it (board_init)
#elif CONFIG_USB_NETWORK
	.netif = &usb_ncm_netif,
#endif
#endif
	.system = &mcu_system,
#if CONFIG_NAND_STORAGE
	.flash = &d578uv_flash,
#endif
#if CONFIG_DIAG
	.diag_line = d578uv_diag_line,
	.diag_reset = d578uv_diag_reset,
#endif
};

// The 12 MHz crystal to 120 MHz, as the stock firmware runs (hardware-d578uv.md 2)
#define CRYSTAL_HZ	12000000u
#define CORE_HZ		120000000u

// Transmitter off: PE6 low, the PA DAC at 0, the shift register's TX bit clear (radio_d578uv.c)
void board_rf_safe(void)
{
	d578uv_rf_safe();
}

void board_early_init(void)
{
	mcu_clock_init(CRYSTAL_HZ, CORE_HZ);
	// PA15, PB3 and PB4 are the NAND's MISO and the RF bus: free them from JTAG, keep SWD
	mcu_jtag_release();
}

void board_init(void)
{
	d578uv_bus_init();	// the backlight (on the RF bus) is set before the radio starts
	d578uv_nand_init();
#if CONFIG_NAND_STORAGE
	// The core's storage (hardware-d578uv.md 6.3): mounted before storage_init reads it. Without it the radio runs on
	// the default settings
	d578uv_flash.init();
#endif
#if CONFIG_DMR
	d578uv_modem_init();
#endif
#if CONFIG_GPS
	d578uv_gps_init();
#endif
}
