// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Inside the UI: the navigation stack, the screen table and the helpers the screens share (ui_app.c, ui_menus.c,
// ui_find.c). Not for other packages.
#ifndef UI_NAV_H
#define UI_NAV_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_input.h"
#include "radio/radio.h"
#include "ui_screens.h"

typedef enum {
	SCR_HOME, SCR_QUICK, SCR_HUB, SCR_FIND, SCR_ACTIONS, SCR_CONTACTS, SCR_MESSAGES, SCR_MESSAGE, SCR_COMPOSE,
	SCR_RECENT, SCR_CHANNELS, SCR_ZONE, SCR_TOOLS, SCR_SETTINGS, SCR_GROUP, SCR_PICKER, SCR_INFO, SCR_CONFIRM,
	SCR_DIAG, SCR_EMERGENCY, SCR_SCAN, SCR_ROAM, SCR_APRS, SCR_APRS_HEARD,
	SCR_COUNT
} ui_scr_t;

// Screen flags
#define SF_LIST		0x01u	// drawn and driven as a list (ui_draw_list)
#define SF_NUMBERED	0x02u	// digits pick the row with that number
#define SF_FILTER	0x04u	// digits type a T9 filter that narrows the rows
#define SF_FIELD	0x08u	// "field use": at most 3 levels from Home (P4)
#define SF_FIND		0x10u	// digits type the query (Find)

typedef struct {
	uint8_t scr;
	uint16_t focus, top;
	uint16_t arg;		// zone index, settings group, message index, info page
} ui_frame_t;

// A station or talkgroup that actions apply to
typedef struct {
	uint32_t id;
	bool group;		// a talkgroup (else a person)
	uint32_t heard_tg;	// a person heard on this talkgroup (0 = privately / unknown)
	bool saved;		// in the contacts
	char name[CODEPLUG_NAME_LEN + 1];
} ui_target_t;

typedef struct {
	const char *name;	// the debug link's / simulator's name for it
	uint8_t flags;
	const char *prefix;	// title
	const char *hint;
	const char *empty;
	uint16_t (*count)(void);
	void (*item)(uint16_t i, ui_item_t *it);
	void (*select)(uint16_t i, bool hold);		// SELECT; hold = long SELECT (save / make permanent)
	void (*context)(uint16_t i);			// CONTEXT (NULL: no actions here)
	bool (*target)(uint16_t i, ui_target_t *t);	// what PTT talks to with this row focused (one-shot)
	const char *(*title)(void);			// title name part (NULL: none)
	const char *(*body)(void);			// text above the rows (NULL: none)
	bool (*key)(const input_event_t *ev);		// screen keys before the list's own (true: used)
	void (*draw)(void);				// own drawing instead of the list
	void (*enter)(void);				// on push
	// Filtered lists (SF_FILTER): rows before filtering and the text each is matched by
	uint16_t (*raw_count)(void);
	bool (*raw_label)(uint16_t raw, char *buf, uint8_t len);
} ui_scr_def_t;

extern const ui_scr_def_t *const ui_scr[SCR_COUNT];
extern ui_state_t ui_st;
extern ui_target_t ui_target;		// the target of SCR_ACTIONS / SCR_COMPOSE

const radio_view_t *ui_view(void);
ui_frame_t *ui_frame(void);
uint8_t ui_depth(void);
void ui_push(uint8_t scr, uint16_t arg);
void ui_pop(void);
void ui_go_home(void);
void ui_toast(const char *text);
void ui_dirty(void);
// The focused row changed: tell the radio what PTT would talk to now
void ui_focus_changed(void);
// Open the actions for a target (SCR_ACTIONS)
void ui_open_actions(const ui_target_t *t);
// Talk to a target: temporary (SELECT) or permanent (long SELECT); goes Home and says what happened
void ui_talk(const ui_target_t *t, bool permanent);
// The board has a key for this action (as a short or a long press)
bool ui_has_action(uint8_t action);
// A setting the UI acts on itself changed (brightness, hints, large text, theme, lock, Home keys, ...)
void ui_apply_setting(uint8_t id, int32_t v);
// Scan on / off (quick 7, Tools 1, long SK2), with a toast that says what scans: the zone, or the band in the VFO
void ui_scan_toggle(void);

// Filtered lists: row i -> raw index; the filter text
uint16_t ui_filter_count(void);
uint16_t ui_filter_raw(uint16_t i);
const char *ui_filter_text(void);
void ui_filter_reset(void);
void ui_filter_rebuild(void);

// T9 (ui_find.c): the key that types c (letters, digits; others 0xFF)
uint8_t ui_t9_key(char c);
// Does any word of text start with the keys typed (digits '0'..'9')? Digits in the text match themselves
bool ui_t9_match(const char *text, const char *keys);
// Find: rerun the search for ui_st query; the screen's rows
void ui_find_run(void);
void ui_find_set(const char *q);
const char *ui_find_query(void);

// Recent (ui_app.c): newest first
#define UI_RECENT_MAX	12
typedef struct {
	uint32_t src, dst;
	bool group, out, to_us;
	uint32_t ms;
	char name[CODEPLUG_NAME_LEN + 1];	// the other station's (or the talkgroup's, for calls we made) name
	bool gps_range;		// the caller sent its position (ETSI GPS Info) and ours was known: how far, which way
	uint16_t gps_bearing;
	uint32_t gps_dist_m;
} ui_recent_t;
uint8_t ui_recent_count(void);
const ui_recent_t *ui_recent_get(uint8_t i);
// The recent entry as an actions target (the caller of a heard call, the target of one we made)
void ui_recent_target(const ui_recent_t *r, ui_target_t *t);
// Name of a contact by ID from the codeplug ("" if none); talkgroup = a group contact
bool ui_contact_name(uint32_t id, bool group, char *out);

// Diagnostics, emergency, banners (ui_app.c)
void ui_emergency_draw(void);
bool ui_emergency_key(const input_event_t *ev);
void ui_diag_draw(void);
bool ui_diag_key(const input_event_t *ev);
void ui_home_draw(void);
bool ui_home_key(const input_event_t *ev);
void ui_quick_draw(void);
bool ui_quick_key(const input_event_t *ev);
void ui_quick_enter(void);

// Text helpers
void ui_sb_target(char *buf, size_t len, const ui_target_t *t);	// "TG 91 World", "Joe 3101234"
void ui_sb_age(char *buf, size_t len, uint32_t ms);

#endif
