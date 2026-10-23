// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RDA5807M broadcast FM receiver (rda5807m.h)
#include <stddef.h>
#include "rda5807m.h"

#define RESET_MS		5	// soft reset pulse
#define SETTLE_MS		60	// from the configuration to the first tune: the oscillator and the tuner start up
#define BLEND_TH_DEFAULT	0x10u	// 0x07 soft blend threshold, the datasheet default
#define VOLUME_DEFAULT		8u

bool rda5807m_chan(uint32_t khz, uint8_t *band, uint16_t *chan)
{
	if (khz < RDA5807M_LO_KHZ || khz > RDA5807M_HI_KHZ || khz % RDA5807M_GRID_KHZ != 0) {
		return false;
	}
	uint32_t base = khz < RDA5807M_SPLIT_KHZ ? RDA5807M_LO_KHZ : RDA5807M_SPLIT_KHZ;
	*band = khz < RDA5807M_SPLIT_KHZ ? RDA5807M_BAND_EAST : RDA5807M_BAND_WORLD;
	*chan = (uint16_t)((khz - base) / RDA5807M_GRID_KHZ);
	return true;
}

uint32_t rda5807m_khz(uint8_t band, uint16_t chan)
{
	uint32_t base = band == RDA5807M_BAND_EAST ? RDA5807M_LO_KHZ : RDA5807M_SPLIT_KHZ;
	return base + (uint32_t)(chan & RDA5807M_READCHAN_MASK) * RDA5807M_GRID_KHZ;
}

static bool wr(rda5807m_t *dev, uint8_t reg, uint16_t val)
{
	dev->writes++;
	return dev->bus->write(dev->bus->ctx, reg, val);
}

void rda5807m_attach(rda5807m_t *dev, const rda5807m_bus_t *bus, const rda5807m_cfg_t *cfg)
{
	dev->bus = bus;
	dev->cfg = *cfg;
	dev->r02 = dev->r03 = dev->r04 = dev->r07 = 0;
	dev->r05 = VOLUME_DEFAULT;
	dev->band = RDA5807M_BAND_WORLD;
	dev->on = false;
	dev->writes = 0;
}

bool rda5807m_probe(rda5807m_t *dev)
{
	uint16_t id = 0;
	return dev->bus->read(dev->bus->ctx, RDA5807M_REG_CHIPID, &id) && (id >> 8) == RDA5807M_CHIP_ID;
}

bool rda5807m_power(rda5807m_t *dev, bool on, uint32_t khz)
{
	if (!on) {
		dev->r02 &= (uint16_t)~(RDA5807M_ENABLE | RDA5807M_SEEK | RDA5807M_DMUTE);
		dev->on = false;
		return wr(dev, RDA5807M_REG_CTRL, dev->r02);
	}
	// Audio outputs driven, muted; the board's reference clock; the datasheet's recommended tuning method
	uint16_t r02 = (uint16_t)(RDA5807M_DHIZ | ((dev->cfg.clk_mode & 7u) << RDA5807M_CLK_SHIFT) | RDA5807M_NEW_METHOD |
		RDA5807M_ENABLE);
	if (dev->cfg.rclk_direct) {
		r02 |= RDA5807M_RCLK_DIRECT;
	}
	bool ok = wr(dev, RDA5807M_REG_CTRL, (uint16_t)(r02 | RDA5807M_SOFT_RESET));
	if (dev->bus->delay_ms != NULL) {
		dev->bus->delay_ms(RESET_MS);
	}
	dev->r02 = r02;
	ok = wr(dev, RDA5807M_REG_CTRL, dev->r02) && ok;
	dev->r04 = dev->cfg.de_50us ? RDA5807M_DE_50US : 0;
	ok = wr(dev, RDA5807M_REG_R4, dev->r04) && ok;
	uint8_t th = dev->cfg.seek_th <= 15 ? dev->cfg.seek_th : 8;
	dev->r05 = (uint16_t)(RDA5807M_INT_MODE | ((uint16_t)th << RDA5807M_SEEKTH_SHIFT) | RDA5807M_LNA_PORT2 |
		(dev->r05 & RDA5807M_VOLUME_MASK));
	ok = wr(dev, RDA5807M_REG_VOL, dev->r05) && ok;
	dev->r07 = (uint16_t)((BLEND_TH_DEFAULT << RDA5807M_BLEND_TH_SHIFT) | RDA5807M_65M_MODE | RDA5807M_SOFTBLEND);
	ok = wr(dev, RDA5807M_REG_R7, dev->r07) && ok;
	dev->on = ok;
	// A tune written straight after the reset is lost: the first one after power-up left static on air until a retune
	if (ok && dev->bus->delay_ms != NULL) {
		dev->bus->delay_ms(SETTLE_MS);
	}
	return ok && rda5807m_tune(dev, khz);
}

