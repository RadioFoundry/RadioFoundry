// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// TYT MD-9600 / Retevis RT90: board descriptor, clocks, reset-time pin levels, key map, the SPI2 lock
// (docs/spec/hardware-md9600.md)
#include "board.h"
#include "config.h"
#include "md9600.h"
#include "pins.h"
#include "FreeRTOS.h"
#include "semphr.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#endif

// The RTOS heap, and with it every task stack, in the CCM (as the DM-1701 and MD-380): nothing on a stack or from
// pvPortMalloc may be a DMA buffer
uint8_t ucHeap[configTOTAL_HEAP_SIZE] ODR_CPU_RAM_NOINIT __attribute__((aligned(8)));

// Zero the CCM's .ccmbss (ODR_CPU_RAM) like .bss: run from __libc_init_array before main and before any interrupt
extern uint32_t _sccmbss[], _eccmbss[];
static void ccm_zero(void)
{
	for (uint32_t *p = _sccmbss; p < _eccmbss; p++)
		*p = 0;
}
__attribute__((section(".preinit_array"), used)) static void (*const ccm_zero_entry)(void) = ccm_zero;

// Both bands on every revision (hardware-md9600.md 5.1, unverified). Transmit stays refused in radio_md9600.c until
// the revision is confirmed and the PA calibration is read
static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

// The radio's four power levels (hardware-md9600.md 5.6: labels, not measured)
static const char *const power_names[] = { "5W", "10W", "25W", "40W" };

// docs/ux/information-architecture.md 7.4 on the front panel (hardware-md9600.md 9.1): Enter and Back, P1 / P2 as
// QUICK / CONTEXT, Up / Down, P3 monitor, P4 talk target, the orange key a user key. The encoder sends ENC_STEP. The
// power key isn't in the map: power_md9600.c watches it. The hand-mic keypad is not read yet
static const keymap_entry_t keymap[] = {
	{ KEY_ENTER, ACT_SELECT, ACT_SELECT, false, false },
	{ KEY_BACK, ACT_BACK, ACT_BACK, false, false },
	{ KEY_P1, ACT_QUICK, ACT_QUICK, false, false },
	{ KEY_P2, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ KEY_P3, ACT_MONITOR, ACT_MONITOR, true, false },
	{ KEY_P4, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ KEY_ALARM, ACT_USER_3, ACT_USER_3, false, false },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): P3, P4, the orange key
static const uint8_t side_keys[] = { KEY_P3, KEY_P4, KEY_ALARM };

#if CONFIG_CPS_OPENFW
// open-firmware-cps.md 4: type 5 is the MD-9600
static uint32_t cps_radio_type(void)
{
	return 5;
}

static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
	.rom_base = 0x08000000u,
	.rom_size = 1024u * 1024u,
	.security_read = md9600_cps_security_read,
};
#endif

const board_t board = {
	.name = "MD-9600",
	// A mobile on a fixed supply: no battery. No CAP_VOLUME_POT: the knob sets the volume in the analog path only.
	// No CAP_KEYPAD until the hand-mic keypad is read; no CAP_GPS until the GPS receive path is written
	.caps = CAP_ENCODER | CAP_NO_BATTERY,
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,
	.display = &md9600_display,
	.input = &md9600_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_count = sizeof(side_keys),
	// No * or # on the front panel: the hints name P1 and P2 instead (docs/spec/ui.md 1)
	.quick_label = "P1",
	.context_label = "P2",
	.radio = &md9600_radio,
	.power = &md9600_power,
	.flash = &md9600_flash,
#if CONFIG_DIAG
	.diag_line = md9600_diag_line,
	.diag_reset = md9600_diag_reset,
#endif
#if CONFIG_DEBUGLINK
	.view_line = md9600_view_line,
#endif
#if CONFIG_USB
	.host_port = &usb_cdc_stream,
#endif
	.system = &stm32_system,
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
};

// hardware-md9600.md 1.2: an 8 MHz HSE; SYSCLK 72 MHz from a 2 MHz PLL input (M 4, N 72, P 2), USB 48 MHz (Q 3),
// APB1 36 MHz, APB2 72 MHz: the DM-1701's plan
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

static SemaphoreHandle_t spi2_lock;

void md9600_spi2_lock(void)
{
	if (spi2_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreTakeRecursive(spi2_lock, portMAX_DELAY);
	}
}

