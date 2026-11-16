// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What this build is: the switches it has and its preset, as VERSION reports them (docs/spec/debuglink.md 3.1). Shared by
// the debug link and the stub entry's listener of a build without it, so the web tools identify a build either way.
#ifndef BUILD_INFO_H
#define BUILD_INFO_H

// features=: a comma list of the switches the tools look for (not every switch), no newline. P25 only where the board
// runs it (radio_p25_fits), so the list can differ between boards of one build
const char *build_features_now(void);
// preset=: the release variant (full, amateur, commercial), or custom; set by the Makefile
extern const char build_preset[];
// off=: every switch this radio could have that the build left off, comma separated; may be empty
extern const char build_off[];

#endif
