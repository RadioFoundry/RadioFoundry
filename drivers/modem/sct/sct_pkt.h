// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SCT DMR / dPMR baseband packet protocol (docs/spec/modem-sct.md): framing, field lengths, the packet builder and
// the parser for what the chip sends. Pure code: no bus, no timing, so the host tests run it as is.
//
// A packet is `84 A9 61 LEN_hi LEN_lo TYPE fields... [2F parity] [00]`. LEN counts the bytes after TYPE (with the
// parity marker and byte when parity is on). A field is its ID byte and data; most IDs have a fixed data length,
// a few carry their own length byte. An odd-length packet gets one 00 that LEN doesn't count.
#ifndef SCT_PKT_H
#define SCT_PKT_H

#include <stdint.h>
#include <stdbool.h>

#define SCT_SYNC0		0x84
#define SCT_SYNC1		0xA9
#define SCT_SYNC2		0x61
#define SCT_HDR_BYTES		6		// sync (3), LEN (2), TYPE
#define SCT_PKT_MAX		256		// a 180-byte DIGC_DATA_FRAME and a few fields fit
#define SCT_PARITY_MARK		0x2F

// TYPE byte
#define SCT_TYPE_AMBE_BYPASS	0x80
#define SCT_TYPE_CODEC		0x40		// the body goes to the codec chip on the SCT's I2C (page, register, value)
#define SCT_TYPE_READ		0x20		// a query, and every report from the chip
#define SCT_TYPE_FAR		0x10
#define SCT_TYPE_CONTROL	0x00
#define SCT_TYPE_DPMR		0x03
#define SCT_TYPE_PROGRAM	0x04
#define SCT_TYPE_DMR		0x05

// Field IDs this driver uses
enum {
	SCT_F_CHAN_D		= 0x01,	// encoded voice: LEN, data
	SCT_F_VOCODER_SEL	= 0x10,
	SCT_F_PARITY_ENABLE	= 0x16,
	SCT_F_ACK		= 0x17,	// a status code, SCT_ACK_*
	SCT_F_WORK_MODE		= 0x18,
	SCT_F_CARRIER_READY	= 0x19,	// bits 1-0: carrier lost, ready, ready with the phase reversed
	SCT_F_PROCESS_MODE	= 0x1A,
	SCT_F_NEW_EVENT		= 0x1B,
	SCT_F_DEMOD_GAIN	= 0x1C,	// u16 MSB first, Q16.11 (2048 = 0 dB); 0 auto calibration
	SCT_F_REPORT_FIELD	= 0x1D,
	SCT_F_BER_REPORT	= 0x1E,
	SCT_F_CALL_STOP		= 0x21,
	SCT_F_SUBAUDIO		= 0x26,
	SCT_F_CODEC_SELECT	= 0x2D,
	SCT_F_Q_PRO_STR		= 0x30,	// queries 0x30-0x33: the answer is LEN, text
	SCT_F_Q_HW_VER		= 0x31,
	SCT_F_Q_SW_VER		= 0x32,
	SCT_F_Q_CID_SN		= 0x33,
	SCT_F_VOCODER_IO_SET	= 0x3E,
	SCT_F_MODOUT_CONFIG	= 0x41,	// two-point modulation: the delay between the two outputs, 4 reserved bytes
	SCT_F_DATA_FRAME	= 0x43,	// DIGC_DATA_FRAME: PROP, LEN, data
	SCT_F_DIGI_MIC_GAIN	= 0x68,	// digital mode microphone gain: u16 MSB first, Q16.11 (2048 = 0 dB)
	SCT_F_DIGI_SPK_GAIN	= 0x69,	// digital mode speaker gain, the same
	SCT_F_SQ_LEVEL		= 0x6C,	// the chip's squelch: gains, OOBE interval and thresholds
	SCT_F_DMR_SLC		= 0x6E,
	SCT_F_DMR_CALL_SLOT	= 0x6F,
	SCT_F_DMR_FLC		= 0x76,
	SCT_F_DMR_CC		= 0x77,
	SCT_F_DMR_CALL_START	= 0x78,
	SCT_F_DMR_CALL_OPTION	= 0x79,
	SCT_F_DMR_OFFSET	= 0x7A,	// the RF_TIMING output's offset and length against the slot timing
	SCT_F_DMR_SLOT_TYPE	= 0x7B,
	SCT_F_DMR_EMB		= 0x7C,
	SCT_F_DMR_CALLED_ID	= 0x7D,
	SCT_F_DMR_OWN_ID	= 0x7E,
	SCT_F_DMR_SLOT_FOUND	= 0x7F,
};

// ACK_MESSAGE codes
enum {
	SCT_ACK_OK		= 0x00,
	SCT_ACK_RESET_READY	= 0x01,	// the application runs (after the boot block)
	SCT_ACK_TOO_LONG	= 0x02,
	SCT_ACK_PARITY		= 0x03,
	SCT_ACK_UNKNOWN		= 0x04,
	SCT_ACK_LENGTH		= 0x05,
	SCT_ACK_PROCESSING	= 0x06,
	SCT_ACK_FAR_DONE	= 0x0A,	// a message to the far end has gone
	SCT_ACK_NOT_ACTIVATED	= 0x0C,
	SCT_ACK_UNSUPPORTED	= 0x0D,
	SCT_ACK_NOT_LOADED	= 0x0E,
};

