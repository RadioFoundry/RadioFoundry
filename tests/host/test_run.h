// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// How a host test runs (header only): in parts that make -j runs side by side (tests/host/Makefile, parts), and
// short.
//
//   int main(int argc, char **argv)
//   {
//           if (!test_parts(argc, argv))      argv[1] K/N: run part K of N (0 <= K < N); no argument: all of it
//                   return 2;
//           if (test_part()) { ... }          the next unit of work, run when it falls in part K
//
// Units are dealt to the parts in turn (unit u is in part u % N), so a run of every part does all the work once and
// a run without an argument does it as before. A unit must not depend on state an earlier unit left.
//
//   for (unsigned i = 0; i < test_short(24000, 2400); i++)   the second count when RFOUNDRY_HOST_SHORT=1 (make
//                                                            HOST_SHORT=1, pull request CI): random cases only
#ifndef RFOUNDRY_TEST_RUN_H
#define RFOUNDRY_TEST_RUN_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_part_k, test_part_n = 1, test_part_unit;

static inline bool test_parts(int argc, char **argv)
{
	if (argc < 2)
		return true;
	if (sscanf(argv[1], "%d/%d", &test_part_k, &test_part_n) == 2 && test_part_n >= 1 && test_part_k >= 0 &&
	    test_part_k < test_part_n)
		return true;
	fprintf(stderr, "usage: %s [K/N]   part K (0 to N-1) of N\n", argv[0]);
	return false;
}

static inline bool test_part(void)
{
	return test_part_unit++ % test_part_n == test_part_k;
}

static inline unsigned test_short(unsigned full, unsigned brief)
{
	const char *e = getenv("RFOUNDRY_HOST_SHORT");
	return e != NULL && strcmp(e, "1") == 0 ? brief : full;
}

#endif
