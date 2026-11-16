// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI's view of trunked channels (docs/spec/dmr-trunking-codeplug.md 4): which system a channel uses, the system's name
// and whether it is set up. The radio core starts and leaves systems as channels are selected
#include <string.h>
#include "config.h"
#include "ui/ui_svc.h"
#include "radio/radio.h"
#include "radio/radio_trunk.h"
#include "codeplug/codeplug_modes.h"
#include "ui/fmt.h"

#if CONFIG_DMR_TRUNK
int ui_svc_t3_channel(uint16_t num, const channel_t *ch, bool *usable)
{
	(void)num;
	if (ch->kind != CODEPLUG_KIND_DMR_TRUNK) {
		return -1;
	}
	if (usable != NULL) {
		*usable = radio_t3_usable(ch->system);
	}
	return ch->system;
}

bool ui_svc_t3_name(uint8_t n, char *name)
{
	return radio_t3_name(n, name);
}

void ui_svc_t3_poll(uint32_t id)
{
	radio_req_t3_poll(id);
}

void ui_svc_t3_dial(const char *digits)
{
#if CONFIG_DMR_TRUNK_GW
	radio_req_t3_dial(digits);
#else
	(void)digits;
#endif
}

#if CONFIG_DMR_TRUNK_GW
typedef struct {
	uint32_t gw;
	char *out;
	size_t len;
	bool on;
} cli_ctx_t;

static void cli_get(const radio_t3_view_t *x, void *ctx)
{
	cli_ctx_t *c = ctx;
	c->on = x->on;
	if (!x->call_cli || x->cli_gw != c->gw || x->cli_n == 0 || x->state != T3_FOLLOW) {
		return;
	}
	char d[T3_DIAL_MAX + 1];
	t3_bcd_text(x->cli_bcd, x->cli_n, d);
	str_copy(c->out, c->len, d);
}
#endif

bool ui_svc_t3_cli(uint32_t gw, char *out, size_t len)
{
#if CONFIG_DMR_TRUNK_GW
	if (!t3_dial_gw(gw) || len == 0) {
		return false;
	}
	out[0] = '\0';
	cli_ctx_t c = { .gw = gw, .out = out, .len = len };
	radio_with_t3_view(cli_get, &c);
	return c.on;
#else
	(void)gw;
	(void)out;
	(void)len;
	return false;
#endif
}
#endif
