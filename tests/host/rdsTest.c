// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The RDS / RBDS group decoder (core/radio/rds.c; airband-and-broadcast.md 4.6): groups built as the standard lays
// them out (IEC 62106: PS in 0A / 0B, RadioText in 2A / 2B), with the receiver's block error levels
#include <stdio.h>
#include <string.h>
#include "radio/rds.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define PI	0xC201u
#define PTY	10u

static uint16_t blk_b(unsigned type, unsigned ver_b, unsigned low5)
{
	return (uint16_t)((type << 12) | (ver_b << 11) | (1u << 10) | (PTY << 5) | (low5 & 0x1Fu));
}

// Group 0A: PS segment seg (2 characters of s at seg * 2), TA ta
static bool ps(rds_t *r, uint16_t pi, unsigned seg, const char *s, unsigned ta)
{
	const uint16_t g[4] = { pi, blk_b(0, 0, (ta << 4) | seg), 0xE0CDu,
				(uint16_t)(((uint8_t)s[seg * 2] << 8) | (uint8_t)s[seg * 2 + 1]) };
	return rds_group(r, g, RDS_ERR_NONE, RDS_ERR_NONE);
}

static bool ps_all(rds_t *r, const char *s)
{
	bool changed = false;
	for (unsigned seg = 0; seg < 4; seg++) {
		changed = ps(r, PI, seg, s, 0) || changed;
	}
	return changed;
}

// Group 2A: 4 characters of a 64-character text at seg * 4; 2B: 2 of 32 at seg * 2
static bool rt2a(rds_t *r, unsigned ab, unsigned seg, const char *s)
{
	const char *c = s + seg * 4;
	const uint16_t g[4] = { PI, blk_b(2, 0, (ab << 4) | seg), (uint16_t)(((uint8_t)c[0] << 8) | (uint8_t)c[1]),
				(uint16_t)(((uint8_t)c[2] << 8) | (uint8_t)c[3]) };
	return rds_group(r, g, RDS_ERR_NONE, RDS_ERR_NONE);
}

static bool rt2b(rds_t *r, unsigned ab, unsigned seg, const char *s)
{
	const char *c = s + seg * 2;
	const uint16_t g[4] = { PI, blk_b(2, 1, (ab << 4) | seg), PI, (uint16_t)(((uint8_t)c[0] << 8) | (uint8_t)c[1]) };
	return rds_group(r, g, RDS_ERR_NONE, RDS_ERR_NONE);
}

static void rt2a_all(rds_t *r, unsigned ab, const char *s, unsigned segs)
{
	for (unsigned seg = 0; seg < segs; seg++) {
		rt2a(r, ab, seg, s);
	}
}

static void test_ps(void)
{
	rds_t r;
	rds_reset(&r);
	CHECK(ps_all(&r, "RADIO 2 "));			// the PI and the program type
	CHECK(r.info.ps[0] == 0);			// the name heard once: not yet
	CHECK(r.info.pi == PI && r.info.pty == PTY && r.info.tp);
	CHECK(ps_all(&r, "RADIO 2 "));
	CHECK(strcmp(r.info.ps, "RADIO 2") == 0);	// trailing space dropped
	CHECK(!ps_all(&r, "RADIO 2 "));			// nothing new
	// One wrong block D: heard once, never shown; the right one keeps the name
	CHECK(!ps(&r, PI, 1, "RAXIO 2 ", 0));
	CHECK(strcmp(r.info.ps, "RADIO 2") == 0);
	CHECK(!ps(&r, PI, 1, "RADIO 2 ", 0));
	// A new name segment by segment, each twice (a dynamic PS)
	CHECK(!ps(&r, PI, 0, "NEWS    ", 0));
	CHECK(ps(&r, PI, 0, "NEWS    ", 0) && strcmp(r.info.ps, "NEDIO 2") == 0);
	// The traffic announcement flag
	CHECK(ps(&r, PI, 2, "NEWS    ", 1) && r.info.ta);
	CHECK(r.info.groups == 17);
}

