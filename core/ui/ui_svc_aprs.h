// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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
bool ui_svc_aprs_send_message(const char *to, const char *text, uint8_t flags);
uint8_t ui_svc_aprs_msg_count(void);
bool ui_svc_aprs_msg(uint8_t i, aprs_msg_t *out, bool read);
bool ui_svc_aprs_msg_delete(uint8_t i);
uint8_t ui_svc_aprs_station_count(void);
bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out);
// How far and which way a heard station is from our position (aprs_position_now, as the beacons and DMR GPS Info use
// it): false when the station or we have no position
bool ui_svc_aprs_range(const aprs_station_t *s, uint32_t *m, uint16_t *deg);

#endif