bool rda5807m_tune(rda5807m_t *dev, uint32_t khz)
{
	uint8_t band;
	uint16_t chan;
	if (!dev->on || !rda5807m_chan(khz, &band, &chan)) {
		return false;
	}
	if (dev->r02 & RDA5807M_SEEK) {
		dev->r02 &= (uint16_t)~RDA5807M_SEEK;	// a tune ends a seek still running
		wr(dev, RDA5807M_REG_CTRL, dev->r02);
	}
	dev->band = band;
	dev->r03 = (uint16_t)(((uint16_t)chan << RDA5807M_CHAN_SHIFT) | ((uint16_t)band << RDA5807M_BAND_SHIFT) |
		RDA5807M_SPACE_50K);
	// The channel first with TUNE low, then TUNE: the chip tunes on TUNE's rising edge, so a write that carries the
	// channel and TUNE together is missed when TUNE is still high from a tune in progress
	bool ok = wr(dev, RDA5807M_REG_CHAN, dev->r03);
	return wr(dev, RDA5807M_REG_CHAN, (uint16_t)(dev->r03 | RDA5807M_TUNE)) && ok;
}

bool rda5807m_seek(rda5807m_t *dev, bool up)
{
	if (!dev->on) {
		return false;
	}
	dev->r02 &= (uint16_t)~(RDA5807M_SEEKUP | RDA5807M_SKMODE);	// wrap at the band edge: one round
	dev->r02 |= RDA5807M_SEEK | (up ? RDA5807M_SEEKUP : 0u);
	return wr(dev, RDA5807M_REG_CTRL, dev->r02);
}

bool rda5807m_volume(rda5807m_t *dev, uint8_t level)
{
	if (level > 15) {
		level = 15;
	}
	dev->r05 = (uint16_t)((dev->r05 & ~RDA5807M_VOLUME_MASK) | level);
	return !dev->on || wr(dev, RDA5807M_REG_VOL, dev->r05);
}

bool rda5807m_mute(rda5807m_t *dev, bool mute)
{
	if (mute) {
		dev->r02 &= (uint16_t)~RDA5807M_DMUTE;
	} else {
		dev->r02 |= RDA5807M_DMUTE;
	}
	return !dev->on || wr(dev, RDA5807M_REG_CTRL, dev->r02);	// SEEK stays set while a seek runs
}

bool rda5807m_status(rda5807m_t *dev, rda5807m_status_t *st)
{
	uint16_t a = 0, b = 0;
	if (!dev->on || !dev->bus->read(dev->bus->ctx, RDA5807M_REG_STATUS, &a) ||
	    !dev->bus->read(dev->bus->ctx, RDA5807M_REG_RSSI, &b)) {
		return false;
	}
	st->khz = rda5807m_khz(dev->band, (uint16_t)(a & RDA5807M_READCHAN_MASK));
	st->done = (a & RDA5807M_STC) != 0;
	st->seek_failed = (a & RDA5807M_SF) != 0;
	st->stereo = (a & RDA5807M_ST) != 0;
	st->station = (b & RDA5807M_FM_TRUE) != 0;
	st->rssi = (uint8_t)(b >> RDA5807M_RSSI_SHIFT);
	if (st->done && (dev->r02 & RDA5807M_SEEK)) {
		// The chip cleared SEEK; the shadow follows, and keeps the channel it stopped on
		dev->r02 &= (uint16_t)~RDA5807M_SEEK;
		dev->r03 = (uint16_t)((dev->r03 & ~(RDA5807M_READCHAN_MASK << RDA5807M_CHAN_SHIFT)) |
			((a & RDA5807M_READCHAN_MASK) << RDA5807M_CHAN_SHIFT));
	}
	return true;
}
