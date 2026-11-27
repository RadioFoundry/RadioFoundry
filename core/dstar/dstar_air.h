// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV as an air interface profile (hal_air.h, docs/spec/dstar-modem.md 2.7): 4800 symbols/s, two levels,
// Gaussian at BT 0.5, +-1200 Hz, the frame sync once after the preamble and the resync pattern every 21st frame. The
// GMSK modulator (dstar_gmsk.c) runs on the soft modem's transmitter with it (air_softmodem_tx).
#ifndef RFOUNDRY_DSTAR_AIR_H
#define RFOUNDRY_DSTAR_AIR_H

#include "../../hal/hal_air.h"

// air_sync_t.id of the two patterns
enum { DSTAR_AIR_SYNC_RESYNC = 0, DSTAR_AIR_SYNC_FRAME = 1 };

extern const air_profile_t dstar_air_profile;

#endif
