// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Baofeng DM-1701: board descriptor, clocks, reset-time pin levels, key map
#include "board.h"
#include "config.h"
#include "dm1701.h"
#include "pins.h"
#include "FreeRTOS.h"
#include "semphr.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#endif

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

static const char *const power_names[] = { "50m", "250m", "500m", "750m", "1W", "2W", "3W", "4W", "5W" };

// docs/ux/information-architecture.md 7.1
static const keymap_entry_t keymap[] = {
	{ KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false },
	{ KEY_RED, ACT_BACK, ACT_BACK, false, false },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ KEY_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false },
	{ KEY_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false },
	{ KEY_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false },
	{ KEY_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false },
	{ KEY_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false },
	{ KEY_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false },
	{ KEY_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false },
	{ KEY_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false },
	{ KEY_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false },
	{ KEY_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false },
	{ KEY_SK1, ACT_MONITOR, ACT_MONITOR, true, false },
	{ KEY_SK2, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ KEY_TOP, ACT_USER_3, ACT_USER_3, false, false },
	{ KEY_FN_LEFT, ACT_USER_1, ACT_USER_1, false, false },
	{ KEY_FN_RIGHT, ACT_USER_4, ACT_USER_4, false, false },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false },
};

const board_t board = {
	.name = "DM-1701",
	.caps = CAP_KEYPAD | CAP_ENCODER | CAP_VOLUME_POT | (CONFIG_DMR ? CAP_DMR : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 4,	// 1 W
	.display = &dm1701_display,
	.input = &dm1701_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.radio = &dm1701_radio,
	.power = &dm1701_power,
	.flash = &dm1701_flash,
#if CONFIG_DIAG
	.diag_line = dm1701_diag_line,
	.diag_reset = dm1701_diag_reset,
#endif
#if CONFIG_DMR
	.dmr = &dm1701_dmr,
	.audio = &dm1701_audio,
	.vocoder = &dm1701_vocoder,
#endif
#if CONFIG_USB
	.host_port = &usb_cdc_stream,
#endif
};

// docs/spec/hardware-dm1701.md 1.2
static const stm32_clock_t clock = {
	.hse_hz = 8000000,
	.hse_bypass = true,
	.pll_m = 4,
	.pll_n = 72,
	.pll_p = RCC_PLLP_DIV2,
	.pll_q = 3,
	.ahb_div = RCC_SYSCLK_DIV1,
	.apb1_div = RCC_HCLK_DIV2,
	.apb2_div = RCC_HCLK_DIV1,
	.flash_latency = FLASH_LATENCY_3,
	.lse = true,
};

static SemaphoreHandle_t lcdbus;

void dm1701_lcdbus_lock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreTakeRecursive(lcdbus, portMAX_DELAY);
	}
}

void dm1701_lcdbus_unlock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreGiveRecursive(lcdbus);
	}
}

// docs/spec/hardware-dm1701.md 2.7: levels set before the pins become outputs. The power latch goes high first.
static void reset_levels(void)
{
	stm32_pin_out(PIN_PORT(PIN_POWER_LATCH), PIN_MASK(PIN_POWER_LATCH), true, false);

	stm32_pin_out(PIN_PORT(PIN_C6000_CS), PIN_MASK(PIN_C6000_CS), true, false);
	stm32_pin_out(PIN_PORT(PIN_C6000_PWD), PIN_MASK(PIN_C6000_PWD), true, false);
	stm32_pin_out(PIN_PORT(PIN_C6000_CLK), PIN_MASK(PIN_C6000_CLK), false, false);
	stm32_pin_out(PIN_PORT(PIN_C6000_MOSI), PIN_MASK(PIN_C6000_MOSI), false, false);
	stm32_pin_out(PIN_PORT(PIN_C6000_VCS), PIN_MASK(PIN_C6000_VCS), true, false);

	stm32_pin_out(PIN_PORT(PIN_LED_GREEN), PIN_MASK(PIN_LED_GREEN), false, false);
	stm32_pin_out(PIN_PORT(PIN_LED_RED), PIN_MASK(PIN_LED_RED), false, false);

	stm32_pin_out(PIN_PORT(PIN_RX_UHF_5V), PIN_MASK(PIN_RX_UHF_5V), false, false);
	stm32_pin_out(PIN_PORT(PIN_RX_VHF_5V), PIN_MASK(PIN_RX_VHF_5V), false, false);
	stm32_pin_out(PIN_PORT(PIN_KEY_ROW0), PIN_MASK(PIN_KEY_ROW0), false, false);
	stm32_pin_out(PIN_PORT(PIN_KEY_ROW1), PIN_MASK(PIN_KEY_ROW1), false, false);
	stm32_pin_out(PIN_PORT(PIN_KEY_ROW2), PIN_MASK(PIN_KEY_ROW2), false, false);
	stm32_pin_out(PIN_PORT(PIN_MIC_POWER), PIN_MASK(PIN_MIC_POWER), false, false);
	stm32_pin_out(PIN_PORT(PIN_GPS_POWER), PIN_MASK(PIN_GPS_POWER), false, false);

	stm32_pin_out(PIN_PORT(PIN_PA_EN1), PIN_MASK(PIN_PA_EN1), false, false);
	stm32_pin_out(PIN_PORT(PIN_PA_EN2), PIN_MASK(PIN_PA_EN2), false, false);
	stm32_pin_out(PIN_PORT(PIN_PA_BAND), PIN_MASK(PIN_PA_BAND), false, false);
	stm32_pin_out(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), false, false);

	stm32_pin_out(PIN_PORT(PIN_SPK_MUTE), PIN_MASK(PIN_SPK_MUTE), true, false);
	stm32_pin_out(PIN_PORT(PIN_AMP_EN), PIN_MASK(PIN_AMP_EN), true, false);
	stm32_pin_out(PIN_PORT(PIN_LCD_BACKLIGHT), PIN_MASK(PIN_LCD_BACKLIGHT), false, false);
	stm32_pin_out(PIN_PORT(PIN_RX_AUDIO_MUX), PIN_MASK(PIN_RX_AUDIO_MUX), true, false);
	stm32_pin_out(PIN_PORT(PIN_FLASH_CS), PIN_MASK(PIN_FLASH_CS), true, false);
	stm32_pin_out(PIN_PORT(PIN_CTCSS_PWM), PIN_MASK(PIN_CTCSS_PWM), true, true);
}

void board_early_init(void)
{
	reset_levels();		// latch the power before anything slow
	stm32_clock_init(&clock);
}

void board_init(void)
{
	lcdbus = xSemaphoreCreateRecursiveMutex();
	dm1701_power.init();
	dm1701_flash.init();
	dm1701_cal_load();
}
