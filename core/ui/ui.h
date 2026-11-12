// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The user interface task (docs/ux/, docs/spec/ui.md)
#ifndef UI_H
#define UI_H

#include "ui_app.h"

void ui_start(void);
// The boot splash without the boot text, drawn as soon as the display is up (the start-up task, before the storage and
// the radio's set-up): the UI task draws it again with the boot text once it starts
void ui_boot_splash(void);
// The screen shown now, for the debug link's view: "home", "quick", "hub", "find", "diag", "hotspot", ... (any task)
const char *ui_screen_name(void);
// What is on screen (screen, depth, focus, banner, lock, Find query), as of the UI task's last pass (any task)
void ui_get_status(ui_app_status_t *st);
// The backlight level last set, percent (any task)
uint8_t ui_backlight(void);

#endif
