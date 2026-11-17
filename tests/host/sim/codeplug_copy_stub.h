// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug store's reads that the tag copy (codeplug_tag_copy) makes, for tests that link the tag layer over a
// fake codeplug: the copy reads its source's name first, which says it was asked for and from where. Nothing more is
// copied: the fake store has no tags
#pragma once
#include <stdint.h>

extern uint16_t sim_tag_copy_from;	// the last copy's source (0: none since the test cleared it)
