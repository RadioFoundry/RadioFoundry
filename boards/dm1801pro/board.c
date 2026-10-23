// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Baofeng DM-1801 Pro: board descriptor, IO mux and reset-time pin levels, key map
#include "board.h"
#include "boot_pins.h"
#include "config.h"
#include "dm1801pro.h"
#include "pins.h"
#include "platform.h"
#include "cal/baofeng/baofeng_cal.h"
#if CONFIG_BCAST_FM
#include "bcast/rda5807m/rda5807m_hal.h"
#include "hrc_bcast.h"
#endif
#if CONFIG_APRS
#include "hrc_afsk.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 470000000, true },
#if CONFIG_AM_RX
	{ 108000000, 136000000, false },	// the air band, AM receive only (docs/spec/airband-and-broadcast.md 3)
#endif
#if CONFIG_BCAST_FM
	{ 65000000, 108000000, false },		// the broadcast band, on the tuner (airband-and-broadcast.md 4)
#endif
};

// The factory calibration holds PA drive points for three power levels (hardware-dm1801pro.md 6.4): low, middle, high
static const char *const power_names[] = { "L", "M", "H" };

// docs/ux/information-architecture.md 7.6. The key bits are read off the radio (input_dm1801pro.c)
static const keymap_entry_t keymap[] = {
	{ KEY_MENU, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_BACK, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ KEY_LEFT, ACT_NAV_LEFT, ACT_USER_1, false, true, 0 },
	{ KEY_RIGHT, ACT_NAV_RIGHT, ACT_USER_4, false, true, 0 },
	{ KEY_VFO_MR, ACT_VFO_TOGGLE, ACT_COPY_VFO, false, false, 0 },
#if CONFIG_DUAL_WATCH
	{ KEY_AB, ACT_AB, ACT_DUAL_WATCH, false, false, 0 },	// zones: hold Left / Right, or Zones
#else
	{ KEY_AB, ACT_ZONE_UP, ACT_ZONE_UP, false, false, 0 },
#endif
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false, '*' },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false, '#' },
	{ KEY_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false, 0 },
	{ KEY_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false, 0 },
	{ KEY_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false, 0 },
	{ KEY_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false, 0 },
	{ KEY_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false, 0 },
	{ KEY_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false, 0 },
	{ KEY_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false, 0 },
	{ KEY_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false, 0 },
	{ KEY_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false, 0 },
	{ KEY_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false, 0 },
	{ KEY_SK1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_SK2, ACT_AB, ACT_USER_2, false, false, 0 },
	{ KEY_SOS, ACT_NONE, ACT_EMERGENCY, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): side key 1, side key 2
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2 };

