// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The RDA5807M driver (drivers/bcast/rda5807m) against a simulated register file that behaves as the datasheet
// describes (chip ID, soft reset, TUNE / SEEK cleared by the chip with STC set some polls later, seek wrapping the band
// once, stations by frequency), first on a register-level bus, then through the bit-banged I2C master
// (drivers/bus/i2c_gpio) and a pin-level I2C slave model of the chip's random-access mode at address 0x11
#include <stdio.h>
#include <string.h>
#include "bcast/rda5807m/rda5807m.h"
#include "bus/i2c_gpio/i2c_gpio.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- the chip model ----
#define MAX_ST 4
static struct {
	uint16_t reg[0x10];
	uint16_t chip_id;
	uint16_t readchan;
	bool stc, sf;
	int pending;		// status reads until STC (a tune or seek is running)
	uint16_t result_chan;	// where it ends
	bool result_sf;
	uint32_t st_khz[MAX_ST];	// stations
	bool st_stereo[MAX_ST];
	uint8_t st_rssi;
	int writes, resets;
	uint16_t log_reg[32], log_val[32];
	int nlog;
	bool bus_fail;
} chip;

static uint8_t band_of(void) { return (uint8_t)((chip.reg[3] >> 2) & 3u); }
static uint32_t khz_of(uint16_t ch) { return rda5807m_khz(band_of(), ch); }
static int station_at(uint32_t khz)
{
	for (int i = 0; i < MAX_ST; i++) {
		if (chip.st_khz[i] != 0 && chip.st_khz[i] == khz) {
			return i;
		}
	}
	return -1;
}

static void chip_reset(void)
{
	memset(&chip, 0, sizeof(chip));
	chip.chip_id = 0x5804;
	chip.st_rssi = 40;
}

static void chip_write(uint8_t r, uint16_t v)
{
	chip.writes++;
	if (chip.nlog < 32) {
		chip.log_reg[chip.nlog] = r;
		chip.log_val[chip.nlog++] = v;
	}
	if (r >= 0x10) {
		return;
	}
	if (r == 2 && (v & RDA5807M_SOFT_RESET)) {
		chip.resets++;
		memset(chip.reg, 0, sizeof(chip.reg));
	}
	bool seeking = (chip.reg[2] & RDA5807M_SEEK) != 0 && chip.pending > 0;
	chip.reg[r] = v;
	if (!(chip.reg[2] & RDA5807M_ENABLE)) {
		return;
	}
	if (r == 3 && (v & RDA5807M_TUNE)) {
		chip.reg[3] &= (uint16_t)~RDA5807M_TUNE;	// cleared by the chip
		chip.stc = chip.sf = false;
		chip.pending = 2;
		chip.result_chan = (uint16_t)(v >> 6);
		chip.result_sf = false;
	}
	if (r == 2 && (v & RDA5807M_SEEK) && !seeking) {
		bool up = (v & RDA5807M_SEEKUP) != 0;
		uint16_t n = band_of() == RDA5807M_BAND_EAST ? 220 : 641;	// channels in the band at 50 kHz
		uint16_t ch = chip.readchan;
		chip.result_sf = true;
		chip.result_chan = chip.readchan;
		for (uint16_t i = 1; i <= n; i++) {
			uint16_t c = (uint16_t)(up ? (ch + i) % n : (ch + n - i) % n);
			if (station_at(khz_of(c)) >= 0) {
				chip.result_chan = c;
				chip.result_sf = false;
				break;
			}
		}
		chip.stc = chip.sf = false;
		chip.pending = 3;
	}
}

static uint16_t chip_read(uint8_t r)
{
	if (r == 0) {
		return chip.chip_id;
	}
	if (r == 0x0A) {
		if (chip.pending > 0 && --chip.pending == 0) {
			chip.readchan = chip.result_chan;
			chip.sf = chip.result_sf;
			chip.stc = true;
			chip.reg[2] &= (uint16_t)~RDA5807M_SEEK;	// cleared by the chip
		}
		int s = station_at(khz_of(chip.readchan));
		return (uint16_t)((chip.stc ? RDA5807M_STC : 0) | (chip.sf ? RDA5807M_SF : 0) |
			(s >= 0 && chip.st_stereo[s] ? RDA5807M_ST : 0) | chip.readchan);
	}
	if (r == 0x0B) {
		int s = station_at(khz_of(chip.readchan));
		return (uint16_t)(((s >= 0 ? chip.st_rssi : 10u) << 9) | (s >= 0 ? RDA5807M_FM_TRUE : 0) | RDA5807M_FM_READY);
	}
	return r < 0x10 ? chip.reg[r] : 0;
}

