// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// RDA5807M single-chip broadcast FM receiver: chip driver, independent of the board. From the public RDA5807M
// datasheet (RDA Microelectronics, "RDA5807M Single-Chip Broadcast FM Radio Tuner", rev 1.1); register summary in
// docs/spec/airband-and-broadcast.md 4.2.
//
// The board supplies a bus with one 16-bit register write and read: the chip's random-access I2C mode at address
// 0x11 (write: register, high byte, low byte; read: register, repeated START, high byte, low byte). The driver keeps
// shadows of the writable registers (0x02-0x07) so each change is one write. Tune and seek only start the operation;
// rda5807m_status says when it is done (STC). Not reentrant: one task.
#ifndef RDA5807M_H
#define RDA5807M_H

#include <stdint.h>
#include <stdbool.h>

#define RDA5807M_I2C_ADDR	0x11u	// random access
#define RDA5807M_CHIP_ID	0x58u	// register 0x00 bits 15:8

// Registers
#define RDA5807M_REG_CHIPID	0x00u
#define RDA5807M_REG_CTRL	0x02u
#define RDA5807M_REG_CHAN	0x03u
#define RDA5807M_REG_R4		0x04u
#define RDA5807M_REG_VOL	0x05u
#define RDA5807M_REG_R7		0x07u
#define RDA5807M_REG_STATUS	0x0Au
#define RDA5807M_REG_RSSI	0x0Bu

// 0x02
#define RDA5807M_DHIZ		0x8000u	// audio outputs driven (0 = high impedance)
#define RDA5807M_DMUTE		0x4000u	// 1 = audio on (0 = muted)
#define RDA5807M_MONO		0x2000u
#define RDA5807M_BASS		0x1000u
#define RDA5807M_RCLK_NONCAL	0x0800u
#define RDA5807M_RCLK_DIRECT	0x0400u	// reference clock fed directly (not a crystal)
#define RDA5807M_SEEKUP		0x0200u
#define RDA5807M_SEEK		0x0100u	// starts a seek; the chip clears it when done
#define RDA5807M_SKMODE		0x0080u	// 1 = stop at the band edge, 0 = wrap
#define RDA5807M_CLK_SHIFT	4	// bits 6:4, reference clock: 0 = 32.768 kHz ... (datasheet table)
#define RDA5807M_NEW_METHOD	0x0004u
#define RDA5807M_SOFT_RESET	0x0002u
#define RDA5807M_ENABLE		0x0001u
// 0x03
#define RDA5807M_CHAN_SHIFT	6	// bits 15:6
#define RDA5807M_TUNE		0x0010u	// starts a tune; the chip clears it when done
#define RDA5807M_BAND_SHIFT	2	// bits 3:2
#define RDA5807M_SPACE_50K	0x0002u	// bits 1:0 = 10
// 0x04
#define RDA5807M_DE_50US	0x0800u	// de-emphasis 50 us (0 = 75 us)
#define RDA5807M_SOFTMUTE	0x0200u
// 0x05
#define RDA5807M_INT_MODE	0x8000u
#define RDA5807M_SEEKTH_SHIFT	8	// bits 11:8
#define RDA5807M_LNA_PORT2	0x0080u	// bits 7:6 = 10, the datasheet default input
#define RDA5807M_VOLUME_MASK	0x000Fu
// 0x07
#define RDA5807M_BLEND_TH_SHIFT	10	// bits 14:10
#define RDA5807M_65M_MODE	0x0200u	// band 3 = 65-76 MHz (0 = 50-76 MHz)
#define RDA5807M_SOFTBLEND	0x0002u
// 0x0A
#define RDA5807M_STC		0x4000u	// seek / tune complete
#define RDA5807M_SF		0x2000u	// seek failed
#define RDA5807M_ST		0x0400u	// stereo
#define RDA5807M_READCHAN_MASK	0x03FFu
// 0x0B
#define RDA5807M_RSSI_SHIFT	9	// bits 15:9, logarithmic
#define RDA5807M_FM_TRUE	0x0100u	// the current channel is a station
#define RDA5807M_FM_READY	0x0080u

// Bands the driver uses (BAND field values) and the 50 kHz grid
enum { RDA5807M_BAND_WORLD = 2, RDA5807M_BAND_EAST = 3 };	// 76-108 MHz, 65-76 MHz (with RDA5807M_65M_MODE)
#define RDA5807M_LO_KHZ		65000u
#define RDA5807M_SPLIT_KHZ	76000u
#define RDA5807M_HI_KHZ		108000u
#define RDA5807M_GRID_KHZ	50u

typedef struct {
	bool (*write)(void *ctx, uint8_t reg, uint16_t val);
	bool (*read)(void *ctx, uint8_t reg, uint16_t *val);
	void (*delay_ms)(uint32_t ms);
	void *ctx;
} rda5807m_bus_t;

typedef struct {
	uint8_t clk_mode;	// 0x02 bits 6:4 for the board's reference clock (0 = 32.768 kHz crystal)
	bool rclk_direct;	// the reference is a clock signal, not a crystal
	bool de_50us;		// de-emphasis 50 us (Europe, most of the world) instead of 75 us (Americas)
	uint8_t seek_th;	// seek SNR threshold 0..15 (datasheet default 8)
} rda5807m_cfg_t;

typedef struct {
	const rda5807m_bus_t *bus;
	rda5807m_cfg_t cfg;
	uint16_t r02, r03, r04, r05, r07;	// shadows (TUNE / SEEK never kept set)
	uint8_t band;		// band of the last tune
	bool on;
	uint32_t writes;	// register writes (diagnostics)
} rda5807m_t;

typedef struct {
	uint32_t khz;		// READCHAN in kHz
	bool done;		// STC
	bool seek_failed;	// SF
	bool stereo;
	bool station;		// FM_TRUE
	uint8_t rssi;		// 0..127
} rda5807m_status_t;

// --- Pure helpers ---
// The band and channel number of a frequency on the 50 kHz grid; false outside 65-108 MHz or off the grid
bool rda5807m_chan(uint32_t khz, uint8_t *band, uint16_t *chan);
// The frequency of a channel number in a band
uint32_t rda5807m_khz(uint8_t band, uint16_t chan);

// --- Chip ---
void rda5807m_attach(rda5807m_t *dev, const rda5807m_bus_t *bus, const rda5807m_cfg_t *cfg);
// Read the chip ID: true when an RDA5807M answers
bool rda5807m_probe(rda5807m_t *dev);
// Power up (soft reset, configuration, tuned to khz, audio muted) or down
bool rda5807m_power(rda5807m_t *dev, bool on, uint32_t khz);
bool rda5807m_tune(rda5807m_t *dev, uint32_t khz);
// Seek in the band of the current frequency (65-76 or 76-108 MHz), wrapping once round it
bool rda5807m_seek(rda5807m_t *dev, bool up);
bool rda5807m_volume(rda5807m_t *dev, uint8_t level);	// 0..15
bool rda5807m_mute(rda5807m_t *dev, bool mute);
bool rda5807m_status(rda5807m_t *dev, rda5807m_status_t *st);

#endif
