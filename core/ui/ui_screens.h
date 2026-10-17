// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Screen drawing (docs/ux/wireframes): layout from the display size, no task or input code, so host tests and the
// host simulator can render any screen at any geometry
#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio.h"
#include "font.h"
#include "gfx.h"
#include "config.h"
#if CONFIG_HOTSPOT
#include "hotspot/hotspot.h"
#endif

#define UI_ENTRY_MAX	8	// VFO entry digits: 3 MHz digits and 5 decimals (10 Hz)
#define UI_ENTRY_MHZ	3
#define UI_QUICK_TILES	9
#define UI_TALL_PX	200	// displays at least this high keep the whole status strip above a screen's title row
#define UI_BANNER_TEXT	64	// two lines at the widest display class

// Banners (information-architecture.md 3, "Banners and interruptions"): over any screen, no focus change
typedef enum {
	UI_BANNER_NONE,
	UI_BANNER_MESSAGE,	// text message received: CONTEXT opens it
	UI_BANNER_CALL_ALERT,	// call alert received: CONTEXT opens the caller's actions
	UI_BANNER_PRIVATE,	// private call to us: CONTEXT opens the caller's actions
	UI_BANNER_RESULT,	// result of something we sent (radio check answered, message delivered, ...)
	UI_BANNER_INFO,		// anything else ("Hotspot started by host. Back to stop.")
} ui_banner_kind_t;

typedef struct {
	uint8_t kind;		// ui_banner_kind_t
	bool warn;		// a failure: amber
	uint32_t until_ms;
	uint32_t id;		// the station it is about (0 = none)
	uint16_t msg;		// UI_BANNER_MESSAGE: the message's index in the inbox
	char title[UI_BANNER_TEXT];
	char text[UI_BANNER_TEXT];
} ui_banner_t;

// What the screens show besides the radio view; owned by the UI (ui_app.c)
typedef struct {
	uint32_t now_ms;
	uint8_t battery_pct;
	bool dmr_off;			// the DMR channel's modem has been off a while (ui_app.c), not just a moment
	bool entry;			// VFO frequency entry in progress
	uint8_t entry_len;
	char entry_digits[UI_ENTRY_MAX + 1];
	const char *toast;		// one-line message over the hint row, NULL = none
	uint8_t quick_focus;		// 0..8 = tiles 1..9
	bool quick_edit;		// the focused tile's value editor is open
	uint8_t brightness;		// backlight percent chosen in quick settings
	uint8_t tone_target;		// Tone editor: RADIO_TONE_BOTH, RADIO_TONE_TX or RADIO_TONE_RX
	subtone_t tone_edit;		// Tone editor: the value picked (shown at once, before the radio's view catches up)
	uint8_t cc_edit;		// Color code editor: the value picked
	uint8_t diag_first;	// first board line on the diagnostics page
	bool locked;		// keys locked (long QUICK)
	bool scanning;
	uint8_t usb_net;	// UI_USB_NET_*: the status strip's USB network word and link arrows
	bool hints;		// the hint row is shown (Settings > Display)
	bool large_text;	// Home shows only the channel and the talk target (Settings > Display)
	bool privacy;		// the next DMR transmission is encrypted
	bool hear_all;		// hear all latched (quick tile 5 on DMR); follows the radio view where the radio has it
	uint16_t auto_lock_s;	// lock the keys after this long without a key press, 0 = never (Settings > Keys)
	bool alias_show;	// show received talker aliases (Settings > Talk & listen)
	bool ptt_locked;	// PTT works while the keys are locked (Settings > Privacy)
	bool hear_all_default;	// hear all on whenever a DMR channel is selected (Settings > Talk & listen)
	bool message_tone;	// a tone when a message arrives (Settings > Sound)
	bool vibrate;		// vibrate for messages, calls to us, emergencies (Settings > Sound, CONFIG_VIBRATE)
	uint8_t updown;		// what Up / Down change on Home: 0 channel, 1 zone, 2 talk target (Settings > Keys)
	uint8_t low_batt_pct;	// warn at this battery percent, 0 = never (Settings > Power)
	uint16_t auto_off_min;	// switch off after this long without use, 0 = never (Settings > Power)
	uint16_t unread;	// unread messages
	const char *home_hint;	// the hint for Home's idle state, built from the board's keys ("OK Menu *Quick #Joe")
	ui_banner_t banner;
} ui_state_t;

