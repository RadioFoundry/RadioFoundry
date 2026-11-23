// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A superhet's RF chips found and bound (superhet_chips.h)
#include "superhet_chips.h"
#include "rf/bk4819/bk4819_superhet.h"
#include "rf/fd6818/fd6818sh_superhet.h"
#include "rf/at1141/at1141_superhet.h"

#define ID_TRIES	3u
#define ID_RESET_MS	100u		// soft reset, this long, release, this long, read again
#define REG_ID		0x00
#define ID_RESET	0x8000u
#define REG_PAGE	0x7F		// the FD6818 family's page select: page 0 before the reset

superhet_chip_t superhet_chip_kind(uint16_t id, bool lo)
{
	switch (id) {
	case BK4819_ID: return SUPERHET_CHIP_BK4819;
	case FD6818SH_ID: return SUPERHET_CHIP_FD6818;
	case FD6818SH_ID_7813: return SUPERHET_CHIP_7813;
	case AT1141_ID: return lo ? SUPERHET_CHIP_AT1141 : SUPERHET_CHIP_NONE;	// a synthesizer: never the transceiver
	default: return SUPERHET_CHIP_NONE;
	}
}

uint16_t superhet_chip_identify(rfbus_t *bus, bool lo, superhet_chip_t *kind)
{
	uint16_t id = 0;
	for (unsigned i = 0; i < ID_TRIES; i++) {
		id = rfbus_read(bus, REG_ID);
		*kind = superhet_chip_kind(id, lo);
		if (*kind != SUPERHET_CHIP_NONE) {
			break;
		}
		rfbus_write(bus, REG_PAGE, 0);
		rfbus_write(bus, REG_ID, ID_RESET);
		rfbus_delay_ms(bus, ID_RESET_MS);
		rfbus_write(bus, REG_ID, 0x0000);
		rfbus_delay_ms(bus, ID_RESET_MS);
	}
	return id;
}

static void init_main(superhet_chips_t *c)
{
	if (c->main_kind == SUPERHET_CHIP_BK4819) {
		bk4819_attach(&c->m.bk, c->cfg->bus_main);
		bk4819_init_main(&c->m.bk);
		bk4819_write(&c->m.bk, BK4819_REG_PA, BK4819_PA_MAX);	// once, never changed: the PA is outside the chip
		c->main = (superhet_main_t){ &bk4819_superhet_main, &c->m.bk };
		return;
	}
	// The 0x7813 part takes the FD6818's main set
	fd6818sh_attach(&c->m.fd, c->cfg->bus_main, FD6818SH_MAIN);
	if (c->cfg->fd_cfg != NULL) {
		fd6818sh_set_cfg(&c->m.fd, c->cfg->fd_cfg);
	}
	fd6818sh_init_main(&c->m.fd);
	c->main = (superhet_main_t){ &fd6818sh_superhet_main, &c->m.fd };
}

static void init_lo(superhet_chips_t *c)
{
	switch (c->lo_kind) {
	case SUPERHET_CHIP_BK4819:
		bk4819_attach(&c->lo_dev.bk, c->cfg->bus_lo);
		bk4819_init_lo(&c->lo_dev.bk, BK4819_LO_PA_MAX);
		c->lo = (superhet_lo_t){ &bk4819_superhet_lo, &c->lo_dev.bk };
		break;
	case SUPERHET_CHIP_AT1141:
		at1141_attach(&c->lo_dev.at, c->cfg->bus_lo);
		at1141_init(&c->lo_dev.at);
		if (c->cfg->at1141_restart) {
			c->lo = (superhet_lo_t){ &at1141_superhet_lo_restart, &c->lo_dev.at };
		} else {
			at1141_disable(&c->lo_dev.at);
			c->lo = (superhet_lo_t){ &at1141_superhet_lo, &c->lo_dev.at };
		}
		break;
	default:
		fd6818sh_attach(&c->lo_dev.fd, c->cfg->bus_lo,
				c->lo_kind == SUPERHET_CHIP_7813 ? FD6818SH_LO_7813 : FD6818SH_LO);
		fd6818sh_init_lo(&c->lo_dev.fd);
		c->lo = (superhet_lo_t){ &fd6818sh_superhet_lo, &c->lo_dev.fd };
		break;
	}
}

bool superhet_chips_init(superhet_chips_t *c, const superhet_chips_cfg_t *cfg)
{
	c->cfg = cfg;
	c->main = (superhet_main_t){ 0 };
	c->lo = (superhet_lo_t){ 0 };
	c->main_id = superhet_chip_identify(cfg->bus_main, false, &c->main_kind);
	c->lo_id = superhet_chip_identify(cfg->bus_lo, true, &c->lo_kind);
	if (c->main_kind == SUPERHET_CHIP_NONE || c->lo_kind == SUPERHET_CHIP_NONE) {
		return false;
	}
	init_main(c);
	init_lo(c);
	if (c->main_kind == SUPERHET_CHIP_BK4819) {
		bk4819_analog_mode(&c->m.bk);
	} else {
		fd6818sh_analog_mode(&c->m.fd);
	}
	return true;
}

const char *superhet_chip_name(superhet_chip_t k)
{
	switch (k) {
	case SUPERHET_CHIP_BK4819: return "BK4819";
	case SUPERHET_CHIP_FD6818: return "FD6818";
	case SUPERHET_CHIP_7813: return "7813";
	case SUPERHET_CHIP_AT1141: return "AT1141";
	default: return "none";
	}
}

char superhet_chip_letter(superhet_chip_t k)
{
	switch (k) {
	case SUPERHET_CHIP_BK4819: return 'B';
	case SUPERHET_CHIP_FD6818: return '6';
	case SUPERHET_CHIP_7813: return '4';
	case SUPERHET_CHIP_AT1141: return 'T';
	default: return '?';
	}
}

void superhet_chips_rx_inverted(superhet_chips_t *c, bool inverted)
{
	if (c->main_kind == SUPERHET_CHIP_BK4819) {
		c->m.bk.rx_inverted = inverted;
	} else {
		c->m.fd.rx_inverted = inverted;
	}
}

void superhet_chips_status(superhet_chips_t *c, uint16_t *r67, uint16_t *r65, uint16_t *r0c)
{
	*r67 = rfbus_read(c->cfg->bus_main, BK4819_REG_RSSI);
	*r65 = rfbus_read(c->cfg->bus_main, BK4819_REG_NOISE);
	*r0c = rfbus_read(c->cfg->bus_main, BK4819_REG_CSS_STATUS);
}

static bk4819_tone_t bk_tone(superhet_tone_t t)
{
	switch (t) {
	case SUPERHET_TONE_CTCSS: return BK4819_TONE_CTCSS;
	case SUPERHET_TONE_DCS: return BK4819_TONE_DCS;
	case SUPERHET_TONE_DCS_INV: return BK4819_TONE_DCS_INV;
	default: return BK4819_TONE_NONE;
	}
}

bool superhet_chips_tone_heard(const superhet_chips_t *c, uint16_t r0c, superhet_tone_t type)
{
	if (type == SUPERHET_TONE_NONE) {
		return true;
	}
	if (c->main_kind == SUPERHET_CHIP_BK4819) {
		return bk4819_heard(&c->m.bk, r0c, bk_tone(type));
	}
	// The FD6818 has no status bit for it: its GPIO0 carries the compare result to a pin
	return c->cfg->css_line != NULL && c->cfg->css_line();
}

uint16_t superhet_chips_main_gpio_word(const superhet_chips_t *c)
{
	return c->main_kind == SUPERHET_CHIP_BK4819 ? c->m.bk.gpio : c->m.fd.r25;
}
