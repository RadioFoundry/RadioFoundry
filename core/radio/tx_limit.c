// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The test TX limit (tx_limit.h; docs/spec/radio-features.md 3.1)
#include "radio/tx_limit.h"
#include <stddef.h>
#if CONFIG_DEBUGLINK	// the whole file: only the debug link sets it

static volatile bool block;
static volatile uint8_t count;
static volatile uint32_t list[TX_LIMIT_MAX];
static uint32_t refused, last_hz;

bool tx_limit_set(bool b, const uint32_t *hz, uint8_t n)
{
	if (n > TX_LIMIT_MAX) {
		return false;
	}
	block = true;		// nothing keys while the list changes
	count = 0;
	for (uint8_t i = 0; i < n; i++) {
		list[i] = hz[i];
	}
	count = n;
	block = b;
	return true;
}

bool tx_limit_allows(uint32_t hz)
{
	if (block) {
		return false;
	}
	uint8_t n = count;
	if (n == 0) {
		return true;		// off
	}
	for (uint8_t i = 0; i < n; i++) {
		if (list[i] == hz) {
			return true;
		}
	}
	return false;
}

void tx_limit_refused(uint32_t hz)
{
	refused++;
	last_hz = hz;
}

void tx_limit_get(tx_limit_info_t *info, uint32_t *hz, uint8_t cap)
{
	uint8_t n = count;
	info->block = block;
	info->n = n;
	info->on = info->block || n != 0;
	info->refused = refused;
	info->last_hz = last_hz;
	for (uint8_t i = 0; hz != NULL && i < n && i < cap; i++) {
		hz[i] = list[i];
	}
}
#endif
