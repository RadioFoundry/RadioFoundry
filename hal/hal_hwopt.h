// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Hardware options: facts about the radio that software can't read and a person has to state, such as which board
// revision a radio with several revisions has. The board keeps them itself, outside the settings record, so a
// settings reset doesn't lose them. Settings > Advanced > Hardware lists them and the debug link reads and sets them
// (HWOPT, docs/spec/debuglink.md 3.23). Optional: board.hwopts is NULL where the radio has none.
#ifndef HAL_HWOPT_H
#define HAL_HWOPT_H

#include <stdint.h>
#include <stdbool.h>

#define HWOPT_NONE	(-1)	// get(): nothing stored

typedef struct {
	const char *name;		// short menu label: "Board revision"
	uint8_t count;			// choices
	const char *const *choices;	// their labels
	int8_t (*get)(void);		// the stored choice, HWOPT_NONE when none is
	bool (*set)(uint8_t choice);	// store a choice; false: refused or not stored
	bool restart;			// a new choice takes effect at the next start-up
} hal_hwopt_t;

#endif