const board_t board = {
	.name = "DM-1801 Pro",
	.caps = CAP_KEYPAD | (CONFIG_DMR ? CAP_DMR : 0) | (CONFIG_BCAST_FM ? CAP_BCAST_FM : 0) |
		(CONFIG_AM_RX ? CAP_AM_RX : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,	// low
	.display = &dm1801pro_display,
	.input = &dm1801pro_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_count = sizeof(side_keys),
	.radio = &dm1801pro_radio,
	.power = &dm1801pro_power,
	.flash = &hrc_flash_spi0,
#if CONFIG_DIAG
	.diag_line = dm1801pro_diag_line,
	.diag_reset = dm1801pro_diag_reset,
#endif
#if CONFIG_DMR
	.dmr = &hrc_dmr,
	.audio = &hrc_audio_pcm,
	.vocoder = &hrc_vocoder,	// the radio's own codec in the slot (codec-dm1801pro.md 4); without it, no voice
#endif
#if CONFIG_HOSTLINK
	.host_port = &hrc_host_uart_stream,
#endif
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// IPv4 over PPP on the programming cable's UART (docs/spec/ppp.md)
#endif
	.system = &hrc_system,
#if CONFIG_BCAST_FM
	.bcast = &rda5807m_hal,
#endif
#if CONFIG_APRS
	.afsk = &hrc_afsk,
#endif
};

// IO mux: only the fields this firmware needs are changed; the rest stay as the boot loader left them. Bit = 1 makes a
// pin a GPIO, 0 gives it to its peripheral (hardware-dm1801pro.md 4)
#define DIPLEX1_SPI0		(0x3Fu << 3)	// SPI0 SCLK / MOSI / MISO
#define DIPLEX2_ADC_BC		((1u << 20) | (1u << 21))	// ADC channels B and C analog

// The power latch is driven high before anything else so the radio stays on once the power key is let go
static void io_init(void)
{
	hrc_pin_out(PIN_POWER_LATCH, true);

	IO_DIPLEX1 &= ~DIPLEX1_SPI0;
	IO_DIPLEX2 &= ~(DIPLEX2_ADC_BC | IO_DIPLEX2_DAC_B);

	for (unsigned i = 0; i < DM1801PRO_BOOT_PIN_COUNT; i++)
		hrc_pin_out(dm1801pro_boot_pins[i].pin, dm1801pro_boot_pins[i].level);

	hrc_pin_in(PIN_RF_DATA);
	hrc_pin_in(PIN_POWER_KEY);
}

// Everything the core stores lives above the write floor (hardware-dm1801pro.md 6.1): the data flash holds the
// radio's own data below 0x700000 (settings, contacts, fonts, voice prompts, recordings), so the open-firmware
// codeplug's 64 KB "EEPROM" image sits at 0x700000 (CODEPLUG_OPENFW_EEPROM_BASE in board.mk), the settings ring and the
// sector journal after it, the 1 MB open-firmware "flash" image at 0x720000 (CODEPLUG_OPENFW_FLASH_BASE) and the native
// codeplug in the last 1 MB. The factory calibration is read only, so the open-firmware CPS gets no calibration window
static const hal_flash_map_t flash_map = {
	.settings_addr = 0x711000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x719000u,	// 2 sectors
	.openfw_eeprom = 0x700000u,
	.openfw_flash = 0x720000u,
	.openfw_end = 0xF00000u,
	.calibration_addr = 0,
	.calibration_size = 0,
	.native_addr = 0xF00000u,
	.native_size = 0x100000u,
};
static const hrc_flash_cfg_t flash_cfg = {
	.cs_pin = PIN_FLASH_CS,
	.write_floor = 0x700000u,
	.map = &flash_map,
};

// A short pulse tells the user the radio is on (hardware-dm1801pro.md 8)
#define POWER_ON_PULSE_MS	300

// Transmitter off from any context (board.h, docs/spec/crash-log.md 8; hardware-dm1801pro.md 5.5): both TX path
// selects and the PA drive (DAC B to 0, powered down). RAM code, register writes only. The FD6818 may still be set to
// transmit (its bus is a bit-banged transaction): with the TX path off its output reaches no keyed PA, and the next
// start sets it up again
HRC_RAMFUNC void board_rf_safe(void)
{
	hrc_pin_force(PIN_TX_UHF, false);
	hrc_pin_force(PIN_TX_VHF, false);
	DAC_DATA_B = 0;
	DAC_PD_CTRL |= DAC_B_OFF;
}

void board_early_init(void)
{
	io_init();
}

#if CONFIG_BCAST_FM
// The broadcast tuner (hardware-dm1801pro.md 7): RDA5807 class on PTA25 / PTA26, enabled by PTA29 low (from the order
// of the stock start and stop code, not measured). Its audio needs the speaker amplifier, which the channel shares
static void bcast_audio(bool on)
{
	dm1801pro_amp(DM1801PRO_AMP_BCAST, on);
}

static const hrc_bcast_cfg_t bcast_cfg = {
	.sda_pin = PIN_BCAST_SDA, .scl_pin = PIN_BCAST_SCL,
	.enable_pin = PIN_BCAST_EN, .enable_level = false,
	.audio_path = bcast_audio,
};
#endif

void board_init(void)
{
	hrc_flash_spi0_configure(&flash_cfg);
	dm1801pro_power.init();
	dm1801pro_power.vibrate(POWER_ON_PULSE_MS);
	hrc_flash_spi0.init();
	bf_cal_load(&hrc_flash_spi0, 0);	// no decoded copy on this radio
#if CONFIG_BCAST_FM
	hrc_bcast_init(&bcast_cfg);
#endif
}
