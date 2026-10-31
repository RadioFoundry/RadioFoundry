// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Find (information-architecture.md 6, task-flows.md 1, 3, 5): digits typed on Home search everything at once.
// The keys are read both as a number and as T9 letters (2 = abc ... 9 = wxyz), matched against the start of each
// word. Order of the results:
//  1. a channel at that position in the zone (numbers of up to 3 digits: "12" then OK = channel 12, P1), then the
//     digits as a frequency in one of the bands, read as VFO entry reads them ("Tune VFO to 461.462"),
//  2. channels of the zone, contacts and talkgroups, zones and places / settings whose names match the letters,
//     contacts whose ID starts with the number (3+ digits),
//  3. the number itself as a talkgroup and as a private call ("TG 3100 (not saved)", "Private call 3100"), so a
//     number that isn't in the codeplug is still usable (P6), and with the contact editor "Create contact 3100" when
//     no contact has that ID.
// Names match as the user reads them (the translation, i18n.h) and as written in English, so a setting is found by
// either; accented Latin-1 letters count as their plain letter on the keys.
#include <string.h>
#include "ui_nav.h"
#include "ui_svc.h"
#include "fmt.h"
#include "font.h"
#include "i18n.h"
#include "codeplug/codeplug.h"
#include "config.h"

#define FIND_KEYS	12
#define FIND_MAX	16

// Places Find opens by name or synonym; the Tools pages hold settings of their own that aren't in Settings. The
// synonyms are English search words, not shown
static const struct {
	const char *name, *syn;
	uint8_t scr;
} places[] = {
	{ TRN("Contacts"), NULL, SCR_CONTACTS }, { TRN("Messages"), NULL, SCR_MESSAGES },
	{ TRN("Recent"), NULL, SCR_RECENT }, { TRN("Channels"), NULL, SCR_ZONES }, { TRN("Tools"), NULL, SCR_TOOLS },
	{ TRN("Settings"), NULL, SCR_SETTINGS },
	{ "VFO", NULL, SCR_ZONES },
#if CONFIG_DMR_ROAMING
	{ TRN("Roaming"), "roam site survey beacon interval length wake probe lock", SCR_ROAM },
#endif
#if CONFIG_APRS
	{ "APRS", "beacon smartbeaconing symbol path kiss tnc gps", SCR_APRS },
	{ TRN("APRS position"), "fixed latitude longitude coordinates", SCR_APRS_POS },
#endif
};
#define PLACES_N	((uint8_t)(sizeof places / sizeof places[0]))

typedef enum { FR_CHANNEL, FR_ZONE, FR_CONTACT, FR_TG, FR_PRIVATE, FR_SETTING, FR_PLACE, FR_FREQ, FR_NEW_CONTACT }
	find_kind_t;

typedef struct {
	uint8_t kind;
	uint32_t ref;
} find_result_t;

static char query[FIND_KEYS + 1];
static find_result_t results[FIND_MAX];
static uint8_t n_results;

// Settings and places reachable by name (ui_menus.c)
uint8_t ui_set_group_count(void);
const char *ui_set_group_name(uint8_t g);
uint8_t ui_set_item_count(uint8_t g);
const char *ui_set_item_name(uint8_t g, uint8_t i, const char **synonyms);
void ui_open_setting(uint8_t g, uint8_t i);

uint8_t ui_t9_key(char c)
{
	static const char keys[] = "22233344455566677778889999";
	// A translation's accented letter as the plain one (Latin-1, as the fonts fold it): the o key types the o of
	// "Configuraci\xF3n"
	c = font_fold(c);
	if (c >= 'a' && c <= 'z')
		return (uint8_t)(keys[c - 'a'] - '0');
	if (c >= 'A' && c <= 'Z')
		return (uint8_t)(keys[c - 'A'] - '0');
	if (c >= '0' && c <= '9')
		return (uint8_t)(c - '0');
	return 0xFF;
}

