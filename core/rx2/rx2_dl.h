// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A second SoC's own debug link server, answering through the main SoC over the link (docs/spec/rx2-link.md 2.3,
// debuglink.md 3.22 FORWARD). The same command numbers and reply layouts as the main SoC's debug link, for what a
// receiver without the radio core has: PING, VERSION, GET_DIAG (board.diag_line), RESET_COUNTERS, GET_LOG (dlog),
// GET_VIEW (the app's section, then the crash log's), INFO_CHIP, READ_MEM (board.system), REBOOT and CRASH. Portable:
// a restart or a crash test asked for runs from rx2_dl_service(), after the reply has gone out.
#ifndef RX2_DL_H
#define RX2_DL_H

#include <stddef.h>
#include <stdint.h>
#include "debuglink/dl_fmt.h"
#include "rx2/rx2_link.h"

// view: the app's own section (its state, the link's counters); NULL for none. link: for the link's counters
void rx2_dl_init(const rx2_recv_t *link, void (*view)(dl_text_t *t));
// rx2_ops_t.dl
size_t rx2_dl_handle(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
// After the link's bytes are handled: a restart or crash test asked for, once its reply is out (may not return)
void rx2_dl_service(void);

#endif
