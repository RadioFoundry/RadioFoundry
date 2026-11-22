// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion framing (docs/spec/ysf-modem.md 3, 4): the FICH's fields, the callsign data, the transmitter
// that turns a header, voice frames and a terminator into a stream of dibits, and the receiver that finds the frame
// sync in a stream of soft symbols, decodes the FICH and hands on callsigns and voice. V/D mode 2 ("DN") is sent;
// V/D mode 1 is received too (its voice goes out as 72-bit AMBE+2 frames for the caller to decode); Voice FR ("VW")
// is sent and received where CONFIG_YSF_VW builds it (ysf_vw.h: full-rate frames for the caller's codec), and Data
// FR is recognized and reported, not decoded.
//
// A frame is 480 dibits (100 ms): the 20-dibit sync, the 100-dibit FICH and 360 dibits of payload in five
// subframes. A dibit is two bits, the first in bit 1 (3.2 (2)); ysf_c4fm_level gives its level.
// No RTOS, no board. The voice codec stays outside: frames go to and come from the caller as 49 bits, one per byte.
#ifndef RFOUNDRY_YSF_FRAME_H
#define RFOUNDRY_YSF_FRAME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ysf/ysf_fec.h"

#define YSF_FRAME_DIBITS	480u
#define YSF_SYNC_DIBITS		20u
#define YSF_FICH_DIBITS		100u
#define YSF_PAYLOAD_DIBITS	360u
#define YSF_SUBFRAMES		5u
#define YSF_SUB_BITS		144u	// one subframe of the payload
#define YSF_SYNC		0xD471C9634DULL	// 40 bits, the leftmost first (4.2.1 (3))
#define YSF_CALLSIGN_LEN	10u	// characters, space filled
#define YSF_RADIO_ID_LEN	5u
#define YSF_RADIO_ID_ALL	"*****"	// the destination radio ID of a call to everyone (radio ID mode)

// FICH fields (figure 4-9, table 4-1)
enum { YSF_FI_HEADER = 0, YSF_FI_COMM = 1, YSF_FI_TERM = 2, YSF_FI_TEST = 3 };
enum { YSF_CM_GROUP = 0, YSF_CM_RADIO_ID = 1, YSF_CM_INDIVIDUAL = 3 };
enum { YSF_DT_VD1 = 0, YSF_DT_DATA_FR = 1, YSF_DT_VD2 = 2, YSF_DT_VOICE_FR = 3 };
enum { YSF_MR_DIRECT = 0, YSF_MR_DOWN_FREE = 1, YSF_MR_DOWN_BUSY = 2 };
#define YSF_CS_CALLSIGNS	2u	// "assign callsigns 1, 2 and 3": the only value the standard defines

typedef struct {
	uint8_t fi;	// frame type: header, communication, terminator, test
	uint8_t cs;	// callsign information
	uint8_t cm;	// call mode
	uint8_t bn, bt;	// block number and total (data)
	uint8_t fn, ft;	// frame number and the last frame number of the rolling pattern
	uint8_t dev;	// 1 narrow deviation
	uint8_t mr;	// message route
	uint8_t voip;	// 1 via the internet
	uint8_t dt;	// data type
	uint8_t sql;	// 1 squelch code off
	uint8_t sc;	// squelch code 0-127 (127 break out). Radios call it DG-ID (docs/spec/ysf-modem.md 3.2)
} ysf_fich_t;

uint32_t ysf_fich_pack(const ysf_fich_t *f);
void ysf_fich_unpack(uint32_t v, ysf_fich_t *f);

// The callsign data a transmission carries (table 4-2): ASCII, space filled, not terminated
typedef struct {
	char dest[YSF_CALLSIGN_LEN];	// CSD1: the destination callsign or group name ("ALL" for a CQ call)
	char src[YSF_CALLSIGN_LEN];	// CSD1: our callsign
	char down[YSF_CALLSIGN_LEN];	// CSD2: the repeater's downlink callsign (spaces when direct)
	char up[YSF_CALLSIGN_LEN];	// CSD2: its uplink callsign
	char rem[4][YSF_RADIO_ID_LEN];	// CSD3: downlink ID, uplink ID, the VoIP station's ID, our radio ID
} ysf_csd_t;

