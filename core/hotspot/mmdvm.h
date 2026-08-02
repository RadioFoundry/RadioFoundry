// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The MMDVM modem protocol (public; written from its description): frames E0 <length> <command> <payload>, the
// length counting the whole frame. The radio answers as a protocol 1 modem with DMR only, simplex on slot 2
// (MMDVMHost Duplex=0, RadioDesk rfgw's roamer, our bench tools). Other modes' commands are refused (NAK).
//
// The framer splits a byte stream (USB CDC, a UART, UDP datagrams) into frames by the length byte, resynchronizing
// on 0xE0 (docs/spec/gotchas.md 8.10). The responder answers them and runs the DMR side through hotspot.c.
// No RTOS here: the host link task feeds it and the host tests drive it directly.
#ifndef ODR_MMDVM_H
#define ODR_MMDVM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MMDVM_FRAME_START	0xE0
#define MMDVM_FRAME_MAX		255	// one length byte; protocol 2's long frames (E0 00 ...) are skipped

#define MMDVM_PROTOCOL_VERSION	1

// Commands
#define MMDVM_GET_VERSION	0x00
#define MMDVM_GET_STATUS	0x01
#define MMDVM_SET_CONFIG	0x02
#define MMDVM_SET_MODE		0x03
#define MMDVM_SET_FREQ		0x04
#define MMDVM_CAL_DATA		0x08
#define MMDVM_RSSI_DATA		0x09
#define MMDVM_SEND_CWID		0x0A
#define MMDVM_DSTAR_HEADER	0x10
#define MMDVM_DSTAR_EOT		0x13
#define MMDVM_DMR_DATA1		0x18
#define MMDVM_DMR_LOST1		0x19
#define MMDVM_DMR_DATA2		0x1A
#define MMDVM_DMR_LOST2		0x1B
#define MMDVM_DMR_SHORTLC	0x1C
#define MMDVM_DMR_START		0x1D
#define MMDVM_DMR_ABORT		0x1E
#define MMDVM_YSF_DATA		0x20
#define MMDVM_P25_HDR		0x30
#define MMDVM_NXDN_DATA		0x40
#define MMDVM_POCSAG_DATA	0x50
#define MMDVM_FM_PARAMS1	0x60
#define MMDVM_ACK		0x70
#define MMDVM_NAK		0x7F
#define MMDVM_SERIAL		0x80
#define MMDVM_TRANSPARENT	0x90
#define MMDVM_QSO_INFO		0x91
#define MMDVM_DEBUG1		0xF1
// Ours, in a range MMDVM leaves unused: the SFR on-air debug switches (core/dmr/sfr.h, hrcSfr.h); answered through
// the extension hooks below, NAKed until the SFR is ported
#define MMDVM_ODR_SFR_DEBUG	0xD5

// NAK reasons
#define MMDVM_NAK_UNKNOWN	1	// not a command this modem knows
#define MMDVM_NAK_WRONG_MODE	2	// not in a mode that takes it (other digital modes, duplex-only commands, no DMR)
#define MMDVM_NAK_TOO_LONG	3
#define MMDVM_NAK_INVALID	4	// bad length or value, a burst type the radio can't send, out-of-range frequency
#define MMDVM_NAK_FULL		5	// no space: wait for GET_STATUS to show some
#define MMDVM_NAK_RF_BUSY	6	// SFR: the inbound call is being repeated

// Modes (SET_MODE, SET_CONFIG's state byte, GET_STATUS's state byte)
#define MMDVM_MODE_IDLE		0
#define MMDVM_MODE_DSTAR	1
#define MMDVM_MODE_DMR		2
#define MMDVM_MODE_YSF		3
#define MMDVM_MODE_P25		4
#define MMDVM_MODE_NXDN		5
#define MMDVM_MODE_POCSAG	6
#define MMDVM_MODE_FM		10
#define MMDVM_MODE_CW		98
#define MMDVM_MODE_LOCKOUT	99
#define MMDVM_MODE_ERROR	100

