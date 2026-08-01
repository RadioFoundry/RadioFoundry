// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 radio: AT1846S (I2C3) + HR_C6000 (bit-banged config SPI) + RF path GPIOs + APC DAC + calibration.
// FM receive and transmit (docs/spec/hardware-dm1701.md 4-8, 12; docs/spec/modem-hrc6000.md 3, 8).
//
// TX switching sequence (gotchas 3.5): the TX frequency is written with the receiver off (reg 0x30 idle, then the
// synthesizer words) and the chip goes straight from idle to FM TX, never through RX on. With this sequence the old
// on-air measurement found no offset, so DM1701_TX_OFFSET_HZ_* default to 0. Measure the carrier with this exact
// sequence and put the measured correction there; do not copy the +3 kHz of the other sequence.
#include <stddef.h>
#include "dm1701.h"
#include "pins.h"
#include "hal_time.h"
#include "rf/at1846s/at1846s.h"
#include "modem/hrc6000/hrc6000.h"
#include "FreeRTOS.h"
#include "task.h"

// Measured TX carrier correction for the sequence above, per bandwidth (Hz, added to the TX frequency)
#define DM1701_TX_OFFSET_HZ_12K5	0
#define DM1701_TX_OFFSET_HZ_25K		0

// APC DAC value when no calibration is loaded: deliberately very low, never an uncalibrated high drive
#define DM1701_DAC_UNCALIBRATED		300u
#define DAC_MAX				4095u

#define UHF_MIN_HZ		300000000u
#define FM_MIC_GAIN		7	// 0..31, as the init value 0xE4 = 0x27
#define DMR_MIC_GAIN		7
#define I2C_TRIES		3
#define I2C_TIMEOUT_MS		5
#define C6000_PWD_WAIT_MS	12	// at least 10 ms after PWD low before the first SPI access

// DM-1701 power interpolation fractions (hardware-dm1701.md 7), per mille: f0 f1 f2 f3
static const uint16_t frac_vhf[4] = { 450, 750, 250, 530 };
static const uint16_t frac_uhf[4] = { 460, 730, 140, 360 };

static struct {
	at1846s_t rf;
	hrc6000_t modem;
	radio_mode_t mode;
	bool tx_on;
	bool rx_valid;
	bool rx_uhf;
	uint32_t rx_hz;
	radio_bw_t rx_bw;
	subtone_t rx_tone;
	uint32_t bus_errors;	// I2C failures and failed chip operations since start (debug)
	dm1701_radio_diag_t diag;
} r;

static I2C_HandleTypeDef i2c3;
static DAC_HandleTypeDef dac;

static bool in_isr(void)
{
	return __get_IPSR() != 0;
}

static void count(bool ok)
{
	if (!ok) {
		r.bus_errors++;
	}
}

// --- AT1846S bus: I2C3 (hardware-dm1701.md 5.1) ---

static void i2c_setup(void)
{
	__HAL_RCC_I2C3_CLK_ENABLE();
	stm32_pin_af(PIN_PORT(PIN_I2C3_SCL), PIN_MASK(PIN_I2C3_SCL), GPIO_AF4_I2C3, true, GPIO_PULLUP);
	stm32_pin_af(PIN_PORT(PIN_I2C3_SDA), PIN_MASK(PIN_I2C3_SDA), GPIO_AF4_I2C3, true, GPIO_PULLUP);
	i2c3.Instance = I2C3;
	i2c3.Init.ClockSpeed = 400000;
	i2c3.Init.DutyCycle = I2C_DUTYCYCLE_2;
	i2c3.Init.OwnAddress1 = 0;
	i2c3.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	i2c3.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	i2c3.Init.OwnAddress2 = 0;
	i2c3.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	i2c3.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
	HAL_I2C_Init(&i2c3);
}

