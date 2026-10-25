// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 buses (docs/spec/hardware-gd77.md 4). Each is built from its entry in bus_cfg.h and handed to the chip glue as
// a generic interface: I2C bus A (AT1846S, EEPROM) and B as i2c_bus_t, the data flash, the display and the HR_C6000
// port as spi_bus_t. Software buses run on GPIO through bus/i2c_gpio and bus/spi_gpio. The hardware choices: I2C0 for
// either two-wire bus (platform/k22f/i2c.c), SPI0 for the HR_C6000 port with its chip select by GPIO or as SPI0's
// PCS0. A set of choices bus_cfg.h refuses leaves those buses refusing every transfer.
#include "gd77.h"
#include "bus_cfg.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "bus/i2c_gpio/i2c_gpio.h"
#include "bus/spi_gpio/spi_gpio.h"
#include "FreeRTOS.h"
#include "semphr.h"

static k22f_status_t status[GD77_BUS_COUNT];

// A set of choices the pins can't carry (bus_cfg.h) leaves every bus refusing
static bool cfg_valid(void)
{
	return gd77_bus_cfg_check(gd77_bus_cfg, GD77_BUS_COUNT) == 0;
}

// --- Open-drain lines for the software I2C buses: released = input (the pull-up takes it high), low = driven 0 ---

static void od_line(uint8_t pin, bool high)
{
	if (high) {
		k22f_pin_dir(pin, false);
	} else {
		k22f_pin_write(pin, false);
		k22f_pin_dir(pin, true);
	}
}

static void a_scl(bool high) { k22f_pin_write(PIN_I2CA_SCL, high); }
static void a_sda(bool high) { od_line(PIN_I2CA_SDA, high); }
static bool a_sda_in(void) { return k22f_pin_read(PIN_I2CA_SDA); }
static void b_scl(bool high) { k22f_pin_write(PIN_I2CB_SCL, high); }
static void b_sda(bool high) { od_line(PIN_I2CB_SDA, high); }
static bool b_sda_in(void) { return k22f_pin_read(PIN_I2CB_SDA); }

// 100 kHz standard mode on both
static const i2c_gpio_t soft_i2c_a = { .scl = a_scl, .sda = a_sda, .sda_in = a_sda_in, .delay_us = hal_delay_us,
				       .half_us = 5 };
static const i2c_gpio_t soft_i2c_b = { .scl = b_scl, .sda = b_sda, .sda_in = b_sda_in, .delay_us = hal_delay_us,
				       .half_us = 5 };

static bool refuse_write(void *ctx, uint8_t addr7, const uint8_t *data, size_t n)
{
	(void)ctx;
	(void)addr7;
	(void)data;
	(void)n;
	return false;
}

static bool refuse_write_read(void *ctx, uint8_t addr7, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
	(void)ctx;
	(void)addr7;
	(void)w;
	(void)wn;
	(void)r;
	(void)rn;
	return false;
}

// A bus whose configuration is refused refuses every transfer
static const i2c_bus_t i2c_refused = { .write = refuse_write, .write_read = refuse_write_read };
static const i2c_bus_t soft_bus_a = I2C_GPIO_BUS(&soft_i2c_a);
static const i2c_bus_t soft_bus_b = I2C_GPIO_BUS(&soft_i2c_b);

static const i2c_bus_t *i2c_a, *i2c_b;
static SemaphoreHandle_t i2c_a_lock;
static volatile bool i2c_a_busy;	// a task-level transaction is on the wire (i2c_a_write_isr stays off)

#define I2C_HW_HZ	100000u		// the rate the software buses run at, until a radio shows the chips take more
// I2C on the pins: open drain with the pull-ups the software bus uses
#define I2C_PCR(mux)	(K22F_MUX(mux) | K22F_OPEN_DRAIN | K22F_PULLUP)

