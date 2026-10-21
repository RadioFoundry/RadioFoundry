// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 power: the power key, "off" as standby, the supply reading and the ADC1 scan (docs/spec/hardware-md9600.md
// 10).
//
// A mobile on a fixed 13.8 V supply: no battery (CAP_NO_BATTERY), and the MCU runs whenever DC is applied; Power_Control
// (PD15) only switches the RF and audio sections. So "off" is a standby: the radio saves, writes a flag to an RTC
// backup register and resets; at start-up the flag keeps it waiting, everything off, until the power key is pressed and
// released. The power key is a matrix position (K0 x K6): the key scan reports it here.
#include "md9600.h"
#include "pins.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

#define STANDBY_MAGIC		0x5B1E9600u	// in RTC_BKP18R: switched off, wait for the power key
#define POWER_OFF_POLLS		20u		// switch_on runs every 100 ms: held 2 s switches off
#define SCANS			8u

// Supply divider (hardware-md9600.md 10.3, unverified): tenths of a volt = raw / 21.3
#define SUPPLY_COUNTS_X10	213u

// Rank order of the MD9600_ADC_ enum
static const uint8_t channels[MD9600_ADC_COUNT] = {
	ADC_CH_NOISE_U, ADC_CH_NOISE_V, ADC_CH_RSSI_U, ADC_CH_RSSI_V, ADC_CH_MIC, ADC_CH_SUPPLY, ADC_CH_MICKEY_ROW,
	ADC_CH_MICKEY_COL,
};
static volatile uint16_t samples[SCANS * MD9600_ADC_COUNT];
static bool adc_ok;
static volatile bool key_down;
static bool key_released;
static uint16_t held;

uint16_t md9600_adc_raw(uint8_t ch)
{
	return adc_ok ? stm32_adc1_scan_avg(ch) : 0;
}

uint16_t md9600_supply_mv(void)
{
	return (uint16_t)((uint32_t)md9600_adc_raw(MD9600_ADC_SUPPLY) * 1000u / SUPPLY_COUNTS_X10);
}

void md9600_power_key(bool down)
{
	key_down = down;
}

static void backup_write(uint32_t v)
{
	RCC->APB1ENR |= RCC_APB1ENR_PWREN;
	(void)RCC->APB1ENR;
	PWR->CR |= PWR_CR_DBP;
	RTC->BKP18R = v;
	(void)RTC->BKP18R;
}

// The power key alone, without the scheduler: row K0 low, column K6 read with its pull-up
static bool power_key_now(void)
{
	stm32_pin_in(PIN_PORT(PIN_KEY_K6), PIN_MASK(PIN_KEY_K6), GPIO_PULLUP);
	PIN_OUT(PIN_KEY_K0, false);
	hal_delay_us(20);
	bool down = !PIN_READ(PIN_KEY_K6);
	PIN_WRITE(PIN_KEY_K0, true);
	return down;
}

// A debounced level: the same reading for 50 ms
static void wait_key(bool down)
{
	uint32_t same = 0;
	while (same < 50u) {
		same = power_key_now() == down ? same + 1u : 0u;
		hal_delay_us(1000);
	}
}

void md9600_standby_check(void)
{
	RCC->APB1ENR |= RCC_APB1ENR_PWREN;
	(void)RCC->APB1ENR;
	if (RTC->BKP18R != STANDBY_MAGIC) {
		return;
	}
	// Everything is off (board.c reset levels); the backlight and the display stay dark
	wait_key(true);
	wait_key(false);
	backup_write(0);
}

static void init(void)
{
	stm32_pin_analog(GPIOA, PIN_MASK(PIN_RSSI_V) | PIN_MASK(PIN_SUPPLY) | PIN_MASK(PIN_MIC_LEVEL) |
		PIN_MASK(PIN_MICKEY_ROW) | PIN_MASK(PIN_MICKEY_COL));
	stm32_pin_analog(GPIOB, PIN_MASK(PIN_RSSI_U) | PIN_MASK(PIN_NOISE_U));
	stm32_pin_analog(PIN_PORT(PIN_NOISE_V), PIN_MASK(PIN_NOISE_V));
	DMA_RAM_OK(samples);
	adc_ok = stm32_adc1_scan_start(channels, MD9600_ADC_COUNT, samples, SCANS);
	key_released = false;
	held = 0;
}

// Power_Control is the radio code's (it powers the receivers); the MCU itself can't switch off. latch(false) is the
// core's last step after saving: everything off, the standby flag, and a reset into the wait above
static void latch(bool on)
{
	if (on) {
		return;
	}
	PIN_WRITE(PIN_APC_EN, false);
	PIN_WRITE(PIN_T5_V, false);
	PIN_WRITE(PIN_T5_U, false);
	PIN_WRITE(PIN_POWER, false);
	backup_write(STANDBY_MAGIC);
	__disable_irq();
	NVIC_SystemReset();
}

static int8_t volume(void)
{
	return -1;	// the knob is analog only
}

// The key that switched the radio on is still down at first: it counts once it has been seen released
static bool switch_on(void)
{
	if (!key_down) {
		key_released = true;
		held = 0;
	} else if (key_released && held < POWER_OFF_POLLS) {
		held++;
	}
	return held < POWER_OFF_POLLS;
}

// No LED lines are known (hardware-md9600.md 8)
static void led(uint8_t which, bool on)
{
	(void)which;
	(void)on;
}

const hal_power_t md9600_power = {
	.init = init,
	.latch = latch,
	.volume = volume,
	.switch_on = switch_on,
	.led = led,
};
