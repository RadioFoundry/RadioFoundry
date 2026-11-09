// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// This board's own build switches (config.mk passes them; a host test passes the ones it builds with). They aren't
// feature switches: no other radio has them, so they stay out of config/features.mk and core/config.h
#ifndef CONFIG_MD9600_H
#define CONFIG_MD9600_H

// The synthesizer driver per board revision: V1-V3 (SKY72310 and the AK2365A IF receivers), V4 (the 24-bit "PLL1590"
// part and the AK2365A), V4A / V5 (LMX2571)
#ifndef CONFIG_MD9600_V1V3
#define CONFIG_MD9600_V1V3	0
#endif
#ifndef CONFIG_MD9600_V4
#define CONFIG_MD9600_V4	0
#endif
#ifndef CONFIG_MD9600_V5
#define CONFIG_MD9600_V5	0
#endif
// The transmit sequence (rfseq_md9600.c), and the HR_C6000's per-burst RF keying for DMR transmit
#ifndef CONFIG_MD9600_TX
#define CONFIG_MD9600_TX	0
#endif
#ifndef CONFIG_MD9600_DMR_TX
#define CONFIG_MD9600_DMR_TX	0
#endif

#endif
