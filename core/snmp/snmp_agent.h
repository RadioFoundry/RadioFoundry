// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The SNMP agent in the firmware (CONFIG_SNMP, docs/spec/snmp.md): the glue between the message processing and MIB
// (snmp.c, snmpMib.c) and the radio. Runs in the host link task:
//
//   USB   requests to UDP 161 of the radio's link address come from the IP gateway's local port table
//         (ipGatewayUdpBindAir) inside hostlink_net_poll and are answered at once; notifications go to the host's
//         address, port 162 (debug link: another port), while the network personality is up
//   RF    requests over the air to our DMR ID, port 161, arrive in the radio task (hostlink_net_packet) and wait in a
//         one-request mailbox for snmp_agent_poll; the answer goes back through the gateway's air side (SAP 4, the
//         data service's listen before talk), at most one per SNMP_RF_ANSWER_GAP_MS. Notifications to the RF manager
//         ID, rate limited (snmpEvents_*)
//
// Settings (communities as hashes, the RF manager, flags) live in settings_t snmp_*; SET (USB, or RF when allowed)
// and the debug link's SNMP command change them.
#ifndef ODR_SNMP_AGENT_H
#define ODR_SNMP_AGENT_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#if CONFIG_SNMP
#include "snmp/snmp.h"
#include "snmp/snmpMib.h"

#define SNMP_USB_MAX		548	// answer size over USB (the gateway's 600-byte frames less the headers)
#define SNMP_RF_MAX		300	// answer size over RF (about 12 rate 1/2 blocks with the headers)
#define SNMP_RF_REQ_MAX		256	// longest request taken over RF
#define SNMP_RF_ANSWER_GAP_MS	1000u	// RF answers at most this often (requests in between are dropped)
#define SNMP_RF_GIVE_UP_MS	2000u	// an RF answer or notification the air side can't take by then is dropped
#define SNMP_EVENT_POLL_MS	100u	// the radio is checked for events this often

// Host link task: once at start (after hostlink_net_init), then every pass
void snmp_agent_init(void);
void snmp_agent_poll(uint32_t now_ms);

typedef struct {
	snmpStats_t snmp;
	uint32_t rf_requests, rf_answers, rf_dropped;	// requests over RF; answers handed to the air; dropped (busy, gap)
	uint32_t traps_usb, traps_rf, traps_dropped;
} snmp_agent_stats_t;
void snmp_agent_stats(snmp_agent_stats_t *s);

// The settings (saved), from the host link task (the debug link)
void snmp_agent_get_config(snmpConfig_t *c);
void snmp_agent_set_config(const snmpConfig_t *c);
// A community from its text; "" = the default (read: "public", write: none)
void snmp_agent_set_community(bool write, const char *text, int len);
// The host's notification port (RAM only, back to 162 at power-up), for test hosts that can't listen on 162
void snmp_agent_set_trap_port(uint16_t port);
uint16_t snmp_agent_trap_port(void);
// Queue a coldStart notification now (the HIL test's check of the notification path)
void snmp_agent_test_trap(void);
#endif

#endif
