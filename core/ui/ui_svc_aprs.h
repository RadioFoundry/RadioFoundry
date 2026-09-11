// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI's way into the APRS engine (core/aprs/aprs.h): ui_svc_aprs.c with CONFIG_APRS, weak stand-ins in ui_aprs.c
// otherwise (and in the UI simulator).
#ifndef UI_SVC_APRS_H
#define UI_SVC_APRS_H

#include <stdint.h>
#include <stdbool.h>
#include "aprs/aprs.h"

// False when APRS isn't in this build
bool ui_svc_aprs_view(aprs_view_t *v);
void ui_svc_aprs_cfg(aprs_cfg_t *c);
void ui_svc_aprs_set_cfg(const aprs_cfg_t *c);
bool ui_svc_aprs_send_now(void);
uint8_t ui_svc_aprs_station_count(void);
bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out);

#endif
