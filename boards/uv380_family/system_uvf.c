// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family start-up: the RTOS heap in the CCM, clocks, reset-time pin levels, the LCD bus lock, the transmitter's
// emergency off. The radio's descriptor, key map and differences are in its board.c (uvf.h)
#include "board.h"
#include "config.h"
#include "uvf.h"
#include "pins.h"
#include "FreeRTOS.h"
#include "semphr.h"
#if CONFIG_NET_PPP && CONFIG_USB_NETWORK
#include "usb/usb_ncm.h"
#include "hostlink/hostlink_ppp.h"
#endif

// The RTOS heap, and with it every task stack, in the CCM: zero wait states and no contention with the audio,
// ADC and backlight DMA on the main SRAM. Nothing on a stack or from pvPortMalloc may be a DMA buffer (DMA can't
// reach the CCM): the DMA buffers are statics in main RAM, checked where each stream is set up (uvf.h DMA_RAM_OK)
uint8_t ucHeap[configTOTAL_HEAP_SIZE] RFOUNDRY_CPU_RAM_NOINIT __attribute__((aligned(8)));

// Zero the CCM's .ccmbss (RFOUNDRY_CPU_RAM) like .bss: run from __libc_init_array before main and before any interrupt
// (the CCM clock is on from reset)
extern uint32_t _sccmbss[], _eccmbss[];
static void ccm_zero(void)
{
	for (uint32_t *p = _sccmbss; p < _eccmbss; p++)
		*p = 0;
}
__attribute__((section(".preinit_array"), used)) static void (*const ccm_zero_entry)(void) = ccm_zero;

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

void uvf_lcdbus_lock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreTakeRecursive(lcdbus, portMAX_DELAY);
	}
}

void uvf_lcdbus_unlock(void)
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

// Before the ROM bootloader entry (stm32_sysboot_check): the same safe levels as at power-up; the power latch stays high
void board_sysboot_prepare(void)
{
	reset_levels();
}

// Transmitter off from any context (board.h, docs/spec/crash-log.md 8; hardware-dm1701.md 6, 7): the TX supply
// (PA_EN1), the APC loop (PA_EN2), the band select low as at reset, and the APC DAC to 0 when its clock is on (it holds
// its value otherwise). Register writes only. The AT1846S and the HR_C6000 are left as they are: they need a bus
// transaction, and with the TX supply off their output reaches no powered PA. The DMR bursts are keyed from the HR_C6000
// RF interrupt, which can't run with interrupts off. The backlight and the rest stay, so the stub's "PC Loader
// Connected" banner stays readable
void board_rf_safe(void)
{
	stm32_pin_force(PIN_PORT(PIN_PA_EN2), PIN_MASK(PIN_PA_EN2), false);
	stm32_pin_force(PIN_PORT(PIN_PA_EN1), PIN_MASK(PIN_PA_EN1), false);
	stm32_pin_force(PIN_PORT(PIN_PA_BAND), PIN_MASK(PIN_PA_BAND), false);
	if (RCC->APB1ENR & RCC_APB1ENR_DACEN) {
		DAC1->DHR12R1 = 0;
	}
}

static uint8_t hse_mode;

// The DM-1701's HSE is an external clock (hardware-dm1701.md 1.2); whether the MD-UV380's is the same or a crystal is
// open (hardware-mduv380.md 2): the other mode is tried when the first never starts, and the diagnostics page says which
// ran. Neither: the clock stays on the HSI (no USB)
void board_early_init(void)
{
	reset_levels();		// latch the power before anything slow
	if (stm32_clock_init(&clock)) {
		hse_mode = clock.hse_bypass ? UVF_HSE_BYPASS : UVF_HSE_CRYSTAL;
		return;
	}
	stm32_clock_t other = clock;
	other.hse_bypass = !clock.hse_bypass;
	hse_mode = stm32_clock_init(&other) ? (other.hse_bypass ? UVF_HSE_BYPASS : UVF_HSE_CRYSTAL) : UVF_HSE_NONE;
}

uint8_t uvf_hse_mode(void)
{
	return hse_mode;
}

void board_init(void)
{
	lcdbus = xSemaphoreCreateRecursiveMutex();
	uvf_power.init();
	uvf_flash_init();
	uvf_cal_load();
	if (uvf_variant.init != NULL) {
		uvf_variant.init();
	}
#if CONFIG_NET_PPP && CONFIG_USB_NETWORK
	hostlink_ppp_set_usb(&usb_ncm_netif);
#endif
}