// Quick settings tiles. The position of each depends on the mode (information-architecture.md 5): ui_quick_kind()
enum {
	QT_POWER, QT_SQUELCH, QT_TONE, QT_MODE, QT_MONITOR, QT_VOX, QT_SCAN, QT_BANDWIDTH, QT_BRIGHTNESS,
	QT_TIMESLOT, QT_CC, QT_HEAR_ALL, QT_PRIVACY, QT_TALK_TO, QT_STEP,
};
// The tile kind at position 0..8 (keys 1..9) for this channel or the VFO
uint8_t ui_quick_kind(int pos, const radio_view_t *v);

// Fonts and region positions for the current display, from gfx_width() / gfx_height()
typedef struct {
	const font_t *tiny, *small, *medium, *large;
	int strip_h;
	int zone_y;		// < 0: no zone line on this display
	int channel_y, talk_y, chips_y;
	int sep_y;		// < 0: no separator
	int activity_y, activity_h;
	int hint_y;
	int row_h;		// list row height
} ui_layout_t;

extern ui_layout_t ui_layout;

// ui_state_t.usb_net: USB is a serial port, a network adapter waiting for the computer ("IP"), or a network adapter
// the computer has brought up, so IP over DMR can flow ("IP" and the up and down arrows, usb-network.md 1)
enum { UI_USB_NET_OFF, UI_USB_NET_WAIT, UI_USB_NET_UP };

// Colors carry meaning only (P5); every color cue also has a word. On 1 bpp panels everything is ink on paper and
// pills are inverse video
typedef struct {
	color_t bg, fg, dim, line, tx, rx, warn, idle, pill_fg, pill_dark_fg, focus, hint_bg, priv;
	color_t focus_fg;		// text on the focus highlight
	color_t chnum;			// Home's channel number
	color_t strip_bg;		// the filled status strip's background (its text is pill_dark_fg)
	color_t strip_warn;		// warnings on it
} ui_colors_t;
extern ui_colors_t ui_col;
// Color theme on color displays (Settings > Display): 0 dark, 1 light, 2 high contrast. After ui_layout_init
void ui_theme_set(uint8_t theme);
// Color displays (Settings > Display): the highlight color (UI_ACCENT_*, ui_svc.h) for list and tile focus and Home's
// channel number, and whether the status strip is filled with it. Both re-apply the theme's colors
void ui_accent_set(uint8_t accent);
void ui_strip_set(bool filled);

// Pick fonts and place the regions; call after gfx_init
void ui_layout_init(void);

// Shared pieces for every screen. ui_draw_top draws the status strip (or the title row that carries the state pill)
// and counts how often it ran, so tests can check every screen draws it (P2)
extern uint32_t ui_strip_draws;
int ui_draw_top(const radio_view_t *v, const ui_state_t *s, const char *prefix, const char *name);
void ui_draw_title(const font_t *f, int x, int y, int max_w, const char *prefix, const char *name, color_t c);
void ui_draw_hint(const char *text, bool toast);
// The bottom row: the toast if there is one, else the hint (unless hints are off)
void ui_draw_bottom(const ui_state_t *s, const char *hint);
// The banner, if one is up, over the lower part of the screen
void ui_draw_banner(const ui_state_t *s);

