// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Hardware options: facts about the radio a person may have to state because software can't always read them, such as
// which board revision a radio with several revisions has. The board keeps them itself, outside the settings record,
// so a settings reset doesn't lose them. Settings > Advanced > Hardware lists them and the debug link reads and sets
// them (HWOPT, docs/spec/debuglink.md 3.25). Optional: board.hwopts is NULL where the radio has none.
//
// A board that can detect the fact reports it through state(): a detected option is hidden, and only an option that
// still needs an answer makes the radio ask for it (the home screen, a refused PTT) and the installers prompt.
#ifndef HAL_HWOPT_H
#define HAL_HWOPT_H

#include <stdint.h>
#include <stdbool.h>

#define HWOPT_NONE	(-1)	// get(): nothing stored

// state(): what the radio needs from a person for this option
typedef enum {
	HWOPT_ST_OK,		// known (stored, or detected and still shown so a person can correct it): nothing to do
	HWOPT_ST_ASK,		// unknown: the radio can't do everything until a person states it (transmit, typically)
	HWOPT_ST_RESTART,	// a choice is stored that applies at the next start-up
	HWOPT_ST_HIDDEN,	// the board detected it: nothing to state, not listed
} hwopt_state_t;

typedef struct {
	const char *name;		// short menu label: "Board revision"
	uint8_t count;			// choices
	const char *const *choices;	// their labels
	int8_t (*get)(void);		// the stored choice, HWOPT_NONE when none is
	bool (*set)(uint8_t choice);	// store a choice; false: refused or not stored
	bool restart;			// a new choice takes effect at the next start-up
	// Optional: hwopt_state_t now. NULL: ASK while nothing is stored, else OK
	uint8_t (*state)(void);
} hal_hwopt_t;

// The option's state (state() or the stored value)
static inline uint8_t hwopt_state(const hal_hwopt_t *o)
{
	if (o->state != NULL) {
		return o->state();
	}
	return o->get() == HWOPT_NONE ? HWOPT_ST_ASK : HWOPT_ST_OK;
}

#endif