// One register transfer. Task context: up to 3 tries 1 ms apart, then the peripheral is reset. ISR context: one
// try, and none at all while a task transfer owns the bus (skip, never wait)
static bool i2c_xfer(uint8_t reg, uint8_t *buf, bool write)
{
	bool isr = in_isr();
	int tries = isr ? 1 : I2C_TRIES;
	for (int i = 0; i < tries; i++) {
		if (isr && HAL_I2C_GetState(&i2c3) != HAL_I2C_STATE_READY) {
			return false;
		}
		HAL_StatusTypeDef st = write
			? HAL_I2C_Mem_Write(&i2c3, AT1846S_I2C_ADDR << 1, reg, I2C_MEMADD_SIZE_8BIT, buf, 2, I2C_TIMEOUT_MS)
			: HAL_I2C_Mem_Read(&i2c3, AT1846S_I2C_ADDR << 1, reg, I2C_MEMADD_SIZE_8BIT, buf, 2, I2C_TIMEOUT_MS);
		if (st == HAL_OK) {
			return true;
		}
		if (i + 1 < tries) {
			hal_delay_ms(1);
		}
	}
	r.bus_errors++;
	if (!isr) {
		HAL_I2C_DeInit(&i2c3);
		i2c_setup();
	}
	return false;
}

static bool rf_write(void *ctx, uint8_t reg, uint16_t val)
{
	uint8_t buf[2] = { (uint8_t)(val >> 8), (uint8_t)val };
	return i2c_xfer(reg, buf, true);
}

static bool rf_read(void *ctx, uint8_t reg, uint16_t *val)
{
	uint8_t buf[2];
	if (!i2c_xfer(reg, buf, false)) {
		return false;
	}
	*val = (uint16_t)((buf[0] << 8) | buf[1]);
	return true;
}

static const at1846s_bus_t rf_bus = { .write = rf_write, .read = rf_read, .delay_ms = hal_delay_ms, .ctx = NULL };

// --- HR_C6000 bus: bit-banged config SPI on PE2-PE5 (hardware-dm1701.md 4.2) ---

// About 110 ns at 72 MHz on top of the GPIO writes, keeping the clock near the 1-2 MHz the chip is known to take
static inline void spi_delay(void)
{
	__NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
}

// Clock idles high; MOSI set, clock low, sample MISO, clock high (the chip latches on the rising edge)
static uint8_t spi_byte(uint8_t out)
{
	uint8_t in = 0;
	for (int i = 0; i < 8; i++) {
		PIN_WRITE(PIN_C6000_MOSI, (out & 0x80u) != 0);
		out = (uint8_t)(out << 1);
		PIN_WRITE(PIN_C6000_CLK, false);
		spi_delay();
		in = (uint8_t)((in << 1) | (PIN_READ(PIN_C6000_MISO) ? 1u : 0u));
		PIN_WRITE(PIN_C6000_CLK, true);
		spi_delay();
	}
	return in;
}

// One transaction with interrupts up to the RTOS syscall priority masked (the HR_C6000 interrupts run at that level),
// so a modem ISR never lands inside it. Safe from tasks and from those ISRs; never blocks
static bool modem_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
	PIN_WRITE(PIN_C6000_CLK, true);
	PIN_WRITE(PIN_C6000_CS, false);
	spi_delay();
	for (uint8_t i = 0; i < hdr_len; i++) {
		spi_byte(hdr[i]);
	}
	for (uint16_t i = 0; i < len; i++) {
		uint8_t in = spi_byte(tx != NULL ? tx[i] : 0xFF);
		if (rx != NULL) {
			rx[i] = in;
		}
	}
	spi_delay();
	PIN_WRITE(PIN_C6000_CS, true);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
	return true;
}

static const hrc6000_bus_t modem_bus = {
	.transfer = modem_transfer, .delay_ms = hal_delay_ms, .delay_us = hal_delay_us, .ctx = NULL,
};

// --- APC DAC and RF path GPIOs (hardware-dm1701.md 6, 7) ---

