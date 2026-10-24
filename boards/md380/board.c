// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TYT MD-380 / MD-390 / Retevis RT3: board descriptor, clocks, reset-time pin levels, key map
// (docs/spec/hardware-md380.md)
#include "board.h"
#include "config.h"
#include "md380.h"
#include "hrc/dmr_glue.h"
#include "gps_uart/gps_uart.h"
#include "hrc/vocoder.h"
#include "pins.h"
#include "FreeRTOS.h"
#include "semphr.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#include "usb/usb_ncm.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

// The RTOS heap, and with it every task stack, in the CCM: zero wait states and off the bus the audio and ADC DMA
// use. Nothing on a stack or from pvPortMalloc may be a DMA buffer (DMA can't reach the CCM): the DMA buffers are
// statics in main RAM, checked where each stream is set up (md380.h DMA_RAM_OK)
uint8_t ucHeap[configTOTAL_HEAP_SIZE] RFOUNDRY_CPU_RAM_NOINIT __attribute__((aligned(8)));

// Zero the CCM's .ccmbss (RFOUNDRY_CPU_RAM) like .bss: run from __libc_init_array before main and before any interrupt
extern uint32_t _sccmbss[], _eccmbss[];
static void ccm_zero(void)
{
	for (uint32_t *p = _sccmbss; p < _eccmbss; p++)
		*p = 0;
}
__attribute__((section(".preinit_array"), used)) static void (*const ccm_zero_entry)(void) = ccm_zero;

// One band per radio (hardware-md380.md 5.1), chosen at build time (board.mk BAND)
#if RFOUNDRY_BAND_VHF
static const band_t bands[] = {
	{ 136000000, 174000000, true },
};
#define BOARD_NAME	"MD-380 VHF"
#else
static const band_t bands[] = {
	{ 400000000, 480000000, true },
};
#define BOARD_NAME	"MD-380"
#endif

// The stock radio has two power levels, about 1 W and 5 W (hardware-md380.md 5.6)
static const char *const power_names[] = { "1W", "5W" };

// docs/ux/information-architecture.md 7.1 on a radio without function or top keys (a few-key map): the
// zone and power are reached from the menus. Matrix positions are unconfirmed (hardware-md380.md 9.1)
static const keymap_entry_t keymap[] = {
	{ KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_RED, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
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
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): the side keys top to bottom
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2 };
static const char *const side_key_names[] = { "SK1", "SK2" };

#if CONFIG_CPS_OPENFW
// open-firmware-cps.md 4: type 7 is the MD-380
static uint32_t cps_radio_type(void)
{
	return 7;
}

static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
	.rom_base = 0x08000000u,
	.rom_size = 1024u * 1024u,
	.security_read = stm32_spi_flash_cps_security_read,
};
#endif

const board_t board = {
	.name = BOARD_NAME,
	// CAP_GPS: the G models; gps_present says whether this one has the module (hardware-md380.md 13). No
	// CAP_VOLUME_POT: the knob sets the volume in the analog path, the MCU only sees its position. CAP_ENCODER: the
	// channel knob is a 16-position switch, reported to the core as steps (input_md380.c)
	.caps = CAP_KEYPAD | CAP_ENCODER | (CONFIG_GPS ? CAP_GPS : 0) | (CONFIG_DMR ? CAP_DMR : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,
	.display = &md380_display,
	.input = &md380_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_names = side_key_names,
	.side_key_count = sizeof(side_keys),
	.radio = &md380_radio,
	.power = &md380_power,
	.flash = &stm32_spi_flash,
#if CONFIG_DIAG
	.diag_line = md380_diag_line,
	.diag_reset = md380_diag_reset,
#endif
#if CONFIG_DMR
	.dmr = &stm32_hrc_dmr,
	.audio = &md380_audio,
	.vocoder = &stm32_vocoder,
#endif
#if CONFIG_USB
	.host_port = &usb_cdc_stream,
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// PPP on the USB serial port, the network adapter under it (board_init)
#elif CONFIG_USB_NETWORK
	.netif = &usb_ncm_netif,
#endif
#endif
	.system = &stm32_system,
	// 2S Li-ion as the other STM32 radios (hardware-md380.md 10.1): low 6.2 V, critical 6.0 V (our choice)
	.battery_low_mv = 6200,
	.battery_critical_mv = 6000,
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
#if CONFIG_GPS
	.gps_present = stm32_gps_present,
#endif
};

// hardware-md380.md 1.2: 8 MHz HSE, 72 MHz system clock, APB1 36 MHz, APB2 72 MHz, USB 48 MHz from PLL Q
static const stm32_clock_t clock_crystal = {
	.hse_hz = 8000000,
	.hse_bypass = false,
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

void md380_lcdbus_lock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreTakeRecursive(lcdbus, portMAX_DELAY);
	}
}

void md380_lcdbus_unlock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreGiveRecursive(lcdbus);
	}
}