// ---- register-level bus ----
static bool b_write(void *ctx, uint8_t reg, uint16_t val)
{
	(void)ctx;
	if (chip.bus_fail) {
		return false;
	}
	chip_write(reg, val);
	return true;
}
static bool b_read(void *ctx, uint8_t reg, uint16_t *val)
{
	(void)ctx;
	if (chip.bus_fail) {
		return false;
	}
	*val = chip_read(reg);
	return true;
}
static int delays;
static void b_delay(uint32_t ms) { delays += (int)ms; }
static const rda5807m_bus_t reg_bus = { b_write, b_read, b_delay, NULL };

static const rda5807m_cfg_t cfg = { .clk_mode = 0, .rclk_direct = false, .de_50us = false, .seek_th = 8 };

static void poll_done(rda5807m_t *d, rda5807m_status_t *st)
{
	for (int i = 0; i < 10; i++) {
		CHECK(rda5807m_status(d, st));
		if (st->done) {
			return;
		}
	}
}

static void test_helpers(void)
{
	uint8_t band;
	uint16_t ch;
	CHECK(rda5807m_chan(87500, &band, &ch) && band == RDA5807M_BAND_WORLD && ch == 230);
	CHECK(rda5807m_chan(108000, &band, &ch) && band == RDA5807M_BAND_WORLD && ch == 640);
	CHECK(rda5807m_chan(76000, &band, &ch) && band == RDA5807M_BAND_WORLD && ch == 0);
	CHECK(rda5807m_chan(65000, &band, &ch) && band == RDA5807M_BAND_EAST && ch == 0);
	CHECK(rda5807m_chan(75950, &band, &ch) && band == RDA5807M_BAND_EAST && ch == 219);
	CHECK(!rda5807m_chan(64950, &band, &ch) && !rda5807m_chan(108050, &band, &ch));
	CHECK(!rda5807m_chan(98425, &band, &ch));	// off the 50 kHz grid
	CHECK(rda5807m_khz(RDA5807M_BAND_WORLD, 230) == 87500 && rda5807m_khz(RDA5807M_BAND_EAST, 219) == 75950);
	for (uint32_t k = 65000; k <= 108000; k += 50) {
		if (!rda5807m_chan(k, &band, &ch) || rda5807m_khz(band, ch) != k) {
			CHECK(!"round trip");
			break;
		}
	}
}

