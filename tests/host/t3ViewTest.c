// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's Tier III view sections (core/debuglink/debuglink_t3.c): each fits one GET_VIEW reply with every
// field at its widest, so no key is cut off by truncated=1 (docs/spec/debuglink.md 3.7, 3.26).
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink.h"
#include "debuglink/dl_frame.h"
#include "radio/radio.h"
#include "radio/radio_trunk.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static radio_t3_view_t wide;
static radio_t3_setup_t staged;

// The radio core and engine as far as the sections use them
uint32_t hal_millis(void) { return 0x7FFFFFFFu; }
void radio_with_t3_view(void (*fn)(const radio_t3_view_t *x, void *ctx), void *ctx) { fn(&wide, ctx); }
void radio_req_t3(uint8_t cmd) { (void)cmd; }
radio_t3_setup_t *radio_t3_staged(void) { return &staged; }
void radio_t3_reset(radio_t3_setup_t *s) { (void)s; }
void radio_t3_fix(radio_t3_setup_t *s) { (void)s; }
bool radio_t3_load(uint8_t n, radio_t3_setup_t *s) { (void)n; (void)s; return false; }
uint8_t t3_dial_pack(const char *digits, uint8_t bcd[T3_DIAL_MAX / 2]) { (void)digits; (void)bcd; return 0; }
void t3_bcd_text(const uint8_t *bcd, uint8_t n, char *out)
{
	(void)bcd;
	memset(out, '9', n);
	out[n] = '\0';
}

// One section as cmd_get_view formats it: the room after status, uptime and next, less the truncation mark's
static size_t section(void (*fn)(const radio_view_t *v, dl_text_t *t), char *buf, bool *full)
{
	static radio_view_t v;
	dl_text_t t;
	dl_text_init(&t, buf, DL_REPLY_MAX - 6u - 16u);
	fn(&v, &t);
	*full = t.full;
	buf[t.len] = '\0';
	return t.len;
}

static bool has(const char *text, const char *key)
{
	return strstr(text, key) != NULL;
}

int main(void)
{
	static char text[DL_REPLY_MAX];
	bool full;

	// Every field at its widest: counters and IDs all ones, the lists and the data full, every age shown
	memset(&wide, 0xFF, sizeof wide);
	// A bool holding 0xFF is undefined (UBSan stops on the load): every flag true instead
	wide.on = wide.unavail = wide.ip_gw = wide.ip_advised = wide.confirmed = true;
	wide.call_group = wide.call_ours = wide.ptt_wait = wide.can_talk = wide.vote_on = wide.hunt_strict = true;
	wide.call_data = wide.pd_wait = wide.stunned = wide.auth_key = wide.asleep = wide.csc_valid = true;
	wide.rx_off = wide.divert_group = wide.call_als = wide.killed = wide.st_group = wide.sd_group = true;
	wide.cli_group = wide.call_cli = wide.supl_group = true;
	wide.nra = sizeof wide.ra / sizeof wide.ra[0];
	wide.ndgna = RADIO_T3_DGNA;
	wide.sd_len = sizeof wide.sd_data;
	wide.supl_len = sizeof wide.supl_data;
	wide.cli_n = T3_DIAL_MAX;
	wide.nadj = sizeof wide.adj / sizeof wide.adj[0];

	section(debuglink_view_t3, text, &full);
	CHECK(!full && has(text, "t3.lent=") && has(text, "t3.lent_calls="));
	section(debuglink_view_t3_roam, text, &full);
	CHECK(!full);
	section(debuglink_view_t3_ra, text, &full);
	CHECK(!full && has(text, "t3.ra3=") && has(text, "t3.turn_slow="));
	section(debuglink_view_t3_svc, text, &full);
	CHECK(!full && has(text, "t3.ip_advised="));
	section(debuglink_view_t3_msg, text, &full);
	CHECK(!full && has(text, "t3.st_age_ms=") && has(text, "t3.sd_age_ms="));
#if CONFIG_DMR_TRUNK_GW
	CHECK(has(text, "t3.cli_age_ms="));
#endif
#if CONFIG_DMR_TRUNK_SUPL
	CHECK(has(text, "t3.supl_age_ms="));
#endif

	printf("t3ViewTest: %d failures\n", failures);
	return failures ? 1 : 0;
}
