// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stub entry (CONFIG_STUB_ENTRY, docs/spec/stub-entry.md): load the host's flash-access RAM monitor into free RAM
// and hand the SoC to it at run time, so the flashing tools update the radio with no key held at power-on. Its own
// feature, apart from the debug link:
//
//   debug link on    the debug link's parser hands command 0x1A (STUB) to stub_entry_request and calls
//                    stub_entry_service after each frame, as it always has
//   debug link off   stub_entry_start registers the stub entry's own listener with the host link: the same framing
//                    (dl_frame.h) answering PING, VERSION and STUB (stub_link.c)
//
// Either way the request and reply bytes are the same. Built without the switch, STUB answers DL_ST_UNAVAILABLE.
#ifndef STUB_ENTRY_H
#define STUB_ENTRY_H

#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "debuglink/dl_frame.h"
#include "ui/i18n.h"

// The two lines of the boot splash the radio shows while the monitor has it (show_banner); the UI gallery draws the
// same screen (stub_entry.png), which the web flasher shows for this state. In English here; the radio shows them
// translated (tr). The marks sit on lines of their own: tools/i18n.py passes over a line that starts with #define
#define STUB_ENTRY_LINE1 \
	TRN("PC Loader Connected")
#define STUB_ENTRY_LINE2 \
	TRN("Power-cycle to exit")

#if CONFIG_STUB_ENTRY
#if !CONFIG_DEBUGLINK
// From hostlink_start, in a build without the debug link: register the listener as the host link's side protocol
void stub_entry_start(void);
#endif
// The STUB command (docs/spec/debuglink.md 3.18): the reply payload in out (out[0] = status), its length returned.
// Answers DL_ST_UNAVAILABLE on a board whose platform has no hal_system_t.enter_stub
size_t stub_entry_request(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
// After the reply to a verified ENTER has been written: quiesce and jump (does not return then); a no-op otherwise.
// From the host link task, after each frame
void stub_entry_service(void);
#else
static inline size_t stub_entry_request(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

static inline void stub_entry_service(void)
{
}
#endif

#endif