static void test_power_and_tune(void)
{
	rda5807m_t d;
	rda5807m_status_t st;
	chip_reset();
	rda5807m_attach(&d, &reg_bus, &cfg);
	CHECK(rda5807m_probe(&d));
	chip.chip_id = 0x1234;
	CHECK(!rda5807m_probe(&d));	// another chip
	chip.chip_id = 0x5804;
	CHECK(!rda5807m_tune(&d, 98500));	// not powered

	delays = 0;
	CHECK(rda5807m_power(&d, true, 98500));
	CHECK(chip.resets == 1 && delays >= 5);
	CHECK(chip.log_reg[0] == 2 && (chip.log_val[0] & RDA5807M_SOFT_RESET));
	uint16_t r2 = chip.reg[2];
	CHECK((r2 & RDA5807M_ENABLE) && (r2 & RDA5807M_DHIZ) && (r2 & RDA5807M_NEW_METHOD) && !(r2 & RDA5807M_SOFT_RESET));
	CHECK(!(r2 & RDA5807M_DMUTE));	// powered up muted
	CHECK((chip.reg[5] & RDA5807M_VOLUME_MASK) == 8 && ((chip.reg[5] >> 8) & 0xF) == 8);
	CHECK(chip.reg[7] & RDA5807M_65M_MODE);
	CHECK(!(chip.reg[4] & RDA5807M_DE_50US));
	// Tuned: channel (98.5 - 76) / 0.05 = 450, world band, 50 kHz spacing, TUNE cleared by the chip
	CHECK((chip.reg[3] >> 6) == 450 && ((chip.reg[3] >> 2) & 3u) == RDA5807M_BAND_WORLD && (chip.reg[3] & 3u) == 2);
	CHECK(!(chip.reg[3] & RDA5807M_TUNE));
	CHECK(rda5807m_status(&d, &st) && !st.done);	// still tuning
	poll_done(&d, &st);
	CHECK(st.done && st.khz == 98500 && !st.station);

	// A station with stereo
	chip.st_khz[0] = 101300;
	chip.st_stereo[0] = true;
	CHECK(rda5807m_tune(&d, 101300));
	poll_done(&d, &st);
	CHECK(st.khz == 101300 && st.station && st.stereo && st.rssi == 40);
	CHECK(!rda5807m_tune(&d, 120000) && !rda5807m_tune(&d, 98425));

	// The East European band
	CHECK(rda5807m_tune(&d, 69800));
	CHECK(((chip.reg[3] >> 2) & 3u) == RDA5807M_BAND_EAST && (chip.reg[3] >> 6) == 96);
	poll_done(&d, &st);
	CHECK(st.khz == 69800);

	// Volume, mute
	CHECK(rda5807m_volume(&d, 3) && (chip.reg[5] & RDA5807M_VOLUME_MASK) == 3);
	CHECK(rda5807m_volume(&d, 40) && (chip.reg[5] & RDA5807M_VOLUME_MASK) == 15);
	CHECK(rda5807m_mute(&d, false) && (chip.reg[2] & RDA5807M_DMUTE));
	CHECK(rda5807m_mute(&d, true) && !(chip.reg[2] & RDA5807M_DMUTE));

	// 50 us de-emphasis and a clock signal reference
	rda5807m_cfg_t c2 = { .clk_mode = 5, .rclk_direct = true, .de_50us = true, .seek_th = 20 };
	chip_reset();
	rda5807m_attach(&d, &reg_bus, &c2);
	CHECK(rda5807m_power(&d, true, 87500));
	CHECK(((chip.reg[2] >> 4) & 7u) == 5 && (chip.reg[2] & RDA5807M_RCLK_DIRECT) && (chip.reg[4] & RDA5807M_DE_50US));
	CHECK(((chip.reg[5] >> 8) & 0xF) == 8);	// out-of-range threshold: the default

	// Off
	CHECK(rda5807m_power(&d, false, 0) && !(chip.reg[2] & RDA5807M_ENABLE));
	CHECK(!rda5807m_status(&d, &st) && !rda5807m_seek(&d, true));

	// A bus that fails
	chip_reset();
	rda5807m_attach(&d, &reg_bus, &cfg);
	chip.bus_fail = true;
	CHECK(!rda5807m_probe(&d) && !rda5807m_power(&d, true, 98500));
}

static void test_seek(void)
{
	rda5807m_t d;
	rda5807m_status_t st;
	chip_reset();
	chip.st_khz[0] = 89100;
	chip.st_khz[1] = 101300;
	rda5807m_attach(&d, &reg_bus, &cfg);
	CHECK(rda5807m_power(&d, true, 87500));
	poll_done(&d, &st);

	CHECK(rda5807m_seek(&d, true));
	CHECK((chip.reg[2] & RDA5807M_SEEK) && (chip.reg[2] & RDA5807M_SEEKUP) && !(chip.reg[2] & RDA5807M_SKMODE));
	CHECK(rda5807m_status(&d, &st) && !st.done);
	// A mute change during the seek keeps it running (SEEK stays set in the write)
	CHECK(rda5807m_mute(&d, false) && (chip.log_val[chip.nlog - 1] & RDA5807M_SEEK));
	poll_done(&d, &st);
	CHECK(st.done && !st.seek_failed && st.khz == 89100 && st.station);
	CHECK(!(d.r02 & RDA5807M_SEEK) && (d.r03 >> 6) == (89100 - 76000) / 50);	// the shadows follow the chip
	CHECK(rda5807m_mute(&d, true) && !(chip.log_val[chip.nlog - 1] & RDA5807M_SEEK));	// no new seek

	CHECK(rda5807m_seek(&d, true));
	poll_done(&d, &st);
	CHECK(st.khz == 101300);
	CHECK(rda5807m_seek(&d, true));	// wraps round the band
	poll_done(&d, &st);
	CHECK(st.khz == 89100);
	CHECK(rda5807m_seek(&d, false));	// down from 89.1 wraps to 101.3
	CHECK(!(chip.reg[2] & RDA5807M_SEEKUP));
	poll_done(&d, &st);
	CHECK(st.khz == 101300);

	// Nothing in the band: seek failed, back where it started
	chip.st_khz[0] = chip.st_khz[1] = 0;
	CHECK(rda5807m_seek(&d, true));
	poll_done(&d, &st);
	CHECK(st.done && st.seek_failed && st.khz == 101300);

	// A tune during a seek stops the seek first
	chip.st_khz[0] = 95000;
	CHECK(rda5807m_seek(&d, true));
	CHECK(rda5807m_tune(&d, 90000) && !(d.r02 & RDA5807M_SEEK));
}

