// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Ensure installed satellites have editable channels in the codeplug.
#ifndef SATELLITE_CHANNELS_H
#define SATELLITE_CHANNELS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	SATELLITE_CHANNELS_SYNC_IDLE,
	SATELLITE_CHANNELS_SYNC_RUNNING,
	SATELLITE_CHANNELS_SYNC_COMPLETE,
	SATELLITE_CHANNELS_SYNC_FAILED,
} satellite_channels_sync_status_t;

bool satellite_channels_sync(void);
bool satellite_channels_sync_schedule(void);
satellite_channels_sync_status_t satellite_channels_sync_status(void);

#endif