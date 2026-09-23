// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The debug link's wire format (docs/spec/debuglink.md): framing, CRC and command dispatch, without the RTOS or the
// board, so the host tests run it. debuglink.c supplies the command handler and the port writer.
//
//   A5 5A cmd seq len_lo len_hi payload[len] crc_lo crc_hi
//
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR) over cmd .. the end of the payload.
// A reply carries cmd | 0x80 and the request's seq; its payload starts with a status byte (DL_ST_*).
#ifndef DEBUGLINK_PROTO_H
#define DEBUGLINK_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DL_SOF1			0xA5u	// never an MMDVM frame (0xE0) nor a legacy CPS request (ASCII letters)
#define DL_SOF2			0x5Au
#define DL_PROTO_VERSION	1u
#define DL_HDR			6u	// SOF1 SOF2 cmd seq len(2)
#define DL_REQ_MAX		64u	// request payload bytes
#define DL_REPLY_MAX		1024u	// reply payload bytes, status included
#define DL_FRAME_TIMEOUT_MS	200u	// a frame left incomplete this long is dropped
#define DL_REPLY_FLAG		0x80u
#define DL_CMD_ERROR		0xFFu	// reply to a request that could not be decoded (bad CRC, too long)

// Commands
enum {
	DL_CMD_PING		= 0x01,
	DL_CMD_VERSION		= 0x02,
	DL_CMD_GET_DIAG		= 0x03,
	DL_CMD_RESET_COUNTERS	= 0x04,
	DL_CMD_SCREENSHOT	= 0x05,
	DL_CMD_KEY		= 0x06,
	DL_CMD_PTT		= 0x07,
	DL_CMD_GET_LOG		= 0x08,
	DL_CMD_GET_VIEW		= 0x09,
	DL_CMD_RADIO		= 0x0A,
	DL_CMD_INFO_CHIP	= 0x0B,
	DL_CMD_READ_MEM		= 0x0C,
	DL_CMD_REBOOT		= 0x0D,
	DL_CMD_DMR_DATA		= 0x0E,	// DMR data service requests (debuglink_dmr_data.c)
	DL_CMD_STORAGE		= 0x0F,	// the settings / journal region: info, SPI flash read, claim (docs/spec/debuglink.md 3.13)
	DL_CMD_AUDIO_PROBE	= 0x10,	// audio chip registers and PCM levels, CONFIG_AUDIO_PROBE (docs/spec/debuglink.md 3.14)
	DL_CMD_SPLIT		= 0x15,	// split duplex (CONFIG_HOTSPOT_SPLIT only; docs/spec/debuglink.md 3.15)
	DL_CMD_APRS		= 0x11,	// APRS: send now, status, message, test call, settings, NMEA (debuglink_aprs.c, 3.16)
	DL_CMD_SNMP		= 0x19,	// the SNMP agent: settings, communities, notification port, test (debuglink_snmp.c, 3.17)
};

// DL_CMD_DMR_DATA operations (first payload byte; docs/spec/debuglink.md 3.12). IDs are u32 little endian
enum {
	DL_DATA_OP_SMS = 1,		// dst u32, flags u8 (bit 0 group, 1 ACK requested, 2 ETSI, 3 the radio's format), text
	DL_DATA_OP_CALL_ALERT,		// dst u32
	DL_DATA_OP_RADIO_CHECK,		// dst u32
	DL_DATA_OP_SETTINGS,		// ARS server u32 (0 off), flags u8 (bit 0 radio check reply, 1 call alert ack, 2 LRRP,
					// 3 SMS in ETSI form, 4 radio check in ETSI form)
	DL_DATA_OP_INBOX,		// index u8 (0 newest) -> src u32, dst u32, group u8, flags u8 (bit 0 unread, 1 ETSI), text
	DL_DATA_OP_MARK_READ,		// index u8
	DL_DATA_OP_EMERGENCY,		// u8: 0 cancel, 1 start (the emergency key), 2 dismiss the alert
	DL_DATA_OP_EMERGENCY_SETTINGS,	// mode u8, dst u32 (0 the channel's), flags u8 (bit 0 dst is a radio, 1 ETSI FID,
					// 2 no alert tone), alarm tries u8, hot mic s u8
};

// DL_CMD_STORAGE operations (first payload byte; docs/spec/debuglink.md 3.13)
enum {
	DL_STORAGE_OP_INFO = 0,		// -> region u8 (0 none, 1 ours, 2 foreign), claim pending u8, then u32 pairs (address,
					// size): settings ring, journal, calibration copy
	DL_STORAGE_OP_READ,		// address u32, length u16 (<= DL_MEM_READ_MAX) -> address u32, the bytes. SPI flash, only
					// inside the three ranges
	DL_STORAGE_OP_CLAIM,		// token u32 (the chip UID's low word) -> reason u8 (as REBOOT's); the erase follows in
					// the housekeeping task: poll the view's storage.region
};

// DL_CMD_APRS operations (first payload byte; docs/spec/debuglink.md 3.16)
enum {
	DL_APRS_OP_SEND_NOW = 1,	// a position beacon now (-> REFUSED: no callsign, frequency or position)
	DL_APRS_OP_STATUS,		// text: a status packet
	DL_APRS_OP_MESSAGE,		// "CALL text": a message with acks (-> BUSY: one still waiting)
	DL_APRS_OP_TEST_CALL,		// callsign (0-9 characters, empty = back to the settings): RAM only
	DL_APRS_OP_SET,			// field u8 (DL_APRS_F_*), value u32 (signed as two's complement): saved
	DL_APRS_OP_NMEA,		// an NMEA sentence, as from the host ("$GPRMC,...*hh")
};