// ---- pin-level I2C slave in front of the chip model (random access, address 0x11) ----
enum { S_IDLE, S_ADDR, S_REG, S_WDATA, S_RDATA, S_IGNORE };
static struct {
	bool scl, sda_m, sda_s;		// master drives, slave drives (open drain: the line is low if either pulls)
	int state;
	int bitn;
	uint8_t sh;
	uint8_t reg;
	uint8_t wbuf[2];
	int widx;
	uint8_t tx;
	int ridx;
	bool master_ack;
	uint8_t addr;			// the slave's address
	int starts, stops, nacks;
} pin;

static bool line(void) { return pin.sda_m && pin.sda_s; }

static void load_tx(void)
{
	uint16_t v = chip_read(pin.reg);
	pin.tx = pin.ridx == 0 ? (uint8_t)(v >> 8) : (uint8_t)v;
}

static void byte_in(bool *ack)
{
	*ack = true;
	switch (pin.state) {
	case S_ADDR:
		if ((pin.sh >> 1) != pin.addr) {
			*ack = false;
			pin.state = S_IGNORE;
		} else {
			pin.state = (pin.sh & 1u) ? S_RDATA : S_REG;
			pin.ridx = 0;
		}
		break;
	case S_REG:
		pin.reg = pin.sh;
		pin.widx = 0;
		pin.state = S_WDATA;
		break;
	case S_WDATA:
		pin.wbuf[pin.widx++] = pin.sh;
		if (pin.widx == 2) {
			chip_write(pin.reg, (uint16_t)((pin.wbuf[0] << 8) | pin.wbuf[1]));
			pin.reg++;
			pin.widx = 0;
		}
		break;
	default:
		*ack = false;
		break;
	}
	if (!*ack) {
		pin.nacks++;
	}
}

static void edge(bool scl, bool sda_m)
{
	bool old_scl = pin.scl, old_line = line();
	pin.scl = scl;
	pin.sda_m = sda_m;
	bool nl = line();
	if (old_scl && scl && old_line != nl) {
		if (!nl) {	// START (or repeated START)
			pin.starts++;
			pin.state = S_ADDR;
			pin.bitn = 0;
			pin.sh = 0;
			pin.sda_s = true;
		} else {	// STOP
			pin.stops++;
			pin.state = S_IDLE;
			pin.sda_s = true;
		}
		return;
	}
	if (pin.state == S_IDLE || pin.state == S_IGNORE) {
		return;
	}
	bool rising = !old_scl && scl, falling = old_scl && !scl;
	if (pin.state == S_RDATA) {
		if (pin.bitn == 10) {	// our ACK of the address: the first bit goes out when SCL falls
			if (falling) {
				load_tx();
				pin.bitn = 0;
				pin.sda_s = (pin.tx & 0x80u) != 0;
			}
			return;
		}
		if (rising) {
			if (pin.bitn < 8) {
				pin.bitn++;
			} else if (pin.bitn == 8) {
				pin.master_ack = !line();
				pin.bitn = 9;
			}
		} else if (falling) {
			if (pin.bitn < 8) {
				pin.sda_s = ((pin.tx >> (7 - pin.bitn)) & 1u) != 0;
			} else if (pin.bitn == 8) {
				pin.sda_s = true;	// the master's ACK slot
			} else if (pin.master_ack) {
				pin.ridx++;
				if (pin.ridx == 2) {
					pin.ridx = 0;
					pin.reg++;
				}
				load_tx();
				pin.bitn = 0;
				pin.sda_s = (pin.tx & 0x80u) != 0;
			} else {
				pin.sda_s = true;
				pin.state = S_IGNORE;	// NACK: wait for STOP
			}
		}
		return;
	}
	// Receiving: address, register, data
	if (rising && pin.bitn < 8) {
		pin.sh = (uint8_t)((pin.sh << 1) | (nl ? 1u : 0u));
		pin.bitn++;
	} else if (falling && pin.bitn == 8) {
		bool ack;
		byte_in(&ack);
		pin.sda_s = !ack;
		pin.bitn = pin.state == S_RDATA ? 10 : 9;
		if (!ack) {
			pin.state = S_IGNORE;
			pin.sda_s = true;
		}
	} else if (falling && pin.bitn == 9) {
		pin.sda_s = true;
		pin.bitn = 0;
		pin.sh = 0;
	}
}

