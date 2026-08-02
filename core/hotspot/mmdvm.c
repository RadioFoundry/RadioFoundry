// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The MMDVM modem protocol: framer and responder (see mmdvm.h)
#include <string.h>
#include "hotspot/mmdvm.h"
#include "hotspot/hotspot.h"

#define DESCRIPTION_MAX		96	// any build ID plus an extension suffix fits (docs/spec/gotchas.md 8.11)
// MMDVM firmware doesn't acknowledge DMR data frames (it only NAKs a refused one); hosts don't wait for an answer
#define ACK_DMR_DATA		0

// ---- Framer ----

void mmdvm_framer_reset(mmdvm_framer_t *f)
{
	f->len = 0;
	f->need = 0;
	f->skip = 0;
}

uint16_t mmdvm_framer_byte(mmdvm_framer_t *f, uint8_t b)
{
	if (f->skip > 0) {
		f->skip--;
		return 0;
	}
	if (f->len == 0) {
		if (b != MMDVM_FRAME_START) {
			f->junk++;
			return 0;
		}
		f->buf[f->len++] = b;
		return 0;
	}
	if (f->len == 1) {
		if (b == 0) {
			// Protocol 2 long frame: E0 00 <length - 255> ...: not for us, skip it whole
			f->buf[f->len++] = b;
			return 0;
		}
		if (b < 3) {
			// Not a frame; this byte may start the next one
			f->skipped++;
			f->len = 0;
			return mmdvm_framer_byte(f, b);
		}
		f->need = b;
		f->buf[f->len++] = b;
		return 0;
	}
	if ((f->len == 2) && (f->need == 0)) {
		f->skipped++;
		f->skip = (uint16_t)(b + 255 - 3);
		f->len = 0;
		return 0;
	}
	f->buf[f->len++] = b;
	if (f->len < f->need) {
		return 0;
	}
	uint16_t n = f->len;
	f->len = 0;
	f->need = 0;
	return n;
}

// ---- Responder ----

static const mmdvm_io_t *io;
static const mmdvm_ext_t *ext;
static uint32_t last_frame_ms;
static uint8_t state = MMDVM_MODE_IDLE;
static uint8_t modes = MMDVM_MODE_BIT_DMR;
static bool simplex = true;
static uint32_t rx_hz, tx_hz;

void mmdvm_send(uint8_t command, const uint8_t *payload, uint8_t len)
{
	uint8_t frame[MMDVM_FRAME_MAX];

	if ((io == NULL) || (len > (MMDVM_FRAME_MAX - 3))) {
		return;
	}
	frame[0] = MMDVM_FRAME_START;
	frame[1] = (uint8_t)(len + 3);
	frame[2] = command;
	if (len > 0) {
		memcpy(frame + 3, payload, len);
	}
	io->write(frame, (size_t)len + 3);
}

static void ack(uint8_t command)
{
	mmdvm_send(MMDVM_ACK, &command, 1);
}

static void nak(uint8_t command, uint8_t reason)
{
	uint8_t p[2] = { command, reason };

	mmdvm_send(MMDVM_NAK, p, 2);
}

void mmdvm_init(const mmdvm_io_t *i)
{
	io = i;
	mmdvm_end();
}

void mmdvm_set_ext(const mmdvm_ext_t *e)
{
	ext = e;
}

void mmdvm_end(void)
{
	hotspot_stop();
	last_frame_ms = 0;
	state = MMDVM_MODE_IDLE;
	modes = MMDVM_MODE_BIT_DMR;
	simplex = true;
	rx_hz = 0;
	tx_hz = 0;
}

uint32_t mmdvm_last_frame_ms(void)
{
	return last_frame_ms;
}

