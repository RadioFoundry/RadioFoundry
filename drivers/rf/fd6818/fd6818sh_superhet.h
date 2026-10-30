// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The FD6818 superhet driver's ops (rf/superhet/superhet.h); the chip pointer is an fd6818sh_t. The LO ops serve the
// 0x7813 too (its fd6818sh_t has the role FD6818SH_LO_7813)
#ifndef FD6818SH_SUPERHET_H
#define FD6818SH_SUPERHET_H

#include "rf/superhet/superhet.h"

extern const superhet_main_ops_t fd6818sh_superhet_main;
extern const superhet_lo_ops_t fd6818sh_superhet_lo;

#endif