static void dac_setup(void)
{
	__HAL_RCC_DAC_CLK_ENABLE();
	stm32_pin_analog(PIN_PORT(PIN_APC_DAC), PIN_MASK(PIN_APC_DAC));
	dac.Instance = DAC1;
	HAL_DAC_Init(&dac);
	DAC_ChannelConfTypeDef ch = { .DAC_Trigger = DAC_TRIGGER_NONE, .DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE };
	HAL_DAC_ConfigChannel(&dac, &ch, DAC_CHANNEL_1);
	HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 0);
	HAL_DAC_Start(&dac, DAC_CHANNEL_1);
}

static void apc_set(uint32_t value)
{
	HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, value > DAC_MAX ? DAC_MAX : value);
}

static void pa_off(void)
{
	PIN_WRITE(PIN_PA_EN2, false);
	PIN_WRITE(PIN_PA_EN1, false);
	PIN_WRITE(PIN_PA_BAND, false);
	apc_set(0);
}

static void rx_supply(bool on, bool uhf)
{
	PIN_WRITE(PIN_RX_UHF_5V, on && uhf);
	PIN_WRITE(PIN_RX_VHF_5V, on && !uhf);
}

static int32_t lerp(int32_t a, int32_t b, uint16_t per_mille)
{
	return a + (b - a) * (int32_t)per_mille / 1000;
}

// Board power level -> DAC value from the four calibrated curves (hardware-dm1701.md 7, DM-1701 fractions)
static uint32_t power_to_dac(const dm1701_cal_point_t *cal, uint8_t level, bool uhf)
{
	const uint16_t *f = uhf ? frac_uhf : frac_vhf;
	int32_t vl = cal->power_dac[0], lo = cal->power_dac[1], mid = cal->power_dac[2], hi = cal->power_dac[3];
	int32_t v;
	switch (level) {
	case 0: v = vl > 160 ? vl - 160 : 0; break;
	case 1: v = vl; break;
	case 2: v = lerp(vl, lo, f[0]); break;
	case 3: v = lerp(vl, lo, f[1]); break;
	case 4: v = lo; break;
	case 5: v = mid; break;
	case 6: v = lerp(mid, hi, f[2]); break;
	case 7: v = lerp(mid, hi, f[3]); break;
	default: v = hi; break;
	}
	if (v < 0) {
		v = 0;
	}
	return (uint32_t)v > DAC_MAX ? DAC_MAX : (uint32_t)v;
}

// --- Helpers ---

static const band_t *find_band(uint32_t hz)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		if (hz >= board.bands[i].lo_hz && hz <= board.bands[i].hi_hz) {
			return &board.bands[i];
		}
	}
	return NULL;
}

static at1846s_tone_t rf_tone(const subtone_t *t, uint16_t *value)
{
	*value = 0;
	if (t == NULL || t->value == 0) {
		return AT1846S_TONE_NONE;
	}
	*value = t->value;
	switch (t->type) {
	case TONE_CTCSS: return AT1846S_TONE_CTCSS;
	case TONE_DCS: return AT1846S_TONE_DCS;
	case TONE_DCS_INV: return AT1846S_TONE_DCS_INV;
	default: return AT1846S_TONE_NONE;
	}
}

static at1846s_bw_t rf_bw(radio_bw_t bw)
{
	return bw == RADIO_BW_25K ? AT1846S_BW_25K : AT1846S_BW_12K5;
}

static void apply_rx_tone(void)
{
	uint16_t value;
	at1846s_tone_t type = rf_tone(&r.rx_tone, &value);
	count(at1846s_set_rx_tone(&r.rf, type, value));
}

// --- hal_radio_t ---

