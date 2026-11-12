// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV power hold, power key and supply voltage (docs/spec/hardware-d578uv.md 3.3).
//
// A mobile on a fixed 13.8 V supply: no battery (CAP_NO_BATTERY). PC3 is the power hold, set at boot. Dropping it
// doesn't cut the supply, so "off" is a standby the radio wakes from on a one second hold of the power key. The supply
// is read on PC1 through an 11:1 divider. The backlight is on the RF bus (radio_d578uv.c). The one LED output found is
// an RF chip's GPIO bit: radio_d578uv.c lights it on transmit, and led() passes the core's receive LED on to it. The
// microphone level for VOX comes from RF chip 0 (radio_d578uv.c): no ADC input carries the microphone.
#include "d578uv.h"
#include "pins.h"
#include "mcu_io.h"
#include "config.h"
#include "hal_time.h"
#include "radio/power_key.h"
#if CONFIG_WATCHDOG
#include "app/watchdog.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

#define STANDBY_POLL_US		10000u
#define STANDBY_WAKE_POLLS	100u	// the power key held this many polls (1 s) wakes the radio from standby
#define SUPPLY_SAMPLES		4u
// mV per count: 3.3 V reference, 12 bits, 11:1 divider (363 / 4096 in 0.1 V)
#define SUPPLY_MV_NUM		36300u
#define SUPPLY_MV_DEN		4096u

static pwrkey_t power_key;

static void init(void)
{
	mcu_pin_out(PIN_POWER_LATCH, true, false);
	mcu_pin_in(PIN_POWER_KEY, MCU_PULL_UP);
	mcu_pin_analog(PIN_SUPPLY_SENSE);
	mcu_pin_analog(PIN_VOL_A);
	mcu_pin_analog(PIN_VOL_B);
	mcu_pin_analog(PIN_PA_TEMP);
	mcu_adc_init();
	pwrkey_init(&power_key);
}

// Dropping PC3 doesn't cut a mobile's supply (measured on a radio): "off" is a standby instead. The transmitter off,
// the switched rail (backlight, RF chips, audio) down, then nothing but the power key: a one second hold restarts the
// radio. The scheduler stays held, so no task drives the unpowered chips
static void latch(bool on)
{
	if (on) {
		mcu_pin_write(PIN_POWER_LATCH, true);
		return;
	}
	vTaskSuspendAll();
	board_rf_safe();
	d578uv_outputs_off();
	mcu_pin_write(PIN_RAIL, false);
	bool released = false;
	uint32_t polls = 0;
	for (;;) {
#if CONFIG_WATCHDOG
		watchdog_kick(hal_millis());	// the idle task never runs here: the hang check counts from this feed
#endif
		hal_delay_us(STANDBY_POLL_US);
		if (mcu_pin_read(PIN_POWER_KEY)) {
			released = true;
			polls = 0;
		} else if (released && ++polls >= STANDBY_WAKE_POLLS) {
			NVIC_SystemReset();
		}
	}
}

// A volume pot's reading, 0..100 over the pot's range 0x96..0xF60
uint8_t d578uv_pot_percent(uint32_t v)
{
	if (v <= 0x96u) {
		return 0;
	}
	if (v >= 0xF60u) {
		return 100;
	}
	return (uint8_t)((v - 0x96u) * 100u / (0xF60u - 0x96u));
}

// The A channel's pot: the core's volume. Side B's pot sets side B's level in the board (radio_d578uv.c); the core
// reads it only for the volume bar
static int8_t volume(void)
{
	return (int8_t)d578uv_pot_percent(mcu_adc_read(ADC_VOL_A));
}

static int8_t volume_b(void)
{
	return (int8_t)d578uv_pot_percent(mcu_adc_read(ADC_VOL_B));
}

static bool switch_on(void)
{
	return pwrkey_on(&power_key, !mcu_pin_read(PIN_POWER_KEY));
}

// The core's LEDs: the face's status LED and the serial microphone's lights
static void led(uint8_t which, bool on)
{
	d578uv_status_led(which, on);
	d578uv_mic_led(which, on);
}

static uint16_t supply_mv(void)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < SUPPLY_SAMPLES; i++) {
		sum += mcu_adc_read(ADC_SUPPLY_SENSE);
	}
	return (uint16_t)(sum * SUPPLY_MV_NUM / (SUPPLY_MV_DEN * SUPPLY_SAMPLES));
}

// The PA's temperature sensor in mV: four readings averaged (3.3 V reference, 12 bits).
// Higher is hotter; its curve to degrees is not known yet (hardware-d578uv.md 5)
static uint16_t pa_temp_mv(void)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < 4u; i++) {
		sum += mcu_adc_read(ADC_PA_TEMP);
	}
	return (uint16_t)(sum * 3300u / (4096u * 4u));
}

static const hal_temp_sensor_t temp_sensors[] = {
	{ .kind = TEMP_KIND_PA, .read_mv = pa_temp_mv },
};

const hal_temp_t d578uv_temp = {
	.sensors = temp_sensors,
	.count = sizeof(temp_sensors) / sizeof(temp_sensors[0]),
	.fan_on = d578uv_fan_on,
};

const hal_power_t d578uv_power = {
	.init = init,
	.latch = latch,
	.volume = volume,
	.volume_b = volume_b,
	.switch_on = switch_on,
	.led = led,
	.supply_mv = supply_mv,
#if CONFIG_VOX
	.mic_level = d578uv_mic_level,
#endif
};
