// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 radio: FM receive on the AT1846S (I2C bus A) with the HR_C6000 (SPI0) as the FM audio chain, as on the DM-1701
// (docs/spec/hardware-gd77.md 5). Bring-up state: receive only. tx() refuses until the factory calibration is read
// and the PA path (TX enable, RF switches, DAC drive) is confirmed on a radio (hardware-gd77.md 5.4, 11); DMR is not
// wired. The RSSI scale is the DM-1701's, unverified here.
#include "gd77.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/at1846s/at1846s.h"
#include "modem/hrc6000/hrc6000.h"

#define AT1846S_ADDR		0x71u	// 7-bit (0xE2 / 0xE3 on the wire)
#define C6K_PWD_HIGH_MS		10u	// the power-down line's high pulse at start-up
#define C6K_PWD_WAIT_MS		10u	// PWD low to the first configuration access (modem-hrc6000.md 1)
#define DMR_MIC_GAIN		0x07u	// the init's default; DMR is not used yet
#define UHF_MIN_HZ		300000000u

static struct {
	at1846s_t rf;
	hrc_t modem;
	radio_mode_t mode;
	bool rx_valid, rx_uhf;
	uint32_t rx_hz;
	radio_bw_t rx_bw;
	subtone_t rx_tone;
	gd77_radio_diag_t diag;
} r;

static void count(bool ok)
{
	if (!ok) {
		r.diag.rf_errors++;
	}
}

// AT1846S registers: an 8-bit register address, then 16 bits high byte first
static bool rf_write(void *ctx, uint8_t reg, uint16_t val)
{
	(void)ctx;
	uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	return gd77_i2c_write(AT1846S_ADDR, b, sizeof b);
}

static bool rf_read(void *ctx, uint8_t reg, uint16_t *val)
{
	(void)ctx;
	uint8_t b[2];
	if (!gd77_i2c_write_read(AT1846S_ADDR, &reg, 1, b, sizeof b)) {
		return false;
	}
	*val = (uint16_t)((b[0] << 8) | b[1]);
	return true;
}

static const at1846s_bus_t rf_bus = { .write = rf_write, .read = rf_read, .delay_ms = hal_delay_ms, .ctx = NULL };

static const hrc_bus_t modem_bus = {
	.transfer = gd77_c6k_transfer,
	.delay_ms = hal_delay_ms,
	.delay_us = hal_delay_us,
	.ctx = NULL,
	.voice = NULL,		// V_SPI on SPI1 comes with DMR
};

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

// The HR_C6000 out of power-down: the control line high for 10 ms, then low; the other control line stays high
// (hardware-gd77.md 5.2: which line is reset and which is power-down is unverified)
static void c6k_power_up(void)
{
	k22f_pin_write(PIN_C6K_E0, true);
	k22f_pin_write(PIN_C6K_PWD, true);
	hal_delay_ms(C6K_PWD_HIGH_MS);
	k22f_pin_write(PIN_C6K_PWD, false);
}

static void init(void)
{
	gd77_i2c_init();
	gd77_c6k_bus_init();
	k22f_pin_write(PIN_TX_EN, false);
	c6k_power_up();
	uint32_t pwd_ms = hal_millis();

	at1846s_attach(&r.rf, &rf_bus);
	r.diag.rf_ok = at1846s_init(&r.rf);
	count(r.diag.rf_ok);
	count(at1846s_read(&r.rf, 0, 0x00, &r.diag.rf_id));

	while ((uint32_t)(hal_millis() - pwd_ms) < C6K_PWD_WAIT_MS) {
		hal_delay_ms(1);
	}
	hrc6000_attach(&r.modem, &modem_bus);
	r.diag.c6k_ok = hrc6000_power_on_init_clock(&r.modem, DMR_MIC_GAIN, &hrc6000_clock_12m288);
	count(r.diag.c6k_ok);
	count(at1846s_init_finish(&r.rf));

	r.mode = RADIO_MODE_FM;
	count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
	count(hrc6000_set_mode_fm_rx(&r.modem));
	count(hrc6000_fm_audio(&r.modem, false));
}

// DMR isn't wired on this board yet (CAP_DMR unset): the radio stays in FM
static void set_mode(radio_mode_t mode)
{
	(void)mode;
	r.mode = RADIO_MODE_FM;
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	if (find_band(hz) == NULL) {
		return false;
	}
	r.rx_hz = hz;
	r.rx_bw = bw;
	r.rx_uhf = hz >= UHF_MIN_HZ;
	r.rx_tone = rx_tone != NULL ? *rx_tone : (subtone_t){ TONE_NONE, 0 };
	r.rx_valid = true;
	k22f_pin_write(PIN_TX_EN, false);

	uint16_t value;
	at1846s_tone_t type = rf_tone(&r.rx_tone, &value);
	count(at1846s_set_bandwidth(&r.rf, rf_bw(bw)));
	count(at1846s_set_mode(&r.rf, AT1846S_MODE_FM));
	count(at1846s_set_freq(&r.rf, hz));
	count(at1846s_set_rx_tone(&r.rf, type, value));
	count(at1846s_set_state(&r.rf, AT1846S_STATE_RX));
	count(hrc6000_set_mode_fm_rx(&r.modem));
	return true;
}

// Refused in this build: no calibration, PA path unconfirmed (hardware-gd77.md 5.4)
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)hz;
	(void)bw;
	(void)power;
	(void)tx_tone;
	return false;
}

static void tx_off(void)
{
	k22f_pin_write(PIN_TX_EN, false);
	if (r.rx_valid) {
		(void)rx(r.rx_hz, r.rx_bw, &r.rx_tone);
	}
}

static void status(radio_status_t *st)
{
	uint8_t rssi, noise;
	uint16_t flags, value;
	at1846s_tone_t type = rf_tone(&r.rx_tone, &value);
	if (!at1846s_read_signal(&r.rf, &rssi, &noise) || !at1846s_read_flags(&r.rf, &flags)) {
		count(false);
		st->rssi_dbm = -140;
		st->noise = 255;
		st->carrier = false;
		st->tone_ok = type == AT1846S_TONE_NONE;
		return;
	}
	r.diag.rssi = rssi;
	r.diag.noise = noise;
	st->rssi_dbm = r.rx_uhf ? (int16_t)(rssi - 151) : (int16_t)(-164 + rssi * 32 / 27);
	st->noise = noise;
	st->carrier = (flags & AT1846S_FLAG_SQ) != 0;
	st->tone_ok = at1846s_tone_match(flags, type);
}

// The HR_C6000's FM audio feed-through and the speaker amplifier candidate (PTB0, hardware-gd77.md 8)
static void audio(bool on)
{
	count(hrc6000_fm_audio(&r.modem, on));
	k22f_pin_write(PIN_AUDIO_EN, on);
}

static void rx_sleep(bool sleep)
{
	if (sleep) {
		count(at1846s_set_state(&r.rf, AT1846S_STATE_IDLE));
	} else if (r.rx_valid) {
		(void)rx(r.rx_hz, r.rx_bw, &r.rx_tone);
	}
}

void gd77_radio_diag(gd77_radio_diag_t *d)
{
	*d = r.diag;
}

const hal_radio_t gd77_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.rx_sleep = rx_sleep,
};