static void get_version(void)
{
	uint8_t p[1 + DESCRIPTION_MAX];
	uint8_t n = 0;
	const char *d = (io->description != NULL) ? io->description : "OpenDigitalRadio";

	p[0] = MMDVM_PROTOCOL_VERSION;
	while ((d[n] != '\0') && (n < DESCRIPTION_MAX)) {
		p[1 + n] = (uint8_t)d[n];
		n++;
	}
	if ((ext != NULL) && (ext->version_suffix != NULL) && (n < DESCRIPTION_MAX)) {
		char suffix[DESCRIPTION_MAX];
		uint8_t s = ext->version_suffix(suffix, (uint8_t)(DESCRIPTION_MAX - n));

		for (uint8_t i = 0; (i < s) && (n < DESCRIPTION_MAX); i++) {
			p[1 + n++] = (uint8_t)suffix[i];
		}
	}
	mmdvm_send(MMDVM_GET_VERSION, p, (uint8_t)(1 + n));
}

static void get_status(void)
{
	uint8_t p[MMDVM_STATUS_BASE_LEN - 3 + MMDVM_STATUS_EXTRA_MAX];
	uint8_t n = MMDVM_STATUS_BASE_LEN - 3;
	uint8_t flags = 0;
	bool dmr = hotspot_running() && (modes & MMDVM_MODE_BIT_DMR);

	memset(p, 0, sizeof(p));
	if (hotspot_transmitting()) {
		flags |= MMDVM_STATUS_TX;
	}
	if (hotspot_rx_overflow()) {
		flags |= MMDVM_STATUS_RX_OVERFLOW;
	}
	if (hotspot_tx_overflow()) {
		flags |= MMDVM_STATUS_TX_OVERFLOW;
	}
	if (hotspot_carrier()) {
		flags |= MMDVM_STATUS_CD;
	}
	if ((ext != NULL) && (ext->status_flags != NULL)) {
		flags |= ext->status_flags();
	}
	p[0] = modes & MMDVM_MODE_BIT_DMR;
	p[1] = state;
	p[2] = flags;
	p[4] = 0;				// DMR slot 1: not used in simplex
	p[5] = dmr ? hotspot_space() : 0;	// DMR slot 2: frames the host may send
	if ((ext != NULL) && (ext->status_extra != NULL)) {
		n = (uint8_t)(n + ext->status_extra(p + n, MMDVM_STATUS_EXTRA_MAX));
	}
	mmdvm_send(MMDVM_GET_STATUS, p, n);
}

