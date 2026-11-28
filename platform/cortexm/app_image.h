// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The application image's span in program flash, for the support info's image hash (hal_system_t.image,
// docs/spec/support-info.md 3): from the linker's __app_base (board.mk APP_BASE) to __image_end, the end of the
// last section the .bin carries (the platform's common.ld)
#ifndef CORTEXM_APP_IMAGE_H
#define CORTEXM_APP_IMAGE_H

#include <stdint.h>

extern const uint8_t __app_base[], __image_end[];

static inline void cortexm_image(uint32_t *addr, uint32_t *len)
{
	*addr = (uint32_t)(uintptr_t)__app_base;
	*len = (uint32_t)(__image_end - __app_base);
}

#endif
