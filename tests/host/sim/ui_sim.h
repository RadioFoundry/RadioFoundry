// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Host simulator for the UI (docs/spec/ui.md 7): the real core/ui code against a simulated board (any display
// geometry, any of the reference key maps), radio core, codeplug (the usability-test codeplug) and the services of
// other packages. Key sequences are scripts of logical actions; screens are saved as PNG.
#ifndef UI_SIM_H
#define UI_SIM_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio.h"

// Display classes (information-architecture.md, wireframes) and key maps (7.1 - 7.5)
typedef struct {
	const char *name;	// "160x120i5", "160x128", "240x320i10x5", "128x64", "240x320"
	uint16_t w, h;
	uint8_t bpp;
	uint8_t inset;		// left and right
	uint8_t inset_tb;	// top and bottom
	bool aa;		// anti-aliased fonts: the radio is built with CONFIG_FONT_AA
	bool big;		// lists and labels one size up: the radio is built with CONFIG_FONT_BIG
} sim_geom_t;

typedef enum { SIM_MAP_K, SIM_MAP_M, SIM_MAP_V, SIM_MAP_F, SIM_MAP_COUNT } sim_map_t;

extern const sim_geom_t sim_geoms[];
extern const int sim_geom_count;
// Also "WxH", "WxHiN" (inset N all round), "WxHiNxM" (N left and right, M top and bottom), "WxHm" (mono)
const sim_geom_t *sim_geom_find(const char *name);

// Start over: the radio on zone 0 channel 1 (DMR), no calls heard, an empty inbox, Home
void sim_reset(const sim_geom_t *g, sim_map_t map);
// Run a script (tokens separated by spaces; see ui_sim.c). Returns false and fills err on the first failure
bool sim_run(const char *script, char *err, size_t err_len);
// Color themes (Settings > Display), as ui_theme_set numbers them: "dark", "light", "high_contrast"; theme: in a
// script. sim_theme_find: the number, or -1
#define SIM_THEME_COUNT	3
extern const char *const sim_themes[SIM_THEME_COUNT];
int sim_theme_find(const char *name);
// A highlight color by its gallery name (red, light_blue, ...; color: in a script): UI_ACCENT_*, or -1
int sim_color_find(const char *name);
// Presses counted since the last sim_reset or sim_count_reset (PTT and long presses count one each)
int sim_presses(void);
void sim_count_reset(void);
// Save the frame buffer as a PNG (scale 1..4)
bool sim_save_png(const char *path, int scale);
// Contact sheet: begin with the number of screens, add the frame buffer under a label after each, save as one
// palette PNG (all cells must be the same display class)
void sim_sheet_begin(int count, int scale);
bool sim_sheet_add(const char *label);
bool sim_sheet_save(const char *path);
// The out directory for "shot:" in scripts (NULL: shots are skipped)
void sim_set_shot_dir(const char *dir, const char *prefix);

// Every text gfx_text_fit shortens (with an ellipsis) goes to this hook (NULL: none); sim_rec.shortened counts them
void sim_set_fit_hook(void (*hook)(const char *src, const char *out));

// Worst-case content (on until the next sim_reset): every channel, zone and contact name at the codeplug's 16
// characters, 4-digit channel numbers, 8-digit talkgroups, 7-digit IDs, 80-channel zones. Script token "worst"
void sim_worst(bool on);

// The layout check: problems on the screen as last drawn. A text outside the safe area, a text its box (clip) cuts,
// two texts on top of each other, a box drawn over part of a text. Each problem is one line of text
#define SIM_LAYOUT_MAX	16
int sim_layout_issues(char out[][160], int max);

// State for checks
typedef struct {
	uint32_t tx_count;		// transmissions started
	uint32_t tx_id;			// the last transmission's target
	bool tx_private;
	uint32_t codeplug_writes;	// save to channel, make default
	uint32_t radio_checks, call_alerts, messages_sent;
	char last_text[48];
	uint32_t beeps;			// beeps asked of the radio (any kind)
	uint8_t last_beep;		// ui_beep_t of the last one
	uint32_t vibes;			// vibrations asked of the radio (any pattern)
	uint8_t last_vibe;		// ui_vibe_t of the last one
	uint32_t tot_s, backlight_s;	// settings handed to the radio core (ui_svc_apply)
	uint32_t long_press_ms;		// handed to the input layer (ui_svc_apply)
	uint32_t shortened;		// texts gfx_text_fit shortened with an ellipsis
} sim_record_t;
extern sim_record_t sim_rec;
const radio_view_t *sim_view(void);
uint32_t sim_now(void);

#endif
