// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The support info: the build's source and the radio's hardware identity (support_info.h)
#include <string.h>
#include "board.h"
#include "ui/fmt.h"
#include "support/support_info.h"

// Set by the Makefile on this file (tools/build_source.py); the host tests build it without
#ifndef RFOUNDRY_GIT_SOURCE
#define RFOUNDRY_GIT_SOURCE	"unknown"
#endif
#ifndef RFOUNDRY_GIT_REPO
#define RFOUNDRY_GIT_REPO	""
#endif

static const char *const source_keys[SUPPORT_SRC_COUNT] = {
	"unknown", "official", "service", "pr", "local", "fork",
};

support_source_t support_source(void)
{
	for (uint8_t s = 0; s < SUPPORT_SRC_COUNT; s++) {
		if (strcmp(RFOUNDRY_GIT_SOURCE, source_keys[s]) == 0) {
			return (support_source_t)s;
		}
	}
	return SUPPORT_SRC_UNKNOWN;
}

const char *support_source_key(support_source_t s)
{
	return s < SUPPORT_SRC_COUNT ? source_keys[s] : source_keys[0];
}

tr_id_t support_source_text(support_source_t s)
{
	switch (s) {
	case SUPPORT_SRC_OFFICIAL:
		return TRN(ABOUT_VALUE_SOURCE_OFFICIAL, "CI build");
	case SUPPORT_SRC_SERVICE:
		return TRN(ABOUT_VALUE_SOURCE_SERVICE, "Build service");
	case SUPPORT_SRC_PR:
		return TRN(ABOUT_VALUE_SOURCE_PR, "Pull request");
	case SUPPORT_SRC_LOCAL:
		return TRN(ABOUT_VALUE_SOURCE_LOCAL, "Local build");
	case SUPPORT_SRC_FORK:
		return TRN(ABOUT_VALUE_SOURCE_FORK, "Fork");
	default:
		return TRN(ABOUT_VALUE_SOURCE_UNKNOWN, "Unknown");
	}
}

const char *support_repo(void)
{
	return RFOUNDRY_GIT_REPO;
}

static bool mcu_line(support_line_t *l, sbuf_t *b)
{
	hal_chip_t c;
	memset(&c, 0, sizeof c);
	if (board.system == NULL || board.system->chip == NULL || !board.system->chip(&c)) {
		return false;
	}
	l->key = "mcu";
	l->label = TRN(ABOUT_LABEL_MCU, "MCU: ");
	if (c.family == HAL_CHIP_HRC7000) {
		sb_str(b, "HR_C7000");
		return true;
	}
	// The STM32F4's DBGMCU IDCODE: device 11:0, silicon revision 31:16 (it may read 0 without a debugger)
	sb_str(b, "STM32F4");
	if (c.idcode != 0u) {
		sb_chr(b, ' ');
		sb_hex(b, c.idcode & 0xFFFu, 3);
		sb_str(b, " rev ");
		sb_hex(b, c.idcode >> 16, 4);
	}
	if (c.flash_kb != 0u) {
		sb_str(b, ", ");
		sb_u32(b, c.flash_kb, 1);
		sb_str(b, " KB");
	}
	return true;
}

static bool flash_line(support_line_t *l, sbuf_t *b)
{
	if (board.flash == NULL || board.flash->jedec == NULL) {
		return false;
	}
	uint32_t id = board.flash->jedec();
	l->key = "flash";
	l->label = TRN(ABOUT_LABEL_FLASH, "Flash: ");
	if (id == 0u) {
		sb_str(b, "-");
		return true;
	}
	sb_hex(b, id, 6);
	return true;
}

static bool display_line(support_line_t *l, sbuf_t *b)
{
	if (board.display == NULL || board.display->panel == NULL) {
		return false;
	}
	hal_display_panel_t p;
	memset(&p, 0, sizeof p);
	p.factory = -1;
	board.display->panel(&p);
	if (p.controller == NULL && !p.id_read && p.factory < 0) {
		return false;
	}
	l->key = "display";
	l->label = TRN(ABOUT_LABEL_DISPLAY, "Display: ");
	// "<controller> <ID> t<type> f<factory byte>", each part the board knows (support-info.md 2)
	if (p.controller != NULL) {
		sb_str(b, p.controller);
		sb_chr(b, ' ');
	}
	if (p.id_read) {
		sb_hex(b, (uint32_t)p.id[0] << 16 | (uint32_t)p.id[1] << 8 | p.id[2], 6);
		sb_chr(b, ' ');
	}
	sb_chr(b, 't');
	sb_u32(b, p.type, 1);
	if (p.factory >= 0) {
		sb_str(b, " f");
		sb_u32(b, (uint32_t)p.factory, 1);
	}
	return true;
}

static uint8_t board_lines(void)
{
	if (board.hw_line == NULL) {
		return 0;
	}
	char buf[SUPPORT_VALUE_MAX];
	uint8_t role, n = 0;
	while (n < UINT8_MAX && board.hw_line(n, &role, buf, sizeof buf)) {
		n++;
	}
	return n;
}

static bool board_line(uint8_t j, support_line_t *l)
{
	uint8_t role = BOARD_HW_COUNT;
	if (board.hw_line == NULL || !board.hw_line(j, &role, l->value, sizeof l->value)) {
		return false;
	}
	l->value[sizeof l->value - 1] = '\0';
	switch (role) {
	case BOARD_HW_DMR:
		l->key = "dmr";
		l->label = TRN(ABOUT_LABEL_DMR_CHIP, "DMR chip: ");
		break;
	case BOARD_HW_RF:
		l->key = "rf";
		l->label = TRN(ABOUT_LABEL_RF_CHIP, "RF chip: ");
		break;
	default:
		l->key = "board";
		l->label = TRN(ABOUT_LABEL_HARDWARE, "Hardware: ");
		break;
	}
	return true;
}

// A stated hardware option (hal_hwopt.h): its name and the choice stored, the ones the board detected too
static bool hwopt_line(uint8_t j, support_line_t *l, sbuf_t *b)
{
	if (j >= board_hwopt_count()) {
		return false;
	}
	const hal_hwopt_t *o = board_hwopt(j);
	l->key = "hwopt";
	l->label = TR_NONE;
	l->label_text = o->name;
	int8_t v = o->get();
	if (v >= 0 && (uint8_t)v < o->count) {
		sb_str(b, o->choices[v]);
	} else {
		sb_str(b, tr(TRN(ABOUT_VALUE_HWOPT_UNSET, "not set")));
	}
	return true;
}

#define FIXED_LINES	3u	// the MCU, the flash, the display

bool support_hw_line(uint8_t i, support_line_t *l)
{
	uint8_t boards = board_lines();
	uint16_t last = (uint16_t)(FIXED_LINES + boards + board_hwopt_count());
	uint8_t n = 0;
	for (uint16_t k = 0; k < last; k++) {
		memset(l, 0, sizeof *l);
		sbuf_t b;
		sb_init(&b, l->value, sizeof l->value);
		bool have;
		if (k == 0) {
			have = mcu_line(l, &b);
		} else if (k == 1) {
			have = flash_line(l, &b);
		} else if (k == 2) {
			have = display_line(l, &b);
		} else if (k < FIXED_LINES + boards) {
			have = board_line((uint8_t)(k - FIXED_LINES), l);
		} else {
			have = hwopt_line((uint8_t)(k - FIXED_LINES - boards), l, &b);
		}
		if (have && n++ == i) {
			return true;
		}
	}
	return false;
}
