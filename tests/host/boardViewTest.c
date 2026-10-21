// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The debug link's board view section (core/debuglink/debuglink_view_board.c): the board's key=value lines under
// "board.", a bound on a board that never ends, a long line cut. Built twice: with a fake board hook, and with
// -DNO_HOOK for a board without one ("board.built=0")
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "debuglink/debuglink.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#ifdef NO_HOOK
#define NAME	"boardViewNoneTest"
#else
#define NAME	"boardViewTest"
static int mode;	// 0: three lines, 1: never ends, 2: one line longer than the section's line buffer

static bool view_line(uint8_t i, char *buf, uint8_t len)
{
	if (mode == 0) {
		static const char *const lines[] = { "clk.core_hz=95232000", "pins.e=0x04000000", "reset.cause=por" };
		if (i >= 3u) {
			return false;
		}
		snprintf(buf, len, "%s", lines[i]);
		return true;
	}
	if (mode == 1) {
		snprintf(buf, len, "n%u=%u", i, i);
		return true;
	}
	if (i > 0u) {
		return false;
	}
	memset(buf, 'k', len);	// no terminator: the section must cut it
	return true;
}
#endif

const board_t board = {
	.name = "test",
#ifndef NO_HOOK
	.view_line = view_line,
#endif
};

static size_t render(char *out, size_t cap)
{
	dl_text_t t;
	dl_text_init(&t, out, cap - 1u);
	debuglink_view_board(&t);
	out[t.len] = '\0';
	return t.len;
}

int main(void)
{
	char out[2048];
#ifdef NO_HOOK
	render(out, sizeof out);
	CHECK(strcmp(out, "board.built=0\n") == 0);
#else
	mode = 0;
	render(out, sizeof out);
	CHECK(strcmp(out, "board.built=1\nboard.clk.core_hz=95232000\nboard.pins.e=0x04000000\nboard.reset.cause=por\n") ==
	      0);

	mode = 1;	// a board that never says it is done: 64 lines at most
	render(out, sizeof out);
	CHECK(strstr(out, "board.n63=63\n") != NULL && strstr(out, "board.n64=") == NULL);

	mode = 2;	// cut to the line buffer, still one line
	render(out, sizeof out);
	char *line = strstr(out, "board.k");
	CHECK(line != NULL && strchr(line, '\n') != NULL && strlen(line) < 80u);
#endif
	if (failures) {
		printf(NAME ": %d failures\n", failures);
		return 1;
	}
	printf(NAME ": all passed\n");
	return 0;
}
