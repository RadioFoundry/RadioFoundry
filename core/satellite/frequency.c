// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stddef.h>
#include "satellite/frequency.h"
#include "satellite/store.h"

static const satellite_frequency_profile_t profile_catalog[] = {
	{ 25544u, "ISS", 437800000u, 145990000u, 670u },
	{ 27607u, "SO-50", 436795000u, 145850000u, 670u },
	{ 40908u, "LILACSAT-2", 437200000u, 144350000u, 0u },
	{ 40931u, "IO-86", 435880000u, 145880000u, 885u },
	{ 43017u, "AO-91", 145960000u, 435250000u, 0u },
	{ 43678u, "PO-101", 145900000u, 437500000u, 1413u },
	{ 61781u, "AO-123", 435400000u, 145850000u, 670u },
	{ 63213u, "TEVEL2-4", 436400000u, 145970000u, 0u },
	{ 63214u, "TEVEL2-5", 436400000u, 145970000u, 0u },
	{ 63215u, "TEVEL2-6", 436400000u, 145970000u, 0u },
	{ 63217u, "TEVEL2-1", 436400000u, 145970000u, 0u },
	{ 63218u, "TEVEL2-3", 436400000u, 145970000u, 0u },
	{ 63219u, "TEVEL2-2", 436400000u, 145970000u, 0u },
	{ 63237u, "TEVEL2-9", 436400000u, 145970000u, 0u },
	{ 63238u, "TEVEL2-7", 436400000u, 145970000u, 0u },
	{ 63239u, "TEVEL2-8", 436400000u, 145970000u, 0u },
	{ 67291u, "RS95S", 436950000u, 145920000u, 670u },
};

uint8_t satellite_frequency_profile_catalog_count(void)
{
	return (uint8_t)(sizeof(profile_catalog) / sizeof(profile_catalog[0]));
}

bool satellite_frequency_profile_catalog_get(uint8_t index, satellite_frequency_profile_t *profile)
{
	if (profile == NULL || index >= satellite_frequency_profile_catalog_count())
		return false;
	*profile = profile_catalog[index];
	return true;
}

bool satellite_frequency_profile(uint32_t catalog_id, satellite_frequency_profile_t *profile)
{
	if (profile == NULL || !satellite_store_profile_get(catalog_id, profile))
		return false;
	for (uint8_t i = 0u; i < satellite_frequency_profile_catalog_count(); i++)
		if (profile_catalog[i].catalog_id == catalog_id) {
			profile->name = profile_catalog[i].name;
			break;
		}
	return true;
}

bool satellite_frequency_needs_arm(uint32_t catalog_id)
{
	return catalog_id == 27607u;
}