static void audio(bool on)
{
	if (on) {
		count(hrc6000_fm_audio(&r.modem, true));
		PIN_WRITE(PIN_RX_AUDIO_MUX, true);	// HR_C6000 line out to the amp
		PIN_WRITE(PIN_AMP_EN, true);
		PIN_WRITE(PIN_SPK_MUTE, false);
	} else {
		PIN_WRITE(PIN_SPK_MUTE, true);
		PIN_WRITE(PIN_AMP_EN, false);
		count(hrc6000_fm_audio(&r.modem, false));
	}
}

static void init(void)
{
	stm32_pin_in(PIN_PORT(PIN_C6000_MISO), PIN_MASK(PIN_C6000_MISO), GPIO_PULLUP);
	PIN_WRITE(PIN_C6000_CS, true);
	PIN_WRITE(PIN_C6000_CLK, true);		// clock idles high
	PIN_WRITE(PIN_MIC_POWER, true);		// kept on in RX and TX (4.5: some speaker-mics see false PTT without bias)
	rx_supply(false, false);
	dac_setup();
	pa_off();
	i2c_setup();

	PIN_WRITE(PIN_C6000_PWD, false);	// the HR_C6000 wakes while the AT1846S initializes
	uint32_t pwd_ms = hal_millis();

	at1846s_attach(&r.rf, &rf_bus);
	count(at1846s_init(&r.rf));

	while ((uint32_t)(hal_millis() - pwd_ms) < C6000_PWD_WAIT_MS) {
		hal_delay_ms(1);
	}
	hrc6000_attach(&r.modem, &modem_bus);
	count(hrc6000_power_on_init(&r.modem, DMR_MIC_GAIN));
	count(at1846s_init_finish(&r.rf));

	r.mode = RADIO_MODE_FM;
	count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
	count(hrc6000_set_mode_fm_rx(&r.modem));
	audio(false);
	r.diag.c6000_ok = hrc6000_read(&r.modem, 4, 0x10, &r.diag.c6000_r10);
}

static void set_mode(radio_mode_t mode)
{
	r.mode = mode;
	if (mode == RADIO_MODE_DMR) {
		// AT1846S baseband set-up only; the HR_C6000 DMR entry (modem-hrc6000.md 3.1) comes with the DMR stack
		count(at1846s_set_mode(&r.rf, AT1846S_MODE_DMR));
	} else {
		count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
		if (!r.tx_on) {
			count(hrc6000_set_mode_fm_rx(&r.modem));
		}
	}
	apply_rx_tone();	// the mode table rewrote reg 0x3A
}

// Enter RX (hardware-dm1701.md 6): PA off first, RX supply for the band, AT1846S to RX, then the HR_C6000
static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	if (find_band(hz) == NULL) {
		return false;
	}
	bool uhf = hz >= UHF_MIN_HZ;
	r.rx_hz = hz;
	r.rx_bw = bw;
	r.rx_tone = rx_tone != NULL ? *rx_tone : (subtone_t){ TONE_NONE, 0 };
	r.rx_uhf = uhf;
	r.rx_valid = true;

	pa_off();
	rx_supply(true, uhf);
	count(at1846s_set_bandwidth(&r.rf, rf_bw(bw)));
	count(at1846s_set_mode(&r.rf, r.mode == RADIO_MODE_DMR ? AT1846S_MODE_DMR : AT1846S_MODE_FM));
	count(at1846s_set_freq(&r.rf, hz));
	apply_rx_tone();
	count(at1846s_set_state(&r.rf, AT1846S_STATE_RX));

	dm1701_cal_point_t cal;
	dm1701_cal_at(hz, &cal);	// on false it fills safe defaults
	count(hrc6000_set_ref_offset(&r.modem, cal.ref_offset, false));
	if (r.mode == RADIO_MODE_FM) {
		count(hrc6000_set_mode_fm_rx(&r.modem));
	}
	r.tx_on = false;
	return true;
}