// DL_APRS_OP_SET fields
enum {
	DL_APRS_F_TX = 0,		// automatic beacons 0/1
	DL_APRS_F_REGION,		// aprs_region_t
	DL_APRS_F_FREQ_HZ,		// the custom region's frequency
	DL_APRS_F_SSID,			// 0-15
	DL_APRS_F_LAT,			// fixed position, 1e-7 degree
	DL_APRS_F_LON,
	DL_APRS_F_POS_SRC,		// aprs_pos_src_t
	DL_APRS_F_INTERVAL_S,		// 0 = none (the engine keeps its minimum)
	DL_APRS_F_SMART,		// SmartBeaconing 0/1
	DL_APRS_F_VIA,			// aprs_via_t
	DL_APRS_F_DMR_DST,		// digital destination ID
	DL_APRS_F_LEVEL,		// AFSK level 0-100
	DL_APRS_F_PATH,			// aprs_path_t
	DL_APRS_F_KISS,			// KISS TNC on the host port 0/1
	DL_APRS_F_DMR_UDT,		// ETSI UDT instead of LRRP 0/1
	DL_APRS_F_AFTER_PTT,		// beacon after an over 0/1
	DL_APRS_F_DISTANCE_M,		// distance trigger, 0 = off
	DL_APRS_F_RX,			// receive on the APRS frequency 0/1
	DL_APRS_F_IGATE,		// RF -> APRS-IS 0/1
	DL_APRS_F_IGATE_TX,		// APRS-IS -> RF 0/1
	DL_APRS_F_DIGI,			// aprs_digi_mode_t
	DL_APRS_F_TXDELAY,		// 10 ms units, 5-100
	DL_APRS_F_DMR_GPS,		// our position in DMR voice calls (ETSI GPS Info) 0/1
};

// DL_CMD_SNMP operations (first payload byte; docs/spec/debuglink.md 3.17)
enum {
	DL_SNMP_OP_SET = 0,		// field u8 (DL_SNMP_F_*), value u32: saved (the trap port: RAM only)
	DL_SNMP_OP_COMMUNITY,		// which u8 (0 read, 1 write), the text (0-32 bytes; empty: read "public", write none)
	DL_SNMP_OP_TEST_TRAP,		// queue a coldStart notification now
};
#define DL_SNMP_COMMUNITY_MAX	32

// DL_SNMP_OP_SET fields
enum {
	DL_SNMP_F_FLAGS = 0,		// SNMP_CFG_* bits (core/snmp/snmpMib.h)
	DL_SNMP_F_RF_MANAGER,		// DMR ID for RF notifications, 0 = none
	DL_SNMP_F_RF_GAP_S,		// seconds between RF notifications, 0 = 60, else 10-3600
	DL_SNMP_F_TRAP_PORT,		// the host's notification port, 0 = 162 (RAM only)
};

// Reply status (the first payload byte)
enum {
	DL_ST_OK		= 0,
	DL_ST_UNKNOWN_CMD	= 1,
	DL_ST_BAD_ARG		= 2,	// wrong length or a value out of range
	DL_ST_BUSY		= 3,	// try again shortly (an injected key still held, rate limit)
	DL_ST_UNAVAILABLE	= 4,	// not built in, or nothing to act on (no display, no such key)
	DL_ST_BAD_CRC		= 5,	// DL_CMD_ERROR only
	DL_ST_TOO_LONG		= 6,	// DL_CMD_ERROR only: request payload over DL_REQ_MAX
	DL_ST_REFUSED		= 7,	// a safety check said no (READ_MEM outside the whitelist, REBOOT gate)
};

uint16_t dl_crc16(uint16_t crc, const uint8_t *p, size_t n);
#define DL_CRC_INIT		0xFFFFu

// One command: the request payload in, the reply payload out (out[0] = status), at most cap bytes. Returns the reply
// payload length (>= 1)
typedef size_t (*dl_handler_fn)(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
// Bytes to the host
typedef void (*dl_write_fn)(const uint8_t *buf, size_t len);

typedef struct {
	uint8_t state;
	uint8_t cmd, seq;
	uint16_t len, got;
	uint16_t crc;
	uint32_t last_ms;
	uint8_t req[DL_REQ_MAX];
	// counters
	uint32_t frames, bad_crc, too_long, timeouts;
} dl_parser_t;

void dl_parser_init(dl_parser_t *p);
// In the middle of a frame (the next bytes belong to it). A frame idle past DL_FRAME_TIMEOUT_MS is dropped here
bool dl_parser_in_frame(dl_parser_t *p, uint32_t now_ms);
// Feed bytes that start a frame (buf[0] == DL_SOF1) or continue one. Takes bytes up to the end of one frame and
// answers it (handler, then write with the reply framed in reply_buf, which must hold DL_HDR + DL_REPLY_MAX + 2).
// Returns the bytes taken; a byte after SOF1 that isn't SOF2 is not taken (it may start something else), so the
// return is 1 then (0 when SOF1 came in the previous chunk). 0 also when len is 0 or buf[0] is not SOF1 outside
// a frame: the caller gives those bytes to someone else
size_t dl_parser_feed(dl_parser_t *p, const uint8_t *buf, size_t len, uint32_t now_ms, dl_handler_fn handler,
		      dl_write_fn write, uint8_t *reply_buf);

// Frame a payload already placed at buf + DL_HDR: header and CRC around it. Returns the frame length
size_t dl_frame(uint8_t *buf, uint8_t cmd, uint8_t seq, uint16_t len);

#endif