void md9600_spi2_unlock(void)
{
	if (spi2_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreGiveRecursive(spi2_lock);
	}
}

// SPI2 for the flash and the LCD: mode 3, PCLK1 36 MHz / 2 = 18 MHz (hardware-md9600.md 12.1). Both chip selects high
// first
void md9600_spi2_init(void)
{
	static bool done;
	if (done) {
		return;
	}
	done = true;
	PIN_OUT(PIN_FLASH_CS, true);
	PIN_OUT(PIN_LCD_CS, true);
	stm32_pin_af(GPIOB, GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15, GPIO_AF5_SPI2, false, GPIO_NOPULL);
	stm32_spi_master_init(SPI2, true, true, 0);
}

// Levels before anything else runs (hardware-md9600.md 2.6): every transmitter supply, the PA loop and the receivers
// off, the HR_C6000 powered down, chip selects high, the amplifier muted. Power_Control (the RF and audio sections)
// stays off until the radio code powers the receivers. PE15 (the AK2365A reset on V1-V4, unknown on V5) is left an
// input until the revision is known; PA13 / PA14 (SWD) are taken by radio_md9600.c
static void reset_levels(void)
{
	PIN_OUT(PIN_APC_EN, false);
	PIN_OUT(PIN_T5_V, false);
	PIN_OUT(PIN_T5_U, false);
	PIN_OUT(PIN_POWER, false);
	PIN_OUT(PIN_C5_U, false);
	PIN_OUT(PIN_C5_V, false);
	PIN_OUT(PIN_VCO_U, true);	// receive VCOs
	PIN_OUT(PIN_VCO_V, true);
	PIN_OUT(PIN_MOD_SW, false);

	PIN_OUT(PIN_C6000_PWD, true);
	PIN_OUT(PIN_C6000_CS, true);
	PIN_OUT(PIN_C6000_CLK, false);
	PIN_OUT(PIN_C6000_MOSI, false);
	PIN_OUT(PIN_C6000_VCS, true);

	PIN_OUT(PIN_PLL_CS_V, true);
	PIN_OUT(PIN_PLL_CS_U, true);
	PIN_OUT(PIN_PLL_DATA, false);
	PIN_OUT(PIN_PLL_CLK, false);
	PIN_OUT(PIN_IF_CS_V, true);
	PIN_OUT(PIN_IF_CS_U, true);
	PIN_OUT(PIN_IF_DATA, false);
	PIN_OUT(PIN_IF_CLK, false);

	PIN_OUT(PIN_AF_MUTE, true);
	PIN_OUT(PIN_FM_MUTE, false);
	PIN_OUT(PIN_FM_SW, false);
	PIN_OUT(PIN_AF_BAND, false);
	PIN_OUT(PIN_TONE_PWM, false);
	PIN_OUT(PIN_BEEP_PWM, false);
	PIN_OUT(PIN_BACKLIGHT, false);
	PIN_OUT(PIN_MIC_GPS_POWER, true);	// hand mic and GPS powered while the radio runs

	PIN_OUT(PIN_FLASH_CS, true);
	PIN_OUT(PIN_LCD_CS, true);
	PIN_OUT(PIN_LCD_A0, false);
	PIN_OUT(PIN_LCD_RST, false);
}

// Before the ROM bootloader entry (stm32_sysboot_check): the same safe levels as at power-up
void board_sysboot_prepare(void)
{
	reset_levels();
}

// Before the flash stub takes over: only the transmitter off
void board_stub_prepare(void)
{
	PIN_WRITE(PIN_APC_EN, false);
	PIN_WRITE(PIN_T5_V, false);
	PIN_WRITE(PIN_T5_U, false);
}

void board_early_init(void)
{
	reset_levels();
	if (!stm32_clock_init(&clock_crystal)) {
		// A crystal is the expected part (hardware-md9600.md 1.2); an oscillator fed in needs bypass mode. Try that
		// before staying on HSI (no USB there)
		stm32_clock_t bypass = clock_crystal;
		bypass.hse_bypass = true;
		(void)stm32_clock_init(&bypass);
	}
	md9600_standby_check();		// switched off: wait here for the power key
}

void board_init(void)
{
	spi2_lock = xSemaphoreCreateRecursiveMutex();
	md9600_power.init();
	md9600_flash.init();
	md9600_cal_load();
	md9600_radio_early();
}