// Enter FM TX (hardware-dm1701.md 6): RX supplies off, HR_C6000 FM TX, AT1846S tuned with the receiver off and
// switched straight to TX, APC drive, then PA enable 1, band select, PA enable 2
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	const band_t *band = find_band(hz);
	if (band == NULL || !band->tx || r.mode != RADIO_MODE_FM) {
		return false;	// DMR transmit comes with the DMR stack
	}
	bool uhf = hz >= UHF_MIN_HZ;
	if (board.power_count > 0 && power >= board.power_count) {
		power = (uint8_t)(board.power_count - 1);
	}

	dm1701_cal_point_t cal;
	bool calibrated = dm1701_cal_at(hz, &cal) && cal.valid;
	uint32_t drive = calibrated ? power_to_dac(&cal, power, uhf) : DM1701_DAC_UNCALIBRATED;

	uint16_t tone_value;
	at1846s_tone_t tone = rf_tone(tx_tone, &tone_value);
	int32_t offset = bw == RADIO_BW_25K ? DM1701_TX_OFFSET_HZ_25K : DM1701_TX_OFFSET_HZ_12K5;

	rx_supply(false, false);

	hrc6000_fm_tx_t fm = {
		.uhf = uhf,
		.wide = bw == RADIO_BW_25K,
		.tone = tone == AT1846S_TONE_NONE ? HRC6000_TONE_NONE
			: tone == AT1846S_TONE_CTCSS ? HRC6000_TONE_CTCSS : HRC6000_TONE_DCS,
		.mod2 = cal.fm_i,
		.mod1 = cal.fm_q,
		.ref_offset = cal.ref_offset,
		.mic_gain = FM_MIC_GAIN,
	};
	count(hrc6000_set_mode_fm_tx(&r.modem, &fm));

	count(at1846s_set_bandwidth(&r.rf, rf_bw(bw)));
	count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
	count(at1846s_set_tx_tone(&r.rf, tone, tone_value));
	count(at1846s_set_freq(&r.rf, (uint32_t)((int32_t)hz + offset)));
	count(at1846s_set_state(&r.rf, AT1846S_STATE_FM_TX));

	apc_set(drive);
	PIN_WRITE(PIN_PA_EN1, true);
	PIN_WRITE(PIN_PA_BAND, uhf);
	PIN_WRITE(PIN_PA_EN2, true);
	r.tx_on = true;
	return true;
}

static void tx_off(void)
{
	pa_off();
	if (r.rx_valid) {
		subtone_t tone = r.rx_tone;
		rx(r.rx_hz, r.rx_bw, &tone);
		return;
	}
	count(at1846s_set_tx_tone(&r.rf, AT1846S_TONE_NONE, 0));
	count(at1846s_set_state(&r.rf, AT1846S_STATE_IDLE));
	count(hrc6000_set_mode_fm_rx(&r.modem));
	r.tx_on = false;
}

static void status(radio_status_t *st)
{
	uint8_t rssi, noise;
	uint16_t flags, value;
	at1846s_tone_t type = rf_tone(&r.rx_tone, &value);
	if (!at1846s_read_signal(&r.rf, &rssi, &noise) || !at1846s_read_flags(&r.rf, &flags)) {
		st->rssi_dbm = -140;
		st->noise = 255;
		st->carrier = false;
		st->tone_ok = type == AT1846S_TONE_NONE;
		return;
	}
	r.diag.rssi = rssi;
	r.diag.noise = noise;
	r.diag.flags = flags;
	// Field calibration constants (hardware-dm1701.md 5.9)
	st->rssi_dbm = r.rx_uhf ? (int16_t)(rssi - 151) : (int16_t)(-164 + rssi * 32 / 27);
	st->noise = noise;
	st->carrier = (flags & AT1846S_FLAG_SQ) != 0;
	st->tone_ok = at1846s_tone_match(flags, type);
}

const hal_radio_t dm1701_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
};

void dm1701_radio_diag(dm1701_radio_diag_t *d)
{
	*d = r.diag;
	d->bus_errors = r.bus_errors;
}
