// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DM-32 factory calibration: TX I/Q center, modulation and PA drive. Layout and use from the disassembly of the 2026
 * OpenDM32 builds (private research notes, section B).
 *
 * The factory data is a 4 KB sector of the 16 MB data flash, in 0x1000..0xC8000, whose last byte is 2: 0x108 bytes from
 * its start, 0x15 from +0x200 and 0x50 (four tables of five BCD frequencies) from +0x300. It's only read: this firmware
 * never writes below the 0x700000 legacy area. If no factory sector is found, the 0x175 byte copy the OpenDM32
 * builds keep at legacy address 0x10000 is used, when it has the right header.
 */

#include <string.h>
// TODO(hal): legacy DM-32 tree headers: the calibration API (functions/calibration.h), SPI_Flash_read and
// SPI_Flash_readRaw (hardware/SPI_Flash.h)
#include "functions/calibration.h"
#include "hardware/SPI_Flash.h"

#define DM32_CAL_LEN				0x175
#define DM32_CAL_LOCAL_ADDR			0x10000		// legacy address (physical 0x710000), written by the OpenDM32 builds
#define DM32_CAL_FACTORY_FIRST		0x1000
#define DM32_CAL_FACTORY_END		0xC9000
#define DM32_CAL_FACTORY_MARKER		2			// last byte of the factory sector

typedef struct __attribute__((packed))
{
	uint8_t  header[8];			// 0x000  00 40 00 47 FF FF FF FF
	uint8_t  unk008[0x0E];
	uint8_t  txCenter;			// 0x016  SIG_CENTER / RF_MOD_BIAS, a signed offset around 128
	uint8_t  uhfMod[5];			// 0x017
	uint8_t  unk01C[0x24];
	uint8_t  uhfPower[3][5];	// 0x040 low, 0x045 mid, 0x04A high (x16)
	uint8_t  unk04F[0x57];
	uint8_t  vhfMod[5];			// 0x0A6
	uint8_t  unk0AB[0x25];
	uint8_t  vhfPower[3][5];	// 0x0D0 low, 0x0D5 mid, 0x0DA high (x16)
	uint8_t  unk0DF[0x29];
	uint8_t  block200[0x15];	// 0x108  (factory sector + 0x200)
	uint8_t  freqTab[4][5][4];	// 0x11D  (factory sector + 0x300) u32 LE, BCD in flash, binary (10 Hz) here
	uint8_t  vhfModK[4];		// 0x16D  mean of f_i * m_i, f in MHz
	uint8_t  uhfModK[4];		// 0x171
} DM32Calibration_t;
_Static_assert(sizeof(DM32Calibration_t) == DM32_CAL_LEN, "DM-32 calibration layout");

static const uint8_t DM32_CAL_HEADER[8] = { 0x00, 0x40, 0x00, 0x47, 0xFF, 0xFF, 0xFF, 0xFF };

static DM32Calibration_t dm32Cal;
static bool dm32CalValid = false;

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint32_t bcd8(uint32_t v)
{
	uint32_t r = 0;

	for (int i = 0; i < 8; i++)
	{
		r = (r * 10) + (v >> 28);
		v <<= 4;
	}

	return r;
}

static bool calibrationDM32ReadFactory(void)
{
	uint32_t sector = 0;
	uint8_t *c = (uint8_t *)&dm32Cal;

	for (uint32_t a = DM32_CAL_FACTORY_FIRST; a < DM32_CAL_FACTORY_END; a += 0x1000)
	{
		uint8_t b = 0;

		if (SPI_Flash_readRaw(a + 0xFFF, &b, 1) && (b == DM32_CAL_FACTORY_MARKER))
		{
			sector = a;
			break;
		}
	}

	if ((sector == 0) ||
			!SPI_Flash_readRaw(sector + 0x000, c + 0x000, 0x108) ||
			!SPI_Flash_readRaw(sector + 0x200, c + 0x108, 0x15) ||
			!SPI_Flash_readRaw(sector + 0x300, c + 0x11D, 0x50))
	{
		return false;
	}

	for (int i = 0; i < 20; i++)
	{
		uint8_t *p = c + 0x11D + (4 * i);
		wr32(p, bcd8(rd32(p)));
	}

	uint32_t kv = 0, ku = 0;
	for (int i = 0; i < 5; i++)
	{
		kv += (rd32(dm32Cal.freqTab[2][i]) / 100000) * dm32Cal.vhfMod[i];
		ku += (rd32(dm32Cal.freqTab[0][i]) / 100000) * dm32Cal.uhfMod[i];
	}
	wr32(dm32Cal.vhfModK, kv / 5);
	wr32(dm32Cal.uhfModK, ku / 5);

	return true;
}

