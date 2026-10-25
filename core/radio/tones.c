// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Standard CTCSS tones and DCS codes, and the tone picker order
#include "radio/tones.h"

#if CONFIG_SUBTONE

const uint16_t tones_ctcss[TONES_CTCSS_COUNT] = {
	670, 693, 719, 744, 770, 797, 825, 854, 885, 915,
	948, 974, 1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
	1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679,
	1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966, 1995,
	2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};

const uint16_t tones_dcs[TONES_DCS_COUNT] = {
	0023, 0025, 0026, 0031, 0032, 0036, 0043, 0047, 0051, 0053, 0054, 0065, 0071, 0072, 0073, 0074,
	0114, 0115, 0116, 0122, 0125, 0131, 0132, 0134, 0143, 0145, 0152, 0155, 0156, 0162, 0165, 0172,
	0174, 0205, 0212, 0223, 0225, 0226, 0243, 0244, 0245, 0246, 0251, 0252, 0255, 0261, 0263, 0265,
	0266, 0271, 0274, 0306, 0311, 0315, 0325, 0331, 0332, 0343, 0346, 0351, 0356, 0364, 0365, 0371,
	0411, 0412, 0413, 0423, 0431, 0432, 0445, 0446, 0452, 0454, 0455, 0462, 0464, 0465, 0466, 0503,
	0506, 0516, 0523, 0526, 0532, 0546, 0565, 0606, 0612, 0624, 0627, 0631, 0632, 0654, 0662, 0664,
	0703, 0712, 0723, 0731, 0732, 0734, 0743, 0754,
};

#define CTCSS_MIN	600u
#define CTCSS_MAX	2600u

bool tones_valid(const subtone_t *t)
{
	switch (t->type) {
	case TONE_NONE: return true;
	case TONE_CTCSS: return t->value >= CTCSS_MIN && t->value <= CTCSS_MAX;
	case TONE_DCS:
	case TONE_DCS_INV: return t->value <= 0777u;
	default: return false;
	}
}

// Sort key along the picker order: 0 none, then CTCSS (1 + Hz x 10), then DCS normal, then DCS inverted
static uint32_t key(const subtone_t *t)
{
	switch (t->type) {
	case TONE_CTCSS: return 1u + t->value;
	case TONE_DCS: return 0x10000u + t->value;
	case TONE_DCS_INV: return 0x20000u + t->value;
	default: return 0;
	}
}

subtone_t tones_picker(uint16_t index)
{
	subtone_t t = { TONE_NONE, 0 };
	if (index == 0 || index >= TONES_PICKER_COUNT) {
		return t;
	}
	index--;
	if (index < TONES_CTCSS_COUNT) {
		t.type = TONE_CTCSS;
		t.value = tones_ctcss[index];
		return t;
	}
	index -= TONES_CTCSS_COUNT;
	t.type = index < TONES_DCS_COUNT ? TONE_DCS : TONE_DCS_INV;
	t.value = tones_dcs[index % TONES_DCS_COUNT];
	return t;
}

// The last picker entry whose key is <= t's (0 at worst: none), and whether it is t exactly
static uint16_t floor_index(const subtone_t *t, bool *exact)
{
	uint32_t k = t->type == TONE_NONE ? 0 : key(t);
	uint16_t best = 0;
	for (uint16_t i = 1; i < TONES_PICKER_COUNT; i++) {
		subtone_t p = tones_picker(i);
		if (key(&p) > k) {
			break;
		}
		best = i;
	}
	subtone_t b = tones_picker(best);
	*exact = key(&b) == k;
	return best;
}

bool tones_standard(const subtone_t *t)
{
	bool exact;
	floor_index(t, &exact);
	return exact;
}

subtone_t tones_step(const subtone_t *t, int dir)
{
	bool exact;
	int i = floor_index(t, &exact);
	if (dir > 0) {
		i++;
	} else if (exact) {
		i--;
	}
	i = (i % TONES_PICKER_COUNT + TONES_PICKER_COUNT) % TONES_PICKER_COUNT;
	return tones_picker((uint16_t)i);
}

#endif
