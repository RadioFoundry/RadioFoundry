// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The support info (docs/spec/support-info.md): where this build came from, and the radio's hardware identity, for the
// About page and the debug link's SUPPORT command. Read only when asked: what the board and the drivers already hold,
// never a bus transfer, so asking can't disturb the radio.
#ifndef SUPPORT_INFO_H
#define SUPPORT_INFO_H

#include <stdbool.h>
#include <stdint.h>
#include "ui/i18n.h"

#define SUPPORT_VALUE_MAX	32u

// Where the build came from (RFOUNDRY_GIT_SOURCE, tools/build_source.py)
typedef enum {
	SUPPORT_SRC_UNKNOWN,	// no git, or no repository to name
	SUPPORT_SRC_OFFICIAL,	// our CI from our repository: the images the web site installs
	SUPPORT_SRC_SERVICE,	// our build service (a custom build from the web installer)
	SUPPORT_SRC_PR,		// our CI, building a pull request
	SUPPORT_SRC_LOCAL,	// someone's build from a clone of our repository
	SUPPORT_SRC_FORK,	// a build from another repository
	SUPPORT_SRC_COUNT
} support_source_t;

support_source_t support_source(void);
const char *support_source_key(support_source_t s);	// the debug link's word: "official", "fork", ...
tr_id_t support_source_text(support_source_t s);	// About's words for it
const char *support_repo(void);				// "owner/repo", "" when not known

typedef struct {
	const char *key;		// the debug link's key: "mcu", "flash", "display", "dmr", "rf", "board", "hwopt"
	tr_id_t label;			// About's label, "Flash: " (TR_NONE: label_text)
	const char *label_text;		// a hardware option's name, as the board gives it
	char value[SUPPORT_VALUE_MAX];
} support_line_t;

// Hardware line i: the MCU, the data flash, the display panel, the board's chips (board_t.hw_line), then its stated
// hardware options. False past the last
bool support_hw_line(uint8_t i, support_line_t *l);

#endif