// Levels set before the pins become outputs (hardware-md380.md 2.6). The power latch goes high first; the transmitter
// supply, PA loop and synthesizer stay off; the GPS power pin is left alone (it may be a flash chip select)
static void reset_levels(void)
{
	stm32_pin_out(PIN_PORT(PIN_POWER_LATCH), PIN_MASK(PIN_POWER_LATCH), true, false);

	stm32_pin_out(PIN_PORT(PIN_C5000_CS), PIN_MASK(PIN_C5000_CS), true, false);
	stm32_pin_out(PIN_PORT(PIN_C5000_PWD), PIN_MASK(PIN_C5000_PWD), true, false);
	stm32_pin_out(PIN_PORT(PIN_C5000_CLK), PIN_MASK(PIN_C5000_CLK), false, false);
	stm32_pin_out(PIN_PORT(PIN_PLL_CLK), PIN_MASK(PIN_PLL_CLK), true, false);
	stm32_pin_out(PIN_PORT(PIN_C5000_MOSI), PIN_MASK(PIN_C5000_MOSI), false, false);
	stm32_pin_out(PIN_PORT(PIN_C5000_VCS), PIN_MASK(PIN_C5000_VCS), true, false);
	stm32_pin_out(PIN_PORT(PIN_PLL_CS), PIN_MASK(PIN_PLL_CS), true, false);

	stm32_pin_out(PIN_PORT(PIN_LED_GREEN), PIN_MASK(PIN_LED_GREEN), false, false);
	stm32_pin_out(PIN_PORT(PIN_LED_RED), PIN_MASK(PIN_LED_RED), false, false);

	// SAVE enables the 5C regulator (U404), which powers the synthesizer's 3.3 V (U602) and part of the receive chain;
	// the stock firmware drives it high too (hardware-md380.md 5.7)
	stm32_pin_out(PIN_PORT(PIN_SAVE), PIN_MASK(PIN_SAVE), true, false);
	stm32_pin_out(PIN_PORT(PIN_TX_5V), PIN_MASK(PIN_TX_5V), false, false);
	stm32_pin_out(PIN_PORT(PIN_APC_EN), PIN_MASK(PIN_APC_EN), false, false);
	stm32_pin_out(PIN_PORT(PIN_RX_5V), PIN_MASK(PIN_RX_5V), false, false);
	stm32_pin_out(PIN_PORT(PIN_VCO_SEL), PIN_MASK(PIN_VCO_SEL), true, false);	// receive VCO
	stm32_pin_out(PIN_PORT(PIN_DMR_SW), PIN_MASK(PIN_DMR_SW), true, false);
	stm32_pin_out(PIN_PORT(PIN_FM_SW), PIN_MASK(PIN_FM_SW), false, false);
	stm32_pin_out(PIN_PORT(PIN_KEY_ROW0), PIN_MASK(PIN_KEY_ROW0), false, false);
	stm32_pin_out(PIN_PORT(PIN_KEY_ROW1), PIN_MASK(PIN_KEY_ROW1), false, false);
	stm32_pin_out(PIN_PORT(PIN_KEY_ROW2), PIN_MASK(PIN_KEY_ROW2), false, false);

	stm32_pin_out(PIN_PORT(PIN_SPK_MUTE), PIN_MASK(PIN_SPK_MUTE), true, false);
	stm32_pin_out(PIN_PORT(PIN_AMP_EN), PIN_MASK(PIN_AMP_EN), false, false);
	stm32_pin_out(PIN_PORT(PIN_FM_MUTE), PIN_MASK(PIN_FM_MUTE), false, false);
	stm32_pin_out(PIN_PORT(PIN_LCD_BACKLIGHT), PIN_MASK(PIN_LCD_BACKLIGHT), false, false);
	stm32_pin_out(PIN_PORT(PIN_FLASH_CS), PIN_MASK(PIN_FLASH_CS), true, false);
	stm32_pin_out(PIN_PORT(PIN_CTCSS_PWM), PIN_MASK(PIN_CTCSS_PWM), false, false);
	stm32_pin_out(PIN_PORT(PIN_BEEP_PWM), PIN_MASK(PIN_BEEP_PWM), false, false);
	// PA13 (W/N_SW) and PA14 (mic power) are the SWD pins: radio_md380.c takes them over later, unless built with
	// MD380_KEEP_SWD for debugging (hardware-md380.md 2.6)
}

// Before the ROM bootloader entry (stm32_sysboot_check): the same safe levels as at power-up
void board_sysboot_prepare(void)
{
	reset_levels();
}

// Transmitter off from any context (board.h, docs/spec/crash-log.md 8; hardware-md380.md 5.6): the PA power loop
// (APC_EN) and the transmitter supply (5TC) off, the receive VCO selected, and the APC DAC to 0 when its clock is on.
// Register writes only. The synthesizer and the HR_C5000 are left as they are (a bus transaction each); with 5TC off
// their output reaches no powered PA. The backlight and the rest stay, so the stub's "PC Loader Connected" banner stays
// readable
void board_rf_safe(void)
{
	stm32_pin_force(PIN_PORT(PIN_APC_EN), PIN_MASK(PIN_APC_EN), false);
	stm32_pin_force(PIN_PORT(PIN_TX_5V), PIN_MASK(PIN_TX_5V), false);
	stm32_pin_force(PIN_PORT(PIN_VCO_SEL), PIN_MASK(PIN_VCO_SEL), true);
	if (RCC->APB1ENR & RCC_APB1ENR_DACEN) {
		DAC1->DHR12R1 = 0;
	}
}

void board_early_init(void)
{
	reset_levels();		// latch the power before anything slow
	if (!stm32_clock_init(&clock_crystal)) {
		// A crystal is what the public notes describe (hardware-md380.md 1.2); an external clock fed in would need
		// bypass mode, as on the other STM32 radios. Try that before staying on HSI (no USB there)
		stm32_clock_t bypass = clock_crystal;
		bypass.hse_bypass = true;
		(void)stm32_clock_init(&bypass);
	}
}

void board_init(void)
{
	lcdbus = xSemaphoreCreateRecursiveMutex();
	md380_power.init();
	md380_flash_init();
	md380_cal_load();
#if CONFIG_NET_PPP && CONFIG_USB_NETWORK
	hostlink_ppp_set_usb(&usb_ncm_netif);
#endif
#if CONFIG_GPS
	md380_gps_init();
#endif
}
