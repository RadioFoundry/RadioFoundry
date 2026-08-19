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
	const char *name;	// "160x120i5", "160x128", "128x64", "240x320"
	uint16_t w, h;
	uint8_t bpp;
	uint8_t inset;
} sim_geom_t;

typedef enum { SIM_MAP_K, SIM_MAP_M, SIM_MAP_V, SIM_MAP_F, SIM_MAP_COUNT } sim_map_t;

extern const sim_geom_t sim_geoms[];
extern const int sim_geom_count;
const sim_geom_t *sim_geom_find(const char *name);	// also "WxH", "WxHiN" (inset N), "WxHm" (mono)

// Start over: the radio on zone 0 channel 1 (DMR), no calls heard, an empty inbox, Home
void sim_reset(const sim_geom_t *g, sim_map_t map);
// Run a script (tokens separated by spaces; see ui_sim.c). Returns false and fills err on the first failure
bool sim_run(const char *script, char *err, size_t err_len);
// Presses counted since the last sim_reset or sim_count_reset (PTT and long presses count one each)
int sim_presses(void);
void sim_count_reset(void);
// Save the frame buffer as a PNG (scale 1..4)
bool sim_save_png(const char *path, int scale);
// The out directory for "shot:" in scripts (NULL: shots are skipped)
void sim_set_shot_dir(const char *dir, const char *prefix);

// State for checks
typedef struct {
	uint32_t tx_count;		// transmissions started
	uint32_t tx_id;			// the last transmission's target
	bool tx_private;
	uint32_t codeplug_writes;	// save to channel, make default
	uint32_t radio_checks, call_alerts, messages_sent;
	char last_text[48];
	uint32_t beeps;			// key beeps asked of the radio
	uint32_t tot_s, backlight_s;	// settings handed to the radio core (ui_svc_apply)
} sim_record_t;
extern sim_record_t sim_rec;
const radio_view_t *sim_view(void);
uint32_t sim_now(void);

#endif