// Spaces, then up to 10 characters of s (upper case, printable ASCII; anything else a space)
void ysf_callsign_set(char out[YSF_CALLSIGN_LEN], const char *s);
// The callsign as a terminated string without its trailing spaces (out has room for 11)
void ysf_callsign_get(const char in[YSF_CALLSIGN_LEN], char *out);
void ysf_csd_init(ysf_csd_t *c, const char *src, const char *dest);
// Radio ID mode (FICH CM 1, table 4-2): the destination field becomes the destination radio ID (dest_id, 5
// characters; YSF_RADIO_ID_ALL calls everyone) and the source's radio ID, which is c->rem[3]. The caller sends CM 1
void ysf_csd_radio_id(ysf_csd_t *c, const char dest_id[YSF_RADIO_ID_LEN]);
// The destination as a person reads it, a terminated string (out has room for 11): in radio ID mode the destination
// radio ID ("ALL" for YSF_RADIO_ID_ALL), since the field's second half is the caller's own ID; otherwise the callsign
// or group name without its trailing spaces
void ysf_dest_show(uint8_t cm, const char dest[YSF_CALLSIGN_LEN], char *out);

// One frame's coded payload, used by both sides: where its subframe parts sit, in payload bits (0..719)
static inline uint32_t ysf_sub_bit(uint32_t sub, uint32_t off)
{
	return sub * YSF_SUB_BITS + off;
}

// --- transmitter ---

// The next voice frame: true with 49 bits (one per byte) in bits; false when the codec has none ready, and the
// transmitter sends the silence frame it was given
typedef bool (*ysf_voice_pull_t)(void *ctx, uint8_t bits[YSF_VOICE_BITS]);
// Voice FR (VW, ysf_vw.h): the next full-rate frame's 88 bits, the first in bit 7 of byte 0; false as above
#define YSF_VW_IMBE_BYTES	11u
typedef bool (*ysf_vw_pull_t)(void *ctx, uint8_t imbe[YSF_VW_IMBE_BYTES]);

typedef struct {
	ysf_fich_t fich;		// the fields the transmission keeps: cm, dev, mr, voip, sql, sc (the rest are set here)
	ysf_csd_t csd;
	uint16_t preamble;		// symbols of alternating -3 / +3 before the first header (rounded up to even; 0 none)
	uint8_t headers;		// header frames before the voice (the standard recommends one; more cover a slow key-up)
	ysf_whiten_t order;		// the DCH units' whitening order (ysf_fec.h)
	uint8_t silence[YSF_VOICE_BITS];	// the codec's own frame for no sound, sent when pull has none
	ysf_voice_pull_t pull;		// NULL: silence until ysf_tx_stop
	void *ctx;
	// Voice FR (VW) in place of V/D mode 2: the full-rate frames from pull_fr (ctx as above), silence_fr when it has
	// none. The header, terminator and FICH say DT 3
	bool vw;
	uint8_t silence_fr[YSF_VW_IMBE_BYTES];
	ysf_vw_pull_t pull_fr;
} ysf_tx_config_t;

typedef enum { YSF_TX_PREAMBLE, YSF_TX_HEADER, YSF_TX_VOICE, YSF_TX_TERM, YSF_TX_DONE } ysf_tx_state_t;

typedef struct {
	ysf_tx_config_t cfg;
	uint8_t state;		// ysf_tx_state_t of the frame being sent
	uint8_t frame[YSF_FRAME_DIBITS / 4u];	// the frame being sent, built a subframe at a time
	uint16_t pos;		// dibits of it sent (of the preamble, before the first header)
	uint8_t headers_sent;
	uint8_t fn;		// the voice frame number, 0..5 rolling (VW: communication frames sent, held at 2)
	bool stop;
	uint32_t frames, voice_frames, silences;
} ysf_tx_t;

void ysf_tx_start(ysf_tx_t *t, const ysf_tx_config_t *cfg);
// The next dibit 0-3, -1 once the terminator has gone (ysf_sym_pull_t fits)
int ysf_tx_dibit(void *tx);
// End the transmission: the voice frame being sent is finished, then the terminator
void ysf_tx_stop(ysf_tx_t *t);
static inline bool ysf_tx_done(const ysf_tx_t *t)
{
	return t->state == YSF_TX_DONE;
}

// --- receiver ---

typedef enum {
	YSF_END_TERMINATOR,	// the terminator frame
	YSF_END_LOST,		// four frame syncs in a row missed (4.7 (2))
} ysf_end_t;

