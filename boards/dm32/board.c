// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Baofeng DM-32UV: board descriptor, IO mux and reset-time pin levels, key map
#include "board.h"
#include "config.h"
#include "dm32.h"
#include <pins.h>
#include "platform.h"
#ifdef BOARD_IO_HEADER
#include BOARD_IO_HEADER
#endif
#include "hrc_afsk.h"
#include "hrc_bcast.h"
#include "ui/fmt.h"
#if CONFIG_BCAST_FM
#include "bcast/rda5807m/rda5807m_hal.h"
#endif
#include "FreeRTOS.h"
#include "semphr.h"
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

#ifndef BOARD_EXTERNAL_DESCRIPTOR
#if CONFIG_RX_EXT
static bool rx_ext_on;
#endif

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
#if CONFIG_AM_RX
	{ 108000000, 136000000, false },	// the air band, AM receive only (docs/spec/airband-and-broadcast.md 3)
#endif
#if CONFIG_BCAST_FM
	{ 65000000, 108000000, false },		// the broadcast band, on the RDA5807M (airband-and-broadcast.md 4)
#endif
#if CONFIG_RX_EXT
	// Receive only past the rated bands, while the user turns it on: the FD6818 band table (hardware-dm32.md 8.3). Keep these last
	{ 16000000, 65000000, false },
	{ 174000000, 400000000, false },
	{ 480000000, 740000000, false },
#endif
};

#include "board_keys.h"

// The support info (docs/spec/support-info.md 2): the FD6818 set-up start-up found (hardware-dm32.md 8.10: A the classic
// RF section, B the newer one, "no lock" neither); the HR_C7000 is the MCU line
static bool hw_line(uint8_t i, uint8_t *role, char *buf, uint8_t len)
{
	if (i != 0) {
		return false;
	}
	char s = dm32_rf_setup();
	*role = BOARD_HW_RF;
	str_copy(buf, len, s == 'A' ? "FD6818 set-up A" : s == 'B' ? "FD6818 set-up B" : "FD6818 no lock");
	return true;
}

const board_t board = {
	.name = "DM-32UV",
	.hw_line = hw_line,
#include "board_fields.h"
};

#endif

static SemaphoreHandle_t lcdbus;

void dm32_lcdbus_lock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreTakeRecursive(lcdbus, portMAX_DELAY);
	}
}

void dm32_lcdbus_unlock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreGiveRecursive(lcdbus);
	}
}

// docs/spec/hardware-dm32.md 5.1: IO mux, pulls, directions and levels. The power latch is driven high before
// anything else so the radio stays on when the knob's own supply path lets go
static void io_init(void)
{
	hrc_pin_out(PIN_POWER_LATCH, true);

	IO_DIPLEX0 = 0xFFFDFD80u;			// JTAG, I2C0, UART1 RXD, PWM0; PTA7/8 GPIO
	IO_DIPLEX1 = 0xFFFFFE07u | 0x38000000u;		// SPI0 to the data flash; CS0 and the rest GPIO
	IO_DIPLEX2 = 0x0FC80100u;			// LCD bus GPIO until the display takes it; ADC B/C, DAC B/C analog
	IOMGR_REN0 = 0xFFFBFFEFu & 0xFEFFFFFFu;
	IOMGR_REN1 = 0x00FFFFFFu;
	IOMGR_REN2 = 0x0FFFFF70u | 0x0C000000u;

	// Outputs at their safe levels: transmitter and receivers off, speaker muted, chip selects high
	hrc_pin_out(PIN_FLASH_CS, true);
	hrc_pin_out(PIN_FD_CS, true);
	hrc_pin_out(PIN_FD_CLK, false);
	hrc_pin_out(PIN_UHF_PA, false);
	hrc_pin_out(PIN_VHF_PA, false);
	hrc_pin_out(PIN_UHF_LNA, false);
	hrc_pin_out(PIN_VHF_LNA, false);
	hrc_pin_out(PIN_MIC_PATH, true);
	hrc_pin_out(PIN_SPK_SOURCE, !SPK_SOURCE_FD6818);	// the codec until FM receive opens the squelch
	hrc_pin_out(PIN_AMP_EN, false);
	hrc_pin_out(PIN_LED_RED, false);
	hrc_pin_out(PIN_LED_GREEN, false);
	hrc_pin_out(PIN_KEY_BACKLIGHT, false);
	hrc_pin_out(PIN_LCD_BACKLIGHT, false);
#ifdef PIN_GPS_POWER
	hrc_pin_out(PIN_GPS_POWER, !GPS_POWER_ON_LEVEL);	// GPS off until something wants it
#endif
#ifdef PIN_BT_ENABLE
	hrc_pin_out(PIN_BT_ENABLE, false);
#endif
	hrc_pin_out(PIN_LCD_RESET, true);
	hrc_pin_out(PIN_LCD_RD, true);
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		hrc_pin_out((uint8_t)(PIN_KEY_ROW0 + r), true);	// rows idle high
	}
	hrc_pin_out(PIN_LCD_DB6, false);

	// Inputs, all without interrupts (the old port enabled GPIO B interrupts on every input: spec 16 #9)
	hrc_pin_in(PIN_POWER_SWITCH);
	hrc_pin_in(PIN_PTT);
	for (uint8_t c = PIN_KEY_COL1; c <= PIN_KEY_COL4; c++) {
		hrc_pin_in(c);
	}
	hrc_pin_in(PIN_ENC_A);
	hrc_pin_in(PIN_ENC_B);
	hrc_pin_in(PIN_FD_GPIO0);
	hrc_pin_in(PIN_FD_GPIO1);
	hrc_pin_in(PIN_FD_DATA);
