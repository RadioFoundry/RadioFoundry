// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Conversion between DMR data bursts (info bits) and 33 byte DMR frames, as used by MMDVM
 *
 * BPTC(196,96) for CSBK, MBC, data headers and Rate 1/2 blocks (12 bytes), the Rate 3/4 Trellis code (18 bytes) and
 * Rate 1 (24 bytes, no FEC), ETSI TS 102 361-1 annex B.
 */

#include <string.h>
#include "hotspot/dmrDataFrame.h"
#include "dmr/fec/dmrFec.h"

// ETSI TS 102 361-1 9.1.1: the 48 sync bits are burst bits 108-155; the MS sourced data sync pattern
#define SYNC_FIRST_BIT		108
#define SYNC_BITS			48
#define MS_DATA_SYNC		0xD5D7F77FD757ULL

#define INFO_BITS			196
#define TRELLIS_DIBITS		98
#define TRELLIS_STEPS		49		// 48 tribits of data and a zero tribit that flushes the encoder
#define TRELLIS_STATES		8

// The 196 info bits are frame bits 0-97 and 166-263, either side of the slot type and sync
static int infoBitPosition(int n)
{
	return (n < 98) ? n : (n + 68);
}

static int getInfoBit(const uint8_t *frame, int n)
{
	int p = infoBitPosition(n);

	return (frame[p >> 3] >> (7 - (p & 7))) & 1;
}

static void setInfoBit(uint8_t *frame, int n, int bit)
{
	int p = infoBitPosition(n);
	uint8_t mask = 0x80 >> (p & 7);

	frame[p >> 3] = bit ? (frame[p >> 3] | mask) : (frame[p >> 3] & ~mask);
}

static int getBit(const uint8_t *data, int n)
{
	return (data[n >> 3] >> (7 - (n & 7))) & 1;
}

static void setBit(uint8_t *data, int n, int bit)
{
	uint8_t mask = 0x80 >> (n & 7);

	data[n >> 3] = bit ? (data[n >> 3] | mask) : (data[n >> 3] & ~mask);
}

// Rate 3/4: table B.7, the dibit interleave
static const uint8_t TRELLIS_INTERLEAVE[TRELLIS_DIBITS] = {
	0, 1, 8, 9, 16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73, 80, 81, 88, 89, 96, 97,
	2, 3, 10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91,
	4, 5, 12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93,
	6, 7, 14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95
};

// Table B.8, the constellation point for (state, tribit); the next state is the tribit
static const uint8_t TRELLIS_ENCODE[TRELLIS_STATES * 8] = {
	0, 8, 4, 12, 2, 10, 6, 14,
	4, 12, 2, 10, 6, 14, 0, 8,
	1, 9, 5, 13, 3, 11, 7, 15,
	5, 13, 3, 11, 7, 15, 1, 9,
	3, 11, 7, 15, 1, 9, 5, 13,
	7, 15, 1, 9, 5, 13, 3, 11,
	2, 10, 6, 14, 0, 8, 4, 12,
	6, 14, 0, 8, 4, 12, 2, 10
};

// Table B.9, constellation point to its two dibits (+3 = 01, +1 = 00, -1 = 10, -3 = 11), first dibit in the high bits
static const uint8_t TRELLIS_POINT_BITS[16] = {
	0x2, 0xA, 0x7, 0xF, 0xE, 0x6, 0xB, 0x3, 0xD, 0x5, 0x8, 0x0, 0x1, 0x9, 0x4, 0xC
};

static void trellisEncode(const uint8_t *payload, uint8_t *frame)
{
	uint8_t dibits[TRELLIS_DIBITS];
	int state = 0;

	for (int i = 0; i < TRELLIS_STEPS; i++)
	{
		int tribit = 0;

		if (i < (TRELLIS_STEPS - 1))
		{
			tribit = (getBit(payload, 3 * i) << 2) | (getBit(payload, (3 * i) + 1) << 1) | getBit(payload, (3 * i) + 2);
		}

		uint8_t point = TRELLIS_POINT_BITS[TRELLIS_ENCODE[(state * 8) + tribit]];
		dibits[2 * i] = point >> 2;
		dibits[(2 * i) + 1] = point & 0x03;
		state = tribit;
	}

	for (int i = 0; i < TRELLIS_DIBITS; i++)
	{
		uint8_t d = dibits[TRELLIS_INTERLEAVE[i]];

		setInfoBit(frame, 2 * i, d >> 1);
		setInfoBit(frame, (2 * i) + 1, d & 1);
	}
}

#if 1// Hamming distance of the 4 bits of each point: corrects more than the symbol level distance on hard bits
static int pointDistance(uint8_t a, uint8_t b)
{
	uint8_t v = a ^ b;
	return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1);
}
#else
// Distance between the symbol levels of the two dibits of each point (01 = +3, 00 = +1, 10 = -1, 11 = -3)
static const int8_t DIBIT_LEVEL[4] = { 1, 3, -1, -3 };

