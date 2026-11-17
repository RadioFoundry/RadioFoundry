// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The host link task's copy of the radio view, one for every service that task runs (debug link, network services,
// SNMP, USRP), instead of a copy each: 760 B or more apiece. Only the host link task touches it. A service refreshes
// it (radio_get_view(&hostlink_view)) before it reads it, so a read between its own refreshes may see a newer view
// another service fetched, never an older one
#ifndef RFOUNDRY_HOSTLINK_VIEW_H
#define RFOUNDRY_HOSTLINK_VIEW_H

#include "radio/radio.h"

extern radio_view_t hostlink_view;

#endif