void calibrationDM32Init(void)
{
	dm32CalValid = calibrationDM32ReadFactory();

	if (!dm32CalValid)
	{
		dm32CalValid = SPI_Flash_read(DM32_CAL_LOCAL_ADDR, (uint8_t *)&dm32Cal, DM32_CAL_LEN) &&
				(memcmp(dm32Cal.header, DM32_CAL_HEADER, sizeof(DM32_CAL_HEADER)) == 0);
	}
}

bool calibrationDM32IsValid(void)
{
	return dm32CalValid;
}

// TX I and Q center, 128 when there's no calibration
uint8_t calibrationGetTxCenter(void)
{
	return (dm32CalValid ? dm32Cal.txCenter : 0x80);
}

// RF_MOD_BIAS_CTRL low half for a TX frequency (10 Hz units): m(f) = K / f, in both bytes
uint16_t calibrationGetModForFrequency(int freq)
{
	uint32_t k = rd32((freq > 29999999) ? dm32Cal.uhfModK : dm32Cal.vhfModK);
	uint32_t f = (uint32_t)freq / 100000;

	if (!dm32CalValid || (f == 0) || (k == 0))
	{
		return 0x4D4D;// what the OpenDM32 builds use before the first frequency is set
	}

	uint8_t b = (uint8_t)(((2 * k) + f) / (2 * f));// k / f, rounded

	return (uint16_t)((b << 8) | b);
}

static int dm32Interp(const uint8_t *t, int idx, int rem, int step)
{
	int lo = t[idx] << 4;
	int hi = (idx < 4) ? (t[idx + 1] << 4) : ((2 * lo) - (t[idx - 1] << 4));
	int v = lo + (((hi - lo) * rem) / step);

	return (v < 0) ? 0 : ((v > 4096) ? 4096 : v);
}

// PA drive points (DAC_DATA_B) for a TX frequency (10 Hz units): five points per band, VHF from 136 MHz every 9.5 MHz,
// UHF from 400 MHz every 17.5 MHz, interpolated (extrapolated past the last)
bool calibrationDM32GetPowerForFrequency(int freq, calibrationPowerValues_t *p)
{
	const uint8_t (*t)[5];
	int base, step;

	if (!dm32CalValid)
	{
		return false;
	}

	if (freq < 13600000)
	{
		p->lowPower = dm32Cal.vhfPower[0][0] << 4;
		p->midPower = dm32Cal.vhfPower[1][0] << 4;
		p->highPower = dm32Cal.vhfPower[2][0] << 4;
		p->veryLowPower = (p->lowPower * 9) / 10;
		return true;
	}

	if (freq > 27999999)
	{
		t = dm32Cal.uhfPower;
		base = 40000000;
		step = 1750000;
	}
	else
	{
		t = dm32Cal.vhfPower;
		base = 13600000;
		step = 950000;
	}

	int d = freq - base;
	if (d < 0)
	{
		d = 0;
	}

	int idx = d / step;
	int rem = d % step;
	if (idx > 4)
	{
		idx = 4;
		rem = d - (4 * step);// past the last point
		if (rem > step)
		{
			rem = step;
		}
	}

	p->lowPower = dm32Interp(t[0], idx, rem, step);
	p->midPower = dm32Interp(t[1], idx, rem, step);
	p->highPower = dm32Interp(t[2], idx, rem, step);
	p->veryLowPower = (p->lowPower * 9) / 10;// power level 0 = low * 0.90, as the OpenDM32 builds

	return true;
}