static const i2c_bus_t *i2c_setup(gd77_bus_id_t id, const i2c_bus_t *soft, const i2c_gpio_t *gpio)
{
	const gd77_bus_cfg_t *c = &gd77_bus_cfg[id];
	if (!cfg_valid()) {
		status[id] = K22F_ERR_ARG;
		return &i2c_refused;
	}
	// A hung slave holding SDA low is clocked free on the pins as GPIO first, whichever master runs the bus
	i2c_gpio_recover(gpio);
	if (!c->hw) {
		status[id] = K22F_OK;
		return soft;
	}
	for (uint32_t k = 0; k < GD77_BUS_PINS && c->pins[k].fn != K22F_FN_NONE; k++) {
		k22f_pin_config(c->pins[k].pin, I2C_PCR(c->mux));
	}
	status[id] = k22f_i2c0_init(I2C_HW_HZ);
	return status[id] == K22F_OK ? &k22f_i2c0_bus : &i2c_refused;
}

void gd77_i2c_init(void)
{
	if (i2c_a_lock == NULL) {
		i2c_a_lock = xSemaphoreCreateMutex();
	}
	i2c_a = i2c_setup(GD77_BUS_I2C_A, &soft_bus_a, &soft_i2c_a);
	i2c_b = i2c_setup(GD77_BUS_I2C_B, &soft_bus_b, &soft_i2c_b);
}

static bool lock(void)
{
	return i2c_a_lock == NULL || xSemaphoreTake(i2c_a_lock, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void unlock(void)
{
	if (i2c_a_lock != NULL) {
		xSemaphoreGive(i2c_a_lock);
	}
}

bool i2c_a_write(uint8_t addr7, const uint8_t *data, uint32_t n)
{
	if (i2c_a == NULL || !lock()) {
		return false;
	}
	i2c_a_busy = true;
	bool ok = i2c_a->write(i2c_a->ctx, addr7, data, n);
	i2c_a_busy = false;
	unlock();
	return ok;
}

bool i2c_a_write_read(uint8_t addr7, const uint8_t *w, uint32_t wn, uint8_t *r, uint32_t rn)
{
	if (i2c_a == NULL || !lock()) {
		return false;
	}
	i2c_a_busy = true;
	bool ok = i2c_a->write_read(i2c_a->ctx, addr7, w, wn, r, rn);
	i2c_a_busy = false;
	unlock();
	return ok;
}

// From the modem interrupts (the DMR per-burst RF switch): one write on bus A, refused at once when a task-level
// transaction is on the wire. The interrupt runs to its end before any task continues, so the two never interleave
bool i2c_a_write_isr(uint8_t addr7, const uint8_t *data, uint32_t n)
{
	if (i2c_a == NULL || i2c_a_busy) {
		return false;
	}
	return i2c_a->write(i2c_a->ctx, addr7, data, n);
}

// Bus B carries no radio function; it is set up (released lines) and offered for the diagnostics only
const i2c_bus_t *gd77_i2c_b(void)
{
	return i2c_b;
}

// --- SPI buses ---

static bool spi_refuse(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	(void)tx;
	(void)rx;
	(void)len;
	return false;
}

static void cs_none(bool active)
{
	(void)active;
}

// Data flash: software, mode 0. Selected = driven low; deselected = released to its external pull-up (PTA19 is also
// XTAL0, so it is never driven high)
static void nor_sck(bool high) { k22f_pin_write(PIN_NOR_SCK, high); }
static void nor_mosi(bool high) { k22f_pin_write(PIN_NOR_MOSI, high); }
static bool nor_miso(void) { return k22f_pin_read(PIN_NOR_MISO); }
static const spi_gpio_t soft_nor = { .sck = nor_sck, .mosi = nor_mosi, .miso = nor_miso, .cpol = false, .cpha = false };

static void nor_select(bool active)
{
	if (active) {
		k22f_pin_write(PIN_NOR_CS, false);
		k22f_pin_dir(PIN_NOR_CS, true);
	} else {
		k22f_pin_dir(PIN_NOR_CS, false);
	}
}

static bool nor_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (status[GD77_BUS_NOR] != K22F_OK) {
		return false;
	}
	spi_gpio_xfer(&soft_nor, tx, rx, len);
	return true;
}

const spi_bus_t gd77_nor_bus = { .select = nor_select, .xfer = nor_xfer };

// Display: software, write only, data set while the clock is low and taken on the rising edge (mode 3). The A0
// (command / data) line stays with the display code
static void lcd_sck(bool high) { k22f_pin_write(PIN_LCD_SCK, high); }
static void lcd_sda(bool high) { k22f_pin_write(PIN_LCD_SDA, high); }
static const spi_gpio_t soft_lcd = { .sck = lcd_sck, .mosi = lcd_sda, .cpol = true, .cpha = true };

static void lcd_select(bool active)
{
	k22f_pin_write(PIN_LCD_CS, !active);
}

static bool lcd_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	(void)rx;
	if (status[GD77_BUS_LCD] != K22F_OK) {
		return false;
	}
	spi_gpio_xfer(&soft_lcd, tx, NULL, len);
	return true;
}