#ifdef BOARD_IO_HEADER
	board_io_init();
#endif
}

// Transmitter off from any context (board.h, docs/spec/crash-log.md 8; hardware-dm32.md 5): both PA selects and the PA
// drive (DAC B to 0, powered down). RAM code, register writes only. The FD6818 may still be set to transmit (its bus is
// a bit-banged transaction): with the PA selects off its output reaches no keyed PA, and the next start sets it up again
HRC_RAMFUNC void board_rf_safe(void)
{
	hrc_pin_force(PIN_UHF_PA, false);
	hrc_pin_force(PIN_VHF_PA, false);
	DAC_DATA_B = 0;
	DAC_PD_CTRL |= DAC_B_OFF;
}

void board_early_init(void)
{
	io_init();
}

// Everything the core stores lives above the write floor (hardware-dm32.md 10.1): the open-firmware codeplug's 64 KB
// "EEPROM" image at 0x700000, a decoded calibration copy from other firmware left alone at 0x710000, the settings ring
// and the sector journal after it, the 1 MB open-firmware "flash" image at 0x720000 with room above it for the DMR ID
// database, the native codeplug in the last 1 MB. The factory calibration is read only, so the open-firmware CPS gets
// no calibration window
#ifndef BOARD_EXTERNAL_DESCRIPTOR
const hal_flash_map_t board_flash_map = {
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
#endif
static const hrc_flash_cfg_t flash_cfg = {
	.cs_pin = PIN_FLASH_CS,
	.write_floor = 0x700000u,	// hardware-dm32.md 10.1
	.map = &board_flash_map,
};

#if CONFIG_GPS
// The ATGM336H on UART1; PTA9 is UART1's RXD from io_init
static const hrc_gps_cfg_t gps_cfg = {
	.uart_base = UART1_BASE,
	.irq = IRQ_UART1,
	.power_pin = PIN_GPS_POWER,
	.power_on_level = GPS_POWER_ON_LEVEL,
};
#endif

#if CONFIG_BCAST_FM
// The RDA5807M on PTA25 / PTA26 (hardware-dm32.md 8.7): no enable line is known unless the board's pins.h gives
// PIN_BCAST_EN and BCAST_EN_ON_LEVEL; its audio is assumed to reach the amplifier as on the other FD6818 radio (bring-up B4)
static const hrc_bcast_cfg_t bcast_cfg = {
	.sda_pin = PIN_BCAST_SDA, .scl_pin = PIN_BCAST_SCL,
#ifdef PIN_BCAST_EN
	.enable_pin = PIN_BCAST_EN, .enable_level = BCAST_EN_ON_LEVEL,
#else
	.enable_pin = HRC_BCAST_NO_PIN,
#endif
	.audio_path = dm32_bcast_audio,
};
#endif

void board_init(void)
{
	hrc_flash_spi0_configure(&flash_cfg);
	lcdbus = xSemaphoreCreateRecursiveMutex();
	dm32_power.init();
	dm32_flash.init();
	bf_cal_load(&hrc_flash_spi0, 0x710000u);	// other firmware may keep a decoded copy there
#if CONFIG_GPS
	hrc_gps_init(&gps_cfg);
#endif
#if CONFIG_BCAST_FM
	hrc_bcast_init(&bcast_cfg);
#endif
}
