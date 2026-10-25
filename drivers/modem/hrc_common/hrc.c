// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Configuration port access and the operations the HR_C6000 and HR_C5000 drivers share (hrc.h)
#include <stddef.h>
#include "hrc.h"

#define CMD_READ	0x80u
#define CMD_EXT		0x40u

const uint8_t hrc_ms_data_sync[6] = { 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 };

void hrc_attach(hrc_t *dev, const hrc_bus_t *bus, const hrc_chip_t *chip)
{
	dev->bus = bus;
	dev->chip = chip;
}

static uint8_t page_of(const hrc_t *dev, uint8_t page)
{
	page &= 7u;
	return page == HRC_PAGE_SYS ? dev->chip->cfg_page : page;
}

bool hrc_write(hrc_t *dev, uint8_t page, uint8_t reg, uint8_t val)
{
	uint8_t hdr[2] = { page_of(dev, page), reg };
	return dev->bus->transfer(dev->bus->ctx, hdr, 2, &val, NULL, 1);
}

bool hrc_read(hrc_t *dev, uint8_t page, uint8_t reg, uint8_t *val)
{
	uint8_t hdr[2] = { (uint8_t)(CMD_READ | page_of(dev, page)), reg };
	return dev->bus->transfer(dev->bus->ctx, hdr, 2, NULL, val, 1);
}

bool hrc_write_burst(hrc_t *dev, uint8_t page, uint8_t reg, const uint8_t *data, uint16_t len)
{
	uint8_t hdr[2] = { page_of(dev, page), reg };
	return dev->bus->transfer(dev->bus->ctx, hdr, 2, data, NULL, len);
}

bool hrc_read_burst(hrc_t *dev, uint8_t page, uint8_t reg, uint8_t *data, uint16_t len)
{
	uint8_t hdr[2] = { (uint8_t)(CMD_READ | page_of(dev, page)), reg };
	return dev->bus->transfer(dev->bus->ctx, hdr, 2, NULL, data, len);
}

bool hrc_write_ext(hrc_t *dev, uint8_t page, uint16_t addr, uint8_t val)
{
	uint8_t hdr[3] = { (uint8_t)(CMD_EXT | page_of(dev, page)), (uint8_t)(addr & 0xFFu),
			   (uint8_t)((addr >> 8) & 7u) };
	return dev->bus->transfer(dev->bus->ctx, hdr, 3, &val, NULL, 1);
}

bool hrc_ram(hrc_t *dev, uint8_t page, uint16_t addr, uint8_t *buf, uint16_t len, bool write)
{
	uint8_t hdr[3] = { (uint8_t)((write ? 0u : CMD_READ) | CMD_EXT | page_of(dev, page)),
			   (uint8_t)(addr & 0xFFu), (uint8_t)((addr >> 8) & 7u) };
	return dev->bus->transfer(dev->bus->ctx, hdr, 3, write ? buf : NULL, write ? NULL : buf, len);
}

bool hrc_set_dmr_id(hrc_t *dev, uint32_t id)
{
	bool low_first = dev->chip->id_low_first;
	bool ok = hrc_write(dev, HRC_PAGE_SYS, 0x14, (uint8_t)(low_first ? id : id >> 16));
	ok &= hrc_write(dev, HRC_PAGE_SYS, 0x15, (uint8_t)(id >> 8));
	ok &= hrc_write(dev, HRC_PAGE_SYS, 0x16, (uint8_t)(low_first ? id >> 16 : id));
	return ok;
}

bool hrc_set_mod_gain(hrc_t *dev, uint8_t mod2, uint8_t mod1)
{
	bool ok = hrc_write(dev, HRC_PAGE_SYS, 0x45, mod2);
	ok &= hrc_write(dev, HRC_PAGE_SYS, 0x46, mod1);
	return ok;
}

bool hrc_set_mic_gain(hrc_t *dev, uint8_t gain)
{
	return dev->chip->set_mic_gain(dev, gain);
}

bool hrc_flush_vocoder(hrc_t *dev)
{
	uint8_t fill[128];
	for (uint32_t i = 0; i < sizeof(fill); i++) {
		fill[i] = 0x7F;
	}
	return hrc_write_burst(dev, HRC_PAGE_VOCODER, 0x00, fill, sizeof(fill));
}
