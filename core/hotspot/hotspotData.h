// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Hotspot packet data from the host: MMDVM data frames collected into burst lists for the radio's data TX
 */

#ifndef RFOUNDRY_HOTSPOTDATA_H
#define RFOUNDRY_HOTSPOTDATA_H

#include <stdbool.h>
#include <stdint.h>
#include "dmr/dmrData.h"

// MMDVM NAK reasons
#define HOTSPOT_DATA_NAK_UNSUPPORTED	4U		// a burst type the radio can't send
#define HOTSPOT_DATA_NAK_FULL			5U		// no space left

// The bursts are collected in a list. A list goes out when it holds as many bursts as its data header (or else its
// preamble CSBKs or MBC) announced, or HOTSPOT_DATA_GAP_MS after its last frame. While it is on air the next list fills
// the rest of the array, so frames keep being taken (DMR_DATA_MAX_BURSTS in all).
#define HOTSPOT_DATA_GAP_MS			180U	// 3 slots without a frame
#define HOTSPOT_DATA_GIVE_UP_MS		2000U	// the channel never became free

// No hardware dependencies, so it can be unit tested on a host (see tests/host).
// storage: DMR_DATA_MAX_BURSTS bursts, e.g. the data service's, which isn't used in hotspot mode
void hotspotDataReset(dmrBurst_t *storage);

// A 33 byte frame with data sync from the host. 0 if it was taken, else the MMDVM NAK reason
uint8_t hotspotDataQueue(uint8_t dataType, const uint8_t *frame, uint32_t nowMs);

// Starts a list that is ready, when canStart (the radio is idle on receive), and frees the one that has gone out
void hotspotDataTick(uint32_t nowMs, bool canStart);

// Anything queued or on air, so the host must not retune
bool hotspotDataIsBusy(void);

// Bursts the host can still send
int hotspotDataSpace(void);

// MMDVMHost writes every frame to the modem with control byte 0 (DMRSlot sets data[1] = 0: the MMDVM firmware sends
// the 33 bytes as they are), only some hosts (BlueDV, the SDR bench tools) set DMR_SYNC_DATA | data type / DMR_SYNC_AUDIO.
// The control byte the frame should have had: as given if it has a sync flag, else from the burst's own sync (data or
// voice, BS, MS or direct mode TS1 / TS2) and slot type. Voice B-F keep the control byte given
#define HOTSPOT_NET_SYNC_DATA		0x40U
#define HOTSPOT_NET_SYNC_AUDIO		0x20U
uint8_t hotspotNetFrameControl(uint8_t control, const uint8_t *frame);

// Provided by the user of the module
bool hotspotDataTxStart(const dmrBurst_t *bursts, int count);	// false if the radio can't start now
bool hotspotDataTxDone(void);									// the transmission started is over, sent or failed

#endif