// One row of a list screen
#define UI_ITEM_DIM	0x01u	// not available here (shown, explained on SELECT)
#define UI_ITEM_TEMP	0x02u	// a temporary change: dot
#define UI_ITEM_WARN	0x04u
#define UI_ITEM_EDIT	0x08u	// the row's inline editor is open
#define UI_ITEM_INFO	0x10u	// a text line, not something to pick (never focused)
#define UI_ITEM_PREVIEW	0x20u	// the label is the start of a longer text (a message): shortened at its end, not the middle

typedef struct {
	char label[36];
	char value[44];		// may hold text alternates (font.h); an empty last alternate leaves the value out
	char num;		// key shown before the label ('1'..'9', '0'), 0 = none
	uint8_t flags;
} ui_item_t;

typedef void (*ui_item_fn)(uint16_t i, ui_item_t *it);

typedef struct {
	const char *prefix, *name;	// title
	const char *header;		// a line under the title (the Find query), NULL = none
	const char *body;		// word-wrapped text above the rows (a message, an info page), NULL = none
	uint16_t count;
	uint16_t focus;
	uint16_t *top;			// first row shown; updated so the focus is visible
	ui_item_fn item;
	const char *empty;		// shown when count is 0
	const char *hint;
} ui_list_t;

void ui_draw_list(const radio_view_t *v, const ui_state_t *s, const ui_list_t *l);
// A page of text (About) in the smallest font, word-wrapped, never shortened: lines from *first on (clamped so the
// last page is full). The status strip only while the radio isn't idle. Returns the largest *first (0: all fits)
int ui_draw_text_page(const radio_view_t *v, const ui_state_t *s, const char *title, const char *text,
	uint16_t *first);
// Rows that fit below the title (and header) on this display, for paging
int ui_list_rows(const ui_list_t *l);

// The boot screen: the mark, the name, the owner's two boot text lines (empty: left out), board and version
void ui_draw_splash(const char *board_name, const char *git, const char *line1, const char *line2);
void ui_draw_home(const radio_view_t *v, const ui_state_t *s);
void ui_draw_quick(const radio_view_t *v, const ui_state_t *s);
#if CONFIG_HOTSPOT
// Hotspot mode (hostlink in MMDVM mode): frequencies, color code, TX / RX, last call, how to leave
void ui_draw_hotspot(const radio_view_t *v, const hotspot_view_t *h, const ui_state_t *s);
#endif
// Received emergency: full screen, the caller, how to acknowledge
void ui_draw_emergency(const radio_view_t *v, const ui_state_t *s, uint32_t id, const char *name);
// Diagnostics: raw key bits, encoder total, radio readings and the board's own lines
void ui_draw_diag(const radio_view_t *v, uint32_t keys, int32_t enc_total, uint8_t first_line);
// The first board line of the page after the one starting at first_line (wraps to 0)
uint8_t ui_diag_next_page(uint8_t first_line);

// Quick settings tile at position 0..8: false if it can't be used here. label and value get short texts
bool ui_quick_tile(int tile, const radio_view_t *v, const ui_state_t *s, char *label, size_t label_len,
	char *value, size_t value_len);

// Whether a board band holds hz (tx: one the radio can transmit in)
bool ui_in_band(uint32_t hz, bool tx);
// Frequency typed as digits (VFO entry, the channel editor): positional from 100 MHz down to 10 Hz, at most
// UI_ENTRY_MAX of them in digits[UI_ENTRY_MAX + 1]. A digit is appended when some band can still start so; false
// when the digits are full, or with a toast naming the bands when no band can
bool ui_freq_entry_digit(char *digits, uint8_t *len, char d);
// The typed frequency, or 0 with a toast saying why (fewer than the MHz digits, not in a band)
uint32_t ui_freq_entry_hz(const char *digits, uint8_t len);
// Text for the board's bands ("136-174 400-480"), for hints
void ui_bands_text(char *out, size_t len);
// The board's power level name
const char *ui_power_name(uint8_t level);

#endif
