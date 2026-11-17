// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// See codeplug_copy_stub.h
#include "codeplug/codeplug.h"
#include "codeplug_copy_stub.h"

uint16_t sim_tag_copy_from;

bool codeplug_channel_name(uint16_t number, char out[CODEPLUG_NAME_LEN + 1])
{
	sim_tag_copy_from = number;
	out[0] = '\0';
	return false;
}

bool codeplug_base_copy(uint16_t from, uint16_t to)
{
	(void)from;
	(void)to;
	return false;
}