static bool word_match(const char *w, const char *keys)
{
	for (; *keys; keys++, w++) {
		if (*w == 0 || *w == ' ')
			return false;
		if (ui_t9_key(*w) != (uint8_t)(*keys - '0'))
			return false;
	}
	return true;
}

bool ui_t9_match(const char *text, const char *keys)
{
	if (keys[0] == 0)
		return true;
	bool start = true;
	for (const char *p = text; *p; p++) {
		bool sep = *p == ' ' || *p == '-' || *p == '.' || *p == '/' || *p == '_';
		if (start && !sep && word_match(p, keys))
			return true;
		start = sep;
	}
	return false;
}

const char *ui_find_query(void) { return query; }

static bool add(uint8_t kind, uint32_t ref)
{
	if (n_results >= FIND_MAX)
		return false;
	for (uint8_t i = 0; i < n_results; i++)
		if (results[i].kind == kind && results[i].ref == ref)
			return true;
	results[n_results].kind = kind;
	results[n_results].ref = ref;
	n_results++;
	return true;
}

static uint32_t query_number(void)
{
	uint32_t n = 0;
	for (const char *p = query; *p; p++) {
		if (n > 1677721u)
			return 0;
		n = n * 10u + (uint32_t)(*p - '0');
	}
	return n;
}

#if CONFIG_DMR
static bool id_prefix(uint32_t id, const char *keys)
{
	char buf[12];
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, id, 1);
	return strncmp(buf, keys, strlen(keys)) == 0;
}
#endif

void ui_find_run(void)
{
	static zone_t z;
	static channel_t ch;
	const radio_view_t *v = ui_view();
	size_t len = strlen(query);
	uint32_t num = query_number();
	n_results = 0;
	if (len == 0)
		return;

	bool have_zone = codeplug_zone(v->zone_index, &z);
	// 1. Channel by position in the zone
	if (have_zone && len <= 3 && num >= 1 && num <= z.count)
		add(FR_CHANNEL, num - 1u);
	// The digits as a frequency
	if (CONFIG_VFO_FM || CONFIG_VFO_DMR) {
		uint32_t hz = ui_freq_parse(query, (uint8_t)len);
		if (hz != 0)
			add(FR_FREQ, hz);
	}
	// 2. Names: channels in this zone
	uint8_t found = 0;
	for (uint16_t i = 0; have_zone && i < z.count && found < 4; i++) {
		if (codeplug_channel(z.channels[i], &ch) && ui_t9_match(ch.name, query)) {
			add(FR_CHANNEL, i);
			found++;
		}
	}
#if CONFIG_DMR
	// Contacts and talkgroups: by name, or by ID for 3 digits and more
	contact_t c;
	found = 0;
	bool known = false;	// a contact has the number as its ID
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		if (!codeplug_contact(i, &c))
			continue;
		known = known || c.id == num;
		if (found < 6 && (ui_t9_match(c.name, query) || (len >= 3 && id_prefix(c.id, query)))) {
			add(FR_CONTACT, i);
			found++;
		}
		if (found >= 6 && (known || !CONFIG_CONTACT_EDIT))
			break;
	}
#endif
	// Zones by name
	uint16_t zn = codeplug_zone_count();
	found = 0;
	for (uint16_t i = 0; i < zn && found < 2; i++) {
		if (codeplug_zone(i, &z) && ui_t9_match(z.name, query)) {
			add(FR_ZONE, i);
			found++;
		}
	}
	// Places and settings by name (and synonyms: "promiscuous" finds Hear all)
	for (uint8_t i = 0; i < PLACES_N; i++)
		if (len >= 2 && (ui_t9_match(tr(places[i].name), query) || ui_t9_match(places[i].name, query) ||
				 (places[i].syn && ui_t9_match(places[i].syn, query))))
			add(FR_PLACE, (uint32_t)i << 8 | places[i].scr);
	found = 0;
	for (uint8_t g = 0; g < ui_set_group_count() && len >= 2; g++) {
		for (uint8_t i = 0; i < ui_set_item_count(g) && found < 3; i++) {
			const char *syn = NULL;
			const char *name = ui_set_item_name(g, i, &syn);
			if (ui_t9_match(tr(name), query) || ui_t9_match(name, query) ||
			    (syn != NULL && ui_t9_match(syn, query))) {
				add(FR_SETTING, (uint32_t)g << 8 | i);
				found++;
			}
		}
	}
