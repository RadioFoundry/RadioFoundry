// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's commands and their operations (docs/spec/debuglink.md 3). The framing, the reply status and the
// commands the stub entry shares (PING, VERSION, STUB) are in dl_frame.h.
#ifndef DEBUGLINK_PROTO_H
#define DEBUGLINK_PROTO_H

#include "debuglink/dl_frame.h"

// Commands
enum {
	// 0x01 PING, 0x02 VERSION: dl_frame.h (the stub entry answers them without the debug link)
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
	// 0x1A STUB: dl_frame.h (core/stubentry, CONFIG_STUB_ENTRY, 3.18)
	DL_CMD_CRASH		= 0x1B,	// the crash log: clear, crash on purpose (CONFIG_CRASH_LOG; debuglink_crash.c, 3.19)
	DL_CMD_CODEPLUG		= 0x1C,	// the fast codeplug protocol (CONFIG_CPS_FAST; core/cps/cps_fast.c, 3.20, cps-fast.md)
	DL_CMD_WATCHDOG		= 0x1D,	// the hardware watchdog's bench tests (CONFIG_WATCHDOG; debuglink_watchdog.c, 3.21)
	DL_CMD_RX2		= 0x1E,	// the second receiver's SoC: state, the update bridge, restart (debuglink_rx2.c, 3.22)
};

// DL_CMD_RX2 operations (first payload byte; docs/spec/debuglink.md 3.22). The token is u32 little endian
enum {
	DL_RX2_OP_INFO = 0,	// -> status, flags u8 (DL_RX2_F_*)
	DL_RX2_OP_BRIDGE = 1,	// token u32 -> status; after the reply the radio bridges the cable to the second SoC
	DL_RX2_OP_RESTART = 2,	// token u32 -> status: restart the second SoC
};
#define DL_RX2_F_PRESENT	0x01u	// the board has a second receiver
#define DL_RX2_F_UP		0x02u	// it answers
#define DL_RX2_F_BRIDGE		0x04u	// the update bridge is there
#define DL_RX2_F_RESTART	0x08u	// so is the restart
#define DL_RX2_TOKEN		0x47445242u	// 'BRDG

// DL_CMD_WATCHDOG operations (first payload byte; docs/spec/debuglink.md 3.21). u32 little endian. Every test ends in
// a watchdog reset, and the reset is what it checks
enum {
	DL_WDT_OP_CLEAR_GAP = 0,	// -> status: restart the longest idle gap measurement (watchdog.max_gap_ms)
	DL_WDT_OP_TEST = 1,		// kind u8 (DL_WDT_TEST_*), token u32 -> status; the test starts after the reply
	DL_WDT_OP_LOAD = 2,		// arg u32 x3, token u32 -> status: re-arm with a raw load (platform units,
					// hal_watchdog_test_load), then starve; unavailable where the platform has no such test
};
enum {
	DL_WDT_TEST_STARVE = 1,		// the idle task stops feeding; everything else runs on (the link answers until the reset)
	DL_WDT_TEST_SPIN = 2,		// the host link's task spins with interrupts on: the idle task never runs again
	DL_WDT_TEST_IRQ_OFF = 3,	// the host link's task spins with the kernel's interrupts masked
};
// The tests' guard, little endian in the payload: bytes 'W' 'D' 'O' 'G', so a stray frame can't reset the radio
#define DL_WDT_TOKEN		0x474F4457u

// DL_CMD_CRASH operations (first payload byte; docs/spec/debuglink.md 3.19)
enum {
	DL_CRASH_OP_CLEAR = 0,	// -> status: forget the kept record
	DL_CRASH_OP_TEST = 1,	// kind u8 (DL_CRASH_TEST_*), token u32 -> status; the crash comes after the reply
};
// HANG: the host link's task spins with interrupts on and the watchdog fed as usual, so the hang check (crash-log.md
// 1.1) keeps the record; unavailable without CONFIG_WATCHDOG
enum { DL_CRASH_TEST_TRAP = 1, DL_CRASH_TEST_ASSERT = 2, DL_CRASH_TEST_HANG = 3 };
// The test's guard, little endian in the payload: bytes 'C' 'R' 'S' 'H' (as CRASH_MAGIC), so a stray frame can't crash
// the radio
#define DL_CRASH_TOKEN		0x48535243u

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
	DL_DATA_OP_IP_SETTINGS,		// compression u8 (0 off, 1 auto, 2 all), confirmed u8 (0 off, 1 rules, 2 all), tries u8
					// (0 the default, 1-8), rule u32 x 2 (0 unused, a radio ID, or 0x80000000 | a UDP port),
					// optional off-net gateway ID u32 (0 none; left as it is when absent)
	DL_DATA_OP_IP_SEND,		// dst u32, flags u8 (bit 0 group, bit 1 an IPv4 destination follows), source port u16,
					// destination port u16, with bit 1 the IPv4 destination u32, payload: a UDP datagram
					// into the IP gateway's air path as if from the host (CONFIG_DMR_IP_GATEWAY)
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
	DL_APRS_F_TO_IS,		// our own packets to APRS-IS (the host helper, or the radio's own client) 0/1
	DL_APRS_F_POWER,		// RF power: 0 = the radio's current level, n = the board's level n - 1
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

#endif
