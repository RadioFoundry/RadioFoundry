// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Zone sharing over DMR data (CONFIG_ZONE_SHARE, docs/spec/zone-share.md): sending a share record to a radio or a
 * talkgroup, and the offers other radios send us. The record itself is made and imported by
 * core/codeplug/zone_import.h; nothing here touches the codeplug.
 *
 *   sending   zone_share_send (any task): the record's fragments go into the air queue at once, all or none; the
 *             end comes as the data service event DMR_DATA_EV_ZONE_SENT or _FAILED and zone_share_send_state()
 *   offers    with offers set to Ask, a whole, valid record from another radio becomes the offer: one at a time
 *             (others are dropped while it is pending), at most ZS_OFFERS_PER_HOUR an hour, gone after
 *             ZS_OFFER_TTL_MS. The event DMR_DATA_EV_ZONE_OFFER tells the UI. The user imports, previews or
 *             rejects it; zone_share_offer_done() frees it. Nothing is applied without the user
 *   directory zone_share_request asks a directory service for the zone of the site the request goes through; its
 *             answer is an offer like any other (taken with offers Off too: the user asked), or a refusal
 *             (DMR_DATA_EV_ZONE_REFUSED, zone_share_last_refusal())
 */

#ifndef RFOUNDRY_ZONE_SHARE_API_H
#define RFOUNDRY_ZONE_SHARE_API_H

#include <stdbool.h>
#include <stdint.h>

#define ZS_OFFERS_OFF			0		// offers are not taken (the service's state until the settings set it)
#define ZS_OFFERS_ASK			1		// offers are shown, applied only when the user imports them

#define ZS_OFFERS_PER_HOUR		6
#define ZS_OFFER_TTL_MS			(10UL * 60UL * 1000UL)
#define ZS_SEND_WAIT_MS			10000	// a fragment the data service doesn't take by then fails the send
#define ZS_REQUEST_WAIT_MS		60000	// a directory answer later than this isn't taken

typedef enum { ZS_SEND_IDLE, ZS_SEND_SENDING, ZS_SEND_SENT, ZS_SEND_FAILED } zs_send_state_t;

typedef struct
{
	bool     pending;
	bool     taken;						// the user opened it: it no longer expires
	bool     group;						// sent to a talkgroup, not to us
	bool     directory;					// the answer to our directory request
	uint32_t src;						// the sender's DMR ID
	uint32_t serial;					// bumped with each new offer
	uint32_t atMs;
	uint16_t length;					// the record's
} zone_share_offer_t;

typedef struct
{
	uint32_t rxMessages;				// datagrams to ZS_PORT handed to us
	uint32_t rxBad;						// not a valid message or record
	uint32_t rxBusy;					// fragments dropped: an offer pending, or another sender's transfer
	uint32_t rxOff;						// fragments dropped: offers Off
	uint32_t offers;					// offers made
	uint32_t rateLimited;				// whole records dropped by the hourly limit
	uint32_t expired;					// offers never answered
	uint32_t sends;						// zone_share_send calls taken
	uint32_t sent;						// ... that went out whole
	uint32_t sendFailed;
	uint32_t requests;					// directory requests sent
	uint32_t refusals;					// directory refusals received
} zone_share_stats_t;

// The radio task: once, then with the data service's tick
void zone_share_init(void);
void zone_share_tick(uint32_t nowMs);

// Any task
void zone_share_set_offers(uint8_t mode);
uint8_t zone_share_offers(void);
// The record to dst (group: a talkgroup, unconfirmed; else confirmed to the radio). False when a send is still going,
// the record is invalid or the air queue has no room for it
bool zone_share_send(uint32_t dst, bool group, const uint8_t *record, int length);
zs_send_state_t zone_share_send_state(void);
// A directory request to the service's ID (docs/spec/zone-share.md 6); false when it couldn't be queued
bool zone_share_request(uint32_t service);
uint8_t zone_share_last_refusal(void);

// The pending offer (false: none). record, when not NULL, points at its octets: valid and unchanged until
// zone_share_offer_done for that serial
bool zone_share_offer(zone_share_offer_t *out, const uint8_t **record);
// The user has it open: it doesn't expire while open
void zone_share_offer_take(uint32_t serial);
// Imported or rejected: freed for the next one
void zone_share_offer_done(uint32_t serial);

void zone_share_get_stats(zone_share_stats_t *out);

#endif