#if CONFIG_DMR
	// 3. The number itself
	if (num >= 1 && num <= 0xFFFFFEu) {
		add(FR_TG, num);
		add(FR_PRIVATE, num);
		if (CONFIG_CONTACT_EDIT && !known)
			add(FR_NEW_CONTACT, num);
	}
#endif
}

void ui_find_set(const char *q)
{
	if (q != query) {
		strncpy(query, q, FIND_KEYS);
		query[FIND_KEYS] = 0;
	}
	ui_find_run();
	ui_frame()->focus = 0;
	ui_frame()->top = 0;
	ui_focus_changed();
	ui_dirty();
}

// --- the Find screen ---

static uint16_t find_count(void) { return n_results; }

static bool find_target(uint16_t i, ui_target_t *t)
{
	if (i >= n_results)
		return false;
	memset(t, 0, sizeof *t);
	const find_result_t *r = &results[i];
	switch (r->kind) {
#if CONFIG_DMR
	case FR_CONTACT: {
		contact_t c;
		if (!codeplug_contact((uint16_t)r->ref, &c))
			return false;
		t->id = c.id;
		t->group = c.type != CONTACT_PRIVATE;
		t->saved = true;
		strcpy(t->name, c.name);
		return true;
	}
#endif
	case FR_TG:
		t->id = r->ref;
		t->group = true;
		return true;
	case FR_PRIVATE:
		t->id = r->ref;
		t->saved = ui_contact_name(r->ref, false, t->name);
		return true;
	default:
		return false;
	}
}

static void find_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	static channel_t ch;
	if (i >= n_results)
		return;
	const find_result_t *r = &results[i];
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sbuf_t vb;
	sb_init(&vb, it->value, sizeof it->value);
	ui_target_t t;
	switch (r->kind) {
	case FR_CHANNEL: {
		const radio_view_t *v = ui_view();
		sb_str(&b, TR("Ch "));
		sb_u32(&b, r->ref + 1u, 1);
		if (codeplug_zone(v->zone_index, &z) && r->ref < z.count && codeplug_channel(z.channels[r->ref], &ch)) {
			sb_chr(&b, ' ');
			sb_str(&b, ch.name);
		}
		sb_str(&vb, TR(TEXT_ALT_OPEN "Channel" TEXT_ALT_NEXT TEXT_ALT_CLOSE));
		break;
	}
	case FR_ZONE:
		if (codeplug_zone((uint16_t)r->ref, &z))
			sb_str(&b, z.name);
		sb_str(&vb, TR(TEXT_ALT_OPEN "Zone" TEXT_ALT_NEXT TEXT_ALT_CLOSE));
		break;
	case FR_CONTACT:
		if (find_target(i, &t)) {
			char name[32];
			if (t.group) {
				ui_sb_target(name, sizeof name, &t);
				sb_str(&b, name);
			} else {
				sb_str(&b, t.name);
				sb_chr(&b, ' ');
				sb_u32(&b, t.id, 1);
			}
			sb_str(&vb, TR(TEXT_ALT_OPEN "Contact" TEXT_ALT_NEXT TEXT_ALT_CLOSE));
		}
		break;
	case FR_TG:
		sb_str(&b, TR("Talk to TG "));
		sb_u32(&b, r->ref, 1);
		sb_str(&vb, TR(TEXT_ALT_OPEN "Not saved" TEXT_ALT_NEXT TEXT_ALT_CLOSE));
		break;
	case FR_PRIVATE:
		sb_str(&b, TR("Private call "));
		sb_u32(&b, r->ref, 1);
		if (find_target(i, &t) && t.name[0]) {
			sb_chr(&b, ' ');
			sb_str(&b, t.name);
		}
		break;
	case FR_FREQ:
		sb_str(&b, TR("Tune VFO to "));
		sb_freq(&b, r->ref);
		sb_str(&vb, TEXT_ALT_OPEN "VFO" TEXT_ALT_NEXT TEXT_ALT_CLOSE);
		break;
	case FR_NEW_CONTACT:
		sb_str(&b, TR("Create contact "));
		sb_u32(&b, r->ref, 1);
		break;
	case FR_SETTING: {
		const char *syn;
		sb_str(&b, tr(ui_set_item_name((uint8_t)(r->ref >> 8), (uint8_t)r->ref, &syn)));
		sb_str(&vb, TEXT_ALT_OPEN);	// the group, left out where the setting's name needs the room
		sb_str(&vb, tr(ui_set_group_name((uint8_t)(r->ref >> 8))));
		sb_str(&vb, TEXT_ALT_NEXT TEXT_ALT_CLOSE);
		break;
	}
	case FR_PLACE: {
		sb_str(&b, tr(places[(r->ref >> 8) % PLACES_N].name));
		sb_str(&vb, TR("Menu"));
		break;
	}
	default:
		break;
	}
}

