// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tools > Trunking (docs/spec/dmr-trunking.md 12, docs/spec/ui.md): the codeplug's Tier III systems; select one to
// use it, select it again to leave it. The row of the one in use says what the radio is doing on it
#include <string.h>
#include "ui_nav.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "config.h"

#if CONFIG_DMR_TRUNK
#include "dmr/trunk/t3_ms.h"

static uint16_t trunk_count(void) { return ui_svc_t3_count(); }

// What the radio is doing on the system in use
static const char *trunk_state(const radio_view_t *v)
{
	if (!v->t3.on) {
		return TR("Starting");
	}
	switch (v->t3.state) {
	case T3_HUNT: return TR("Searching");
	case T3_FOLLOW: return TR("In call");
	case T3_CAMPED:
		if (v->t3.reg == T3_REG_OK) {
			return TR("Registered");
		}
		return v->t3.reg == T3_REG_DENIED ? TR("Refused") : TR("On site");
	default: return TR("Off");
	}
}

static void trunk_item(uint16_t row, ui_item_t *it)
{
	uint8_t n;
	char name[CODEPLUG_NAME_LEN + 1];
	if (!ui_svc_t3_system((uint8_t)row, &n, name)) {
		return;
	}
	str_copy(it->label, sizeof it->label, name[0] != '\0' ? name : TR("System"));
	if (ui_svc_t3_selected() == (int)n) {
		str_copy(it->value, sizeof it->value, trunk_state(ui_view()));
	}
}

static void trunk_select(uint16_t row, bool hold)
{
	(void)hold;
	uint8_t n;
	char name[CODEPLUG_NAME_LEN + 1];
	if (!ui_svc_t3_system((uint8_t)row, &n, name)) {
		return;
	}
	bool leaving = ui_svc_t3_selected() == (int)n;
	if (!ui_svc_t3_select(n)) {
		ui_toast(TR("No control channel"));
		return;
	}
	if (leaving) {
		ui_toast(TR("Trunking off"));
		return;
	}
	ui_go_home();
	ui_toast(TR("Searching"));
}

const ui_scr_def_t ui_trunk_screen = {
	.name = "trunk", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Trunking"), .hint = TRN("Select  Back"),
	.empty = TRN("No trunked systems"),
	.count = trunk_count, .item = trunk_item, .select = trunk_select,
};
#endif
