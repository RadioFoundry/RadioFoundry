// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The user interface task (docs/ux/)
#ifndef UI_H
#define UI_H

void ui_start(void);
// The screen shown now, for the debug link's view: "home", "quick", "diag" or "hotspot" (any task; a word read)
const char *ui_screen_name(void);

#endif