static void set_config(const uint8_t *p, uint8_t len)
{
	if (len < 7) {
		nak(MMDVM_SET_CONFIG, MMDVM_NAK_INVALID);
		return;
	}
	if ((p[3] != MMDVM_MODE_IDLE) && (p[3] != MMDVM_MODE_DMR)) {
		nak(MMDVM_SET_CONFIG, MMDVM_NAK_INVALID);
		return;
	}
	simplex = (p[0] & MMDVM_CONFIG_SIMPLEX) != 0;
	modes = p[1] & MMDVM_MODE_BIT_DMR;	// other modes asked for are simply not enabled
	state = p[3];
	hotspot_config(p[6] & 0x0F, simplex, (uint16_t)(p[2] * 10));
	if ((ext != NULL) && (ext->config_changed != NULL)) {
		ext->config_changed(simplex, rx_hz, tx_hz);
	}
	ack(MMDVM_SET_CONFIG);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_freq(const uint8_t *p, uint8_t len)
{
	if (len < 9) {
		nak(MMDVM_SET_FREQ, MMDVM_NAK_INVALID);
		return;
	}
	uint32_t rx = le32(p + 1);
	uint32_t tx = le32(p + 5);
	uint8_t level = (len >= 10) ? p[9] : 255;	// p[10..13]: the POCSAG frequency, not used

	if (!hotspot_set_freq(rx, tx, level)) {
		nak(MMDVM_SET_FREQ, MMDVM_NAK_INVALID);
		return;
	}
	rx_hz = rx;
	tx_hz = tx;
	if ((ext != NULL) && (ext->config_changed != NULL)) {
		ext->config_changed(simplex, rx_hz, tx_hz);
	}
	ack(MMDVM_SET_FREQ);
}

static void set_mode(const uint8_t *p, uint8_t len)
{
	if ((len < 1) || ((p[0] != MMDVM_MODE_IDLE) && (p[0] != MMDVM_MODE_DMR))) {
		nak(MMDVM_SET_MODE, MMDVM_NAK_INVALID);
		return;
	}
	if ((p[0] == MMDVM_MODE_DMR) && !(modes & MMDVM_MODE_BIT_DMR)) {
		nak(MMDVM_SET_MODE, MMDVM_NAK_WRONG_MODE);
		return;
	}
	state = p[0];
	ack(MMDVM_SET_MODE);
}

static void dmr_data(uint8_t command, const uint8_t *p, uint8_t len, uint32_t now_ms)
{
	if ((command == MMDVM_DMR_DATA1) || !hotspot_running() || !(modes & MMDVM_MODE_BIT_DMR)) {
		nak(command, MMDVM_NAK_WRONG_MODE);	// simplex: slot 2 only
		return;
	}
	if (len < (1 + MMDVM_DMR_FRAME_LEN)) {
		nak(command, MMDVM_NAK_INVALID);
		return;
	}
	if (state == MMDVM_MODE_IDLE) {
		state = MMDVM_MODE_DMR;		// as a modem does when DMR traffic starts
	}
	uint8_t r = hotspot_net_frame(p[0], p + 1, now_ms);

	if (r != 0) {
		nak(command, r);
	} else if (ACK_DMR_DATA) {
		ack(command);
	}
}

void mmdvm_frame(const uint8_t *frame, uint16_t len, uint32_t now_ms)
{
	if ((len < 3) || (frame[0] != MMDVM_FRAME_START) || (frame[1] != len)) {
		return;
	}
	uint8_t command = frame[2];
	const uint8_t *p = frame + 3;
	uint8_t plen = (uint8_t)(len - 3);

	last_frame_ms = (now_ms != 0) ? now_ms : 1;
	if (!hotspot_running()) {
		hotspot_start(now_ms);	// the first frame takes the modem; retried while the radio refuses
	}

	switch (command) {
	case MMDVM_GET_VERSION:
		get_version();
		break;
	case MMDVM_GET_STATUS:
		get_status();
		break;
	case MMDVM_SET_CONFIG:
		set_config(p, plen);
		break;
	case MMDVM_SET_FREQ:
		set_freq(p, plen);
		break;
	case MMDVM_SET_MODE:
		set_mode(p, plen);
		break;
	case MMDVM_DMR_DATA1:
	case MMDVM_DMR_DATA2:
		dmr_data(command, p, plen, now_ms);
		break;
	case MMDVM_DMR_ABORT:
		hotspot_net_abort();
		break;
	case MMDVM_DMR_START:
	case MMDVM_DMR_SHORTLC:
		nak(command, MMDVM_NAK_WRONG_MODE);	// duplex only
		break;
	case MMDVM_SEND_CWID:
		nak(command, MMDVM_NAK_WRONG_MODE);	// no CW keyer: the host must not count on it for its ID
		break;
	case MMDVM_QSO_INFO:
	case MMDVM_ACK:
	case MMDVM_NAK:
		break;					// informative; never answered
	case MMDVM_ODR_SFR_DEBUG:
		if ((ext != NULL) && (ext->vendor != NULL)) {
			uint8_t r = ext->vendor(command, p, plen);

			if (r == 0) {
				ack(command);
			} else {
				nak(command, r);
			}
		} else {
			nak(command, MMDVM_NAK_UNKNOWN);
		}
		break;
	default:
		// D-Star, YSF, P25, NXDN, POCSAG, FM, M17, calibration, serial passthrough ...
		nak(command, (command >= MMDVM_DSTAR_HEADER) && (command < MMDVM_ACK) ? MMDVM_NAK_WRONG_MODE
										       : MMDVM_NAK_UNKNOWN);
		break;
	}
}

void mmdvm_poll(uint32_t now_ms)
{
	hotspot_poll(now_ms);
}
