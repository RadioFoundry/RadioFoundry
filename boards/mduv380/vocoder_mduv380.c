// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-UV380 vocoder: the radio's own AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32 radios";
// the codec: docs/spec/codec-mduv380.md). The merge on the user's machine (rfoundry, tools/codec_extract.py, the web
// flasher) writes the codec's tables at their address in the TYT image, its code right after them and the descriptor at
// the end of sector 11 (platform/stm32f4/codec_desc.h). The calls and the checks are the shared ones
// (platform/stm32f4/hrc/vocoder.c); this file says which builds and where. Without a valid descriptor and codec,
// present() is false and the radio is FM only.
#include "../uv380_family/uvf.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

static const stm32_codec_profile_t *const profiles[] = { NULL };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 0 };

#endif	// CONFIG_DMR
