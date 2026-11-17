// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What this build is: the switches it has and its preset, as VERSION reports them (docs/spec/debuglink.md 3.1). Shared by
// the debug link and the stub entry's listener of a build without it, so the web tools identify a build either way.
#ifndef BUILD_INFO_H
#define BUILD_INFO_H

// features=: a comma list of the switches the tools look for (not every switch), no newline: build_modes_now then
// build_features_now. The modes (P25, P25_TRUNK, NXDN) only where the board runs them (radio_p25_fits,
// radio_nxdn_fits), so the list can differ between boards of one build; each ends in a comma, and may be ""
const char *build_modes_now(void);
const char *build_features_now(void);
// preset=: the release variant (full, amateur, commercial), or custom; set by the Makefile
extern const char build_preset[];
// off=: every switch this radio could have that the build left off, comma separated; may be empty
extern const char build_off[];
// start=: the language the radio starts in until the user picks one (CONFIG_LANG_DEFAULT_<CODE>), its ISO 639-1 code,
// "en" for English. off= can't say it: a start-language switch added after this build is in neither list
const char *build_start_now(void);

#endif