static void find_select(uint16_t i, bool hold)
{
	if (i >= n_results)
		return;
	const find_result_t *r = &results[i];
	const radio_view_t *v = ui_view();
	ui_target_t t;
	switch (r->kind) {
	case FR_CHANNEL:
		if (v->vfo)
			radio_req_vfo(false);
		radio_req_zone_pos((uint16_t)r->ref);
		ui_go_home();
		break;
	case FR_ZONE:
		radio_req_zone((uint16_t)r->ref);
		ui_go_home();
		break;
	case FR_FREQ:
		radio_req_vfo_set(r->ref);	// opens the VFO there; its mode, power and tones stay
		ui_go_home();
		break;
#if CONFIG_CONTACT_EDIT
	case FR_NEW_CONTACT:
		// A typed number: the editor starts on a person for a DMR ID's 7 digits and more, else a talkgroup
		memset(&t, 0, sizeof t);
		t.id = r->ref;
		t.group = r->ref < 1000000u;
		ui_new_contact(&t);
		break;
#endif
	case FR_CONTACT:
	case FR_TG:
	case FR_PRIVATE:
		if (find_target(i, &t))
			ui_talk(&t, hold);
		break;
	case FR_SETTING:
		ui_open_setting((uint8_t)(r->ref >> 8), (uint8_t)r->ref);
		break;
	case FR_PLACE:
		ui_push((uint8_t)r->ref, 0);
		break;
	default:
		break;
	}
}

static void find_context(uint16_t i)
{
	ui_target_t t;
	if (find_target(i, &t))
		ui_open_actions(&t);
	else
		ui_toast(TR("No actions for this result"));
}

static bool find_key(const input_event_t *ev)
{
	int d = ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 ? ev->action - ACT_DIGIT_0 : -1;
	if (d >= 0 && (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG)) {
		size_t len = strlen(query);
		if (len < FIND_KEYS) {
			query[len] = (char)('0' + d);
			query[len + 1] = 0;
			ui_find_set(query);
		}
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		size_t len = strlen(query);
		if (len <= 1) {
			query[0] = 0;
			ui_pop();
		} else {
			query[len - 1] = 0;
			ui_find_set(query);
		}
		return true;
	}
	return false;
}

const ui_scr_def_t ui_find_screen = {
	.name = "find", .flags = SF_LIST | SF_FIND | SF_FIELD,
	.hint = TRN("PTT talk  OK open  # more"), .empty = TRN("No match: Back deletes a key"),
	.count = find_count, .item = find_item, .select = find_select, .context = find_context, .target = find_target,
	.key = find_key,
};