static void p_scl(bool high) { edge(high, pin.sda_m); }
static void p_sda(bool high) { edge(pin.scl, high); }
static bool p_sda_in(void) { return line(); }
static void p_delay(uint32_t us) { (void)us; }
static const i2c_gpio_t gpio = { p_scl, p_sda, p_sda_in, p_delay, 5 };

static bool g_write(void *ctx, uint8_t reg, uint16_t val)
{
	(void)ctx;
	uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	return i2c_gpio_write(&gpio, RDA5807M_I2C_ADDR, b, 3);
}
static bool g_read(void *ctx, uint8_t reg, uint16_t *val)
{
	(void)ctx;
	uint8_t r[2];
	if (!i2c_gpio_write_read(&gpio, RDA5807M_I2C_ADDR, &reg, 1, r, 2)) {
		return false;
	}
	*val = (uint16_t)((r[0] << 8) | r[1]);
	return true;
}
static const rda5807m_bus_t gpio_bus = { g_write, g_read, NULL, NULL };

static void test_i2c_gpio(void)
{
	chip_reset();
	memset(&pin, 0, sizeof(pin));
	pin.scl = pin.sda_m = pin.sda_s = true;
	pin.addr = RDA5807M_I2C_ADDR;

	// One register write: START, address, register, two bytes, STOP
	uint8_t w[3] = { 0x05, 0x88, 0x8F };
	CHECK(i2c_gpio_write(&gpio, RDA5807M_I2C_ADDR, w, 3));
	CHECK(chip.reg[5] == 0x888F && pin.starts == 1 && pin.stops == 1 && pin.nacks == 0);
	// A read with a repeated START
	uint16_t v = 0;
	CHECK(g_read(NULL, 0x00, &v) && v == 0x5804 && pin.starts == 3 && pin.stops == 2);
	CHECK(g_read(NULL, 0x05, &v) && v == 0x888F);
	// Nobody at another address: NACK, false, the bus left idle
	CHECK(!i2c_gpio_write(&gpio, 0x10, w, 3));
	CHECK(pin.nacks == 1 && pin.state == S_IDLE && pin.scl && line());
	uint8_t r[2];
	CHECK(!i2c_gpio_write_read(&gpio, 0x60, &w[0], 1, r, 2));
	// Recovery leaves the bus idle
	i2c_gpio_recover(&gpio);
	CHECK(pin.scl && line() && pin.state == S_IDLE);

	// The whole driver over the pins
	rda5807m_t d;
	rda5807m_status_t st;
	chip.st_khz[0] = 104700;
	rda5807m_attach(&d, &gpio_bus, &cfg);
	CHECK(rda5807m_probe(&d));
	CHECK(rda5807m_power(&d, true, 100000));
	CHECK((chip.reg[2] & RDA5807M_ENABLE) && (chip.reg[3] >> 6) == 480);
	CHECK(rda5807m_seek(&d, true));
	poll_done(&d, &st);
	CHECK(st.done && st.khz == 104700 && st.station);
	CHECK(pin.nacks == 2);	// only the two deliberate ones
}

int main(void)
{
	test_helpers();
	test_power_and_tune();
	test_seek();
	test_i2c_gpio();
	printf("# rda5807mTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
