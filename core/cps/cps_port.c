// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The legacy CPS engine on the host link (cps_legacy.h): replies through hostlink_write, ticks from the host link
// task while a CPS session lasts, reboot through board.system
#include "cps/cps_legacy.h"
#include "hostlink/hostlink.h"
#include "board.h"
#include "hal_time.h"
#include "debuglink/dlog.h"

#define REBOOT_DELAY_MS		500	// the reply drains, the PC closes the port (legacy-cps-interop.md 2.3)

static void port_write(const uint8_t *buf, size_t len)
{
	hostlink_write(buf, len);
}

static void port_reboot(bool save)
{
	dlog("cps: %s", save ? "save and reboot" : "reboot");
	hal_delay_ms(REBOOT_DELAY_MS);
	if (board.system != NULL && board.system->reboot != NULL) {
		board.system->reboot(HAL_REBOOT_NORMAL);
	}
}

static const cps_io_t io = { .write = port_write, .reboot = port_reboot };

static void handler(const uint8_t *buf, size_t len, uint32_t now_ms)
{
	cps_feed(buf, len, now_ms);
}

void cps_legacy_start(void)
{
	cps_init(&io);
	hostlink_set_cps_handler(handler);
}
