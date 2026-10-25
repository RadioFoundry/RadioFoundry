// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 clock rates, without the register map, so FreeRTOSConfig.h can read them. APB peripherals run from sys / 2
// (hardware-dm32.md 1.1). HRC_CPU_HZ is the clock the application runs at: 168 MHz unless the board sets it in board.mk.
// The loader in front of it may leave the PLL at another rate: HRC_LOADER_CPU_HZ (default the same). The Baofeng
// bootloader leaves 168 MHz, another radio's IAP loader 72 MHz. When the two differ, clock.c reprograms the PLL at
// start-up and hrc_clock_restore_loader() puts the loader's clocks back before a hand-over to RAM code built for them
// (the runtime stub) or a soft reset
#ifndef PLATFORM_HRC7000_CLOCK_HZ_H
#define PLATFORM_HRC7000_CLOCK_HZ_H

#ifndef HRC_CPU_HZ
#define HRC_CPU_HZ		168000000u
#endif
#ifndef HRC_LOADER_CPU_HZ
#define HRC_LOADER_CPU_HZ	HRC_CPU_HZ
#endif
#define HRC_APB_HZ		(HRC_CPU_HZ / 2u)
#define HRC_LOADER_APB_HZ	(HRC_LOADER_CPU_HZ / 2u)

#endif