// What a transmission has told the receiver so far: the fields of its latest good FICH and the callsigns, each
// filled when a header, a terminator or a V/D frame's DCH carried it with a good CRC
typedef struct {
	ysf_fich_t fich;
	ysf_csd_t csd;
	uint8_t have;		// YSF_HAVE_* bits: which parts of csd arrived
} ysf_call_t;

#define YSF_HAVE_DEST	0x01u
#define YSF_HAVE_SRC	0x02u
#define YSF_HAVE_DOWN	0x04u
#define YSF_HAVE_UP	0x08u
#define YSF_HAVE_REM12	0x10u
#define YSF_HAVE_REM34	0x20u

typedef struct {
	// A frame with a good FICH began: the mode (DN, VW, VD1, data) for the screen; a new transmission on the first
	void (*frame)(void *ctx, const ysf_call_t *call, bool first);
	// The callsign data grew (call->have): show the caller
	void (*call)(void *ctx, const ysf_call_t *call);
	// V/D mode 2 voice: 49 codec bits and a quality figure (ysf_vd2_voice_decode)
	void (*voice)(void *ctx, const uint8_t bits[YSF_VOICE_BITS], uint8_t quality);
	// V/D mode 1 voice: the 72-bit AMBE+2 frame as sent (with the codec's own FEC), nine bytes most significant
	// first. NULL: V/D mode 1 voice is dropped
	void (*voice72)(void *ctx, const uint8_t frame[9]);
	void (*end)(void *ctx, ysf_end_t why);
	void *ctx;
} ysf_rx_cb_t;

// Voice FR (VW): a full-rate frame's 88 bits and what its codes corrected (YSF_VW_ERRS), with ysf_rx_cb_t's ctx. Set
// apart from the callbacks above (ysf_rx_voice_fr), so callers without VW keep their initializers
typedef void (*ysf_voice_fr_t)(void *ctx, const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t errs);

typedef enum { YSF_RX_SEARCH, YSF_RX_SYNC, YSF_RX_FICH, YSF_RX_PAYLOAD } ysf_rx_state_t;

// Sync tolerances: bits of the 40 that may be wrong
#define YSF_SYNC_SEARCH_ERRORS	3	// searching (a false sync then also needs a FICH with a good CRC)
#define YSF_SYNC_TRACK_ERRORS	8	// where the next sync is due

typedef struct {
	ysf_rx_cb_t cb;
	uint8_t state;		// ysf_rx_state_t
	bool invert;		// the sync was found with its levels inverted
	bool in_call;		// a transmission is being received (a good FICH since the last end)
	uint64_t hard;		// the last 40 hard bits, newest in bit 0
	uint16_t pos;		// dibits into the current part
	uint8_t misses;		// frame syncs missed in a row
	bool fich_ok;		// this frame's FICH decoded
	uint8_t soft[2u * YSF_DCH_BITS_LONG / 2u];	// soft bits of the part being collected, packed (ysf_soft_put)
	ysf_call_t call;
	// Counters for the debug link and the tests. late_syncs: transmissions joined after their header
	uint32_t syncs, late_syncs, fich_good, fich_bad, frames, voices, voice72s, ends, losts;
	uint32_t csd_good, csd_bad, order_all;	// order_all: DCH units whose CRC matched YSF_WHITEN_ALL
	uint32_t sync_bits, sync_errors;	// hard bit errors in the syncs of locked frames: a raw BER estimate
	uint32_t voice_frs, voice_fr_errors;	// VW voice frames, and the bits their codes corrected
	ysf_voice_fr_t voice_fr;	// NULL: VW voice is dropped
} ysf_rx_t;

void ysf_rx_init(ysf_rx_t *r, const ysf_rx_cb_t *cb);
// Where VW voice goes (after ysf_rx_init, which clears it)
static inline void ysf_rx_voice_fr(ysf_rx_t *r, ysf_voice_fr_t fn)
{
	r->voice_fr = fn;
}
// One soft symbol from the demodulator: the matched filter value and the value of a +1 level (ysf_sym_push_t fits)
void ysf_rx_symbol(void *rx, int16_t soft, int16_t unit);
// One hard dibit, from a modem that decides its symbols itself (a chip's own 4FSK receiver)
void ysf_rx_dibit(ysf_rx_t *r, uint8_t dibit);
// The receiver holds a frame sync (the demodulator's loops may slow down)
static inline bool ysf_rx_locked(const ysf_rx_t *r)
{
	return r->state != YSF_RX_SEARCH;
}

#endif
