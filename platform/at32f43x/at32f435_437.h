// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What TinyUSB's dwc2 driver reads from Artery's device header (dwc2_at32.h includes <at32f435_437.h>): the OTGFS
// interrupts and the core clock. Found on the include path in place of Artery's header, which the tree doesn't carry
#ifndef RFOUNDRY_AT32F435_437_SHIM_H
#define RFOUNDRY_AT32F435_437_SHIM_H

#include "at32f43x.h"

extern uint32_t SystemCoreClock;
#define system_core_clock	SystemCoreClock

#endif