static int pointDistance(uint8_t a, uint8_t b)
{
	int d1 = DIBIT_LEVEL[a >> 2] - DIBIT_LEVEL[b >> 2];
	int d2 = DIBIT_LEVEL[a & 3] - DIBIT_LEVEL[b & 3];

	return ((d1 < 0) ? -d1 : d1) + ((d2 < 0) ? -d2 : d2);
}
#endif

// Hard decision Viterbi decoder. Returns the number of bit errors on the best path
static int trellisDecode(const uint8_t *frame, uint8_t *payload)
{
	uint8_t dibits[TRELLIS_DIBITS];
	uint8_t from[TRELLIS_STEPS][TRELLIS_STATES];// the state each state was reached from
	uint16_t metric[TRELLIS_STATES];
	uint16_t next[TRELLIS_STATES];
	const uint16_t UNREACHED = 0xFFFF;

	for (int i = 0; i < TRELLIS_DIBITS; i++)
	{
		dibits[TRELLIS_INTERLEAVE[i]] = (getInfoBit(frame, 2 * i) << 1) | getInfoBit(frame, (2 * i) + 1);
	}

	metric[0] = 0;
	for (int s = 1; s < TRELLIS_STATES; s++)
	{
		metric[s] = UNREACHED;
	}

	for (int i = 0; i < TRELLIS_STEPS; i++)
	{
		uint8_t received = (dibits[2 * i] << 2) | dibits[(2 * i) + 1];
		int tribits = (i < (TRELLIS_STEPS - 1)) ? 8 : 1;// the last tribit is always 0

		for (int t = 0; t < TRELLIS_STATES; t++)
		{
			next[t] = UNREACHED;
		}

		for (int s = 0; s < TRELLIS_STATES; s++)
		{
			if (metric[s] == UNREACHED)
			{
				continue;
			}

			for (int t = 0; t < tribits; t++)
			{
				uint16_t m = metric[s] + pointDistance(TRELLIS_POINT_BITS[TRELLIS_ENCODE[(s * 8) + t]], received);

				if (m < next[t])
				{
					next[t] = m;
					from[i][t] = s;
				}
			}
		}
		memcpy(metric, next, sizeof(metric));
	}

	// The path ends in state 0; walk it back, the state after each step is that step's tribit
	int state = 0;
	for (int i = TRELLIS_STEPS - 1; i > 0; i--)
	{
		state = from[i][state];

		int bit = 3 * (i - 1);
		setBit(payload, bit, (state >> 2) & 1);
		setBit(payload, bit + 1, (state >> 1) & 1);
		setBit(payload, bit + 2, state & 1);
	}

	return metric[0];
}

// Rate 1: table B.10B, the 192 data bits are info bits 0-95 and 100-195; info bits 96-99 are reserved (sent as 0)
static void rate1Encode(const uint8_t *payload, uint8_t *frame)
{
	for (int i = 0; i < INFO_BITS; i++)
	{
		int bit = 0;

		if (i < 96)
		{
			bit = getBit(payload, i);
		}
		else if (i >= 100)
		{
			bit = getBit(payload, i - 4);
		}
		setInfoBit(frame, i, bit);
	}
}

static void rate1Decode(const uint8_t *frame, uint8_t *payload)
{
	for (int i = 0; i < 192; i++)
	{
		setBit(payload, i, getInfoBit(frame, (i < 96) ? i : (i + 4)));
	}
}

bool dmrDataFrameToBurst(uint8_t dataType, const uint8_t *frame, dmrBurst_t *burst)
{
	memset(burst->payload, 0, sizeof(burst->payload));
	burst->dataType = dataType;
	burst->flags = DMR_BURST_FLAG_MCU_CRC;// the CRC and mask are already in the frame

	if (dataType == DT_RATE_34_DATA)
	{
		burst->length = 18;
		trellisDecode(frame, burst->payload);
	}
	else if (dataType == DT_RATE_1_DATA)
	{
		burst->length = 24;
		rate1Decode(frame, burst->payload);
	}
	else
	{
		burst->length = 12;
		dmrFecBptcDecode(frame, burst->payload);
	}
	return true;
}

// MS sourced, as the hotspot is on the mobile side of the air interface
bool dmrDataBurstToFrame(const dmrBurst_t *burst, uint8_t colorCode, uint8_t *frame)
{
	if ((burst->dataType == DT_RATE_34_DATA) && (burst->length == 18))
	{
		trellisEncode(burst->payload, frame);
	}
	else if ((burst->dataType == DT_RATE_1_DATA) && (burst->length == 24))
	{
		rate1Encode(burst->payload, frame);
	}
	else if ((burst->length == 12) && (burst->dataType != DT_RATE_34_DATA) && (burst->dataType != DT_RATE_1_DATA))
	{
		dmrFecBptcEncode(burst->payload, frame);
	}
	else
	{
		return false;
	}

	dmrFecSlotTypeEncode(colorCode, burst->dataType, frame);
	for (int i = 0; i < SYNC_BITS; i++)
	{
		setBit(frame, SYNC_FIRST_BIT + i, (int)((MS_DATA_SYNC >> (SYNC_BITS - 1 - i)) & 1));
	}
	return true;
}
