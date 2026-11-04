// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MD-380's panel type choice (boards/md380/panel_md380.c, hardware-md380.md 7): the controller from its ID, the
// orientation from the factory byte where it was programmed
#include <stdio.h>
#include <string.h>
#include "panel_md380.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const uint8_t GC9102[3] = { 0x00, 0x91, 0x02 };	// the bench radio's answer (2026-10-27)
static const uint8_t ST7735S[3] = { 0x7C, 0x89, 0xF0 };
static const uint8_t NONE[3] = { 0x00, 0x00, 0x00 };
static const uint8_t BLANK[3] = { 0xFF, 0xFF, 0xFF };

static uint8_t type_of(int16_t factory, const uint8_t id[3])
{
	return md380_panel_detect(factory, id).type;
}

static bool is(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

int main(void)
{
	md380_panel_t p;

	// GC9xxx: normal unless the byte says flipped (bit 0 clear), in either stock layout
	CHECK(type_of(0xFF, GC9102) == 3);	// the bench radio: erased byte
	CHECK(type_of(-1, GC9102) == 3);
	CHECK(type_of(3, GC9102) == 3);
	CHECK(type_of(2, GC9102) == 2);
	CHECK(type_of(1, GC9102) == 3);	// early stock: 1 = normal
	CHECK(type_of(0, GC9102) == 2);	// early stock: 0 = flipped
	CHECK(type_of(0xFE, GC9102) == 2);	// only the low bits count
	CHECK(type_of(0xFF, (const uint8_t[3]){ 0x00, 0x91, 0x06 }) == 3);	// another GC91xx

	// ST7735: normal unless the byte says rotated (1)
	CHECK(type_of(0xFF, ST7735S) == 0);
	CHECK(type_of(-1, ST7735S) == 0);
	CHECK(type_of(0, ST7735S) == 0);
	CHECK(type_of(1, ST7735S) == 1);
	CHECK(type_of(3, ST7735S) == 0);	// a GC9xxx type on an ST7735: its init, not the other's

	// Unknown or unread ID: the factory byte, type 0 when it says nothing
	CHECK(type_of(0xFF, NONE) == 0);
	CHECK(type_of(-1, BLANK) == 0);
	CHECK(type_of(2, NONE) == 2);
	CHECK(type_of(0x07, BLANK) == 3);

	// What the debug link shows: the controller found and what chose the type
	p = md380_panel_detect(0xFF, GC9102);
	CHECK(is(p.controller, "gc9xxx") && is(p.source, "id"));
	p = md380_panel_detect(1, ST7735S);
	CHECK(is(p.controller, "st7735") && is(p.source, "id"));
	p = md380_panel_detect(2, NONE);
	CHECK(is(p.controller, "unknown") && is(p.source, "factory"));
	p = md380_panel_detect(0xFF, BLANK);
	CHECK(is(p.controller, "unknown") && is(p.source, "default"));

	printf("md380PanelTest: %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
