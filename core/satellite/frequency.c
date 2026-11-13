// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stddef.h>
#include "satellite/frequency.h"

static const satellite_frequency_profile_t fm_profiles[] = {
	{ 25544u, 437800000u, 145990000u, 670u },
	{ 27607u, 436795000u, 145850000u, 670u },
	{ 40908u, 437200000u, 144350000u, 0u },
	{ 40931u, 435880000u, 145880000u, 885u },
	{ 43017u, 145960000u, 435250000u, 0u },
	{ 43678u, 145900000u, 437500000u, 1413u },
	{ 61781u, 435400000u, 145850000u, 670u },
	{ 63213u, 436400000u, 145970000u, 0u },
	{ 63214u, 436400000u, 145970000u, 0u },
	{ 63215u, 436400000u, 145970000u, 0u },
	{ 63217u, 436400000u, 145970000u, 0u },
	{ 63218u, 436400000u, 145970000u, 0u },
	{ 63219u, 436400000u, 145970000u, 0u },
	{ 63237u, 436400000u, 145970000u, 0u },
	{ 63238u, 436400000u, 145970000u, 0u },
	{ 63239u, 436400000u, 145970000u, 0u },
	{ 67291u, 436950000u, 145920000u, 670u },
};

bool satellite_frequency_profile(uint32_t catalog_id, satellite_frequency_profile_t *profile)
{
	if (profile == NULL)
		return false;
	for (uint8_t i = 0u; i < sizeof(fm_profiles) / sizeof(fm_profiles[0]); i++) {
		if (fm_profiles[i].catalog_id != catalog_id)
			continue;
		*profile = fm_profiles[i];
		return true;
	}
	return false;
}