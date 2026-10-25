// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The board view section (docs/spec/debuglink.md 3.7): what the board reports about its own hardware through
// board.view_line, for bring-up and the HIL tests of a radio being ported (pin levels, the clock plan, chip IDs). The
// keys are the board's, under "board."; the core knows nothing of them.
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"

#define LINE_MAX	64	// one key=value line from the board
#define LINES_MAX	64	// a bound on a board that never says it is done

void debuglink_view_board(dl_text_t *t)
{
	char line[LINE_MAX];

	if (board.view_line == NULL) {
		dl_text(t, "board.built=0\n");
		return;
	}
	dl_text(t, "board.built=1\n");
	for (uint8_t i = 0; i < LINES_MAX && board.view_line(i, line, sizeof line); i++) {
		line[sizeof line - 1u] = '\0';
		dl_text(t, "board.%s\n", line);
	}
}