const spi_bus_t gd77_lcd_bus = { .select = lcd_select, .xfer = lcd_xfer };

// HR_C6000 port: SPI0, mode 1, about 770 kHz at a 49 MHz bus (bus / 2 / 64 x 2, clock_math.h k22f_dspi_hz), the chip
// select by GPIO, or SPI0's PCS0 on the same pin with the GPIO version's 1 us setup and hold. Each transaction masks
// interrupts up to the RTOS syscall level, where the modem interrupts run (dmr_gd77.c), so they never land inside one
// (modem-hrc6000.md 1)
static const k22f_dspi_cfg_t c6k_spi = { .inst = 0, .cpol = false, .cpha = true, .pbr = 0, .br = 6, .dbr = true,
					 .cs_ns = 1000 };
#define C6K_PCS		0u	// PTD0 = SPI0_PCS0

static void c6k_select(bool active)
{
	if (active) {
		k22f_pin_write(PIN_C6K_CS, false);
		hal_delay_us(1);
	} else {
		hal_delay_us(1);
		k22f_pin_write(PIN_C6K_CS, true);
	}
}

static bool c6k_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	return k22f_dspi_xfer(0, tx, rx, len);
}

static const spi_bus_t c6k_hw = { .select = c6k_select, .xfer = c6k_xfer };
static const spi_bus_t spi_missing = { .select = cs_none, .xfer = spi_refuse };
static const spi_bus_t *c6k_bus = &spi_missing;
static bool c6k_pcs;

// A software port on PTD0..3 isn't written (the pins carry SPI0, the faster choice)
void gd77_c6k_bus_init(void)
{
	const gd77_bus_cfg_t *c = &gd77_bus_cfg[GD77_BUS_C6K];
	c6k_bus = &spi_missing;
	c6k_pcs = false;
	if (!cfg_valid()) {
		status[GD77_BUS_C6K] = K22F_ERR_ARG;
		return;
	}
	if (!c->hw) {
		status[GD77_BUS_C6K] = K22F_ERR_NOT_IMPLEMENTED;
		return;
	}
	if (c->hw_cs) {
		// The pin goes from its idle GPIO high to PCS0, which the module holds high (PCSIS) while deselected
		k22f_dspi_init(&c6k_spi);
		k22f_pin_config(PIN_C6K_CS, K22F_MUX(c->mux) | K22F_DRIVE_HIGH);
		c6k_pcs = true;
	} else {
		k22f_dspi_init(&c6k_spi);
		c6k_bus = &c6k_hw;
	}
	status[GD77_BUS_C6K] = K22F_OK;
}

// One configuration port transaction with the chip select held: header, then data
bool gd77_c6k_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	(void)ctx;
	UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
	if (c6k_pcs) {
		bool ok = k22f_dspi_xfer_pcs(0, C6K_PCS, hdr, NULL, hdr_len, len == 0u) &&
			  (len == 0u || k22f_dspi_xfer_pcs(0, C6K_PCS, tx, rx, len, true));
		portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
		return ok;
	}
	c6k_bus->select(true);
	bool ok = c6k_bus->xfer(hdr, NULL, hdr_len) && c6k_bus->xfer(tx, rx, len);
	c6k_bus->select(false);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
	return ok;
}

// The software SPI buses need only their clock at its idle level
void gd77_bus_init(void)
{
	if (!cfg_valid()) {
		status[GD77_BUS_NOR] = K22F_ERR_ARG;
		status[GD77_BUS_LCD] = K22F_ERR_ARG;
		return;
	}
	spi_gpio_idle(&soft_nor);
	spi_gpio_idle(&soft_lcd);
	status[GD77_BUS_NOR] = K22F_OK;
	status[GD77_BUS_LCD] = K22F_OK;
}

k22f_status_t gd77_bus_status(uint8_t id)
{
	return id < GD77_BUS_COUNT ? status[id] : K22F_ERR_ARG;
}