// SET_CONFIG (protocol 1) payload: [0] flags, [1] modes enabled, [2] TX delay / 10 ms, [3] state, [4] RX level,
// [5] CW ID level, [6] DMR color code, [7] DMR delay, [8] oscillator offset, [10] DMR TX level, ...
#define MMDVM_CONFIG_SIMPLEX	0x80	// flags: Duplex=0
#define MMDVM_MODE_BIT_DMR	0x02	// modes enabled / GET_STATUS byte 3

// GET_STATUS (protocol 1) frame bytes: 3 modes enabled, 4 state, 5 flags, 6 D-Star space, 7 DMR slot 1 space,
// 8 DMR slot 2 space, 9 YSF, 10 P25, 11 NXDN, 12 POCSAG space; 13 on: our extension's bytes (SFR debug counters)
#define MMDVM_STATUS_TX			0x01
#define MMDVM_STATUS_ADC_OVERFLOW	0x02
#define MMDVM_STATUS_RX_OVERFLOW	0x04
#define MMDVM_STATUS_TX_OVERFLOW	0x08
#define MMDVM_STATUS_LOCKOUT		0x10
#define MMDVM_STATUS_DAC_OVERFLOW	0x20
#define MMDVM_STATUS_CD			0x40
#define MMDVM_STATUS_INBOUND_BUSY	0x80	// ours (SFR): an inbound call is being repeated
#define MMDVM_STATUS_BASE_LEN		13
#define MMDVM_STATUS_EXTRA_MAX		120

// DMR_DATA control byte
#define MMDVM_DMR_SYNC_DATA	0x40
#define MMDVM_DMR_SYNC_AUDIO	0x20
#define MMDVM_DMR_FRAME_LEN	33

// ---- Framer ----
typedef struct {
	uint8_t buf[MMDVM_FRAME_MAX];
	uint16_t len;		// bytes in buf
	uint16_t need;		// the frame's length once known, 0 before
	uint16_t skip;		// bytes of a long (protocol 2) frame still to skip
	uint32_t junk;		// bytes thrown away while looking for 0xE0
	uint32_t skipped;	// frames skipped (too long, or a bad length byte)
} mmdvm_framer_t;

void mmdvm_framer_reset(mmdvm_framer_t *f);
// One byte in; returns the frame's length when it completes one (the frame is in f->buf until the next call), else 0
uint16_t mmdvm_framer_byte(mmdvm_framer_t *f, uint8_t b);

// ---- Responder ----
typedef struct {
	// Bytes to the host (one whole frame per call)
	void (*write)(const uint8_t *buf, size_t len);
	const char *description;	// GET_VERSION text, e.g. "OpenDigitalRadio DM-1701 ba226121"
} mmdvm_io_t;

// Optional extension (the SFR, when ported): our vendor command, GET_STATUS bytes after byte 12, a GET_VERSION
// suffix (" SFR TS1>TS2 TS", docs/spec/gotchas.md 8.11), and what SET_CONFIG / SET_FREQ said (SFR is chosen by
// the simplex flag clear and RX = TX, evaluated after either, gotchas 8.12)
typedef struct {
	uint8_t (*vendor)(uint8_t command, const uint8_t *payload, uint8_t len);	// 0 = ACK, else NAK reason
	uint8_t (*status_extra)(uint8_t *buf, uint8_t max);				// bytes written
	uint8_t (*version_suffix)(char *buf, uint8_t max);				// chars written
	void (*config_changed)(bool simplex, uint32_t rx_hz, uint32_t tx_hz);
	uint8_t (*status_flags)(void);							// ORed into byte 5
} mmdvm_ext_t;

void mmdvm_init(const mmdvm_io_t *io);
void mmdvm_set_ext(const mmdvm_ext_t *ext);

// One complete frame from the host (as the framer returns it)
void mmdvm_frame(const uint8_t *frame, uint16_t len, uint32_t now_ms);
// Housekeeping, every few ms
void mmdvm_poll(uint32_t now_ms);
// The host went away (timeout or port closed): stop the hotspot, forget the configuration
void mmdvm_end(void);

// hal_millis() of the last frame from the host (0 = none since mmdvm_init / mmdvm_end)
uint32_t mmdvm_last_frame_ms(void);
// Send one frame (command + payload) to the host; a hotspot_send_fn: hotspot_init(modem, mmdvm_send)
void mmdvm_send(uint8_t command, const uint8_t *payload, uint8_t len);

#endif
