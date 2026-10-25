// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host test stubs for functions/dmrDataService.c: just enough of the firmware (trx, HR-C6000, settings, UI, sound,
 * USB) for the service to compile on the host. Every header the service includes from the firmware trees is a file in
 * this directory that includes this one; the definitions are in dmrDataServiceTest.c
 */

#ifndef _DMR_DATA_SERVICE_STUBS_H_
#define _DMR_DATA_SERVICE_STUBS_H_

#include <stdbool.h>
#include <stdint.h>
#include "dmr/dmrData.h"

// ticks
uint32_t ticksGetMillis(void);

// trx
#define RADIO_MODE_DIGITAL				2
#define DMR_MODE_RMO					1
#define BAND_LIMITS_NONE				0
extern uint32_t trxDMRID;
extern uint32_t trxTalkGroupOrPcId;
extern int trxDMRModeRx;
int trxGetMode(void);
bool trxCheckFrequencyInAmateurBand(uint32_t freq);
bool trxCarrierDetected(void);
int trxGetDMRTimeSlot(void);
int trxGetDMRColorCode(void);

// HR-C6000
#define DMR_DATA_TX_RUNNING				1
int HRC6000DataTxGetStatus(void);
bool HRC6000DataTxStart(const dmrBurst_t *bursts, int count);
uint32_t HRC6000GetReceivedSrcId(void);

// settings and codeplug
#define USB_MODE_HOTSPOT				1
#define BIT_USB_NETWORK					1
#define CHANNEL_FLAG_RX_ONLY			1
typedef struct { uint32_t txFreq; } struct_codeplugChannel_t;
typedef struct { int NOT_IN_CODEPLUG_numTGsInGroup; uint32_t NOT_IN_CODEPLUG_contactsTG[32]; } struct_codeplugRxGroup_t;
#define BIT_DMR_ARS						2
typedef struct { int txFreqLimited; uint32_t bitfieldOptions; uint8_t dmrArsId[3]; int32_t magicNumber;
					uint8_t gpsReportDest[3]; uint16_t gpsReportInterval; uint8_t gpsReportFlags; } settingsStruct_t;
#define GPS_REPORT_FLAG_GROUP		0x01
#define GPS_REPORT_FLAG_AFTER_CALL	0x02
#define GPS_REPORT_ID_GET()		((uint32_t)((nonVolatileSettings.gpsReportDest[0] << 16) | (nonVolatileSettings.gpsReportDest[1] << 8) | nonVolatileSettings.gpsReportDest[2]))
extern settingsStruct_t originalNonVolatileSettings;
bool settingsIsOptionBitSetFromSettings(settingsStruct_t *sets, int bit);
extern int settingsUsbMode;
extern settingsStruct_t nonVolatileSettings;
extern struct_codeplugChannel_t *currentChannelData;
extern struct_codeplugRxGroup_t currentRxGroupData;
int codeplugChannelGetFlag(struct_codeplugChannel_t *channel, int flag);
bool settingsIsOptionBitSet(int bit);
void settingsSetOptionBit(int bit, bool set);
void settingsSetDirty(void);

// UI, sound, power saving
#define MAX_DMR_ID_CONTACT_TEXT_LENGTH	20
#define NOTIFICATION_MESSAGE_LEN_MAX	64
#define NOTIFICATION_TYPE_MESSAGE		0
#define NOTIFICATION_ID_MESSAGE			0
#define CONTACT_CALLTYPE_PC				1
#define ECOPHASE_POWERSAVE_INACTIVE		0
#define COM_REQUESTBUFFER_SIZE			64
typedef struct { char text[MAX_DMR_ID_CONTACT_TEXT_LENGTH]; } dmrIdDataStruct_t;
typedef struct { uint32_t userDMRId; } uiDataGlobal_t;
extern uiDataGlobal_t uiDataGlobal;
extern const int16_t MELODY_PRIVATE_CALL[];
void soundSetMelody(const int16_t *melody);
bool contactIDLookup(uint32_t id, int callType, char *name);
bool dmrIDLookup(uint32_t id, dmrIdDataStruct_t *record);
void displayLightTrigger(bool on);
void uiNotificationShow(int type, int id, uint32_t ms, const char *message, bool immediate);
void rxPowerSavingSetState(int state);

#endif
