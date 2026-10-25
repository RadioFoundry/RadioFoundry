// SPDX-License-Identifier: MIT Copyright (c) 2026 The RadioFoundry Authors What the open-firmware CPS protocol shim
// (core/cps, docs/spec/open-firmware-cps.md) needs to know about the radio beyond the flash map: its identity in the
// radio info, and the memories behind the read-only areas. Optional: board.cps is NULL on radios without the shim
#ifndef HAL_CPS_H
#define HAL_CPS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	// The open-firmware radio type the radio info reports (open-firmware-cps.md 4), e.g. 8 for one radio with a BGR
	// panel
	uint32_t (*radio_type)(void);
	// Area 5 (MCU ROM): offset 0 is this address, reads past rom_size are refused; size 0 = area refused
	uint32_t rom_base;
	uint32_t rom_size;
	// Area 10 (flash security registers) by the chip's register address; NULL = area refused
	bool (*security_read)(uint32_t addr, void *buf, uint32_t len);
} hal_cps_t;

#endif