static void test_errors(void)
{
	rds_t r;
	rds_reset(&r);
	ps_all(&r, "ABCDEFGH");
	// Block B in doubt: the group is dropped
	uint16_t g[4] = { PI, blk_b(0, 0, 0), 0, ('X' << 8) | 'Y' };
	CHECK(!rds_group(&r, g, RDS_ERR_NONE, RDS_ERR_LARGE));
	CHECK(!rds_group(&r, g, RDS_ERR_NONE, RDS_ERR_BAD));
	CHECK(r.info.groups == 4 && r.ps_heard[0] == 'A');
	// Block A bad: the rest is still read (the PI is all it carries)
	uint16_t g2[4] = { 0x1234u, blk_b(0, 0, 3), 0, ('G' << 8) | 'H' };
	CHECK(rds_group(&r, g2, RDS_ERR_BAD, RDS_ERR_SMALL) == false && r.info.pi == PI);
	for (unsigned seg = 0; seg < 3; seg++)
		ps(&r, PI, seg, "ABCDEFGH", 0);
	CHECK(strcmp(r.info.ps, "ABCDEFGH") == 0);
	// Characters without a glyph
	rds_reset(&r);
	ps_all(&r, "A\x01" "B\x91" "CDEF");
	ps_all(&r, "A\x01" "B\x91" "CDEF");
	CHECK(strcmp(r.info.ps, "A B?CDEF") == 0);
}

static void test_pi(void)
{
	rds_t r;
	rds_reset(&r);
	ps(&r, PI, 0, "ONE     ", 0);
	CHECK(r.info.pi == 0);				// once: not yet
	CHECK(ps(&r, PI, 1, "ONE     ", 0) && r.info.pi == PI);
	ps_all(&r, "ONE     ");
	ps_all(&r, "ONE     ");
	CHECK(strcmp(r.info.ps, "ONE") == 0);
	// Another station once (an error in block A that slipped through): its group is dropped
	CHECK(!ps(&r, 0x5555u, 0, "TWO     ", 0));
	CHECK(r.info.pi == PI && strcmp(r.info.ps, "ONE") == 0);
	CHECK(!ps(&r, PI, 0, "ONE     ", 0));
	// Twice in a row: a new station, nothing of the old one stays
	CHECK(!ps(&r, 0x5555u, 0, "TWO     ", 0));
	CHECK(ps(&r, 0x5555u, 1, "TWO     ", 0));
	CHECK(r.info.pi == 0x5555u && r.info.ps[0] == 0 && r.info.rt[0] == 0);
}

static void test_rt(void)
{
	rds_t r;
	rds_reset(&r);
	static const char text[] = "Now playing: a song\r";	// 20 characters: segments 0-4, the end in 4
	rt2a_all(&r, 0, text, 5);
	CHECK(r.info.rt[0] == 0);
	rt2a_all(&r, 0, text, 4);
	CHECK(r.info.rt[0] == 0);			// the segment with the end still once
	CHECK(rt2a(&r, 0, 4, text));
	CHECK(strcmp(r.info.rt, "Now playing: a song") == 0);
	// The A/B flag toggles: a new text, the old one goes at once
	CHECK(rt2a(&r, 1, 0, "Next"));
	CHECK(r.info.rt[0] == 0);
	// A full 64 characters, no end mark, spaces at the end dropped
	char full[65];
	memset(full, ' ', 64);
	memcpy(full, "Traffic on the ring road", 24);
	full[64] = 0;
	rt2a_all(&r, 1, full, 16);
	rt2a_all(&r, 1, full, 15);
	CHECK(r.info.rt[0] == 0);
	CHECK(rt2a(&r, 1, 15, full));
	CHECK(strcmp(r.info.rt, "Traffic on the ring road") == 0);
	// 2B: 32 characters, 2 a group
	char t2b[33];
	memset(t2b, ' ', 32);
	memcpy(t2b, "Version B text\r", 15);
	t2b[32] = 0;
	for (int k = 0; k < 2; k++) {
		for (unsigned seg = 0; seg < 8; seg++)
			rt2b(&r, 0, seg, t2b);
	}
	CHECK(strcmp(r.info.rt, "Version B text") == 0);
	// A wrong block C or D heard once never shows; the text heard twice does
	rds_reset(&r);
	rt2a(&r, 0, 0, "Xbcd\r   ");
	rt2a(&r, 0, 0, "abcd\r   ");
	rt2a(&r, 0, 1, "abcd\r   ");
	rt2a(&r, 0, 1, "abcd\r   ");
	CHECK(r.info.rt[0] == 0);
	CHECK(rt2a(&r, 0, 0, "abcd\r   ") && strcmp(r.info.rt, "abcd") == 0);
	CHECK(r.info.groups == 5);
	// A line feed shows as a space
	rds_reset(&r);
	rt2a_all(&r, 0, "AB\nC\r   ", 2);
	rt2a_all(&r, 0, "AB\nC\r   ", 2);
	CHECK(strcmp(r.info.rt, "AB C") == 0);
}

int main(void)
{
	test_ps();
	test_errors();
	test_pi();
	test_rt();
	printf("rdsTest: %d checks, %s\n", checks, failures ? "FAILED" : "all passed");
	return failures != 0;
}
