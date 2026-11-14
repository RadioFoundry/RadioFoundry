// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The text editor's Back: one press removes one character, a Chinese name by its three UTF-8 bytes, an ASCII or
// Latin-1 name one byte at a time (core/ui/text_edit.h). The text layer's rule (gfx_utf8_next) is the same, so
// what Back removes is what the screen drew.
#include <stdio.h>
#include <string.h>
#include "ui/text_edit.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Back once over a text of len bytes; the result is NUL-terminated in buf, its length returned
static uint8_t back(const char *text, uint8_t len, char *buf)
{
	memcpy(buf, text, len);
	uint8_t n = text_edit_backspace(buf, len);
	buf[n] = '\0';
	return n;
}

static void one(const char *text, uint8_t len, const char *want, uint8_t want_len)
{
	char buf[64];
	uint8_t n = back(text, len, buf);
	if (n != want_len || memcmp(buf, want, n) != 0) {
		printf("FAIL %s:%d: Back over %u bytes from \"%s\" gave (%u) not (%u)\n",
		       __FILE__, __LINE__, len, text, n, want_len);
		failures++;
	}
}

int main(void)
{
	char buf[64];

	// Empty, and a single ASCII character
	one("", 0, "", 0);
	one("A", 1, "", 0);
	CHECK(back("", 0, buf) == 0 && buf[0] == '\0');

	// An ASCII name shortens by one byte on every press
	one("COUNTY_DMRGM", 12, "COUNTY_DMRG", 11);
	one("ABCD", 4, "ABC", 3);
	one("AB C", 4, "AB ", 3);

	// A Latin-1 byte is one character on its own, even in the continuation-byte range (a degree sign, 0xB0)
	one("A\xB0", 2, "A", 1);
	one("\xB0\xB1", 2, "\xB0", 1);
	one("A\xB7" "B", 3, "A\xB7", 2);
	// A font glyph byte (0x80 = ellipsis) is one character too
	one("A\x80", 2, "A", 1);
	// "Café": 0xE9 has no continuations, so it is one Latin-1 byte
	one("Caf\xE9", 4, "Caf", 3);
	// "à la": 0xE0 followed by a space, one byte at a time
	one("\xE0 la", 4, "\xE0 l", 3);
	// A 3-byte lead with no continuations (a cut-off name) is one byte
	one("AB\xE4", 3, "AB", 2);

	// A Chinese name goes in one press, by all three UTF-8 bytes
	one("\xE4\xB8\xAD", 3, "", 0);				// "中"
	one("A\xE4\xB8\xAD", 4, "A", 1);			// "A中"
	one("\xE4\xB8\xAD" "A", 4, "\xE4\xB8\xAD", 3);		// "中A"
	one("\xE4\xB8\xAD\xE9\x81\x93", 6, "\xE4\xB8\xAD", 3);	// "中道" -> "中"
	one("\xE4\xB8\xAD\xB0", 4, "\xE4\xB8\xAD", 3);		// "中°" -> "中"
	one("A\xE4\xB8\xAD" "B\xB0", 6, "A\xE4\xB8\xAD" "B", 5);

	// An overlong form isn't UTF-8 (gfx_utf8_next reads it byte by byte): Back removes one byte
	one("\xE0\x80\x80", 3, "\xE0\x80", 2);
	// A trailing run of continuation-looking Latin-1 bytes after a lead is decoded as one sequence, exactly as
	// the text layer draws it: all three bytes go together
	one("A\xE9\xB0\xB1", 4, "A", 1);

	printf(failures ? "# textEditTest FAILED (%d)\n" : "# textEditTest PASSED\n", failures);
	return failures ? 1 : 0;
}