// NEW_EVENT codes (DMR)
enum {
	SCT_EV_DATA_SYNC_FEC	= 1,	// data sync, FEC error in the block
	SCT_EV_DATA_SYNC	= 4,
	SCT_EV_VOICE_SYNC	= 5,
	SCT_EV_SYNC_LOST	= 8,
	SCT_EV_CARRIER		= 12,
	SCT_EV_CARRIER_LOST	= 13,
};

// DIGC_DATA_FRAME PROP byte in DMR: bit 4 a voice burst (data = the 9-byte LC for the embedded signaling), bits 3-0
// the data type (TS 102 361-1 slot type, the same numbers as dmr_burst_kind_t's data kinds)
#define SCT_PROP_VOICE		0x10
#define SCT_PROP_TYPE		0x0F
#define SCT_FRAME_MAX		180

// Field data length after the ID in a packet to the chip: >= 0 fixed, SCT_LEN_BYTE a length byte then that many
// bytes, SCT_LEN_FRAME a DIGC_DATA_FRAME (PROP, LEN, data), SCT_LEN_UNKNOWN an ID this table doesn't know
#define SCT_LEN_BYTE		(-1)
#define SCT_LEN_FRAME		(-2)
#define SCT_LEN_UNKNOWN		(-3)
int sct_field_len(uint8_t id);
// The same for a report from the chip, where a few fields differ (query answers carry a length byte)
int sct_report_len(uint8_t id);

typedef struct {
	uint8_t buf[SCT_PKT_MAX];
	uint16_t len;		// bytes written so far; after sct_pkt_end, the bytes to send (with any pad)
	bool bad;		// a field didn't fit or had the wrong length: sct_pkt_end returns 0
} sct_pkt_t;

void sct_pkt_begin(sct_pkt_t *p, uint8_t type);
// A field with a fixed data length (len must match sct_field_len) or a length byte (len is written before the data)
void sct_pkt_add(sct_pkt_t *p, uint8_t id, const uint8_t *data, uint8_t len);
void sct_pkt_add1(sct_pkt_t *p, uint8_t id, uint8_t value);
// A DIGC_DATA_FRAME
void sct_pkt_add_frame(sct_pkt_t *p, uint8_t prop, const uint8_t *data, uint8_t len);
// A codec-bypass body (TYPE SCT_TYPE_CODEC): raw bytes, no field IDs
void sct_pkt_add_raw(sct_pkt_t *p, const uint8_t *data, uint8_t len);
// LEN, parity (when on) and the pad. Returns the bytes to send, 0 if the packet is bad
uint16_t sct_pkt_end(sct_pkt_t *p, bool parity);

// One field as a board supplies it: its ID and data (len as sct_field_len gives it)
typedef struct {
	uint8_t id;
	uint8_t len;
	const uint8_t *data;
} sct_field_t;

// One write to the codec chip through the baseband (a codec-bypass packet, TYPE SCT_TYPE_CODEC: page, register,
// value; the chip adds the codec's I2C address). The chip answers with a TYPE SCT_TYPE_CODEC packet holding an
// ACK_MESSAGE
typedef struct {
	uint8_t page;
	uint8_t reg;
	uint8_t value;
} sct_codec_write_t;

// The codec-bypass packet for one write: header, page, register, value and the pad, no parity
#define SCT_CODEC_PKT_BYTES	10
void sct_pkt_codec(uint8_t out[SCT_CODEC_PKT_BYTES], const sct_codec_write_t *w);

// Byte-stream reassembly of what the chip sends. Bytes outside a packet (pad, noise) are skipped
typedef struct {
	uint8_t buf[SCT_PKT_MAX];
	uint16_t pos;
	uint16_t need;		// total bytes of the packet being read, 0 while still in the header
	uint32_t skipped;	// bytes outside any packet
	uint32_t overruns;	// packets longer than buf
} sct_rx_t;

void sct_rx_reset(sct_rx_t *rx);
// Feed one byte. True when buf holds a whole packet (header and body, *len bytes); the next feed starts afresh
bool sct_rx_feed(sct_rx_t *rx, uint8_t b, uint16_t *len);

// Walking a received packet's fields
typedef struct {
	const uint8_t *p;
	uint16_t left;
	uint8_t type;
} sct_fields_t;

// pkt holds len bytes from sct_rx_feed. With parity on, the parity is checked and stripped. False if the header or
// parity is wrong
bool sct_fields_begin(sct_fields_t *it, const uint8_t *pkt, uint16_t len, bool parity);
// The next field: its ID and data (for SCT_LEN_BYTE fields the data after the length byte; for a data frame
// PROP, LEN, data as sent). A write's answer (every field of a packet without the read bit, or a field with a single
// byte left after its ID): *ack is true and data[0] the status. False at the end (left 0) or on an ID the table
// doesn't know or a field cut short (left nonzero)
bool sct_fields_next(sct_fields_t *it, uint8_t *id, const uint8_t **data, uint8_t *len, bool *ack);

// 8-digit BCD of an ID (DMR_OWN_ID_BCD, DMR_CALLED_ID_BCD), most significant digits first
void sct_id_bcd(uint32_t id, uint8_t out[4]);

#endif
