// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's state machine (core/radio/radio_sm.c) against a fake board and a fake codeplug
#include "input/input.h"
#include "input/keyassign.h"
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "codeplug/codeplug_modes.h"
#if CONFIG_APRS
#include "aprs/aprs_beacon.h"	// APRS_WHY_*
#endif
#include "radio/radio_sfr.h"
#include "radio/radio_bands.h"
#include "radio/tones.h"
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/ambe/ambeVoice.h"
#include "sim/dmr_sim_modem.h"
#include "radio/radio_hotspot.h"
#include "hotspot/hotspot.h"
#include "hotspot/mmdvm.h"
#include "settings/settings.h"
#include "dmr/dmr_data_api.h"
#include "dmr/dmr_emergency.h"
#include "ui/ui_svc.h"
#include "storage/storage.h"
#include "codeplug/codeplug_kv.h"
#include "dmr/dmrPrivacy.h"
#include "dmr/dmrData.h"
#include "sim/sim_flash.h"
#include "sim/codeplug_copy_stub.h"
#include "radio/vibe.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"
#include "radio/tx_limit.h"
#include "radio/battery.h"
#if CONFIG_DMR_TRUNK_TX
#include "radio/radio_trunk.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "dmr/fec/dmrCach.h"
#include "dmr/trunk/t3_pdu.h"
#include "dmr/trunk/t3_sys.h"
#endif
#include "fuzz.h"
#include "position/position.h"
#include "radio/beep.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake radio ----
static struct {
	int set_mode_calls;
	uint32_t rx_hz;
	radio_bw_t rx_bw;
	subtone_t rx_tone;
	int rx_calls;
	radio_bw_t tx_bw;
	subtone_t tx_tone;
	int tx_calls;
	uint32_t tx_hz;
	uint8_t tx_power;
	int tx_off_calls;
	bool keyed;		// between tx() and tx_off()
	bool audio;
	bool led[2];
	radio_status_t st;
} hw;

static void r_init(void) {}
static radio_mode_t hw_mode;
// As on the DM-1701: the modem starts only with the board in DMR mode (dm1701_dmr_enter)
static void r_set_mode(radio_mode_t m) { hw_mode = m; hw.set_mode_calls++; sim.refuse_start = m != RADIO_MODE_DMR; }
static bool r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
	hw.rx_hz = hz;
	hw.rx_bw = bw;
	hw.rx_tone = *t;
	hw.rx_calls++;
	return true;
}
static bool tx_fail;			// the board refuses to key (synthesizer won't lock, PA fault)
static bool r_tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t)
{
	if (tx_fail) {
		return false;
	}
	hw.tx_calls++;
	hw.tx_hz = hz;
	hw.tx_bw = bw;
	hw.tx_tone = *t;
	hw.tx_power = p;
	hw.keyed = true;
	return true;
}
static void r_tx_off(void) { hw.tx_off_calls++; hw.keyed = false; }
// The scan and dual watch tests: the far radio (fake air 1) is heard only on far_hz, a carrier there while it sends
// (0: on every frequency, no carrier of its own)
static uint32_t far_hz;
static bool far_on_air(void);
static void r_status(radio_status_t *st)
{
	*st = hw.st;
	if (far_hz != 0) {
		bool here = hw.rx_hz == far_hz && far_on_air();
		st->noise = here ? 20 : 120;
		st->rssi_dbm = here ? -80 : -125;
	}
}
static void r_audio(bool on) { hw.audio = on; }
// FM TX tone and its Morse keying (the hotspot's CW ID): every change logged with the time
static struct {
	uint16_t hz;		// tx_tone now, 0 = the microphone
	bool down;		// tx_tone_key
	int changes;		// key edges
	uint32_t first_down_ms, last_up_ms;
	uint32_t down_ms;	// total key-down time
	uint32_t edge_ms;
} tone;
static uint32_t now;
static bool r_tx_tone(uint16_t hz) { tone.hz = hz; if (hz == 0) tone.down = false; return true; }
static bool r_tx_tone_key(bool down)
{
	if (tone.hz == 0 || down == tone.down) {
		return tone.hz != 0;
	}
	if (down) {
		if (tone.first_down_ms == 0) {
			tone.first_down_ms = now;
		}
	} else {
		tone.down_ms += now - tone.edge_ms;
		tone.last_up_ms = now;
	}
	tone.edge_ms = now;
	tone.down = down;
	tone.changes++;
	return true;
}
static char dtmf_on;		// tx_dtmf: the digit sounding
static bool r_tx_dtmf(char digit) { dtmf_on = digit; return true; }
static int16_t ref_trim_ppb = 1;	// ref_trim: the last trim (1: never called)
static bool r_ref_trim(int16_t ppb) { ref_trim_ppb = ppb; return true; }
static hal_radio_t fake_radio = { .init = r_init, .set_mode = r_set_mode, .rx = r_rx, .tx = r_tx, .tx_off = r_tx_off,
					.status = r_status, .audio = r_audio, .tx_tone = r_tx_tone,
					.tx_tone_key = r_tx_tone_key, .tx_dtmf = r_tx_dtmf, .ref_trim = r_ref_trim };

// (fake_radio.volume is set by test_soft_volume only)
static void p_led(uint8_t which, bool on) { hw.led[which] = on; }

static bool ptt_stuck;
static bool p_ptt_stuck(void) { return ptt_stuck; }
static const hal_input_t fake_input = { .ptt_line_stuck = p_ptt_stuck };

// ---- fake PCM audio and vocoder (core/dmr/ambe runs on them) ----
static struct {
	bool started;
	int starts, stops;
	bool pcm;
	bool feed;		// run() hands the callback a 20 ms microphone block, as a driver does (off: no blocks)
	audio_block_cb_t cb;
	void *ctx;
} aud;
static bool a_start(audio_block_cb_t cb, void *ctx) { aud.cb = cb; aud.ctx = ctx; aud.started = true; aud.starts++; return true; }
static void a_stop(void) { aud.started = false; aud.stops++; }
static void a_pcm(bool on) { aud.pcm = on; }
static const hal_audio_t fake_audio = { .start = a_start, .stop = a_stop, .speaker_pcm = a_pcm };

static bool codec_present;
static bool v_present(void) { return codec_present; }
static void v_reset(void) {}
static void v_encode(const int16_t *pcm, uint8_t *bits) { (void)pcm; memset(bits, 0, VOCODER_BITS); }
// Each frame the decoder is given, to a test that looks (the NXDN voice path's check)
static void (*v_decode_hook)(const uint8_t *bits);
static void v_decode(const uint8_t *bits, int16_t *pcm)
{
	if (v_decode_hook != NULL) {
		v_decode_hook(bits);
	}
	memset(pcm, 0, VOCODER_FRAME_SAMPLES * 2);
}
static uint32_t v_options(void) { return VOCODER_OPT_NOISE_REDUCTION | VOCODER_OPT_TONE_FRAMES | VOCODER_OPT_AGC; }
static void v_set_options(uint32_t opts) { (void)opts; }
#if CONFIG_P25 || defined(HAL_VOCODER_LEND_BYTES)
static bool lend_refuse;		// a vocoder with nothing to lend
static uint32_t lends;
#endif
#ifdef HAL_VOCODER_LEND_BYTES
// As the HR_C7000 platform's: the codec's memory lent while no DMR voice runs (P25 runs in it)
#define LENDS	1u		// what a P25 start borrows
static uint64_t lend_mem[HAL_VOCODER_LEND_BYTES / 8];
static void *v_lend(size_t *len)
{
	lends++;
	*len = sizeof(lend_mem);
	return lend_refuse ? NULL : lend_mem;
}
#else
// P25 runs in its own memory, which NXDN shares
#define LENDS	0u
#define v_lend NULL
#endif
static const hal_vocoder_t fake_vocoder = { v_present, v_reset, v_encode, v_decode, NULL, v_options, v_set_options,
					    NULL, v_lend };
static const char *codec_profile;
static const char *v_profile(void) { return codec_profile; }
static uint8_t p_mic(void) { return 0; }
static hal_power_t fake_power = { .led = p_led };	// mic_level set by the VOX check, vibrate by the vibrate check
static int vibrations;
static void p_vibrate(uint16_t ms) { (void)ms; vibrations++; }

static const band_t bands[] = {
	{ 108000000, 135999999, false },	// receive only
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
	{ 480000000, 520000000, false },	// the receive-only extension (test_rx_ext), off unless a test turns it on
};
static bool rx_ext_on;
static const char *const power_names[] = { "L", "M", "H" };
// Keys: only what the side key glue (ui_svc_storage.c) needs, two side keys and an arrow programmable on Home
static const keymap_entry_t keymap[] = {
	{ 0, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ 1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ 2, ACT_TALK_TARGET, ACT_USER_2, false, false, 0 },
	{ 3, ACT_NAV_LEFT, ACT_USER_1, false, true, 0 },
};
static const uint8_t side_keys[] = { 1, 2 };
static const uint8_t home_keys[] = { 3 };

static int gps_enable_calls;
static bool gps_enabled;
static bool gps_here;	// the module answered (board_has_gps): only test_position has one
static bool fake_gps_present(void) { return gps_here; }
static void fake_gps_enable(bool on)
{
	gps_enable_calls++;
	gps_enabled = on;
}
static const hal_gps_t fake_gps = { .present = fake_gps_present, .enable = fake_gps_enable };

// A hardware option (hal_hwopt.h) whose state the tests set; hidden (detected) unless a test says otherwise
static uint8_t fake_hwopt_st = HWOPT_ST_HIDDEN;
static const char *const fake_hwopt_choices[] = { "A", "B" };
static int8_t fake_hwopt_get(void) { return 0; }
static bool fake_hwopt_set(uint8_t c) { (void)c; return true; }
static uint8_t fake_hwopt_state(void) { return fake_hwopt_st; }
static const hal_hwopt_t fake_hwopts[] = {
	{ "Revision", 2, fake_hwopt_choices, fake_hwopt_get, fake_hwopt_set, true, fake_hwopt_state },
};

#if CONFIG_AIR
// ---- fake air interface: board.air (0) and a far radio's provider (1), each the other's far end. A chunk every 30
// ms (run), as a modem chip's records come ----
#include "air/air_syms.h"
#include "p25/p25_engine.h"
#include "p25/p25_codeplug.h"
#include "hostlink/build_info.h"
#if CONFIG_P25_TRUNK
#include "p25/p25_tsbk.h"
#endif
#define AIR_CHUNK	144
#define AIR_DEPTH	4
typedef struct {
	air_event_cb_t cb;
	void *ctx;
	bool open, on, started;
	air_tx_chunk_t q[AIR_DEPTH];
	uint8_t qn;
	uint32_t now_us, opens, closes, begins, aborts, underruns, sent;
	uint32_t keyed_violations;	// board.air modulated with the transmitter off (hal_air.h rule 3)
} fake_air_t;
static fake_air_t fair[2];
static bool air_nofit;			// board.air can't carry P25
static uint32_t air_rng = 0x1234567;

static void air_ev(fake_air_t *p, uint8_t kind, const air_chunk_t *c)
{
	air_event_t e = { .kind = kind, .t_us = p->now_us, .chunk = c };
	if (p->cb != NULL) {
		p->cb(&e, p->ctx);
	}
}

#if CONFIG_P25_TRUNK
// A P25 trunked site around the radio (test_p25_trunk): its control channel on cc_hz, and the far radio's voice
// heard only on voice_hz. On any other frequency the receiver hears noise
static struct {
	bool on;
	uint32_t cc_hz, voice_hz;
	uint16_t grant_tg, grant_ch;	// a group voice grant sent, 0: none
	bool grant_enc;
	bool iden2;			// identifier 2 (out of the radio's bands) sent too
	uint32_t units, rot;
	uint8_t unit[P25_UNIT_BYTES_MAX];
	uint16_t unit_syms, unit_at;
	// The inbound side: what the radio sends on cc_in_hz, and how the site answers (SITE_ANS_*)
	uint32_t cc_in_hz;
	uint8_t ans_reg, ans_aff, ans_req;
	uint8_t in[P25_UNIT_BYTES_MAX];
	uint16_t in_n;
	uint16_t reg_rx, aff_rx, req_rx, bad_rx;
	p25_isp_t last;
	uint8_t out[P25_TSBK_BYTES];	// an answer, sent as the first block of the next out_reps units
	uint8_t out_reps;
	uint8_t grant_after;		// a queued request: units until its grant
} site;
enum { SITE_ANS_ACCEPT, SITE_ANS_DENY, SITE_ANS_NONE, SITE_ANS_QUEUE };
static void site_chunk(uint8_t *syms);
static void site_inbound(const uint8_t *syms, uint16_t n, bool last);
#endif

static bool far_on_air(void) { return fair[1].on; }

static void air_deliver(fake_air_t *to, const uint8_t *syms, uint16_t n)
{
	if (far_hz != 0 && to == &fair[0] && hw.rx_hz != far_hz) {
		return;		// the far radio sends on far_hz; the radio is tuned elsewhere
	}
#if CONFIG_P25_TRUNK
	if (site.on && to == &fair[0] && hw.rx_hz != site.voice_hz) {
		return;		// the far radio sends on the voice channel; the radio is tuned elsewhere
	}
#endif
	int8_t soft[AIR_CHUNK];
	air_chunk_t c = { .t_us = to->now_us, .nsyms = n, .sync_at = -1, .hard = syms, .soft = soft };
	for (uint16_t i = 0; i < n; i++) {
		soft[i] = (int8_t)(32 * air_sym_level(air_sym_get(syms, i)));
	}
	if (to->open && !to->on) {
		air_ev(to, AIR_EV_RX, &c);
	}
}

// One chunk period at provider i
static void air_step1(uint8_t i)
{
	fake_air_t *p = &fair[i], *far = &fair[i ^ 1];
	p->now_us += 30000;
#if CONFIG_P25_TRUNK
	if (i == 0 && !p->on && site.on && (hw.rx_hz == site.cc_hz || hw.rx_hz != site.voice_hz || !far->on)) {
		uint8_t syms[AIR_CHUNK / 4];
		if (hw.rx_hz == site.cc_hz) {
			site_chunk(syms);
		} else {
			for (uint8_t k = 0; k < sizeof(syms); k++) {
				air_rng ^= air_rng << 13;
				air_rng ^= air_rng >> 17;
				air_rng ^= air_rng << 5;
				syms[k] = (uint8_t)air_rng;
			}
		}
		bool was = site.on;
		site.on = false;	// (past air_deliver's filter)
		air_deliver(p, syms, AIR_CHUNK);
		site.on = was;
		return;
	}
#endif
	if (!p->on) {
		if (!far->on) {		// nobody sends: the receiver hands on noise
			uint8_t noise[AIR_CHUNK / 4];
			for (uint8_t k = 0; k < sizeof(noise); k++) {
				air_rng ^= air_rng << 13;
				air_rng ^= air_rng >> 17;
				air_rng ^= air_rng << 5;
				noise[k] = (uint8_t)air_rng;
			}
			air_deliver(p, noise, AIR_CHUNK);
		}
		return;
	}
	if (i == 0 && !hw.keyed) {
		p->keyed_violations++;
	}
	if (!p->started) {
		p->started = true;
		air_ev(p, AIR_EV_TX_STARTED, NULL);
	}
	if (p->qn == 0) {
		uint8_t idle[AIR_CHUNK / 4];
		memset(idle, 0x5F, sizeof(idle));
		p->underruns++;
		air_deliver(far, idle, AIR_CHUNK);
		air_ev(p, AIR_EV_TX_UNDERRUN, NULL);
		return;
	}
	air_tx_chunk_t c = p->q[0];
	memmove(p->q, p->q + 1, (size_t)(--p->qn) * sizeof(c));
	p->sent += c.nsyms;
#if CONFIG_P25_TRUNK
	if (i == 0 && site.on && hw.tx_hz == site.cc_in_hz) {
		site_inbound(c.syms, c.nsyms, (c.flags & AIR_TX_LAST) != 0);	// the site's receiver, not the far radio
	} else
#endif
	air_deliver(far, c.syms, c.nsyms);
	if (c.flags & AIR_TX_LAST) {
		p->on = false;
		air_ev(p, AIR_EV_TX_DONE, NULL);
	} else {
		air_ev(p, AIR_EV_TX_SPACE, NULL);
	}
}

// 4800 symbols/s (P25, NXDN at 9600 bit/s): everything. 2400 symbols/s (NXDN at 4800 bit/s): what air_fit2400 says
// for each provider (a provider that transmits a profile only, or receives it only)
static uint32_t air_fit2400[2] = { AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT, AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT };
static uint32_t air_fit(uint8_t i, const air_profile_t *pr)
{
	if (air_nofit) {
		return 0;
	}
	return pr->sym_rate == 4800 ? AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT : pr->sym_rate == 2400 ? air_fit2400[i] : 0;
}
static bool air_open(uint8_t i, air_event_cb_t cb, void *ctx)
{
	fake_air_t *p = &fair[i];
	uint32_t opens = p->opens, closes = p->closes;
	memset(p, 0, sizeof(*p));
	p->opens = opens + 1;
	p->closes = closes;
	p->cb = cb;
	p->ctx = ctx;
	p->open = true;
	return true;
}
static void air_close(uint8_t i) { fair[i].open = fair[i].on = false; fair[i].cb = NULL; fair[i].qn = 0; fair[i].closes++; }
static bool air_begin(uint8_t i) { if (fair[i].qn == 0 || !fair[i].open) return false; fair[i].on = true; fair[i].started = false; fair[i].begins++; return true; }
static bool air_queue(uint8_t i, const air_tx_chunk_t *c)
{
	if (fair[i].qn == AIR_DEPTH) {
		return false;
	}
	fair[i].q[fair[i].qn++] = *c;
	return true;
}
static void air_end(uint8_t i) { if (fair[i].qn != 0) fair[i].q[fair[i].qn - 1].flags |= AIR_TX_LAST; }
static void air_abort(uint8_t i) { fair[i].qn = 0; fair[i].on = false; fair[i].aborts++; air_ev(&fair[i], AIR_EV_TX_DONE, NULL); }

#define FAKE_AIR(i) \
	static uint32_t air_fit##i(const air_profile_t *pr) { return air_fit(i, pr); } \
	static bool air_open##i(const air_profile_t *pr, air_event_cb_t cb, void *ctx) { (void)pr; return air_open(i, cb, ctx); } \
	static void air_close##i(void) { air_close(i); } \
	static void air_restart##i(void) { } \
	static bool air_begin##i(void) { return air_begin(i); } \
	static bool air_queue##i(const air_tx_chunk_t *c) { return air_queue(i, c); } \
	static uint8_t air_space##i(void) { return (uint8_t)(AIR_DEPTH - fair[i].qn); } \
	static void air_end##i(void) { air_end(i); } \
	static void air_abort##i(void) { air_abort(i); } \
	static uint32_t air_now##i(void) { return fair[i].now_us; } \
	static bool air_load##i(uint32_t *w, uint32_t *s) { *w = 1000u + i; *s = 20000u; return true; } \
	static const hal_air_t fake_air##i = { air_fit##i, air_open##i, air_close##i, air_restart##i, air_begin##i, air_queue##i, \
					       air_space##i, air_end##i, air_abort##i, air_now##i, NULL, NULL, air_load##i, NULL, AIR_CHUNK, \
					       AIR_DEPTH, 60, 100 };
FAKE_AIR(0)
FAKE_AIR(1)
static void air_step(void);	// with the far radio's engine, below
#else
static bool far_on_air(void) { return false; }
#endif

const board_t board = {
	.name = "fake",
	.hwopts = fake_hwopts,
	.hwopt_count = 1,
	.caps = CAP_GPS,
	.gps = &fake_gps,
	.bands = bands,
	.band_count = 4,
	.rx_ext_count = 1,
	.rx_ext_on = &rx_ext_on,
	.power_count = 3,
	.power_names = power_names,
	.power_default = 1,
	.keymap = keymap,
	.keymap_len = 4,
	.side_keys = side_keys,
	.side_key_count = 2,
	.home_keys = home_keys,
	.home_key_count = 1,
	.radio = &fake_radio,
	.power = &fake_power,
	.input = &fake_input,
	.dmr = &sim_modem,
	.audio = &fake_audio,
	.vocoder = &fake_vocoder,
#if CONFIG_AIR
	.air = &fake_air0,
#endif
	.flash = &sim_flash,		// no map until test_factory_reset: RAM only, as on a radio without one
};

// ---- fake codeplug ----
#define NCH 8
static channel_t chans[NCH + 1];
static bool used[NCH + 1];
static zone_t zones[3];
static uint16_t nzones;
static codeplug_source_t source = CODEPLUG_OPENFW;

static uint32_t cp_serial;
codeplug_source_t codeplug_source(void) { return source; }
static uint16_t cp_quick[CODEPLUG_QUICK_KEYS];	// the quick-key slots: the side key assignment
static bool cp_quick_fail;
bool codeplug_quick_keys(uint16_t out[CODEPLUG_QUICK_KEYS]) { memcpy(out, cp_quick, sizeof(cp_quick)); return true; }
bool codeplug_set_quick_keys(const uint16_t in[CODEPLUG_QUICK_KEYS])
{
	if (cp_quick_fail) {
		return false;
	}
	memcpy(cp_quick, in, sizeof(cp_quick));
	cp_serial++;
	return true;
}
uint32_t codeplug_serial(void) { return cp_serial; }
uint16_t codeplug_channel_count(void) { return NCH; }
bool codeplug_channel(uint16_t n, channel_t *out)
{
	if (n < 1 || n > NCH || !used[n]) {
		return false;
	}
	*out = chans[n];
	codeplug_tag_apply(n, out);	// the real tag layer over the fake settings area: a live tag gives the kind
	return true;
}
uint16_t codeplug_zone_count(void) { return nzones; }
bool codeplug_zone(uint16_t i, zone_t *out)
{
	if (i >= nzones) {
		return false;
	}
	*out = zones[i];
	return true;
}

// DMR records: the radio's ID, contacts, RX group lists
static uint32_t own_dmr_id = 9996543;
static const contact_t contacts[] = {
	{ "Main Ops", 201, CONTACT_GROUP, 0 },
	{ "Joe", 9991234, CONTACT_PRIVATE, 0 },
	{ "Local", 9, CONTACT_GROUP, 0 },
};
uint32_t codeplug_dmr_id(void) { return own_dmr_id; }

// The codeplug's settings area: the ARS server key (the gateway's is built out, CONFIG_DMR_IP_GATEWAY=0) and the
// Settings > Sound tone keys, one-byte bools (kv_tone: -1 absent)
static bool kv_ars_present;
static uint8_t kv_serial;
static uint32_t kv_ars;
static int16_t kv_tone[CODEPLUG_KEY_TX_DENY_TONE - CODEPLUG_KEY_CALL_END_TONE + 1] = { -1, -1, -1, -1 };
static bool kv_held;	// a CPS session holds the flash, or another firmware's codeplug: no key can be written
// Record keys (0x0100 up: systems, system entries, secrets, channel tags, Tier III's own), kept in key order
#define KV_T3_MAX	24
static struct {
	uint16_t key;
	uint8_t len;
	uint8_t v[96];		// a system record is CODEPLUG_SYS_HDR + its block
} kv_t3[KV_T3_MAX];
static uint8_t kv_t3_n;
static void kv_t3_set(uint16_t key, const void *v, uint8_t len)
{
	uint8_t i = 0;
	while (i < kv_t3_n && kv_t3[i].key < key) {
		i++;
	}
	CHECK(len <= sizeof(kv_t3[0].v) && kv_t3_n < KV_T3_MAX);
	if (i == kv_t3_n || kv_t3[i].key != key) {
		memmove(&kv_t3[i + 1], &kv_t3[i], (kv_t3_n - i) * sizeof(kv_t3[0]));
		kv_t3_n++;
	}
	kv_t3[i].key = key;
	kv_t3[i].len = len;
	memcpy(kv_t3[i].v, v, len);
	kv_serial++;
}
static void kv_t3_clear(void)
{
	kv_t3_n = 0;
	kv_serial++;
}
static bool kv_is_tone(uint16_t key) { return key >= CODEPLUG_KEY_CALL_END_TONE && key <= CODEPLUG_KEY_TX_DENY_TONE; }
int codeplug_kv_get(uint16_t key, void *buf, uint16_t max)
{
	for (uint8_t i = 0; i < kv_t3_n; i++) {
		if (kv_t3[i].key == key) {
			memcpy(buf, kv_t3[i].v, max < kv_t3[i].len ? max : kv_t3[i].len);
			return kv_t3[i].len;
		}
	}
	if (kv_is_tone(key)) {
		if (kv_tone[key - CODEPLUG_KEY_CALL_END_TONE] < 0) {
			return -1;
		}
		if (max > 0) {
			*(uint8_t *)buf = (uint8_t)kv_tone[key - CODEPLUG_KEY_CALL_END_TONE];
		}
		return 1;
	}
	if (key != CODEPLUG_KEY_ARS_SERVER || !kv_ars_present) {
		return -1;
	}
	memcpy(buf, &kv_ars, max < 4 ? max : 4);
	return 4;
}
// One set: the ARS server as a u32, a tone as one byte
static bool kv_set(uint16_t key, const uint8_t *v, uint16_t len)
{
	if (kv_is_tone(key) && len == 1) {
		kv_tone[key - CODEPLUG_KEY_CALL_END_TONE] = v[0];
	} else if (key == CODEPLUG_KEY_ARS_SERVER && len == 4) {
		kv_ars_present = true;
		kv_ars = v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
	} else {
		return false;
	}
	kv_serial++;	// as codeplug_kv_write: the area's serial, no codeplug reload
	return true;
}
uint8_t codeplug_kv_serial(void) { return kv_serial; }
// The tag layer's writes (codeplug_tag.c): none in these tests
bool codeplug_kv_set(uint16_t key, const void *value, uint16_t len) { (void)key; (void)value; (void)len; return false; }
bool codeplug_kv_remove(uint16_t key) { (void)key; return false; }
// The walk: c->at the next single key to try, up to CODEPLUG_KEY_TX_DENY_TONE, then the Tier III keys in order
void codeplug_kv_begin(codeplug_kv_cursor_t *c, uint16_t lo, uint16_t hi)
{
	c->lo = lo;
	c->hi = hi;
	c->at = lo;
	c->first = 0;	// here the next Tier III key's index
	c->done = 0;
}
int codeplug_kv_next(codeplug_kv_cursor_t *c, uint16_t *key, void *buf, uint16_t max)
{
	for (; !c->done && c->at <= c->hi && c->at <= CODEPLUG_KEY_TX_DENY_TONE; c->at++) {
		int n = codeplug_kv_get(c->at, buf, max);
		if (n >= 0) {
			*key = c->at++;
			return n;
		}
	}
	while (!c->done && c->first < kv_t3_n) {	// then the Tier III keys, above every single key
		uint8_t i = (uint8_t)c->first++;
		if (kv_t3[i].key >= c->lo && kv_t3[i].key <= c->hi) {
			*key = kv_t3[i].key;
			memcpy(buf, kv_t3[i].v, max < kv_t3[i].len ? max : kv_t3[i].len);
			return kv_t3[i].len;
		}
	}
	c->done = 1;
	return -1;
}
bool codeplug_kv_write(const codeplug_kv_op_t *ops, uint8_t n)
{
	if (kv_held) {
		return false;
	}
	for (uint8_t i = 0; i < n; i++) {
		if (!kv_set(ops[i].key, ops[i].value, ops[i].len)) {
			return false;
		}
	}
	return true;
}
bool codeplug_set_dmr_id(uint32_t id)
{
	own_dmr_id = id;
	cp_serial++;
	return true;
}
void codeplug_boot_text(char line1[CODEPLUG_NAME_LEN + 1], char line2[CODEPLUG_NAME_LEN + 1]) { line1[0] = line2[0] = 0; }
void codeplug_radio_name(char out[CODEPLUG_NAME_LEN + 1]) { out[0] = 0; }
#if CONFIG_APRS_AFSK
// APRS (radioCoreTestAprs): no APRS channel in the codeplug
bool codeplug_aprs(uint8_t n, codeplug_aprs_t *out) { memset(out, 0, sizeof(*out)); return false; }
bool codeplug_set_aprs_path(uint8_t n, const char *path1, const char *path2) { return false; }
bool codeplug_set_aprs_ssid(uint8_t n, uint8_t ssid) { return false; }
#endif
#if CONFIG_APRS
// ... and no host link
#include "hostlink/hostlink.h"
void hostlink_set_tnc(const hostlink_tnc_t *tnc) { (void)tnc; }
#endif
bool codeplug_set_radio_name(const char *name) { (void)name; return false; }
bool codeplug_set_boot_text(const char *line1, const char *line2) { (void)line1; (void)line2; return false; }
bool codeplug_contact(uint16_t index, contact_t *out)
{
	if (index < 1 || index > sizeof(contacts) / sizeof(contacts[0])) {
		return false;
	}
	*out = contacts[index - 1];
	return true;
}
bool codeplug_contacts_each(codeplug_contact_fn fn, void *ctx)
{
	contact_t c;
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		if (!fn(i, codeplug_contact(i, &c) ? &c : NULL, ctx)) {
			break;
		}
	}
	return true;
}
uint16_t codeplug_contact_find_index(uint32_t id, bool group, contact_t *out)
{
	for (unsigned i = 0; i < sizeof(contacts) / sizeof(contacts[0]); i++) {
		if (contacts[i].id == id && (contacts[i].type != CONTACT_PRIVATE) == group) {
			if (out != NULL)
				*out = contacts[i];
			return (uint16_t)(i + 1u);
		}
	}
	return 0;
}
static uint32_t contact_finds, iddb_lookups;
static contact_t added_contact;	// a contact written after the radio started (id 0 = none)
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out)
{
	contact_finds++;
	if (added_contact.id != 0 && added_contact.id == id && (added_contact.type != CONTACT_PRIVATE) == group) {
		*out = added_contact;
		return true;
	}
	for (unsigned i = 0; i < sizeof(contacts) / sizeof(contacts[0]); i++) {
		if (contacts[i].id == id && (contacts[i].type != CONTACT_PRIVATE) == group) {
			*out = contacts[i];
			return true;
		}
	}
	if (group && id == 3100) {
		*out = (contact_t){ "TG USA", 3100, CONTACT_GROUP, 0 };
		return true;
	}
	return false;
}
static codeplug_dmr_settings_t dmr_settings = { "Q0TEST", 0, 0 };	// no hang times: the stack's 10 s
bool codeplug_dmr_settings(codeplug_dmr_settings_t *out)
{
	*out = dmr_settings;
	return true;
}
bool dmr_iddb_lookup(uint32_t id, char *out, size_t len)
{
	iddb_lookups++;
	if (id != 9995555) {
		return false;
	}
	snprintf(out, len, "QD9XYZ Robert Longname Springfield");
	return true;
}
bool codeplug_listen_list(uint16_t index, listen_list_t *out)
{
	if (index != 1 && index != 2) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (index == 2) {		// a private contact first: no talk target, the default skips it
		strcpy(out->name, "Joe first");
		out->count = 2;
		out->contacts[0] = 2;
		out->contacts[1] = 1;
		return true;
	}
	strcpy(out->name, "Local TGs");
	out->count = 3;
	out->contacts[0] = 1;
	out->contacts[1] = 2;	// private: not a listen entry, but its name is known
	out->contacts[2] = 3;
	return true;
}

static void add(uint16_t n, const char *name, uint32_t hz, uint8_t mode, bool rx_only, uint8_t power)
{
	memset(&chans[n], 0, sizeof(chans[n]));
	strcpy(chans[n].name, name);
	chans[n].rx_hz = hz;
	chans[n].tx_hz = hz;
	chans[n].mode = mode;
	chans[n].rx_only = rx_only;
	chans[n].power = power;
	used[n] = true;
}

static void setup_codeplug(void)
{
	memset(used, 0, sizeof(used));
	add(1, "Simplex", 146520000, RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
	add(2, "Weather", 162550000, RADIO_MODE_FM, true, CODEPLUG_POWER_DEFAULT);
	// 3 is empty
	add(4, "DMR rptr", 438500000, RADIO_MODE_DMR, false, 2);
	chans[4].color_code = 2;
	chans[4].timeslot = 1;
	chans[4].contact = 1;
	chans[4].listen_list = 1;
	add(5, "Air", 121500000, RADIO_MODE_FM, false, 0);	// receive-only band
	add(6, "Other", 145500000, RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
	// zone "Tones": a wide repeater with TX tone only, a narrow one with DCS 023 inverted both ways and different RX
	add(7, "Rptr wide", 146940000, RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
	chans[7].tx_hz = 146340000;
	chans[7].bw = RADIO_BW_25K;
	chans[7].tx_tone = (subtone_t){ TONE_CTCSS, 1000 };
	add(8, "Rptr DCS", 444100000, RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
	chans[8].tx_tone = (subtone_t){ TONE_DCS_INV, 023 };
	chans[8].rx_tone = (subtone_t){ TONE_DCS, 0754 };
	memset(zones, 0, sizeof(zones));
	strcpy(zones[0].name, "Local");
	zones[0].count = 5;
	zones[0].channels[0] = 1;
	zones[0].channels[1] = 2;
	zones[0].channels[2] = 3;
	zones[0].channels[3] = 4;
	zones[0].channels[4] = 5;
	strcpy(zones[1].name, "Two");
	zones[1].count = 1;
	zones[1].channels[0] = 6;
	strcpy(zones[2].name, "Tones");
	zones[2].count = 2;
	zones[2].channels[0] = 7;
	zones[2].channels[1] = 8;
	nzones = 3;
}

static radio_sm_t sm;

static void req(uint8_t type, int32_t arg)
{
	radio_req_t r = { type, arg, 0 };
	radio_sm_request(&sm, &r, now);
}

static void poll_for(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
		now += RADIO_POLL_MS;
		radio_sm_poll(&sm, now);
	}
}

static void rx_signal(uint8_t noise, bool tone_ok)
{
	hw.st.noise = noise;
	hw.st.tone_ok = tone_ok;
	hw.st.rssi_dbm = noise < 50 ? -80 : -125;
}

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

// A boot with the settings as they are (RAM only here: the fake board has no flash)
static void boot(void)
{
	ambeVoiceStop();
	memset(&aud, 0, sizeof(aud));
	sim_reset();
	memset(&hw, 0, sizeof(hw));
	rx_signal(120, true);
	radio_sm_init(&sm, now);
}

static void start(void)
{
	settings_init();	// nothing saved: the defaults
	ambeVoiceStop();	// a stream left from the last test
	memset(&aud, 0, sizeof(aud));
	sim_reset();
	memset(&hw, 0, sizeof(hw));
	rx_signal(120, true);
	now = 1000;
	radio_sm_init(&sm, now);
}

static void test_startup(void)
{
	setup_codeplug();
	start();
	CHECK(!sm.view.vfo);
	CHECK(sm.view.zone_index == 0);
	CHECK(sm.view.zone_pos == 0);
	CHECK(sm.view.zone_size == 5);
	CHECK(strcmp(sm.view.zone_name, "Local") == 0);
	CHECK(sm.view.channel_number == 1);
	CHECK(strcmp(sm.view.ch.name, "Simplex") == 0);
	CHECK(hw.set_mode_calls == 1);
	CHECK(hw.rx_hz == 146520000);
	CHECK(sm.view.power == 1);		// CODEPLUG_POWER_DEFAULT -> board.power_default
	CHECK(sm.view.squelch == RADIO_SQUELCH_DEFAULT);
	CHECK(sm.view.tot_s == 180);
	CHECK(sm.view.vfo_step_hz == 12500);
	CHECK(sm.view.source == CODEPLUG_OPENFW);
	CHECK(sm.view.state == RS_IDLE);
	CHECK(!hw.audio && !hw.led[LED_GREEN] && !hw.led[LED_RED]);
}

// The VFO's step: auto (0, the default) follows the width; a step set stays whatever the width; kept in the settings
static void test_vfo_step_auto(void)
{
	setup_codeplug();
	start();
	req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && sm.view.vfo_step_auto && sm.view.vfo_step_hz == 12500);
	uint32_t hz = sm.view.ch.rx_hz;
	req(RREQ_VFO_STEP, 1);
	CHECK(sm.view.ch.rx_hz == hz + 12500);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	CHECK(sm.view.vfo_step_auto && sm.view.vfo_step_hz == 25000);
	req(RREQ_VFO_STEP, 1);
	CHECK(sm.view.ch.rx_hz == hz + 37500);
	req(RREQ_VFO_STEP_SIZE, 5000);
	CHECK(!sm.view.vfo_step_auto && sm.view.vfo_step_hz == 5000);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	CHECK(sm.view.vfo_step_hz == 5000);
	settings_t st;
	settings_read(&st);
	CHECK(st.vfo_step_hz == 5000);
	req(RREQ_VFO_STEP_SIZE, 0);
	CHECK(sm.view.vfo_step_auto && sm.view.vfo_step_hz == 12500);
	settings_read(&st);
	CHECK(st.vfo_step_hz == 0);
}

static void test_channel_step(void)
{
	setup_codeplug();
	start();
	uint32_t s = sm.serial;
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 2 && hw.rx_hz == 162550000);
	CHECK(sm.serial != s);
	req(RREQ_CH_STEP, 1);			// skips the empty slot 3
	CHECK(sm.view.channel_number == 4 && sm.view.zone_pos == 3);
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 5);
	req(RREQ_CH_STEP, 1);			// wraps
	CHECK(sm.view.channel_number == 1 && sm.view.zone_pos == 0);
	req(RREQ_CH_STEP, -1);			// wraps backwards
	CHECK(sm.view.channel_number == 5);
	req(RREQ_CH_STEP, -2);
	CHECK(sm.view.channel_number == 2);
	req(RREQ_ZONE_POS, 3);
	CHECK(sm.view.channel_number == 4);
	req(RREQ_ZONE_POS, 2);			// empty: no change
	CHECK(sm.view.channel_number == 4);
	req(RREQ_ZONE, 1);
	CHECK(sm.view.zone_index == 1 && sm.view.channel_number == 6 && strcmp(sm.view.zone_name, "Two") == 0);
	req(RREQ_CH_STEP, 1);			// one channel: stays
	CHECK(sm.view.channel_number == 6);
	req(RREQ_ZONE, 7);			// no such zone
	CHECK(sm.view.zone_index == 1);
}

// A channel of a kind this build doesn't run (docs/spec/channel-modes.md 6): tuned in FM with the modem off, the squelch
// never opens, PTT refused for the mode (before receive only), never a VFO copy, the next channel works as before
static void test_parked_kind(void)
{
	setup_codeplug();
	chans[6].mode = RADIO_MODE_DMR;
	chans[6].kind = CODEPLUG_KIND_P25;		// not run: no P25 in this build, or a board that can't carry it
	chans[6].rx_only = false;			// its tag allows TX: the build still refuses
#if CONFIG_AIR
	air_nofit = true;
#endif
	start();
	req(RREQ_ZONE, 1);
	CHECK(sm.view.channel_number == 6 && sm.view.ch.kind == CODEPLUG_KIND_P25);
	CHECK(sm.board_mode == RADIO_MODE_FM && hw_mode == RADIO_MODE_FM && !sim.started && hw.rx_hz == 145500000);
	rx_signal(10, true);
	poll_for(200);
	CHECK(sm.view.state == RS_IDLE && !hw.audio);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_MODE && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	CHECK(radio_vfo_copy_check(&sm.view.ch, false) != VFO_COPY_OK);
	// P25 trunked and NXDN the same where the board can't carry them (with their switches on or off), unknown ids too.
	// A board that carries the mode runs it: test_p25_stored, test_nxdn_stored
	static const uint8_t kinds[] = { CODEPLUG_KIND_P25_TRUNK, CODEPLUG_KIND_NXDN };
	for (unsigned i = 0; i < sizeof kinds; i++) {
		chans[6].kind = kinds[i];
		req(RREQ_ZONE, 0);
		req(RREQ_ZONE, 1);
		CHECK(sm.view.ch.kind == kinds[i] && sm.board_mode == RADIO_MODE_FM && !sim.started);
		req(RREQ_PTT, 1);
		CHECK(sm.view.tx_deny == TXDENY_MODE && hw.tx_calls == 0);
		req(RREQ_PTT, 0);
	}
	chans[6].kind = 0x99;
	req(RREQ_ZONE, 0);
	req(RREQ_ZONE, 1);
	CHECK(sm.view.ch.kind == 0x99 && sm.board_mode == RADIO_MODE_FM);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_MODE && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	rx_signal(120, true);
#if CONFIG_AIR
	air_nofit = false;
#endif
	setup_codeplug();
}

static void test_squelch(void)
{
	setup_codeplug();
	start();
	CHECK(radio_sm_squelch_open_below(1) == 64);
	CHECK(radio_sm_squelch_open_below(9) == 40);
	uint8_t thr = radio_sm_squelch_open_below(RADIO_SQUELCH_DEFAULT);

	poll_for(100);
	CHECK(sm.view.state == RS_IDLE && !hw.audio);
	rx_signal(thr, true);			// at the threshold: still closed
	poll_for(50);
	CHECK(sm.view.state == RS_IDLE);
	rx_signal(thr - 10, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX && hw.audio && hw.led[LED_GREEN]);
	CHECK(sm.view.rssi_dbm == -80);
	CHECK(radio_view_wakes(&sm.view));	// FM: what opens the squelch plays, so it lights the screen
	rx_signal(thr + RADIO_SQL_HYST_DB - 1, true);	// inside the hysteresis: stays open
	poll_for(200);
	CHECK(sm.view.state == RS_RX);
	CHECK(sm.view.last_rx_ms == 0);
	rx_signal(thr + RADIO_SQL_HYST_DB, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);		// one poll above is not enough
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && !hw.audio && !hw.led[LED_GREEN]);
	CHECK(sm.view.last_rx_ms == now);
	// The carrier goes: the noise jumps far over the line, and one poll closes it (a shorter squelch tail)
	rx_signal(thr - 10, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	rx_signal(thr + RADIO_SQL_HYST_DB + RADIO_SQL_FAST_CLOSE_DB - 1, true);	// just short of it: two polls as before
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	rx_signal(thr - 10, true);
	poll_for(RADIO_POLL_MS);
	rx_signal(thr + RADIO_SQL_HYST_DB + RADIO_SQL_FAST_CLOSE_DB, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && !hw.audio);

	rx_signal(10, false);			// no RX tone on this channel: the board's tone flag doesn't matter
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	rx_signal(120, true);
	poll_for(100);
	CHECK(sm.view.state == RS_IDLE);
	req(RREQ_SQUELCH, -(int)(RADIO_SQUELCH_DEFAULT - 1));	// the most open level short of 0
	rx_signal(UINT8_MAX, true);		// a receiver that can't hear (an RF chip the board doesn't drive): never opens
	poll_for(200);
	CHECK(sm.view.squelch == 1 && sm.view.state == RS_IDLE && !radio_view_wakes(&sm.view));
	req(RREQ_CLEAR_TEMP, 0);

	req(RREQ_SQUELCH, -9);			// to 0: always open
	CHECK(sm.view.squelch == 0 && sm.view.temporary);
	rx_signal(200, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX && hw.audio);
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(sm.view.squelch == RADIO_SQUELCH_DEFAULT && !sm.view.temporary);
	poll_for(100);
	CHECK(sm.view.state == RS_IDLE);
	req(RREQ_SQUELCH, 20);
	CHECK(sm.view.squelch == RADIO_SQUELCH_MAX);

	// Monitor opens the speaker without changing the state
	req(RREQ_MONITOR, 1);
	CHECK(sm.view.monitor && hw.audio && sm.view.state == RS_IDLE);
	req(RREQ_MONITOR, 0);
	CHECK(!sm.view.monitor && !hw.audio);

	// A DMR channel: the state follows the call stack, not the carrier, and the FM speaker stays shut
	req(RREQ_ZONE_POS, 3);
	rx_signal(10, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && !hw.audio);
}

static void test_tx(void)
{
	setup_codeplug();
	start();
	rx_signal(10, true);
	poll_for(50);
	CHECK(sm.view.state == RS_RX);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && hw.tx_hz == 146520000 && hw.tx_power == 1);
	CHECK(!sm.view.tx_auto && sm.view.last_tx_ms != 0 && radio_view_wakes(&sm.view));	// the user's own
	CHECK(!hw.audio && hw.led[LED_RED] && !hw.led[LED_GREEN]);
	CHECK(sm.view.last_rx_ms != 0);		// the reception ended when TX started
	req(RREQ_CH_STEP, 1);			// no retuning while transmitting
	CHECK(sm.view.channel_number == 1);
	poll_for(2000);
	CHECK(sm.view.tx_elapsed_ms == 2000);
	req(RREQ_PTT, 1);			// repeated down: nothing
	CHECK(hw.tx_calls == 1);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 1 && !hw.led[LED_RED]);
	req(RREQ_PTT, 0);
	CHECK(hw.tx_off_calls == 1);

	// RX-only channel
	req(RREQ_CH_STEP, 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == 1);
	CHECK(radio_view_wakes(&sm.view));	// the screen says why
	CHECK(hw.led[LED_RED] && !hw.led[LED_GREEN]);	// the deny flash starts red (radio-features.md 5.7)
	poll_for(100);
	CHECK(!hw.led[LED_RED] && hw.led[LED_GREEN]);	// then green
	poll_for(500);
	CHECK(!hw.led[LED_RED] && !hw.led[LED_GREEN]);	// then dark: the flash runs once
	CHECK(sm.view.state == RS_TX_DENIED);	// until release
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && sm.view.tx_deny == TXDENY_NONE && hw.tx_off_calls == 1);

	// DMR channel without the codec: receive only
	req(RREQ_CH_STEP, 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_CODEC && hw.tx_calls == 1);
	req(RREQ_PTT, 0);

	// Receive-only band
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 5 && sm.view.power == 0);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BAND && hw.tx_calls == 1);
	req(RREQ_PTT, 0);
}

// A transmission that starts while a deny still flashes ends the flash: red shows the air, not the refusal before
static void test_deny_flash_then_tx(void)
{
	setup_codeplug();
	start();
	req(RREQ_CH_STEP, 1);			// the RX-only channel
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && hw.led[LED_RED] && !hw.led[LED_GREEN]);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, -1);			// back to one that transmits, the flash still running
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && hw.led[LED_RED] && !hw.led[LED_GREEN]);
	poll_for(100);				// the flash's green step
	CHECK(hw.led[LED_RED] && !hw.led[LED_GREEN] && (sm.view.leds & RADIO_LED_RED) != 0);
	poll_for(600);
	CHECK(hw.led[LED_RED] && !hw.led[LED_GREEN]);	// and no flash back after it
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && !hw.led[LED_RED]);
}

static void test_timeout(void)
{
	setup_codeplug();
	start();
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	poll_for(179000);
	CHECK(sm.view.state == RS_TX);
	poll_for(1000);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TIMEOUT);
	CHECK(hw.tx_off_calls == 1 && hw.led[LED_RED] && !hw.led[LED_GREEN]);	// the deny flash, the transmitter off
	poll_for(5000);
	CHECK(sm.view.state == RS_TX_DENIED && !hw.led[LED_RED]);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 1);
	req(RREQ_PTT, 1);			// a fresh press transmits again
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 2);
	req(RREQ_PTT, 0);
}

static void test_power(void)
{
	setup_codeplug();
	start();
	req(RREQ_POWER, 1);
	CHECK(sm.view.power == 2 && sm.view.temporary);
	req(RREQ_POWER, 1);			// wraps
	CHECK(sm.view.power == 0 && sm.view.temporary);
	req(RREQ_POWER, 1);			// back to the channel's level: not temporary
	CHECK(sm.view.power == 1 && !sm.view.temporary);
	req(RREQ_POWER, -1);
	CHECK(sm.view.power == 0 && sm.view.temporary);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_power == 0);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);			// a channel change drops the temporary change
	CHECK(sm.view.power == 1 && !sm.view.temporary);
	req(RREQ_POWER, 1);
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(sm.view.power == 1 && !sm.view.temporary);
}

static void test_vfo(void)
{
	setup_codeplug();
	start();
	req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && sm.view.channel_number == 0 && strcmp(sm.view.ch.name, "VFO") == 0);
	CHECK(sm.view.ch.rx_hz == 146520000 && hw.rx_hz == 146520000);	// from the channel
	req(RREQ_VFO_STEP, 1);
	CHECK(sm.view.ch.rx_hz == 146532500 && sm.view.ch.tx_hz == 146532500 && hw.rx_hz == 146532500);
	req(RREQ_CH_STEP, -1);			// channel step in the VFO steps the frequency
	CHECK(sm.view.ch.rx_hz == 146520000);
	req(RREQ_VFO_SET, 174000000);
	CHECK(sm.view.ch.rx_hz == 174000000);
	req(RREQ_VFO_STEP, 1);			// past the top of the band: the next band
	CHECK(sm.view.ch.rx_hz == 400000000);
	req(RREQ_VFO_STEP, -1);
	CHECK(sm.view.ch.rx_hz == 174000000);
	req(RREQ_VFO_SET, 480000000);
	req(RREQ_VFO_STEP, 1);			// past the top band: wraps to the bottom
	CHECK(sm.view.ch.rx_hz == 108000000);
	req(RREQ_VFO_SET, 520000000);		// out of every band: refused
	CHECK(sm.view.ch.rx_hz == 108000000);
	req(RREQ_PTT, 1);			// receive-only band
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BAND && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	req(RREQ_VFO_SET, 446000000);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 446000000);
	req(RREQ_PTT, 0);

	req(RREQ_VFO, 0);			// back to the channel
	CHECK(!sm.view.vfo && sm.view.channel_number == 1 && hw.rx_hz == 146520000);
	req(RREQ_VFO, 1);			// the VFO remembers its frequency
	CHECK(sm.view.ch.rx_hz == 446000000);
	req(RREQ_VFO, 0);
	req(RREQ_VFO_SET, 440000000);		// typing a frequency on a channel goes to the VFO
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 440000000);
	req(RREQ_ZONE_POS, 1);			// picking a channel leaves it
	CHECK(!sm.view.vfo && sm.view.channel_number == 2);

	// Split (RREQ_VFO_TX): the VFO only; TX where it may transmit; a new RX keeps the offset where TX still can
	req(RREQ_VFO_TX, 445000000);		// on a channel: refused
	CHECK(sm.view.ch.tx_hz != 445000000);
	req(RREQ_VFO_SET, 440000000);
	uint32_t rx_before = hw.rx_hz;
	req(RREQ_VFO_TX, 445000000);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 440000000 && sm.view.ch.tx_hz == 445000000);
	CHECK(hw.rx_hz == rx_before);		// the receiver stays where it is
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 445000000);
	req(RREQ_VFO_TX, 446000000);		// not while transmitting
	CHECK(sm.view.ch.tx_hz == 445000000);
	req(RREQ_PTT, 0);
	req(RREQ_VFO_TX, 120000000);		// a receive-only band: refused
	CHECK(sm.view.ch.tx_hz == 445000000);
	req(RREQ_VFO_STEP, 1);			// the offset moves with RX
	CHECK(sm.view.ch.tx_hz - sm.view.ch.rx_hz == 5000000);
	req(RREQ_VFO_SET, 446000000);
	CHECK(sm.view.ch.tx_hz == 451000000);
	req(RREQ_VFO_SET, 146520000);		// another band: simplex, though 151.52 could transmit
	CHECK(sm.view.ch.rx_hz == 146520000 && sm.view.ch.tx_hz == 146520000);
	req(RREQ_VFO_TX, 147120000);		// back to UHF with a VHF split: simplex too
	req(RREQ_VFO_SET, 446000000);
	CHECK(sm.view.ch.tx_hz == 446000000);
	req(RREQ_VFO_TX, 451000000);
	req(RREQ_VFO_SET, 478000000);		// TX would leave the band: simplex
	CHECK(sm.view.ch.tx_hz == 478000000);
	req(RREQ_VFO_TX, 473000000);
	req(RREQ_VFO_TX, 0);			// 0: simplex again
	CHECK(sm.view.ch.tx_hz == 478000000);
	uint32_t serial = sm.serial;		// the UI sees a new split at once
	req(RREQ_VFO_TX, 473000000);
	CHECK(sm.serial != serial);
	req(RREQ_VFO_TX, 0);
}

static void test_no_codeplug(void)
{
	memset(used, 0, sizeof(used));
	nzones = 0;
	source = CODEPLUG_NONE;
	start();
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 146520000 && hw.rx_hz == 146520000);
	CHECK(sm.view.source == CODEPLUG_NONE);
	req(RREQ_VFO, 0);			// nothing to go back to
	CHECK(sm.view.vfo);
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.ch.rx_hz == 146532500);
	source = CODEPLUG_OPENFW;

	// Zones that exist but have no readable channel
	setup_codeplug();
	memset(used, 0, sizeof(used));
	start();
	CHECK(sm.view.vfo);
}

static void test_codeplug_tones(void)
{
	setup_codeplug();
	start();
	CHECK(hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_NONE);
	req(RREQ_ZONE, 2);
	CHECK(sm.view.channel_number == 7 && hw.rx_hz == 146940000);
	CHECK(hw.rx_bw == RADIO_BW_25K && hw.rx_tone.type == TONE_NONE);	// a TX-only tone: no tone squelch
	CHECK(sm.view.tone_ok);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_hz == 146340000 && hw.tx_bw == RADIO_BW_25K && hw.tx_tone.type == TONE_CTCSS && hw.tx_tone.value == 1000);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);
	CHECK(hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_DCS && hw.rx_tone.value == 0754);
	CHECK(!sm.view.tone_ok);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_tone.type == TONE_DCS_INV && hw.tx_tone.value == 023 && hw.tx_bw == RADIO_BW_12K5);
	req(RREQ_PTT, 0);
}

static void test_tone_squelch(void)
{
	setup_codeplug();
	start();
	req(RREQ_ZONE, 2);
	req(RREQ_CH_STEP, 1);			// RX DCS 754
	uint8_t thr = radio_sm_squelch_open_below(RADIO_SQUELCH_DEFAULT);

	rx_signal(thr - 20, false);		// carrier, no code: closed
	poll_for(500);
	CHECK(sm.view.state == RS_IDLE && !hw.audio && !sm.view.tone_ok);
	rx_signal(thr - 20, true);
	poll_for(RADIO_POLL_MS);		// one poll of a match is not enough
	CHECK(sm.view.state == RS_IDLE);
	poll_for(RADIO_POLL_MS * (RADIO_TONE_ON_POLLS - 1));
	CHECK(sm.view.state == RS_RX && hw.audio && sm.view.tone_ok);

	// The flag chatters: short drop-outs keep the squelch open
	for (int i = 0; i < 20; i++) {
		rx_signal(thr - 20, i % 3 != 0);
		poll_for(RADIO_POLL_MS * (1 + i % 4 % 3));
		CHECK(sm.view.state == RS_RX);
	}
	rx_signal(thr - 20, true);
	poll_for(RADIO_POLL_MS);
	// The tone goes away for good, carrier stays: closes after the debounce and the close count
	rx_signal(thr - 20, false);
	poll_for(RADIO_POLL_MS * (RADIO_TONE_OFF_POLLS - 1));
	CHECK(sm.view.state == RS_RX);
	poll_for(RADIO_POLL_MS * (1 + RADIO_SQL_CLOSE_POLLS));
	CHECK(sm.view.state == RS_IDLE && !hw.audio && !sm.view.tone_ok);

	// Tone without a carrier (noise above the threshold): closed
	rx_signal(thr + 20, true);
	poll_for(500);
	CHECK(sm.view.state == RS_IDLE && sm.view.tone_ok);
	// Carrier back with the tone: opens at once (the tone is already heard)
	rx_signal(thr - 20, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	// Carrier gone: closes on the noise alone
	rx_signal(thr + 20, true);
	poll_for(RADIO_POLL_MS * RADIO_SQL_CLOSE_POLLS);
	CHECK(sm.view.state == RS_IDLE);

	// Monitor overrides tone squelch
	rx_signal(thr - 20, false);
	poll_for(500);
	req(RREQ_MONITOR, 1);
	CHECK(hw.audio && sm.view.state == RS_IDLE);
	req(RREQ_MONITOR, 0);
	CHECK(!hw.audio);

	// Squelch level 0 with an RX tone: the tone alone decides
	req(RREQ_SQUELCH, -9);
	rx_signal(200, false);
	poll_for(200);
	CHECK(sm.view.state == RS_IDLE);
	rx_signal(200, true);
	poll_for(RADIO_POLL_MS * RADIO_TONE_ON_POLLS);
	CHECK(sm.view.state == RS_RX);
	req(RREQ_CLEAR_TEMP, 0);

	// A channel without an RX tone ignores the board's tone flag
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 7 && sm.view.tone_ok);
	rx_signal(thr - 20, false);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
}

static void test_temp_bw_tone(void)
{
	setup_codeplug();
	start();
	CHECK(sm.view.ch.bw == RADIO_BW_12K5 && sm.view.temp == 0);
	int calls = hw.rx_calls;
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	CHECK(sm.view.ch.bw == RADIO_BW_25K && hw.rx_bw == RADIO_BW_25K && hw.rx_calls == calls + 1);
	CHECK(sm.view.temporary && sm.view.temp == RADIO_TEMP_BW);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);	// no change: no retune
	CHECK(hw.rx_calls == calls + 1);
	req(RREQ_BANDWIDTH, 7);			// not a bandwidth
	CHECK(sm.view.ch.bw == RADIO_BW_25K);

	// One tone for both directions
	subtone_t t100 = { TONE_CTCSS, 1000 };
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, t100));
	CHECK(sm.view.ch.tx_tone.type == TONE_CTCSS && sm.view.ch.rx_tone.value == 1000);
	CHECK(hw.rx_tone.type == TONE_CTCSS && hw.rx_tone.value == 1000);
	CHECK(sm.view.temp == (RADIO_TEMP_BW | RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE));
	CHECK(!sm.view.tone_ok);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_tone.type == TONE_CTCSS && hw.tx_tone.value == 1000 && hw.tx_bw == RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, (subtone_t){ TONE_NONE, 0 }));	// not while transmitting
	CHECK(sm.view.ch.tx_tone.type == TONE_CTCSS);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	CHECK(sm.view.ch.bw == RADIO_BW_25K);
	req(RREQ_PTT, 0);

	// TX only: no retune
	calls = hw.rx_calls;
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_DCS, 023 }));
	CHECK(sm.view.ch.tx_tone.type == TONE_DCS && sm.view.ch.rx_tone.type == TONE_CTCSS && hw.rx_calls == calls);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_RX, (subtone_t){ TONE_DCS_INV, 023 }));
	CHECK(hw.rx_tone.type == TONE_DCS_INV && hw.rx_tone.value == 023 && hw.rx_calls == calls + 1);
	// Invalid tones are refused
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, (subtone_t){ TONE_CTCSS, 50 }));
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, (subtone_t){ 9, 1000 }));
	CHECK(sm.view.ch.tx_tone.type == TONE_DCS && sm.view.ch.rx_tone.type == TONE_DCS_INV);
	// Setting the channel's own value again clears that flag
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, (subtone_t){ TONE_NONE, 0 }));
	CHECK(sm.view.temp == RADIO_TEMP_BW);

	// BACK on Home undoes everything and retunes
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, t100));
	calls = hw.rx_calls;
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(!sm.view.temporary && sm.view.temp == 0 && sm.view.ch.bw == RADIO_BW_12K5 && sm.view.ch.rx_tone.type == TONE_NONE);
	CHECK(hw.rx_calls == calls + 1 && hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_NONE);

	// Undo during TX: the receiver is retuned when PTT is released
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_PTT, 1);
	calls = hw.rx_calls;
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(hw.rx_calls == calls && sm.view.ch.bw == RADIO_BW_12K5);
	req(RREQ_PTT, 0);
	CHECK(hw.rx_calls == calls + 1 && hw.rx_bw == RADIO_BW_12K5);

	// A reference trim change (RREQ_RETUNE): one tune once it has held still RADIO_RETUNE_SETTLE_MS, not one a step;
	// during TX, at the release
	calls = hw.rx_calls;
	req(RREQ_RETUNE, 0);
	poll_for(RADIO_RETUNE_SETTLE_MS / 2);
	req(RREQ_RETUNE, 0);
	poll_for(RADIO_RETUNE_SETTLE_MS - RADIO_POLL_MS);
	CHECK(hw.rx_calls == calls);
	poll_for(2 * RADIO_POLL_MS);
	CHECK(hw.rx_calls == calls + 1);
	poll_for(RADIO_RETUNE_SETTLE_MS);
	CHECK(hw.rx_calls == calls + 1);
	req(RREQ_RETUNE, 0);
	req(RREQ_PTT, 1);
	poll_for(RADIO_RETUNE_SETTLE_MS + RADIO_POLL_MS);
	CHECK(hw.rx_calls == calls + 1);
	req(RREQ_PTT, 0);
	CHECK(hw.rx_calls == calls + 2);
	poll_for(RADIO_RETUNE_SETTLE_MS);
	CHECK(hw.rx_calls == calls + 2);

	// A channel change drops them
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, t100));
	req(RREQ_CH_STEP, 1);
	req(RREQ_CH_STEP, -1);
	CHECK(sm.view.temp == 0 && sm.view.ch.tx_tone.type == TONE_NONE && hw.rx_tone.type == TONE_NONE);

	// DMR channel: refused
	req(RREQ_ZONE_POS, 3);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, t100));
	CHECK(sm.view.ch.bw == RADIO_BW_12K5 && sm.view.ch.tx_tone.type == TONE_NONE && sm.view.temp == 0);

	// VFO: its own settings, not temporary, remembered
	req(RREQ_ZONE_POS, 0);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);	// temporary on the channel
	req(RREQ_VFO, 1);
	CHECK(sm.view.ch.bw == RADIO_BW_12K5 && sm.view.temp == 0);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_CTCSS, 885 }));
	CHECK(sm.view.ch.bw == RADIO_BW_25K && sm.view.ch.tx_tone.value == 885 && !sm.view.temporary);
	CHECK(hw.rx_bw == RADIO_BW_25K && hw.rx_tone.type == TONE_NONE);
	req(RREQ_VFO, 0);
	CHECK(sm.view.ch.bw == RADIO_BW_12K5 && sm.view.ch.tx_tone.type == TONE_NONE && hw.rx_bw == RADIO_BW_12K5);
	req(RREQ_VFO, 1);
	CHECK(sm.view.ch.bw == RADIO_BW_25K && sm.view.ch.tx_tone.type == TONE_CTCSS && sm.view.ch.tx_tone.value == 885);
	CHECK(hw.rx_bw == RADIO_BW_25K);
}

static void test_tone_lists(void)
{
	// Spot checks and order
	CHECK(tones_ctcss[0] == 670 && tones_ctcss[12] == 1000 && tones_ctcss[TONES_CTCSS_COUNT - 1] == 2541);
	CHECK(tones_dcs[0] == 023 && tones_dcs[TONES_DCS_COUNT - 1] == 0754);
	for (int i = 1; i < TONES_CTCSS_COUNT; i++)
		CHECK(tones_ctcss[i] > tones_ctcss[i - 1]);
	for (int i = 1; i < TONES_DCS_COUNT; i++)
		CHECK(tones_dcs[i] > tones_dcs[i - 1]);

	// Stepping visits every picker entry once and wraps
	subtone_t t = { TONE_NONE, 0 };
	for (int i = 1; i <= TONES_PICKER_COUNT; i++) {
		t = tones_step(&t, 1);
		subtone_t want = tones_picker((uint16_t)(i % TONES_PICKER_COUNT));
		CHECK(t.type == want.type && t.value == want.value);
		CHECK(tones_standard(&t));
	}
	t = tones_step(&t, -1);
	CHECK(t.type == TONE_DCS_INV && t.value == 0754);
	t = tones_step(&(subtone_t){ TONE_CTCSS, 670 }, -1);
	CHECK(t.type == TONE_NONE);
	t = tones_step(&(subtone_t){ TONE_CTCSS, 2541 }, 1);
	CHECK(t.type == TONE_DCS && t.value == 023);
	t = tones_step(&(subtone_t){ TONE_DCS, 023 }, 1);
	CHECK(t.type == TONE_DCS_INV && t.value == 023);
	t = tones_step(&(subtone_t){ TONE_DCS_INV, 023 }, 1);
	CHECK(t.type == TONE_DCS && t.value == 025);
	t = tones_step(&(subtone_t){ TONE_DCS, 025 }, -1);
	CHECK(t.type == TONE_DCS_INV && t.value == 023);
	t = tones_step(&(subtone_t){ TONE_DCS, 0754 }, 1);
	CHECK(t.type == TONE_DCS_INV && t.value == 0754);

	// A non-standard tone steps to its neighbors
	subtone_t odd = { TONE_CTCSS, 1500 };
	CHECK(!tones_standard(&odd) && tones_valid(&odd));
	t = tones_step(&odd, 1);
	CHECK(t.type == TONE_CTCSS && t.value == 1514);
	t = tones_step(&odd, -1);
	CHECK(t.type == TONE_CTCSS && t.value == 1462);
	CHECK(!tones_valid(&(subtone_t){ TONE_CTCSS, 2700 }));
	CHECK(!tones_valid(&(subtone_t){ TONE_DCS, 01000 }));
}

// ---- DMR: the call stack on a simulated modem, driven like the radio task drives it ----

// Advance in 5 ms steps: a modem slot every 30 ms, the stack tick every RADIO_TICK_MS, the status poll every
// RADIO_POLL_MS
static int16_t burst_rssi = -85;	// what the simulated modem reports per burst (0 = it doesn't)
static bool rssi_alt;			// the fake radio's reading alternates strong / floor every 30 ms (TDMA)

static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		if (rssi_alt) {
			hw.st.rssi_dbm = (now / 30) % 2 ? -125 : -70;
		}
		if (now % 30 == 0) {
			sim_slot();
#if CONFIG_AIR
			air_step();
#endif
		}
		if (aud.feed && aud.started && aud.cb != NULL && now % 20 == 0) {
			static const int16_t mic[AUDIO_BLOCK_SAMPLES];
			int16_t spk[AUDIO_BLOCK_SAMPLES];
			aud.cb(mic, spk, aud.ctx);
		}
		if (now % RADIO_TICK_MS == 0) {
			radio_sm_tick(&sm, now);
		}
		if (now % RADIO_POLL_MS == 0) {
			radio_sm_poll(&sm, now);
		}
	}
}

static void mk_lc(uint8_t lc[9], bool group, uint32_t dst, uint32_t src)
{
	dmr_lc_t l = { .flco = group ? DMR_FLCO_GROUP_VOICE : DMR_FLCO_UNIT_VOICE, .dst = dst, .src = src };
	dmr_lc_pack(&l, lc);
}

static void burst_lc(const uint8_t lc[9], uint8_t cc, bool terminator)
{
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = terminator ? DMR_BURST_TERMINATOR_LC : DMR_BURST_VOICE_LC_HEADER;
	b.sync = DMR_SYNC_MS;
	b.cc = cc;
	b.crc_ok = true;
	b.in_step = true;
	b.len = 12;
	dmr_lc_encode_full(lc, terminator ? DMR_RS_MASK_TERMINATOR : DMR_RS_MASK_VOICE_HEADER, b.payload);
	sim_rx(&b);
}

static void burst_voice(uint8_t index, uint8_t cc, const uint8_t *lc)
{
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.sync = index == 0 ? DMR_SYNC_MS : DMR_SYNC_NONE;
	b.cc = cc;
	b.crc_ok = true;
	b.in_step = true;
	b.rssi_dbm = burst_rssi;
	b.len = 27;
	for (int f = 0; f < 3; f++) {
		memcpy(&b.payload[f * 9], dmr_ambe_silence, 9);
	}
	if (lc != NULL) {
		b.lc_valid = true;
		memcpy(b.lc, lc, 9);
	}
	sim_rx(&b);
}

// A call heard: three headers, superframes of voice, a terminator if end
static void hear_call(bool group, uint32_t dst, uint32_t src, uint8_t cc, int superframes, bool end)
{
	uint8_t lc[9];
	mk_lc(lc, group, dst, src);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, cc, false);
		run(60);
	}
	for (int sf = 0; sf < superframes; sf++) {
		for (int i = 0; i < 6; i++) {
			burst_voice((uint8_t)i, cc, i == 5 ? lc : NULL);
			run(60);
		}
	}
	if (end) {
		burst_lc(lc, cc, true);
		run(100);
	}
}

static void dmr_start(bool codec)
{
	setup_codeplug();
	codec_present = codec;
	start();
	req(RREQ_ZONE_POS, 3);		// channel 4: DMR, CC 2, TS 1, talk target TG 201, listen list 1
	run(50);
}

static void test_dmr_rx(void)
{
	dmr_start(false);
	CHECK(sim.started && sim.cc_filter == 2 && sim.cfg.own_id == 9996543 && !sim.cfg.repeater);
	const radio_dmr_view_t *d = &sm.view.dmr;
	CHECK(d->active && !d->codec && d->own_id == 9996543);
	CHECK(d->target_id == 201 && !d->target_private && strcmp(d->target_name, "Main Ops") == 0);
	CHECK(d->cc == 2 && d->ts == 1 && !d->rmo && d->state == RDMR_IDLE && d->last_ms == 0);

	// A group call on the talk target: received, shown, played (silence without the codec)
	hear_call(true, 201, 9991234, 2, 1, false);
	CHECK(sm.view.state == RS_RX && d->state == RDMR_RX);
	CHECK(radio_view_wakes(&sm.view));	// a call that plays lights the screen
	CHECK(d->call.src == 9991234 && d->call.dst == 201 && d->call.group && strcmp(d->call.dst_name, "Main Ops") == 0);
	CHECK(strcmp(d->call.src_name, "Joe") == 0);	// a private contact in the listen list
	CHECK(d->rssi_dbm == -85);
	CHECK(hw.led[LED_GREEN] && !hw.audio);		// FM speaker path stays off: DMR voice is PCM
	CHECK(aud.started);
	ambe_voice_stats_t vs;
	ambeVoiceGetStats(&vs);
	CHECK(vs.rx_frames == 0 && vs.rx_lost == 0);	// the fake audio callback never runs; frames were queued
	burst_lc((uint8_t[9]){ 0 }, 2, true);
	uint8_t lc[9];
	mk_lc(lc, true, 201, 9991234);
	burst_lc(lc, 2, true);
	run(100);
	CHECK(sm.view.state == RS_IDLE && d->state == RDMR_IDLE && !hw.led[LED_GREEN]);
	CHECK(d->last_ms != 0 && d->last.src == 9991234 && d->last.dst == 201 && sm.view.last_rx_ms != 0);
	CHECK(!d->reply_active);	// the talk target's own talkgroup: no reply window

	// TG 9 (in the listen list) opens the reply window to TG 9; BACK (RREQ_REPLY_CANCEL) ends it
	hear_call(true, 9, 9995555, 2, 1, true);
	CHECK(d->reply_active && d->target_id == 9 && d->reply_s == 10 && strcmp(d->target_name, "Local") == 0);
	run(2000);
	CHECK(d->reply_active && d->reply_s == 8);
	req(RREQ_REPLY_CANCEL, 0);
	run(20);
	CHECK(!d->reply_active && d->target_id == 201);

	// A talkgroup outside the list: shown as busy, not played
	hear_call(true, 3100, 9997777, 2, 1, false);
	CHECK(sm.view.state == RS_RX && d->state == RDMR_RX_OTHER && d->call.dst == 3100 && !hw.led[LED_GREEN]);
	CHECK(!radio_view_wakes(&sm.view));	// one we don't play doesn't
	run(500);			// fades out
	CHECK(sm.view.state == RS_IDLE);
	// MONITOR on DMR: a short press toggles digital monitor (the hear all latch) on its release (#19)
	req(RREQ_MONITOR, 1);
	CHECK(!d->hear_all_latch && !sm.view.monitor);
	req(RREQ_MONITOR, 0);
	CHECK(d->hear_all && d->hear_all_latch && !sm.view.monitor && !hw.audio);
	CHECK(sim.cc_filter == -1 && sim.locked_ts == 0);	// any color code, either slot
	hear_call(true, 3100, 9997777, 2, 1, true);
	CHECK(d->last.dst == 3100);
	// Released after its hold function ran: no toggle
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 2);
	CHECK(d->hear_all_latch);
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 0);
	CHECK(!d->hear_all && !d->hear_all_latch && sim.cc_filter == 2);
	// Another color code: ignored
	hear_call(true, 201, 9991234, 5, 1, false);
	CHECK(sm.view.state == RS_IDLE);
	run(500);

	// Without the codec PTT is refused before anything is keyed
	int tx = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_CODEC && hw.tx_calls == tx);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE);

	// Leaving DMR stops the stack and the PCM stream
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !sim.started && !d->active && aud.stops > 0 && !aud.started);
	req(RREQ_CH_STEP, -1);
	CHECK(sim.started && d->active);
}

static void reload_channel(void);

// A channel change publishes the new channel's color code, timeslot and repeater mode with it, in one update: a
// reader between the change and the call stack's next tick (dual watch's snapshot of the pane, at every hop) never
// sees the new channel with the old one's timeslot
static void test_dmr_channel_change_view(void)
{
	dmr_start(false);
	const radio_dmr_view_t *d = &sm.view.dmr;
	chans[1].mode = RADIO_MODE_DMR;
	chans[1].rx_hz = 438600000;
	chans[1].tx_hz = 433600000;
	chans[1].color_code = 5;
	chans[1].timeslot = 2;
	reload_channel();
	run(50);
	CHECK(sm.view.channel_number == 4 && d->ts == 1 && d->cc == 2 && !d->rmo);
	req(RREQ_ZONE_POS, 0);			// no poll: what a reader sees right after the change
	CHECK(sm.view.channel_number == 1 && d->ts == 2 && d->cc == 5 && d->rmo);
	req(RREQ_ZONE_POS, 3);
	CHECK(sm.view.channel_number == 4 && d->ts == 1 && d->cc == 2 && !d->rmo);
	run(50);
	CHECK(d->ts == 1 && d->cc == 2 && !d->rmo);
}

static void test_dmr_tx(void)
{
	dmr_start(true);
	CHECK(sm.view.dmr.codec);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && hw.tx_hz == 438500000 && hw.tx_power == 2);
	CHECK(sim.tx_begins == 1 && sim.tx_active);
	run(600);
	CHECK(sm.view.dmr.state == RDMR_TX && hw.led[LED_RED] && sim.air_count >= 6);
	CHECK(sim.air[0].kind == DMR_BURST_VOICE_LC_HEADER && sim.air[3].kind == DMR_BURST_VOICE);
	CHECK(sm.view.dmr.call.dst == 201 && sm.view.dmr.call.src == 9996543);
	req(RREQ_CH_STEP, 1);		// no retuning on air
	CHECK(sm.view.channel_number == 4);

	// Release: on air until the superframe ends and the modem reports TX done, then tx_off once
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && hw.tx_off_calls == 0);
	bool early = false;
	for (int i = 0; i < 100 && sm.view.state == RS_TX; i++) {
		run(10);
		if (hw.tx_off_calls != 0 && sim.tx_active) {
			early = true;
		}
	}
	CHECK(!early && sm.view.state == RS_IDLE && hw.tx_off_calls == 1 && !sim.tx_active && !hw.led[LED_RED]);
	CHECK(sim.air[sim.air_count - 1].kind == DMR_BURST_TERMINATOR_LC);
	run(100);
	CHECK(hw.tx_off_calls == 1);

	// Pressed again while the last one is ending: one arming, keeps going
	req(RREQ_PTT, 1);
	run(400);
	req(RREQ_PTT, 0);
	run(10);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 2);
	run(600);
	req(RREQ_PTT, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 2);
}

// The end of a DMR transmission goes through the board's dmr_tx_end (the glue: the last burst left the receiver on):
// handled, no tx_off() and no receive set-up, so a reply 30 ms after our terminator is heard; refused, tx_off() as before
static int tx_end_calls;
static bool tx_end_ok;
static bool r_dmr_tx_end(void) { tx_end_calls++; return tx_end_ok; }

static void test_dmr_tx_end(void)
{
	dmr_start(true);
	fake_radio.dmr_tx_end = r_dmr_tx_end;
	for (int pass = 0; pass < 2; pass++) {
		tx_end_ok = pass == 0;
		int ends = tx_end_calls, offs = hw.tx_off_calls, rxs = hw.rx_calls;
		req(RREQ_PTT, 1);
		run(600);
		req(RREQ_PTT, 0);
		CHECK(tx_end_calls == ends);	// on air until the modem's TX done
		run(1000);
		CHECK(sm.view.state == RS_IDLE && !sm.dmr_keyed && tx_end_calls == ends + 1);
		CHECK(hw.tx_off_calls == offs + (tx_end_ok ? 0 : 1) && hw.rx_calls == rxs);
	}
	fake_radio.dmr_tx_end = NULL;
}

// A modem with no slot timing takes the transmission and never sends (MD-380, HR_C5000 not answering): the radio must
// not stay keyed. Released: back to receive with the arming dropped; held: refused until release
static void test_dmr_stalled(void)
{
	dmr_start(true);
	aud.feed = true;	// the encoder fills voice bursts, so the stack gets past the headers as on the radio
	sim.stalled = true;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && sim.tx_begins == 1);
	run(400);
	req(RREQ_PTT, 0);
	run(DMR_CALL_TX_STALL_MS);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 1 && !sim.tx_active && !hw.led[LED_RED]);
	CHECK(sim.tx_aborts == 1 && sim.air_count == 0);

	req(RREQ_PTT, 1);
	run(DMR_CALL_TX_STALL_MS + 100);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REJECTED && hw.tx_off_calls == 2);
	req(RREQ_PTT, 0);
	run(20);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 2);
	sim.stalled = false;
	aud.feed = false;
}

// A channel with no TX contact talks to the first talkgroup of its RX group list (issue #4: qdmr's open-firmware
// encoder writes a channel's contact or its list, never both); talkgroup 9 only with neither; a contact always wins
static void test_dmr_list_talk_target(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;

	dmr_start(true);
	CHECK(d->target_id == 201);	// channel 4's contact, Main Ops
	chans[4].contact = 0;		// no contact, list 1: Main Ops first
	chans[4].listen_list = 1;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(d->target_id == 201 && !d->target_private && strcmp(d->target_name, "Main Ops") == 0);

	chans[4].listen_list = 2;	// Joe (private) first: skipped
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(d->target_id == 201 && !d->target_private);

	chans[4].listen_list = 0;	// neither: talkgroup 9
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(d->target_id == 9 && !d->target_private);

	chans[4].contact = 3;		// a contact wins over the list
	chans[4].listen_list = 1;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(d->target_id == 9 && strcmp(d->target_name, "Local") == 0);

	// What PTT sends: the list's talkgroup, as a group call
	chans[4].contact = 0;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX && sm.view.dmr.call.dst == 201 && sm.view.dmr.call.group);
	req(RREQ_PTT, 0);
	run(1000);
	setup_codeplug();
}

// The VFO keeps quick-settings changes (#486, #525): power is the VFO's own, squelch the radio's default (as a
// channel save writes it); neither is left temporary, so Home offers no save and leaving the VFO doesn't undo them
static void test_vfo_quick_settings_kept(void)
{
	setup_codeplug();
	start();
	req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && !sm.view.temporary);
	uint8_t p = sm.view.power, sq = sm.view.squelch;
	req(RREQ_POWER, 1);
	CHECK(sm.view.power != p && !sm.view.temporary && !(sm.view.temp & RADIO_TEMP_POWER));
	CHECK(sm.vfo_ch.power == sm.view.power);
	p = sm.view.power;
	req(RREQ_SQUELCH, sq < RADIO_SQUELCH_MAX ? 1 : -1);
	uint8_t sq2 = sm.view.squelch;
	CHECK(sq2 != sq && !sm.view.temporary && !(sm.view.temp & RADIO_TEMP_SQUELCH) && sm.base_squelch == sq2);
	run(50);
	settings_t *st = settings_begin();
	CHECK(st->squelch == sq2 && st->vfo_power == p);
	settings_end(now);
	req(RREQ_VFO, 0);			// a channel: squelch is radio-wide, so the new default applies there too
	CHECK(!sm.view.vfo && sm.view.squelch == sq2 && !sm.view.temporary);
	req(RREQ_VFO, 1);			// back in the VFO: both kept
	CHECK(sm.view.vfo && sm.view.power == p && sm.view.squelch == sq2 && !sm.view.temporary);
	boot();					// and across a restart
	run(50);
	CHECK(sm.view.vfo && sm.view.power == p && sm.view.squelch == sq2);
	req(RREQ_VFO, 0);
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}

#if !CONFIG_VFO_DMR
// Without the DMR VFO (CONFIG_VFO_DMR=0) the VFO is analog and every DMR request in it is refused
static void test_vfo_no_dmr(void)
{
	dmr_start(true);
	req(RREQ_VFO_SET, 446000000);
	req(RREQ_MODE, RADIO_MODE_DMR);
	req(RREQ_DMR_CC, 7);
	req(RREQ_DMR_TS, 2);
	req(RREQ_DMR_TARGET, 3100);
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started && hw_mode == RADIO_MODE_FM);
	CHECK(sm.vfo_ch.color_code == 1 && sm.vfo_ch.timeslot == 1 && sm.vfo_talk_id == 0 && !sm.view.temporary);
	CHECK(!ui_svc_save_talk_target(3100, false));
	boot();				// a record that says DMR (another build wrote it): this one starts analog
	settings_t *st = settings_begin();
	st->vfo_mode = RADIO_MODE_DMR;
	settings_set_vfo_talk(st, 3100, false);
	settings_end(now);
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started);
	req(RREQ_VFO_STEP, 1);		// and saves leave the talk target as the other build wrote it
	st = settings_begin();
	CHECK(settings_vfo_talk_id(st) == 3100);
	settings_end(now);
	req(RREQ_VFO, 0);
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}
#else
// The DMR VFO (CONFIG_VFO_DMR): mode, color code, slot and talk target are the VFO's own settings, kept across
// restarts, and receive and transmit follow them like a DMR channel's
static void test_vfo_dmr(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;

	dmr_start(true);			// channel 4: DMR, CC 2, TS 1, TG 201
	req(RREQ_VFO_SET, 446000000);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started && hw_mode == RADIO_MODE_FM);
	// FM -> DMR on the same frequency: the call stack starts, the board's mode follows, no temporary dot
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started && hw_mode == RADIO_MODE_DMR && hw.rx_hz == 446000000);
	CHECK(!sm.view.temporary && sm.view.temp == 0 && d->active && d->own_id == 9996543);
	CHECK(d->cc == 1 && d->ts == 1 && d->target_id == 9 && !d->target_private && !d->target_manual);
	CHECK(hw.rx_tone.type == TONE_NONE);
	// A split: through a repeater, set up again at once (no retune); simplex again: direct
	CHECK(!d->rmo);
	req(RREQ_VFO_TX, 451000000);
	run(50);
	CHECK(d->rmo && hw.rx_hz == 446000000);
	req(RREQ_VFO_TX, 0);
	run(50);
	CHECK(!d->rmo);
	// Receive filtering as a DMR channel with no RX group list: the talk target's group (9) and nothing else
	hear_call(true, 9, 9995555, 1, 1, true);
	CHECK(d->last_ms != 0 && d->last.dst == 9 && d->last.src == 9995555);
	hear_call(true, 3100, 9997777, 1, 1, false);
	CHECK(d->state == RDMR_RX_OTHER);
	run(500);
	hear_call(true, 9, 9995555, 5, 1, false);	// another color code: not heard
	CHECK(sm.view.state == RS_IDLE);
	run(500);

	// Color code, slot and a typed talk target
	req(RREQ_DMR_CC, 7);
	req(RREQ_DMR_TS, 2);
	req(RREQ_DMR_TARGET, 3100);
	run(50);
	CHECK(d->cc == 7 && d->ts == 2 && sm.view.ch.color_code == 7 && sm.view.ch.timeslot == 2);
	CHECK(!sm.view.temporary && sm.vfo_ch.color_code == 7 && sm.vfo_ch.timeslot == 2);
	CHECK(d->target_id == 3100 && !d->target_private && !d->target_manual && sim.cc_filter == 7);
	hear_call(true, 3100, 9997777, 7, 1, true);
	CHECK(d->last.dst == 3100 && d->last.src == 9997777);
	// The debug link's one request for mode, color code and slot (RADIO op 31), 0xFF / 0 = keep
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 9, 0 }, 3) == DL_ST_OK);
	run(20);
	CHECK(d->cc == 9 && d->ts == 2 && sm.vfo_ch.color_code == 9);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 0xFF, 1 }, 3) == DL_ST_OK);
	run(20);
	CHECK(d->cc == 9 && d->ts == 1);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0, 0xFF, 0 }, 3) == DL_ST_OK);
	run(20);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !sim.started);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 1, 7, 2 }, 3) == DL_ST_OK);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started && d->cc == 7 && d->ts == 2);
	// P25 (2) in a build without it: unavailable; past P25: a bad argument
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 2, 0xFF, 0 }, 3) ==
	      (CONFIG_P25 ? DL_ST_OK : DL_ST_UNAVAILABLE));
#if CONFIG_P25
	CHECK(sm.view.ch.mode == RADIO_MODE_P25);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 1, 0xFF, 0 }, 3) == DL_ST_OK);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started);
#endif
	// NXDN (3) the same; past NXDN: a bad argument
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 3, 0xFF, 0 }, 3) ==
	      (CONFIG_NXDN ? DL_ST_OK : DL_ST_UNAVAILABLE));
#if CONFIG_NXDN
	CHECK(sm.view.ch.mode == RADIO_MODE_NXDN && radio_nxdn_running() && !sim.started);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 1, 0xFF, 0 }, 3) == DL_ST_OK);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started && !radio_nxdn_running());
#endif
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 4, 0xFF, 0 }, 3) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 16, 0 }, 3) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 0xFF, 3 }, 3) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 0xFF }, 2) == DL_ST_BAD_ARG);
	CHECK(debuglink_is_feature_op(DL_OP_DMR_FIELDS));
	// PTT sends the typed target from our own ID
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 446000000 && d->call.dst == 3100 && d->call.group &&
	      d->call.src == 9996543);
	req(RREQ_PTT, 0);
	run(1000);
	// A private target
	req(RREQ_DMR_TARGET, (int32_t)((1u << 24) | 9997777u));
	run(50);
	CHECK(d->target_id == 9997777 && d->target_private && !d->target_manual);
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX && d->call.dst == 9997777 && !d->call.group);
	req(RREQ_PTT, 0);
	run(1000);
	// A call back is for now: the VFO's target is still there after it is cleared
	req(RREQ_DMR_TARGET, 3100);
	hear_call(false, 9996543, 9995555, 7, 1, true);
	req(RREQ_DMR_CALL_BACK, 0);
	CHECK(d->target_manual && d->target_id == 9995555 && d->target_private);
	req(RREQ_DMR_TARGET_CLEAR, 0);
	run(20);
	CHECK(!d->target_manual && d->target_id == 3100 && !d->target_private);

	// Back to FM and again: the stack stops and starts, the settings stay
	req(RREQ_MODE, RADIO_MODE_FM);
	run(20);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !sim.started && !d->active && hw_mode == RADIO_MODE_FM);
	req(RREQ_DMR_CC, 3);			// color code only in DMR
	CHECK(sm.vfo_ch.color_code == 7);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sim.started && d->cc == 7 && d->ts == 2 && d->target_id == 3100);

	// A restart: the VFO comes back in DMR with all of it
	req(RREQ_DMR_TARGET, (int32_t)((1u << 24) | 9997777u));
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && hw.rx_hz == 446000000);
	CHECK(d->cc == 7 && d->ts == 2 && d->target_id == 9997777 && d->target_private);
	// The channel is not touched by any of it
	req(RREQ_VFO, 0);
	run(50);
	CHECK(!sm.view.vfo && sm.view.channel_number == 4 && d->cc == 2 && d->ts == 1 && d->target_id == 201 &&
	      !sm.view.temporary);
	req(RREQ_ZONE_POS, 0);			// an FM channel
	run(20);
	CHECK(!sim.started && sm.view.ch.mode == RADIO_MODE_FM);
	req(RREQ_VFO, 1);			// the VFO is still a DMR one
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started && d->cc == 7 && d->target_id == 9997777);
	req(RREQ_VFO_STEP, 1);			// stepping the frequency retunes the DMR VFO
	run(50);
	CHECK(sm.view.ch.rx_hz == 446012500 && hw.rx_hz == 446012500 && sim.started && d->cc == 7);

	// A record from before the DMR VFO (mode FM, the old color code and slot bytes, no target) reads as FM
	settings_t *st = settings_begin();
	st->vfo_mode = RADIO_MODE_FM;
	st->vfo_cc = 1;
	st->vfo_ts = 1;
	st->vfo_bw &= 0x0Fu;			// the old bandwidth byte: a plain radio_bw_t
	st->vfo_talk_lo = 0;
	settings_end(now);
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started && sm.view.ch.color_code == 1);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sim.started && d->cc == 1 && d->ts == 1 && d->target_id == 9);
	// An ID out of range in a damaged record is dropped
	st = settings_begin();
	st->vfo_cc = 0xF1;
	st->vfo_ts = 0x3D;			// bits 2-5: 0xF; the ID is 0xFF0000 here, a valid 24 bit number
	st->vfo_talk_lo = 0;
	settings_end(now);
	boot();
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(d->cc == 1 && d->ts == 1 && d->target_id == 0xFF0000u && !d->target_private);
	// Leave the VFO the way the other tests expect it
	req(RREQ_VFO, 0);
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}
#endif

static void reload_channel(void);

// Copy to VFO (RREQ_VFO_COPY, docs/spec/dmr-voice.md 2.2): what is in effect on the channel becomes the VFO's own
// settings, the VFO opens, the channel stays as it was
static void test_vfo_copy(void)
{
	setup_codeplug();
	start();
	// An FM channel with an offset and a TX tone (channel 7, zone "Tones"); temporary bandwidth, RX tone and power
	req(RREQ_ZONE, 2);
	run(20);
	CHECK(sm.view.channel_number == 7 && !sm.view.vfo);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_RX, (subtone_t){ TONE_CTCSS, 885 }));
	req(RREQ_POWER, 1);			// the channel's default is 1: this is the high step
	CHECK(sm.view.temporary && sm.view.power == 2);
	req(RREQ_VFO_COPY, 0);
	run(20);
	CHECK(sm.view.vfo && sm.view.channel_number == 0 && !sm.view.temporary && sm.view.temp == 0);
	CHECK(sm.view.ch.rx_hz == 146940000 && sm.view.ch.tx_hz == 146340000 && hw.rx_hz == 146940000);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.view.ch.bw == RADIO_BW_12K5);
	CHECK(sm.view.ch.tx_tone.type == TONE_CTCSS && sm.view.ch.tx_tone.value == 1000);
	CHECK(sm.view.ch.rx_tone.type == TONE_CTCSS && sm.view.ch.rx_tone.value == 885 && hw.rx_tone.value == 885);
	CHECK(sm.view.power == 2 && sm.vfo_ch.power == 2);
	CHECK(strcmp(sm.view.ch.name, "VFO") == 0);
	req(RREQ_PTT, 1);			// the split: TX on the channel's TX frequency
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 146340000 && hw.tx_power == 2);
	req(RREQ_PTT, 0);
	// The VFO's values are its own settings, kept across a restart; the channel was not touched
	boot();
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 146940000 && sm.view.ch.tx_hz == 146340000 &&
	      sm.view.ch.bw == RADIO_BW_12K5 && sm.view.ch.rx_tone.value == 885 && sm.view.ch.tx_tone.value == 1000 &&
	      sm.view.power == 2);
	req(RREQ_VFO, 0);
	run(20);
	CHECK(!sm.view.vfo && sm.view.channel_number == 7 && sm.view.ch.bw == RADIO_BW_25K && !sm.view.temporary &&
	      sm.view.ch.rx_tone.type == TONE_NONE && chans[7].bw == RADIO_BW_25K && sm.view.power == 1);
	// In the VFO the request does nothing, and neither does it while transmitting
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 440000000);
	req(RREQ_VFO_COPY, 0);
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 440000000);
	req(RREQ_VFO, 0);
	req(RREQ_PTT, 1);
	req(RREQ_VFO_COPY, 0);
	CHECK(!sm.view.vfo && sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	run(20);
	// A frequency outside the board's bands: refused, nothing changes
	chans[7].rx_hz = 520000000;
	chans[7].tx_hz = 520000000;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 0);
	run(20);
	CHECK(radio_vfo_copy_check(&sm.view.ch, false) == VFO_COPY_OUT_OF_BAND);
	CHECK(debuglink_radio_feature_op(DL_OP_VFO_COPY, NULL, 0) == DL_ST_REFUSED);
	req(RREQ_VFO_COPY, 0);
	run(20);
	CHECK(!sm.view.vfo && sm.view.ch.rx_hz == 520000000 && sm.vfo_ch.rx_hz == 440000000);
	chans[7].rx_hz = 146940000;
	chans[7].tx_hz = 146340000;
	chans[7].tx_tone.type = TONE_NONE;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 0);
	run(20);
	// The debug link op: the same request; nothing to do in the VFO
	CHECK(debuglink_is_feature_op(DL_OP_VFO_COPY));
	CHECK(debuglink_radio_feature_op(DL_OP_VFO_COPY, NULL, 0) == DL_ST_OK);
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 146940000 && sm.view.ch.tx_tone.type == TONE_NONE);
	CHECK(debuglink_radio_feature_op(DL_OP_VFO_COPY, NULL, 0) == DL_ST_UNAVAILABLE);
	CHECK(radio_vfo_copy_check(&sm.view.ch, true) == VFO_COPY_ALREADY);
	// The DTMF op (radio-features.md 3.7): a key's edges, refused unless a key would send now
	CHECK(debuglink_is_feature_op(DL_OP_DTMF));
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ '5', 1 }, 2) == DL_ST_REFUSED);
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ '5' }, 1) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ 'x', 1 }, 2) == DL_ST_BAD_ARG);
	req(RREQ_PTT, 1);
	run(60);
	CHECK(sm.view.state == RS_TX && sm.view.dtmf_ready);
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ 'D', 1 }, 2) == DL_ST_OK);
	run(20);
	CHECK(dtmf_on == 'D' && sm.view.dtmf == 'D');
	{
		static char buf[2048];
		dl_text_t t;
		dl_text_init(&t, buf, sizeof buf);
		debuglink_view_radio(&sm.view, &t);
		CHECK(strstr(buf, "dtmf=D\n") != NULL && strstr(buf, "dtmf_ready=1") != NULL &&
		      strstr(buf, "dtmf_digits=D\n") != NULL && strstr(buf, "dtmf_sent=1\n") != NULL);
	}
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ 'D', 0 }, 2) == DL_ST_OK);
	run(200);
	CHECK(dtmf_on == 0 && sm.view.dtmf == 0);
	req(RREQ_PTT, 0);
	run(20);
	// The UI's service call: the code of a refusal, or the request
	req(RREQ_VFO, 0);
	run(20);
	CHECK(ui_svc_vfo_copy(&sm.view) == VFO_COPY_OK && sm.view.vfo);
	CHECK(ui_svc_vfo_copy(&sm.view) == VFO_COPY_ALREADY);
	req(RREQ_VFO, 0);
	// An FM channel leaves the VFO's DMR values alone
#if CONFIG_VFO_DMR
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 440000000);
	req(RREQ_MODE, RADIO_MODE_DMR);
	req(RREQ_DMR_CC, 9);
	req(RREQ_DMR_TARGET, 3100);
	req(RREQ_VFO, 0);
	req(RREQ_VFO_COPY, 0);
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && sm.vfo_ch.color_code == 9 && sm.vfo_talk_id == 3100);
	req(RREQ_VFO, 0);
#endif
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}

#if CONFIG_VFO_DMR
// A DMR channel: color code, timeslot and the talk target (a contact is its ID and the group / private flag)
static void test_vfo_copy_dmr(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;

	dmr_start(true);			// channel 4: DMR, CC 2, TS 1, contact 1 = group 201
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && hw_mode == RADIO_MODE_DMR);
	CHECK(sm.view.ch.rx_hz == 438500000 && d->cc == 2 && d->ts == 1 && d->target_id == 201 && !d->target_private);
	CHECK(sm.vfo_talk_id == 201 && !sm.vfo_talk_private && !d->target_manual && sim.cc_filter == 2);
	// The VFO has its own settings now: they stay when the channel is left, and the channel is as before
	req(RREQ_DMR_CC, 5);
	req(RREQ_VFO, 0);
	run(50);
	CHECK(!sm.view.vfo && d->cc == 2 && d->target_id == 201 && sm.vfo_ch.color_code == 5 && sm.view.channel_number == 4);
	// A private contact
	chans[4].contact = 2;
	reload_channel();
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && d->target_id == 9991234 && d->target_private && sm.vfo_talk_private && d->cc == 2);
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX && d->call.dst == 9991234 && !d->call.group && hw.tx_hz == 438500000);
	req(RREQ_PTT, 0);
	run(1000);
	req(RREQ_VFO, 0);
	// No contact: what the channel itself would send, the RX group list's first talkgroup (201), else talkgroup 9
	chans[4].contact = 0;
	reload_channel();
	CHECK(d->target_id == 201);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && d->target_id == 201 && !d->target_private && sm.vfo_talk_id == 201);
	req(RREQ_VFO, 0);
	chans[4].listen_list = 0;
	reload_channel();
	CHECK(d->target_id == 9);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && d->target_id == 9 && !d->target_private && sm.vfo_talk_id == 9);
	req(RREQ_VFO, 0);
	chans[4].listen_list = 1;
	// Temporary changes in effect are copied: color code, slot, a talk target chosen for now
	chans[4].contact = 1;
	reload_channel();
	req(RREQ_DMR_CC, 7);
	req(RREQ_DMR_TS, 2);
	req(RREQ_DMR_TARGET, (int32_t)((1u << 24) | 9997777u));
	run(50);
	CHECK(sm.view.temporary && d->target_manual && d->target_id == 9997777);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && !sm.view.temporary && d->cc == 7 && d->ts == 2 && d->target_id == 9997777 &&
	      d->target_private && !d->target_manual && sm.vfo_ch.color_code == 7 && sm.vfo_ch.timeslot == 2);
	// All of it survives a restart
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && d->cc == 7 && d->ts == 2 &&
	      d->target_id == 9997777 && d->target_private && hw.rx_hz == 438500000);
	req(RREQ_VFO, 0);
	run(50);
	CHECK(!sm.view.vfo && d->cc == 2 && d->ts == 1 && d->target_id == 201 && !sm.view.temporary);
	req(RREQ_VFO, 0);
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}
#else
static void test_vfo_copy_dmr(void)
{
	// Without the DMR VFO a DMR channel is refused, nothing changes
	dmr_start(true);
	CHECK(radio_vfo_copy_check(&sm.view.ch, false) == VFO_COPY_NO_MODE);
	CHECK(ui_svc_vfo_copy(&sm.view) == VFO_COPY_NO_MODE);
	CHECK(debuglink_radio_feature_op(DL_OP_VFO_COPY, NULL, 0) == DL_ST_REFUSED);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(!sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && sm.view.channel_number == 4);
	setup_codeplug();
}
#endif

static void test_dmr_denied(void)
{
	// Polite access: a call on our slot and color code refuses PTT, and the arming is released
	dmr_start(true);
	hear_call(true, 3100, 9997777, 2, 1, false);
	CHECK(sm.view.state == RS_RX);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BUSY);
	CHECK(hw.tx_calls == 1 && hw.tx_off_calls == 1 && sim.tx_begins == 0);
	run(100);
	CHECK(sm.view.state == RS_TX_DENIED);
	req(RREQ_PTT, 0);
	run(20);
	CHECK(sm.view.state != RS_TX_DENIED);
	run(600);

	// Through a repeater that never answers: three wake-ups, then refused
	chans[4].tx_hz = 433500000;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(sm.view.dmr.rmo && sim.cfg.repeater && sim.locked_ts == 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 433500000);
	run(500);
	CHECK(sm.view.dmr.state == RDMR_TX_WAIT);
	run(4000);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_REPEATER && hw.tx_off_calls == 2);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE);

	// No DMR ID anywhere
	own_dmr_id = 0;
	dmr_start(true);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_ID && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	own_dmr_id = 9996543;

	// RX-only channel and out-of-band checks come first on DMR channels too
	dmr_start(true);
	chans[4].rx_only = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	chans[4].rx_only = false;
}

// The LEDs while a repeater is woken and on a deny (radio-features.md 5.7): red blinks once per wake-up burst, not
// solid, and the deny flashes red, green, red, green, red, green; a direct call is solid red throughout
static void test_dmr_wake_leds(void)
{
	// Direct: solid red from PTT to release
	dmr_start(true);
	req(RREQ_PTT, 1);
	bool solid = true;
	for (int i = 0; i < 100; i++) {
		run(5);
		solid = solid && hw.led[LED_RED] && (sm.view.leds & RADIO_LED_RED) != 0;
	}
	CHECK(solid && sm.view.dmr.state == RDMR_TX);
	req(RREQ_PTT, 0);
	run(600);
	CHECK(!hw.led[LED_RED]);

	// Through a repeater that never answers
	chans[4].tx_hz = 433500000;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	req(RREQ_PTT, 1);
	uint32_t ons = 0, on_ms = 0, wait_ms = 0;
	bool was = hw.led[LED_RED];
	ons += was ? 1u : 0u;
	while (sm.view.state == RS_TX && wait_ms < 6000) {
		run(5);
		wait_ms += 5;
		if (sm.view.state != RS_TX) {
			break;		// refused: the deny flash, below
		}
		bool r = hw.led[LED_RED];
		CHECK(r == ((sm.view.leds & RADIO_LED_RED) != 0));	// the view says what is driven
		CHECK(!hw.led[LED_GREEN]);
		if (r && !was) {
			ons++;
		}
		on_ms += r ? 5u : 0u;
		was = r;
	}
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_REPEATER);
	CHECK(ons == DMR_CALL_WAKEUP_ATTEMPTS);			// one blink per wake-up burst
	CHECK(on_ms <= DMR_CALL_WAKEUP_ATTEMPTS * (RADIO_LED_WAKE_MS + 10u) && on_ms * 4u < wait_ms);	// not solid
	CHECK(on_ms >= DMR_CALL_WAKEUP_ATTEMPTS * (RADIO_LED_WAKE_MS - 10u));	// each long enough to see
	// The deny flash: red, green, red, green, red, green, then dark
	char seen[16] = { 0 };
	uint8_t n = 0;
	char last = 0;
	for (uint32_t t = 0; t < RADIO_LED_DENY_STEPS * RADIO_LED_DENY_STEP_MS + 200u; t += 5) {
		char c = hw.led[LED_RED] ? (hw.led[LED_GREEN] ? 'b' : 'r') : (hw.led[LED_GREEN] ? 'g' : '-');
		if (c != last && n + 1u < sizeof(seen)) {
			seen[n++] = c;
			last = c;
		}
		run(5);
	}
	CHECK(strcmp(seen, "rgrgrg-") == 0);
	req(RREQ_PTT, 0);
	chans[4].tx_hz = chans[4].rx_hz;
	setup_codeplug();
}

// A PTT line stuck low: red and green in turn, one at a time, until it isn't (the power cycle)
static void test_ptt_line_stuck(void)
{
	bool red = hw.led[LED_RED], green = hw.led[LED_GREEN];
	ptt_stuck = true;
	run(10);
	uint32_t turns = 0;
	bool was = hw.led[LED_RED];
	for (uint32_t t = 0; t < 1000u; t += 5) {
		CHECK(hw.led[LED_RED] != hw.led[LED_GREEN]);
		turns += hw.led[LED_RED] != was;
		was = hw.led[LED_RED];
		run(5);
	}
	CHECK(turns >= 1000u / RADIO_LED_STUCK_STEP_MS - 1u && turns <= 1000u / RADIO_LED_STUCK_STEP_MS + 1u);
	ptt_stuck = false;
	run(20);
	CHECK(hw.led[LED_RED] == red && hw.led[LED_GREEN] == green);
}

static void test_hotspot(void)
{
	CHECK(radio_sm_hotspot_freq_ok(438800000, 438800000));
	CHECK(!radio_sm_hotspot_freq_ok(121500000, 121500000));	// TX not allowed there
	CHECK(!radio_sm_hotspot_freq_ok(438800000, 0));
	CHECK(radio_sm_hotspot_power(255) == 2 && radio_sm_hotspot_power(0) == 0 && radio_sm_hotspot_power(128) == 1);

	dmr_start(true);
	// Refused while PTT is held
	req(RREQ_PTT, 1);
	req(RREQ_HS_ENTER, 0);
	CHECK(!sm.hotspot);
	req(RREQ_PTT, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE);

	int modes = hw.set_mode_calls;
	int tx0 = hw.tx_calls, off0 = hw.tx_off_calls;
	req(RREQ_HS_ENTER, 0);
	CHECK(sm.hotspot && sm.view.hotspot && !sim.started && sim.cb == NULL && !sm.view.dmr.active);
	CHECK(hw.set_mode_calls == modes);	// already in DMR
	radio_req_t r = { RREQ_HS_FREQ, 438800000, 438800000 };
	radio_sm_request(&sm, &r, now);
	CHECK(hw.rx_hz == 438800000 && hw.tx_hz == 438800000 && hw.tx_calls == tx0 + 1 &&
		hw.tx_power == 1);
	req(RREQ_HS_POWER, 255);
	CHECK(hw.tx_calls == tx0 + 2 && hw.tx_power == 2);
	r.arg = 121500000;	// refused: the radio stays where it was
	r.arg2 = 121500000;
	radio_sm_request(&sm, &r, now);
	CHECK(hw.rx_hz == 438800000);

	// Channel keys, monitor and PTT do nothing; the carrier comes from the noise reading
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 4);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_HOTSPOT);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == off0);
	rx_signal(10, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.hs_carrier && sm.view.state == RS_IDLE);
	rx_signal(120, true);
	poll_for(RADIO_POLL_MS);
	CHECK(!sm.hs_carrier);

	// Leave: disarmed, the channel back, the call stack has the modem again
	req(RREQ_HS_LEAVE, 0);
	CHECK(!sm.hotspot && !sm.view.hotspot && hw.tx_off_calls == off0 + 1);
	CHECK(hw.rx_hz == 438500000 && sim.started && sim.cb != NULL && sm.view.dmr.active);
	hear_call(true, 201, 9991234, 2, 1, false);
	CHECK(sm.view.state == RS_RX);
	run(500);

	// From an FM channel: into DMR mode and back
	req(RREQ_CH_STEP, 1);
	modes = hw.set_mode_calls;
	req(RREQ_HS_ENTER, 446000000);
	CHECK(sm.hotspot && hw.set_mode_calls == modes + 1 && hw.rx_hz == 446000000);
	req(RREQ_HS_LEAVE, 0);
	CHECK(!sm.hotspot && hw.set_mode_calls == modes + 2 && hw.rx_hz == 121500000 && !sim.started);
	CHECK(hw.tx_off_calls == off0 + 1);	// never armed: no TX frequency was set
}

// ---- The hotspot session end to end: host link frames -> mmdvm.c -> hotspot.c -> radio_hotspot.c ops -> the radio
// state machine -> the modem, with the radio task modeled: requests are queued and handled either at once (the
// radio task is above the host link task and waiting on its queue) or later (it was busy elsewhere) ----

#define HS_Q	16
static radio_req_t hs_q[HS_Q];
static int hs_qn;
static bool radio_preempts = true;	// a post runs the radio task at once
static int mm_replies, mm_acks, mm_naks;
static uint8_t mm_last[64];

static void radio_pass(void)
{
	for (int i = 0; i < hs_qn; i++) {
		radio_sm_request(&sm, &hs_q[i], now);
	}
	hs_qn = 0;
	radio_hs_watch(&sm, now);
	radio_hs_publish(&sm);
}

static void hs_post(const radio_req_t *r, bool must)
{
	(void)must;
	if (hs_qn < HS_Q) {
		hs_q[hs_qn++] = *r;
	}
	if (radio_preempts) {
		radio_pass();
	}
}

static void mm_write(const uint8_t *buf, size_t len)
{
	mm_replies++;
	if (len >= 3 && buf[2] == MMDVM_ACK) {
		mm_acks++;
	}
	if (len >= 3 && buf[2] == MMDVM_NAK) {
		mm_naks++;
	}
	memcpy(mm_last, buf, len < sizeof(mm_last) ? len : sizeof(mm_last));
}

static const mmdvm_io_t mm_io = { .write = mm_write, .description = "RadioFoundry fake" };

static void mm_cmd(uint8_t cmd, const uint8_t *p, uint8_t n)
{
	uint8_t f[64];
	f[0] = MMDVM_FRAME_START;
	f[1] = (uint8_t)(n + 3);
	f[2] = cmd;
	if (n > 0) memcpy(f + 3, p, n);	// p may be NULL for a command without a payload
	mmdvm_frame(f, (uint16_t)(n + 3), now);
}

// The host link task's loop for ms: mmdvm_poll every 5 ms; the radio task's tick and poll on their periods
static void hs_run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		if (now % 30 == 0) {
			sim_slot();
		}
		if (now % RADIO_TICK_MS == 0) {
			radio_sm_tick(&sm, now);
		}
		if (now % RADIO_POLL_MS == 0) {
			radio_sm_poll(&sm, now);
			radio_pass();
		}
		mmdvm_poll(now);
	}
}

static void hs_session_start(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	chans[4].rx_hz = chans[4].tx_hz = 453625000;	// a DMR channel
	req(RREQ_ZONE_POS, 3);
	run(50);
	hs_qn = 0;
	radio_hs_init(hs_post);
	radio_hs_publish(&sm);
	hotspot_set_radio(&radio_hs_ops);
	hotspot_init(&sim_modem, mmdvm_send);
	mmdvm_init(&mm_io);
	mm_replies = mm_acks = mm_naks = 0;
}

static void test_hotspot_session(void)
{
	hotspot_stats_t st;
	hotspot_view_t v;

	// The radio task answers the enter at once: the first frame takes the modem
	hs_session_start();
	CHECK(sim.started && sim.cb != NULL && sm.view.dmr.active);
	radio_preempts = true;
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	CHECK(mm_replies == 1 && mm_last[2] == MMDVM_GET_VERSION);
	hotspot_get_stats(&st);
	hotspot_get_view(&v);
	CHECK(sm.hotspot && v.running && st.enter_attempts == 1 && hotspot_running());
	CHECK(sim.started && sim.cb == hotspot_modem_event);
	mm_cmd(MMDVM_GET_STATUS, NULL, 0);
	CHECK(mm_last[2] == MMDVM_GET_STATUS && mm_last[8] > 0);	// DMR slot 2 space: the hotspot runs
	hs_run(4000);
	hotspot_get_view(&v);
	CHECK(v.running && sm.hotspot);	// the orphan watch leaves it alone
	mmdvm_end();
	radio_pass();
	CHECK(!sm.hotspot && sim.cb != NULL && sim.cb != hotspot_modem_event && sm.view.dmr.active);

	// The radio task busy when the first frame comes (the enter is handled after the post returned): taken on the
	// next attempt, HOTSPOT_ENTER_RETRY_MS later
	hs_session_start();
	radio_preempts = false;
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	hotspot_get_stats(&st);
	CHECK(st.enter_attempts == 1 && !hotspot_running());
	mm_cmd(MMDVM_GET_STATUS, NULL, 0);
	CHECK(mm_last[2] == MMDVM_GET_STATUS && mm_last[8] == 0);	// no space until the hotspot runs
	hs_run(HOTSPOT_ENTER_RETRY_MS + 50);
	hotspot_get_stats(&st);
	hotspot_get_view(&v);
	CHECK(st.enter_attempts == 2 && v.running && sm.hotspot && sim.cb == hotspot_modem_event);
	radio_preempts = true;
	mmdvm_end();
	radio_pass();
	CHECK(!sm.hotspot && sm.view.dmr.active);
	// A second session on the same boot (the first ended with the port closed, after a full configuration): the
	// next host frame takes the modem again
	hs_session_start();
	radio_preempts = true;
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	{
		uint8_t fr[13] = { 0 };
		uint32_t hz = 453625000;
		for (int i = 0; i < 4; i++) {
			fr[1 + i] = (uint8_t)(hz >> (8 * i));
			fr[5 + i] = (uint8_t)(hz >> (8 * i));
		}
		fr[9] = 255;
		mm_cmd(MMDVM_SET_FREQ, fr, 10);
		uint8_t cfg[21] = { MMDVM_CONFIG_SIMPLEX, MMDVM_MODE_BIT_DMR, 10, 0, 128, 128, 1 };
		mm_cmd(MMDVM_SET_CONFIG, cfg, sizeof(cfg));
		uint8_t m = MMDVM_MODE_DMR;
		mm_cmd(MMDVM_SET_MODE, &m, 1);
	}
	CHECK(mm_naks == 0 && hotspot_running() && sm.hs_tx_hz == 453625000 && sm.dmr_keyed);
	hs_run(500);
	mmdvm_end();
	hs_run(100);
	CHECK(!sm.hotspot && !sm.dmr_keyed && sm.view.dmr.active);
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	hs_run(100);
	hotspot_get_stats(&st);
	hotspot_get_view(&v);
	CHECK(st.enter_attempts == 2 && v.running && sm.hotspot && sim.cb == hotspot_modem_event);
	CHECK(st.starts == 2 && st.stops == 1);	// counted across sessions: enter_attempts 2 is two sessions here
	mmdvm_end();
	radio_pass();
	hotspot_get_stats(&st);
	CHECK(st.stops == 2 && !hotspot_running());
}

// The position, the VFO and the preferences come back after a reboot (core/settings), temporary changes don't, and
// a codeplug change underneath moves the radio to a channel that still exists
static void test_persist(void)
{
	setup_codeplug();
	start();
	req(RREQ_ZONE, 1);
	CHECK(sm.view.zone_index == 1 && sm.view.channel_number == 6);
	boot();
	CHECK(!sm.view.vfo && sm.view.zone_index == 1 && sm.view.channel_number == 6);
	req(RREQ_ZONE, 0);
	req(RREQ_ZONE_POS, 3);
	CHECK(sm.view.channel_number == 4);
	boot();
	CHECK(sm.view.zone_index == 0 && sm.view.zone_pos == 3 && sm.view.channel_number == 4);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sm.board_mode == RADIO_MODE_DMR);

	// The VFO and its settings
	req(RREQ_VFO_SET, 145600000);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_CTCSS, 1000 }));
	CHECK(sm.view.vfo);
	boot();
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 145600000 && sm.view.ch.bw == RADIO_BW_25K);
	CHECK(sm.view.ch.tx_tone.type == TONE_CTCSS && sm.view.ch.tx_tone.value == 1000 && hw.rx_hz == 145600000);
	req(RREQ_VFO, 0);
	boot();
	CHECK(!sm.view.vfo && sm.view.channel_number == 4);
	req(RREQ_VFO, 1);
	CHECK(sm.view.ch.rx_hz == 145600000);	// the VFO kept its frequency while on the channel
	req(RREQ_VFO, 0);

	// Temporary changes are not saved; the base squelch and master power are preferences. The old DMR ID override's
	// bytes are ignored: the own ID is the codeplug's
	req(RREQ_SQUELCH, 2);
	CHECK(sm.view.temporary);
	boot();
	CHECK(!sm.view.temporary && sm.view.squelch == RADIO_SQUELCH_DEFAULT);
	settings_t *st = settings_begin();
	st->squelch = 2;
	st->master_power = 0;
	st->unused_dmr_id = 9997001;
	settings_end(now);
	req(RREQ_RELOAD, 0);
	CHECK(sm.view.squelch == 2 && !sm.view.temporary);
	CHECK(sm.view.dmr.own_id == 9996543);
	req(RREQ_ZONE_POS, 0);			// "Simplex": default power = the master power now
	CHECK(sm.view.power == 0);
	boot();
	CHECK(sm.view.squelch == 2 && sm.view.power == 0 && sm.view.dmr.own_id == 9996543);

	// The codeplug changes underneath: the channel is deleted, the radio moves on to the next one in the zone
	req(RREQ_ZONE_POS, 3);
	CHECK(sm.view.channel_number == 4);
	used[4] = false;
	cp_serial++;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.channel_number == 5 && sm.view.zone_pos == 4);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.board_mode == RADIO_MODE_FM);
	boot();
	CHECK(sm.view.channel_number == 5);
	// Renamed in place: the new name shows
	strcpy(chans[5].name, "Air 2");
	cp_serial++;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.channel_number == 5 && strcmp(sm.view.ch.name, "Air 2") == 0);
	// A saved zone that no longer exists: the first zone
	req(RREQ_ZONE, 2);
	nzones = 2;
	boot();
	CHECK(sm.view.zone_index == 0);

	// The radio core's settings (radio_set_settings) are saved and in effect after a reboot
	radio_settings_t rs;
	radio_settings_default(&rs);
	CHECK(sm.set.tot_s == rs.tot_s && sm.set.key_beep);
	rs.tot_s = 45;
	rs.key_beep = false;
	rs.roger_beep = true;
	rs.vox_level = 99;			// clamped to 10
	radio_sm_set_settings(&sm, &rs, now);
	settings_t cur;
	settings_read(&cur);
	CHECK(cur.rs_stored == 1 && cur.rs_tot_s == 45 && cur.rs_vox_level == 10);
	boot();
	CHECK(sm.set.tot_s == 45 && !sm.set.key_beep && sm.set.roger_beep && sm.set.vox_level == 10);
	settings_begin()->rs_stored = 0;	// never set: the defaults
	settings_end(now);
	boot();
	CHECK(sm.set.tot_s == RADIO_TOT_DEFAULT_S && sm.set.key_beep && !sm.set.roger_beep);

	// The DMR data service's settings: saved when they change, back after a reboot
	dmrDataSettings_t ds;
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 0 && ds.radioCheckReply && ds.callAlertAck && ds.smsFormat == DMR_SMS_FORMAT_ETSI_IP);
	ds.arsServer = 9990999;
	ds.radioCheckReply = true;
	ds.callAlertAck = false;
	CHECK(ds.ipGatewayId == 0);		// no off-net route by default
	ds.ipGatewayId = 9990500;
	dmr_data_set_settings(&ds);
	run(50);
	settings_read(&cur);
	CHECK(cur.ds_stored == 1 && cur.ds_ars_server == 9990999 && cur.ds_flags == (SETTINGS_DS_RADIO_CHECK_REPLY |
											SETTINGS_DS_LRRP_REPLY | SETTINGS_DS_SMS_ETSI_IP));
	CHECK(cur.ds_ip_gateway == 9990500);
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 9990999 && ds.radioCheckReply && !ds.callAlertAck && ds.lrrpReply);
	CHECK(ds.ipGatewayId == 9990500);
	// Only the ID's 24 bits are read back; a record from before the gateway ID has 0 there: off
	settings_begin()->ds_ip_gateway = 0xFF000000u | 9990501;
	settings_end(now);
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.ipGatewayId == (9990501 & 0xFFFFFFu));
	settings_begin()->ds_ip_gateway = 0;
	settings_end(now);
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.ipGatewayId == 0);
	// A record from before ETSI IP text (no format bit) keeps TMS: only the default changed
	settings_t *old = settings_begin();
	old->ds_flags = SETTINGS_DS_CALL_ALERT_ACK | SETTINGS_DS_LRRP_REPLY;
	old->ds_ip_cnf = 0;
	settings_end(now);
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.smsFormat == DMR_SMS_FORMAT_TMS && ds.callAlertAck);
	// ... and, saved before radio check answers became the default (no SETTINGS_DS_CHECK_SET), answers them
	CHECK(ds.radioCheckReply);
	// Turned off by the user: saved with the mark, off after a reboot
	ds.radioCheckReply = false;
	dmr_data_set_settings(&ds);
	run(50);
	settings_read(&cur);
	CHECK((cur.ds_ip_cnf & SETTINGS_DS_CHECK_SET) && !(cur.ds_flags & SETTINGS_DS_RADIO_CHECK_REPLY));
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(!ds.radioCheckReply);
	settings_begin()->ds_stored = 0;
	settings_end(now);
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 0 && ds.radioCheckReply && ds.callAlertAck && ds.smsFormat == DMR_SMS_FORMAT_ETSI_IP);

	// The codeplug's settings area (docs/spec/codeplug-settings.md 5): a key that is present sets the ARS server, at
	// start and when the codeplug changes, and the radio's own copy follows it
	ds.arsServer = 3100999;
	dmr_data_set_settings(&ds);
	run(50);
	kv_ars_present = true;
	kv_ars = 3100777;
	cp_serial++;			// as a CPS commit or a TFTP write
	run(100);
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 3100777);
	settings_read(&cur);
	CHECK(cur.ds_ars_server == 3100777);
	// A change that isn't a codeplug write doesn't hold against the key
	ds.arsServer = 5;
	dmr_data_set_settings(&ds);
	run(50);
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 3100777);
	// The menu writes the key (a codeplug job), 0 turns ARS off, and a reboot reads it back
	CHECK(ui_svc_setting_get(UI_SET_ARS_SERVER) == 3100777);
	CHECK(ui_svc_setting_set(UI_SET_ARS_SERVER, 0) && storage_job_busy());
	CHECK(!ui_svc_setting_set(UI_SET_ARS_SERVER, 1));	// one job at a time
	uint32_t cp_was = cp_serial;
	storage_job_poll();
	CHECK(kv_ars_present && kv_ars == 0 && !storage_job_busy());
	run(100);
	CHECK(ui_svc_setting_get(UI_SET_ARS_SERVER) == 0);
	CHECK(cp_serial == cp_was);	// the key's value taken without a codeplug reload
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 0);
	CHECK(!ui_svc_setting_set(UI_SET_ARS_SERVER, CODEPLUG_DMR_ID_MAX + 1));
	// Absent (a codeplug from a tool that doesn't carry the area): the radio keeps what it has, and a change holds
	kv_ars_present = false;
	cp_serial++;
	run(100);
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 0);
	ds.arsServer = 3100888;
	dmr_data_set_settings(&ds);
	run(50);
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 3100888);
	// A key present at start that the radio's own copy doesn't hold (a factory reset leaves the codeplug, a power
	// cut before the copy was saved): the copy follows it, so the radio keeps the key's value once the key is gone
	kv_ars_present = true;
	kv_ars = 3100555;
	boot();
	run(100);
	settings_read(&cur);
	CHECK(cur.ds_ars_server == 3100555);
	kv_ars_present = false;
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 3100555);
	settings_begin()->ds_stored = 0;
	settings_end(now);
	boot();

	// Power off: nothing to deregister, so the radio core is done at once
	req(RREQ_POWER_OFF, 0);
	run(50);
	CHECK(sm.power_off_req && !sm.power_off_busy);
	// An auto power off that didn't cut the power (the knob still on): canceled, the radio runs on
	req(RREQ_POWER_OFF, 1);
	run(50);
	CHECK(!sm.power_off_req && !sm.power_off_busy);
	setup_codeplug();
	start();
}

// The air header's LC of the last transmission that started at air[first]
static void air_lc(unsigned first, dmr_lc_t *l)
{
	dmr_lc_unpack(sim.air[first].payload, l);
}

static void reload_channel(void)
{
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
}

static void tx_once(void)
{
	req(RREQ_PTT, 1);
	run(600);
	req(RREQ_PTT, 0);
	run(1000);
}

static void test_dmr_voice_features(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;
	dmr_lc_t l;

	// A typed private ID: PTT calls it, its name comes from the DMR ID database (cut to the name field)
	dmr_start(true);
	CHECK(!d->target_manual && !d->alias_tx && !d->privacy);
	req(RREQ_DMR_TARGET, (int32_t)(9995555u | (1u << 24)));
	run(20);
	CHECK(d->target_manual && d->target_id == 9995555 && d->target_private);
	CHECK(strcmp(d->target_name, "QD9XYZ Robert Lo") == 0);
	unsigned first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(sim.air[first].kind == DMR_BURST_VOICE_LC_HEADER && l.flco == DMR_FLCO_UNIT_VOICE && l.dst == 9995555 &&
	      l.src == 9996543);
	// Looked up once, then from the cache
	uint32_t finds = contact_finds, looks = iddb_lookups;
	run(2000);
	hear_call(true, 201, 9995555, 2, 1, true);
	CHECK(contact_finds == finds && iddb_lookups == looks);
	// Back to the channel's target
	req(RREQ_DMR_TARGET_CLEAR, 0);
	run(20);
	CHECK(!d->target_manual && d->target_id == 201 && !d->target_private);

	// A codeplug contact as the target; a channel change ends it
	req(RREQ_DMR_CONTACT, 2);
	run(20);
	CHECK(d->target_manual && d->target_id == 9991234 && d->target_private && strcmp(d->target_name, "Joe") == 0);
	req(RREQ_CH_STEP, 1);
	req(RREQ_CH_STEP, -1);
	run(50);
	CHECK(!d->target_manual && d->target_id == 201);
	req(RREQ_DMR_CONTACT, 99);	// no such contact: nothing changes
	run(20);
	CHECK(!d->target_manual);

	// A private call to us: shown with its name, emergency flag passed on; then call back to the caller
	uint8_t lc[9];
	mk_lc(lc, false, 9996543, 9995555);
	lc[2] = DMR_SVC_EMERGENCY;
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 2, false);
		run(60);
	}
	for (int i = 0; i < 6; i++) {
		burst_voice((uint8_t)i, 2, i == 5 ? lc : NULL);
		run(60);
	}
	CHECK(d->state == RDMR_RX && d->call.to_us && !d->call.group && d->call.emergency);
	CHECK(strcmp(d->call.src_name, "QD9XYZ Robert Lo") == 0);
	burst_lc(lc, 2, true);
	run(100);
	CHECK(d->reply_active && d->target_private && d->target_id == 9995555);	// the reply window
	req(RREQ_REPLY_CANCEL, 0);
	run(20);
	req(RREQ_DMR_CALL_BACK, 0);	// still possible after the window: from the last call heard
	run(20);
	CHECK(d->target_manual && d->target_private && d->target_id == 9995555);
	// A group call heard last: call back goes to its talkgroup, named from all the contacts
	hear_call(true, 3100, 9997777, 2, 1, true);
	req(RREQ_DMR_TARGET_CLEAR, 0);
	req(RREQ_DMR_HEAR_ALL, 1);
	hear_call(true, 3100, 9997777, 2, 1, true);
	req(RREQ_DMR_HEAR_ALL, 0);
	req(RREQ_DMR_CALL_BACK, 0);
	run(20);
	CHECK(d->target_manual && !d->target_private && d->target_id == 3100 && strcmp(d->target_name, "TG USA") == 0);
	req(RREQ_DMR_TARGET_CLEAR, 0);

	// Codeplug hang times become the reply windows (group 3 s, private 6 s)
	dmr_settings.group_hang_ms = 3000;
	dmr_settings.private_hang_ms = 6000;
	dmr_start(true);
	hear_call(true, 9, 9995555, 2, 1, true);
	CHECK(d->reply_active && d->reply_s == 3);
	run(3100);
	CHECK(!d->reply_active);
	hear_call(false, 9996543, 9995555, 2, 1, true);
	CHECK(d->reply_active && d->reply_s == 6);
	dmr_settings.group_hang_ms = dmr_settings.private_hang_ms = 0;

	// Talker alias TX when the channel asks for it on its timeslot: the radio name in alternate superframes
	dmr_start(true);
	chans[4].alias_tx = CODEPLUG_ALIAS_TS2_TEXT;	// TS1 channel: not on this one
	reload_channel();
	CHECK(!d->alias_tx);
	chans[4].alias_tx = CODEPLUG_ALIAS_TS1_TEXT;
	reload_channel();
	CHECK(d->alias_tx);
	dmr_call_stats_t cs0, cs1;
	dmr_call_get_stats(&cs0);
	req(RREQ_PTT, 1);
	run(360 * 4);
	req(RREQ_PTT, 0);
	run(1000);
	dmr_call_get_stats(&cs1);
	CHECK(cs1.tx_alias_lcs - cs0.tx_alias_lcs >= 1);	// "Q0TEST" in every other superframe (details: dmrCallTest)
	chans[4].alias_tx = 0;

	// Channel TX time-out from the codeplug
	dmr_start(true);
	chans[4].tot_s = 2;
	reload_channel();
	req(RREQ_PTT, 1);
	run(3000);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TIMEOUT);
	req(RREQ_PTT, 0);
	run(500);
	chans[4].tot_s = 0;

	// Basic privacy key: our calls go out with the privacy FID and service option; turned off again
	dmr_start(true);
	req(RREQ_DMR_PRIVACY, (int32_t)((1u << 16) | 0u));	// Motorola key 0 is not valid: ignored
	CHECK(!d->privacy);
	req(RREQ_DMR_PRIVACY, (int32_t)((1u << 16) | 7u));
	CHECK(d->privacy);
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(l.fid == 0x10 && l.svc == DMR_SVC_PRIVACY);
	req(RREQ_DMR_PRIVACY, 0);
	CHECK(!d->privacy);
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(l.fid == 0 && l.svc == 0);
}

// ---- DMR emergency in the radio core (docs/spec/dmr-emergency.md): the hot mic keys a call with the emergency service
// option to the target, PTT takes it over, cancel ends the option; a heard emergency call raises one alert ----

static void hear_emergency_call(uint32_t dst, uint32_t src)
{
	uint8_t lc[9];
	dmr_lc_t l = { .flco = DMR_FLCO_GROUP_VOICE, .svc = DMR_SVC_EMERGENCY, .dst = dst, .src = src };
	dmr_lc_pack(&l, lc);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 2, false);
		run(60);
	}
	for (int i = 0; i < 6; i++) {
		burst_voice((uint8_t)i, 2, i == 5 ? lc : NULL);
		run(60);
	}
	burst_lc(lc, 2, true);
	run(100);
}

static ui_event_t last_ev;	// the UI glue fakes below (ui_notify)
static int n_ev;

// A private contact added (an edit, a CPS write) after its ID was already called: the name shows from the next
// codeplug read on, not the cached "no name"
static void test_dmr_contact_added(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;
	dmr_start(true);
	req(RREQ_DMR_TARGET, (int32_t)(9996001u | (1u << 24)));
	run(20);
	CHECK(d->target_manual && d->target_id == 9996001 && d->target_private && d->target_name[0] == '\0');
	added_contact = (contact_t){ "Charlie Harper", 9996001, CONTACT_PRIVATE, 0 };
	cp_serial++;
	poll_for(RADIO_POLL_MS);
	run(20);
	CHECK(d->target_id == 9996001 && d->target_private && strcmp(d->target_name, "Charlie Harper") == 0);
	memset(&added_contact, 0, sizeof(added_contact));
}

#if CONFIG_APRS_DMR
// APRS over DMR (docs/spec/aprs.md 8): a beacon goes to the data service as a position report, from a build with
// the AX.25 backend and from one without it (radioCoreTestAprsDmr: no callsign, no APRS frequency, nothing of FM
// APRS in the way). Where there is no choice the stored "Send via" is not asked
static void test_aprs_dmr_beacon(void)
{
	aprs_view_t v0, v1;
	aprs_cfg_t c;

	boot();
	dmr_start(true);
	CHECK(ui_svc_setting_set(UI_SET_POS_FIXED_LAT, 337490000) && ui_svc_setting_set(UI_SET_POS_FIXED_LON, -843880000));
	aprs_get_cfg(&c);
	c.via = CONFIG_APRS_AFSK ? APRS_VIA_DMR : APRS_VIA_RF;	// DMR only: "radio" stored by another build still goes out
	c.dmr_dst = 310999;
	c.dmr_udt = false;
	aprs_set_cfg(&c, now);
	run(50);
	aprs_get_view(&v0);
	CHECK(v0.pos_valid && v0.lat == 337490000);
	CHECK(aprs_send_now());
	run(3000);
	aprs_get_view(&v1);
	CHECK(v1.dmr_sent == v0.dmr_sent + 1 && v1.dmr_failed == v0.dmr_failed && v1.rf_sent == v0.rf_sent);
	CHECK(v1.last_why == APRS_WHY_MANUAL);
	// The short UDT form too
	c.dmr_udt = true;
	aprs_set_cfg(&c, now);
	CHECK(aprs_send_now());
	run(3000);
	aprs_get_view(&v0);
	CHECK(v0.dmr_sent == v1.dmr_sent + 1 && v0.dmr_failed == v1.dmr_failed);
	// Automatic beacons: the interval sends without anyone asking
	c.tx_on = true;
	c.smart = false;
	c.interval_s = 60;
	aprs_set_cfg(&c, now);
	run(130000);
	aprs_get_view(&v1);
	CHECK(v1.dmr_sent >= v0.dmr_sent + 2 && v1.rf_sent == v0.rf_sent);
	c.tx_on = false;
	aprs_set_cfg(&c, now);
	// Off a DMR channel there is nowhere to send it: counted as failed, nothing keyed
	req(RREQ_ZONE_POS, 0);
	run(200);
	aprs_get_view(&v0);
	if (!CONFIG_APRS_AFSK) {
		CHECK(aprs_send_now());
		run(1000);
		aprs_get_view(&v1);
		CHECK(v1.dmr_sent == v0.dmr_sent && v1.dmr_failed == v0.dmr_failed + 1);
	}
	position_cfg_t pc;
	position_get_cfg(&pc);
	pc.fix_lat = pc.fix_lon = 0;
	position_set_cfg(&pc, now);
}
#endif

#if CONFIG_APRS_RX
// One NMEA sentence from src to dst (a private call) on UDP 10110 (SAP 3), as the data service hears it
static void hear_dmr_nmea(uint32_t src, uint32_t dst, const char *sentence, size_t length)
{
	uint8_t data[9 + 96] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x27, 0x7e, 0x27, 0x7e };
	static dmrBurst_t b[DMR_DATA_MAX_BURSTS];

	memcpy(&data[9], sentence, length);
	int nb = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_UDPIP_COMPRESSION, false, dst, src, data,
				    (int)(9 + length), DMR_TYPE_RATE_1_2, 4, b, DMR_DATA_MAX_BURSTS);
	CHECK(nb > 0);
	for (int i = 0; i < nb; i++) {
		now += 60;
		dmrDataServiceRxBurst(&b[i], now);
	}
	run(20);
}

// DMR APRS as a stock radio sends it (#401): through radio_sm's hook into the APRS station list under the sender's
// callsign in the ID database (else its DMR ID), to us or to anyone else (the APRS gateway). A sentence without a
// fix, or out of range, neither adds a station nor moves one
static void test_dmr_aprs_position(void)
{
	static const char heard[] = "$GPRMC,062544.000,A,3000.0000,N,8700.0000\0,W,0.00,0.00,051126,,,A\0";
	static const char *const none[] = {
		"$GPRMC,062544.000,V,3100.0000,N,08600.0000,W,,,051126,,,N",	// void status: no fix
		"$GPRMC,062544.000,A,9100.0000,N,08600.0000,W,,,051126,,,A",	// past 90 degrees
	};
	aprs_station_t st;

	dmr_start(false);
	uint8_t before = aprs_station_count();
	hear_dmr_nmea(9991234, 9996543, heard, sizeof(heard) - 1);
	CHECK(aprs_station_count() == before + 1);
	CHECK(aprs_station(0, &st) && strcmp(st.call, "9991234") == 0 && st.has_pos && st.lat == 300000000 &&
	      st.lon == -870000000);
	for (unsigned i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
		hear_dmr_nmea(9991234, 9996543, none[i], strlen(none[i]));	// the same station: not moved
		hear_dmr_nmea(9991235, 9996543, none[i], strlen(none[i]));	// another: not added
		CHECK(aprs_station_count() == before + 1);
		CHECK(aprs_station(0, &st) && strcmp(st.call, "9991234") == 0 && st.lat == 300000000 && st.lon == -870000000);
	}
	// The usual case: another radio's report to the APRS gateway (BrandMeister's <MCC>999), heard on the channel
	hear_dmr_nmea(9991236, 310999, heard, sizeof(heard) - 1);
	CHECK(aprs_station_count() == before + 2);
	CHECK(aprs_station(0, &st) && strcmp(st.call, "9991236") == 0 && st.lat == 300000000 && st.lon == -870000000);

	// EP, the name: an ID in the ID database is listed under its callsign, one not in it under the number (above)
	hear_dmr_nmea(9995555, 310999, heard, sizeof(heard) - 1);
	CHECK(aprs_station_count() == before + 3);
	CHECK(aprs_station(0, &st) && strcmp(st.call, "QD9XYZ") == 0 && st.lat == 300000000 && st.lon == -870000000);
}
#endif

static void test_dmr_emergency(void)
{
	dmr_lc_t l;
	dmr_start(true);
	dmr_emergency_settings_t es;
	dmr_emergency_settings_default(&es);
	es.mode = DMR_EMERGENCY_MODE_CALL;	// the alarm CSBKs: dmrDataChainTest
	es.hot_mic_s = 2;
	dmr_emergency_set_settings(&es);
	run(20);
	settings_t st;
	settings_read(&st);
	CHECK(st.em_stored == 1 && st.em_mode == DMR_EMERGENCY_MODE_CALL && st.em_hot_mic_s == 2);

	// The key: the hot mic keys the channel's talk target (TG 201) with the emergency option, impolite
	unsigned first = sim.air_count;
	int tx = hw.tx_calls;
	CHECK(dmr_emergency_start());
	run(50);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_HOT_MIC && sm.em_keyed && sm.view.state == RS_TX && hw.tx_calls == tx + 1);
	run(600);
	air_lc(first, &l);
	CHECK(sim.air[first].kind == DMR_BURST_VOICE_LC_HEADER && (l.svc & DMR_SVC_EMERGENCY) && l.dst == 201 &&
	      l.flco == DMR_FLCO_GROUP_VOICE);
	run(1600);	// the 2 s run out: released
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_ACTIVE && !sm.em_keyed);
	run(1000);
	CHECK(sm.view.state == RS_IDLE && sim.air[sim.air_count - 1].kind == DMR_BURST_TERMINATOR_LC);

	// Still in emergency: the user's own call carries the option
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK((l.svc & DMR_SVC_EMERGENCY) && l.dst == 201);
	// Canceled: the next call doesn't
	dmr_emergency_cancel();
	run(20);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_IDLE);
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(!(l.svc & DMR_SVC_EMERGENCY) && l.dst == 201);

	// PTT pressed during the hot mic takes the call over: it stays keyed past the hot mic, ends on release
	es.hot_mic_s = 1;
	es.dst = 9;		// a set emergency target
	dmr_emergency_set_settings(&es);
	run(20);
	first = sim.air_count;
	CHECK(dmr_emergency_start());
	run(300);
	CHECK(sm.view.state == RS_TX && sm.em_keyed);
	req(RREQ_PTT, 1);
	CHECK(!sm.em_keyed);
	run(1500);
	CHECK(sm.view.state == RS_TX && dmr_emergency_state() == DMR_EMERGENCY_ACTIVE);
	air_lc(first, &l);
	CHECK((l.svc & DMR_SVC_EMERGENCY) && l.dst == 9);
	req(RREQ_PTT, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE);
	dmr_emergency_cancel();
	run(20);

	// Heard: an emergency call on a group we listen to raises the alert and its tone once
	dmr_emergency_stats_t s0, s1;
	dmr_emergency_get_stats(&s0);
	hear_emergency_call(201, 9991234);
	dmr_emergency_alert_t a;
	CHECK(dmr_emergency_alert(&a) && a.kind == DMR_EMERGENCY_ALERT_CALL && a.src == 9991234 && a.dst == 201 && a.group);
	run(500);
	hear_emergency_call(201, 9991234);	// the next over: the same alert
	dmr_emergency_get_stats(&s1);
	CHECK(s1.alerts == s0.alerts + 1 && s1.alerts_call == s0.alerts_call + 1 && s1.tones == s0.tones + 1);
	n_ev = 0;
	ui_svc_poll();		// the UI's pass: the alert as UI_EV_EMERGENCY, once
	ui_svc_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_EMERGENCY && last_ev.id == 9991234 && !last_ev.ok &&
	      strcmp(last_ev.name, "Joe") == 0);
	dmr_emergency_dismiss();
	run(20);
	CHECK(!dmr_emergency_alert(&a));

	// A message heard: one UI_EV_MESSAGE. Reading and deleting it change the inbox, not the last event: nothing again
	{
		static dmrBurst_t b[DMR_DATA_MAX_BURSTS];
		int nb = dmrDataBuildIPText(false, 9996543, 9991234, "hello", DMR_TYPE_RATE_1_2, 0, b, DMR_DATA_MAX_BURSTS);
		CHECK(nb > 0);
		int before = sms_inbox_count();
		for (int i = 0; i < nb; i++) {
			now += 60;
			dmrDataServiceRxBurst(&b[i], now);
		}
		run(20);
		CHECK(sms_inbox_count() == before + 1);
		n_ev = 0;
		ui_svc_poll();
		CHECK(n_ev == 1 && last_ev.kind == UI_EV_MESSAGE && last_ev.id == 9991234);
		sms_inbox_mark_read(0);
		ui_svc_poll();
		CHECK(sms_inbox_delete(0));
		ui_svc_poll();
		CHECK(n_ev == 1);
	}

	// The UI's emergency key (ui_svc_emergency, core/ui/ui_svc_emergency.c)
	CHECK(ui_svc_feat_ext() & UI_FEAT_EMERGENCY);
	CHECK(ui_svc_emergency(true));
	run(50);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_HOT_MIC && sm.view.state == RS_TX);
	CHECK(ui_svc_emergency(true));		// already on
	CHECK(ui_svc_emergency(false));
	run(20);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_IDLE && !sm.em_keyed);
	run(1500);
	CHECK(sm.view.state == RS_IDLE);
}

// ---- The hotspot's CW ID (SEND_CWID) on a board with tx_tone_key: FM carrier on the hotspot TX frequency, the tone
// keyed in Morse at 20 wpm after a 150 ms lead, then back to DMR on the RX frequency with the keying armed ----

static void hs_configure(void)
{
	uint8_t fr[13] = { 0 };
	uint32_t hz = 453625000;
	for (int i = 0; i < 4; i++) {
		fr[1 + i] = (uint8_t)(hz >> (8 * i));
		fr[5 + i] = (uint8_t)(hz >> (8 * i));
	}
	fr[9] = 255;
	mm_cmd(MMDVM_SET_FREQ, fr, 10);
	uint8_t cfg[21] = { MMDVM_CONFIG_SIMPLEX, MMDVM_MODE_BIT_DMR, 10, 0, 128, 128, 1 };
	mm_cmd(MMDVM_SET_CONFIG, cfg, sizeof(cfg));
	uint8_t m = MMDVM_MODE_IDLE;
	mm_cmd(MMDVM_SET_MODE, &m, 1);
}

static void test_hotspot_cwid(void)
{
	hs_session_start();
	radio_preempts = true;
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	hs_configure();
	CHECK(mm_naks == 0 && hotspot_running() && sm.dmr_keyed && hw_mode == RADIO_MODE_DMR);
	memset(&tone, 0, sizeof(tone));
	radio_hs_cw_stats_t cs0, cs;
	radio_hs_cw_get_stats(&cs0);
	int modes = hw.set_mode_calls;
	uint32_t t0 = now;
	// "AB": A .- (5 units), the character gap (3), B -... (9): 17 units, 1020 ms; key down 10 units, 600 ms
	mm_cmd(MMDVM_SEND_CWID, (const uint8_t *)"AB", 2);
	CHECK(mm_last[2] == MMDVM_ACK && mm_naks == 0);
	CHECK(hw_mode == RADIO_MODE_FM && hw.set_mode_calls == modes + 1 && hw.tx_hz == 453625000 && tone.hz == 800);
	CHECK(!sm.dmr_keyed && sm.cw_phase != 0 && hw.led[LED_RED]);
	mm_cmd(MMDVM_GET_STATUS, NULL, 0);
	CHECK(mm_last[2] == MMDVM_GET_STATUS && (mm_last[5] & MMDVM_STATUS_TX));	// the TX flag while it keys
	mm_cmd(MMDVM_SEND_CWID, (const uint8_t *)"AB", 2);
	CHECK(mm_last[2] == MMDVM_NAK);		// not idle meanwhile
	hs_run(1500);
	CHECK(tone.changes == 12);		// six elements
	CHECK(tone.first_down_ms >= t0 + 150 && tone.first_down_ms <= t0 + 160);
	CHECK(tone.last_up_ms - tone.first_down_ms >= 1010 && tone.last_up_ms - tone.first_down_ms <= 1030);
	CHECK(tone.down_ms >= 590 && tone.down_ms <= 610);
	radio_hs_cw_get_stats(&cs);
	CHECK(cs.sent == cs0.sent + 1 && cs.wpm == 20 && !cs.active && cs.last_ms >= 1300 && cs.last_ms <= 1340);
	// Back: DMR mode (the modem set up again), the RX frequency, the keying armed, the TX flag clear
	CHECK(hw_mode == RADIO_MODE_DMR && hw.set_mode_calls == modes + 2 && hw.rx_hz == 453625000 && sm.dmr_keyed);
	CHECK(sm.cw_phase == 0 && tone.hz == 0 && !hw.led[LED_RED]);
	mm_cmd(MMDVM_GET_STATUS, NULL, 0);
	CHECK(!(mm_last[5] & MMDVM_STATUS_TX));

	// Hotspot mode left in the middle of an ID: cut, counted as not sent
	mm_cmd(MMDVM_SEND_CWID, (const uint8_t *)"TEST", 4);
	hs_run(300);
	CHECK(sm.cw_phase != 0 && tone.hz == 800);
	mmdvm_end();
	hs_run(100);
	radio_hs_cw_get_stats(&cs);
	CHECK(cs.refused == cs0.refused + 1 && !cs.active && sm.cw_phase == 0 && tone.hz == 0 && !sm.hotspot);
	CHECK(hw_mode == RADIO_MODE_DMR && sm.view.dmr.active);
}

// The UI's on-the-fly DMR changes (temporary timeslot, color code and mode), hear all latched, the one-shot talk
// target of a focused row and the reply window setting (docs/spec/ui.md 4)
static void test_ui_requests(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;
	dmr_lc_t l;

	// Timeslot 2 for now; asking for the channel's slot again clears it
	dmr_start(true);
	req(RREQ_DMR_TS, 2);
	run(20);
	CHECK((sm.view.temp & RADIO_TEMP_TS) && sm.view.ch.timeslot == 2 && d->ts == 2);
	req(RREQ_DMR_TS, 1);
	run(20);
	CHECK(!sm.view.temporary && sm.view.ch.timeslot == 1 && d->ts == 1);
	req(RREQ_DMR_TS, 3);
	CHECK(!sm.view.temporary);
	// Color code 7 for now: the old color code isn't heard any more, the new one is
	req(RREQ_DMR_CC, 7);
	run(20);
	CHECK((sm.view.temp & RADIO_TEMP_CC) && d->cc == 7 && sm.view.ch.color_code == 7);
	hear_call(true, 201, 9991234, 2, 1, false);
	CHECK(sm.view.state == RS_IDLE);
	run(500);
	hear_call(true, 201, 9991234, 7, 1, true);
	CHECK(d->last_ms != 0 && d->last.src == 9991234);
	// Undo (BACK on Home): the channel's color code and timeslot again
	req(RREQ_DMR_TS, 2);
	req(RREQ_CLEAR_TEMP, 0);
	run(20);
	CHECK(!sm.view.temporary && d->cc == 2 && d->ts == 1 && sm.view.ch.color_code == 2 && sm.view.ch.timeslot == 1);
	// FM for now stops the call stack (color code refused on FM); undo starts it again
	req(RREQ_MODE, RADIO_MODE_FM);
	run(20);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && (sm.view.temp & RADIO_TEMP_MODE) && !d->active && !sim.started);
	req(RREQ_DMR_CC, 3);
	CHECK(sm.view.ch.color_code == 2 && !(sm.view.temp & RADIO_TEMP_CC));
	req(RREQ_CLEAR_TEMP, 0);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && d->active && sim.started && !sm.view.temporary);
	// An FM channel as DMR for now; a channel change drops it
	req(RREQ_ZONE_POS, 0);
	run(20);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && d->active && (sm.view.temp & RADIO_TEMP_MODE) && d->ts == 1);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(!sm.view.temporary && d->active && d->cc == 2);
	// Nothing changes while transmitting
	req(RREQ_PTT, 1);
	run(100);
	req(RREQ_DMR_CC, 5);
	req(RREQ_MODE, RADIO_MODE_FM);
	CHECK(sm.view.ch.color_code == 2 && sm.view.ch.mode == RADIO_MODE_DMR);
	req(RREQ_PTT, 0);
	run(1000);

	// Hear all latched: other talkgroups play until it is turned off, or the channel changes
	req(RREQ_DMR_HEAR_ALL, 1);
	CHECK(d->hear_all && d->hear_all_latch);
	hear_call(true, 3100, 9997777, 2, 1, false);
	CHECK(d->state == RDMR_RX);
	run(500);
	req(RREQ_DMR_HEAR_ALL, 0);
	CHECK(!d->hear_all && !d->hear_all_latch);
	req(RREQ_REPLY_CANCEL, 0);		// TG 3100's reply window would let it through
	hear_call(true, 3100, 9997777, 2, 1, false);
	CHECK(d->state == RDMR_RX_OTHER);
	run(500);
	req(RREQ_DMR_HEAR_ALL, 1);
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 0);
	CHECK(!d->hear_all_latch);		// the monitor key's press turned it off again
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 0);
	CHECK(d->hear_all && d->hear_all_latch);	// and on
	req(RREQ_CH_STEP, 1);
	req(RREQ_CH_STEP, -1);
	run(50);
	CHECK(!d->hear_all_latch && !d->hear_all);

	// One-shot: PTT with Joe focused calls Joe once and keeps him for the reply window, then TG 201 again
	req(RREQ_DMR_FOCUS, (int32_t)(9991234u | (1u << 24)));
	CHECK(d->focus_id == 9991234 && d->focus_private);
	unsigned first = sim.air_count;
	req(RREQ_PTT, 1);
	run(600);
	CHECK(d->one_shot && d->target_id == 9991234 && d->target_private);
	req(RREQ_PTT, 0);
	run(1000);
	air_lc(first, &l);
	CHECK(l.flco == DMR_FLCO_UNIT_VOICE && l.dst == 9991234);
	CHECK(d->one_shot && d->reply_active && d->reply_s >= 8 && d->target_id == 9991234);
	req(RREQ_DMR_FOCUS, 0);		// back on Home: PTT inside the window still answers Joe
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(l.flco == DMR_FLCO_UNIT_VOICE && l.dst == 9991234 && d->one_shot);
	run(8000);
	CHECK(d->one_shot && d->reply_active && d->reply_s <= 2);
	run(1500);
	CHECK(!d->one_shot && !d->reply_active && d->target_id == 201 && !d->target_private);
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(l.flco == DMR_FLCO_GROUP_VOICE && l.dst == 201);
	// BACK on Home (reply cancel) ends it at once
	req(RREQ_DMR_FOCUS, 9);
	tx_once();
	CHECK(d->one_shot && d->target_id == 9 && !d->target_private);
	req(RREQ_REPLY_CANCEL, 0);
	run(20);
	CHECK(!d->one_shot && d->target_id == 201);
	// The talk target itself focused: an ordinary transmission
	req(RREQ_DMR_FOCUS, 201);
	tx_once();
	CHECK(!d->one_shot);
	req(RREQ_DMR_FOCUS, 0);
	CHECK(d->focus_id == 0);

	// The reply window setting replaces the codeplug's hang time (and the stack's 10 s)
	radio_settings_t rs = sm.set;
	rs.reply_s = 3;
	radio_sm_set_settings(&sm, &rs, now);
	hear_call(true, 9, 9995555, 2, 1, true);
	CHECK(d->reply_active && d->reply_s == 3);
	run(3100);
	CHECK(!d->reply_active);
	rs.reply_s = 0;			// off
	radio_sm_set_settings(&sm, &rs, now);
	hear_call(true, 9, 9995555, 2, 1, true);
	CHECK(!d->reply_active);
	settings_t st;
	settings_read(&st);
	CHECK(st.rs_reply_s == 0 && st.rs_stored == 1);
	rs.reply_s = RADIO_REPLY_CODEPLUG;
	radio_sm_set_settings(&sm, &rs, now);
	hear_call(true, 9, 9995555, 2, 1, true);
	CHECK(d->reply_active && d->reply_s == 10);
	run(11000);
}

// ---- The UI's service glue (core/ui/ui_svc_radio.c, ui_svc_storage.c, ui_svc_data.c) on the radio core: the
// requests go straight into the state machine here instead of through radio_task.c's queue ----

uint32_t hal_millis(void) { return now; }
void hal_delay_ms(uint32_t ms) { now += ms; }	// the PA sweep op's wait
void radio_get_view(radio_view_t *v) { *v = sm.view; }
uint32_t radio_view_serial(void) { return sm.serial; }
void radio_get_settings(radio_settings_t *s) { *s = sm.set; }
void radio_set_settings(const radio_settings_t *s) { radio_sm_set_settings(&sm, s, now); }
void radio_req_monitor(bool down) { req(RREQ_MONITOR, down); }
void radio_req_reload(void) { req(RREQ_RELOAD, 0); }
void radio_req_volume(uint8_t step) { req(RREQ_VOLUME, step); }
void radio_req_volume_override(int8_t step) { req(RREQ_VOLUME_OVERRIDE, step); }
void radio_req_volume_hold(bool hold) { req(RREQ_VOLUME_HOLD, hold); }
void radio_req_clear_temporary(void) { req(RREQ_CLEAR_TEMP, 0); }
void radio_req_retune(void) { req(RREQ_RETUNE, 0); }
#if CONFIG_NXDN
// As radio_task.c does it: the settings to the module, the request to the state machine
void radio_req_nxdn_settings(const radio_nxdn_settings_t *s)
{
	radio_nxdn_set_settings(s);
	req(RREQ_NXDN_SET, 0);
}
#endif
// Not under test here: debuglink_view_radio.c, linked for the volume operation, refers to them
void radio_req_scan_dual(uint16_t pos) { (void)pos; }
void radio_req_zone_pos(uint16_t pos) { req(RREQ_ZONE_POS, pos); }
void radio_req_zone(uint16_t index) { req(RREQ_ZONE, index); }
void radio_req_scan_vfo(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void radio_req_tone_burst(bool on, uint32_t max_ms) { (void)on; (void)max_ms; }
void radio_req_dtmf(char digit, bool down) { req(RREQ_DTMF, (uint8_t)digit | (down ? 0x100 : 0)); }
bool radio_dtmf_ready(void) { return sm.view.dtmf_ready; }
radio_state_t radio_get_state(void) { return (radio_state_t)sm.view.state; }
input_inject_t input_inject_ptt(bool down, uint32_t max_ms) { (void)down; (void)max_ms; return INPUT_INJ_OK; }
void radio_req_scan(uint8_t cmd) { req(RREQ_SCAN, cmd); }
void radio_req_vfo_step_size(uint32_t hz) { req(RREQ_VFO_STEP_SIZE, (int32_t)hz); }
void radio_req_dual(const radio_dual_ref_t ref[2], uint8_t sel)
{
	radio_req_t r = { RREQ_DUAL, 0, 0 };
	if (ref != NULL) {
		r.arg = (int32_t)radio_dual_arg(&ref[0], sel == 0);
		r.arg2 = radio_dual_arg(&ref[1], sel != 0);
	}
	radio_sm_request(&sm, &r, now);
}
bool radio_get_vfo(uint8_t index, channel_t *out)
{
	if (index == sm.vfo_idx) {
		*out = sm.vfo_ch;
		return sm.vfo_seeded;
	}
	*out = sm.vfo_other.ch;
	return sm.vfo_other.seeded;
}
bool radio_get_dual(radio_dual_ref_t ref[2], uint8_t *sel, uint8_t *at)
{
	ref[0] = sm.dual[0];
	ref[1] = sm.dual[1];
	*sel = sm.dual_sel;
	*at = sm.dual_at;
	return sm.scan.kind == SCAN_DUAL;
}
void radio_req_roam(uint8_t cmd) { req(RREQ_ROAM, cmd); }
void radio_req_beep(uint8_t b) { req(RREQ_BEEP, b); }
void radio_req_beep_after(uint8_t b, uint32_t delay_ms)
{
	radio_req_t r = { RREQ_BEEP, b, delay_ms };
	radio_sm_request(&sm, &r, now);
}
void radio_req_vibrate(uint8_t v) { req(RREQ_VIBRATE, v); }
void radio_req_dmr_target(uint32_t id, bool p) { req(RREQ_DMR_TARGET, (int32_t)((id & 0xFFFFFFu) | (p ? 1u << 24 : 0))); }
void radio_req_dmr_target_clear(void) { req(RREQ_DMR_TARGET_CLEAR, 0); }
void radio_req_dmr_timeslot(uint8_t ts) { req(RREQ_DMR_TS, ts); }
void radio_req_dmr_color_code(uint8_t cc) { req(RREQ_DMR_CC, cc); }
void radio_req_vfo_sfr(uint8_t a) { req(RREQ_VFO_SFR, a); }
void radio_req_mode(uint8_t mode) { req(RREQ_MODE, mode); }
#if CONFIG_P25
// As radio_task.c: the settings copied in, then the request
void radio_req_p25_settings(const radio_p25_settings_t *s) { radio_p25_set_settings(s); req(RREQ_P25_SET, 0); }
#endif
void radio_req_vfo_copy(void) { req(RREQ_VFO_COPY, 0); }
radio_vfo_copy_t radio_req_vfo_copy_checked(void)
{
	radio_vfo_copy_t r = radio_vfo_copy_check(&sm.view.ch, sm.view.vfo);
	if (r == VFO_COPY_OK) {
		radio_req_vfo_copy();
	}
	return r;
}
void radio_req_dmr_hear_all(bool on) { req(RREQ_DMR_HEAR_ALL, on); }
static int focus_posts;
void radio_req_dmr_focus(uint32_t id, bool p)
{
	focus_posts++;
	req(RREQ_DMR_FOCUS, (int32_t)((id & 0xFFFFFFu) | (p ? 1u << 24 : 0)));
}
void radio_req_dmr_privacy(uint8_t type, uint16_t key) { req(RREQ_DMR_PRIVACY, (int32_t)(((uint32_t)type << 16) | key)); }
static uint16_t input_long, input_repeat;
void input_set_timing(uint16_t l, uint16_t r) { input_long = l; input_repeat = r; }
static bool ptt_blocked;
void input_set_ptt_block(bool b) { ptt_blocked = b; }
static bool dtmf_blocked;
void input_set_dtmf_block(bool b) { dtmf_blocked = b; }
static keymap_entry_t got_keymap[4];
static int keymap_sets;
void input_set_keymap(const keymap_entry_t *map) { memcpy(got_keymap, map, sizeof(got_keymap)); keymap_sets++; }
void hostlink_set_hotspot_allowed(bool on) { (void)on; }
void hostlink_settings_changed(void) { }
static int32_t ram[UI_SET_COUNT];
int32_t ui_svc_ram_get(uint8_t id) { return id < UI_SET_COUNT ? ram[id] : 0; }
bool ui_svc_ram_set(uint8_t id, int32_t v) { if (id >= UI_SET_COUNT) return false; ram[id] = v; return true; }
static ui_event_t last_ev;
static int n_ev;
bool ui_notify(const ui_event_t *ev) { last_ev = *ev; n_ev++; return true; }
// Zone sharing's events are formatted by ui_svc_zone.c, not built here: passed on as they are
bool ui_svc_zone_event(uint8_t kind, uint32_t id, ui_event_t *ev) { (void)kind; ev->id = id; return true; }
static int cp_writes;
static bool cp_write_fails;
// Another firmware's codeplug (codeplug.h): the UI's writes ask first (ui_svc_storage.c)
static bool cp_foreign, cp_accepted;
static int cp_new, cp_reloads;
bool codeplug_foreign(void) { return cp_foreign && !cp_accepted; }
bool codeplug_start_new(void) { cp_new++; cp_foreign = false; return true; }
void codeplug_accept(bool on) { cp_accepted = on; }
codeplug_source_t codeplug_reload(void) { cp_reloads++; cp_serial++; return source; }
// The contact editor's write (ui_svc_storage.c); the radio core's tests don't add contacts
bool codeplug_set_contact(uint16_t index, const contact_t *c) { (void)index; (void)c; return false; }
bool codeplug_set_channel(uint16_t n, const channel_t *ch)
{
	if (cp_write_fails || n < 1 || n > NCH) {
		return false;
	}
	chans[n] = *ch;
	used[n] = true;
	cp_writes++;
	cp_serial++;
	return true;
}

bool codeplug_delete_channel(uint16_t n)
{
	if (n < 1 || n > NCH) {
		return false;
	}
	used[n] = false;
	cp_serial++;
	return true;
}

// Zone writes (the zone editor's job, zone_edit.c on these fakes)
static int zone_writes;
static uint16_t zone_written;
bool codeplug_zones_stored(void) { return nzones > 0; }
uint16_t codeplug_zone_capacity(void) { return CODEPLUG_ZONE_MAX; }
uint16_t codeplug_zone_written(void) { return zone_written; }
uint16_t codeplug_zone_add_index(void) { return nzones; }	// these fakes append
bool codeplug_set_zone(uint16_t i, const zone_t *z)
{
	if (i > nzones || i >= 3) {
		return false;
	}
	zones[i] = *z;
	if (i == nzones) {
		nzones++;
	}
	zone_written = i;
	zone_writes++;
	cp_serial++;
	return true;
}
bool codeplug_delete_zone(uint16_t i)
{
	if (i >= nzones) {
		return false;
	}
	memmove(&zones[i], &zones[i + 1u], (size_t)(nzones - i - 1u) * sizeof(zones[0]));
	nzones--;
	zone_writes++;
	cp_serial++;
	return true;
}

static int job_runs;
static void test_job(void *ctx) { (void)ctx; job_runs++; }

#if CONFIG_DUAL_WATCH
// Dual watch through the UI's service (ui_svc_dual on radio_req_dual): any zone or the VFO; what can't work is refused
// at once, the rest goes to the radio core (radioFeaturesTest covers the watch itself)
static void test_ui_dual(void)
{
	setup_codeplug();
	start();
	req(RREQ_ZONE_POS, 0);
	const ui_dual_ref_t a = { false, 0, 0 }, tones = { false, 2, 1 }, vfo = { true, 0, 0 };
	const ui_dual_ref_t same[2] = { a, a }, empty[2] = { a, { false, 0, 2 } }, past[2] = { a, { false, 1, 1 } };
	const ui_dual_ref_t nozone[2] = { a, { false, 5, 0 } };
	CHECK(!ui_svc_dual(same, 0) && !ui_svc_dual(empty, 0) && !ui_svc_dual(past, 1) && !ui_svc_dual(nozone, 0));
	CHECK(sm.view.scan == SCAN_OFF);
	// Another zone's channel as B, selected: the watch runs, then stops on B
	const ui_dual_ref_t pair[2] = { a, tones };
	CHECK(ui_svc_dual(pair, 1) && sm.view.scan == SCAN_DUAL && sm.dual_sel == 1);
	CHECK(ui_svc_dual(pair, 1) && sm.view.scan == SCAN_DUAL);	// again: nothing changes
	{
		// The debug link's features section: the pair, the selected one, where the receiver is; it still fits a reply
		static char buf[DL_REPLY_MAX - 6 - 16];
		dl_text_t t;
		dl_text_init(&t, buf, sizeof buf);
		debuglink_view_radio(&sm.view, &t);
		char at[24];
		snprintf(at, sizeof at, "dual.sel=b\ndual.at=%c\n", 'a' + sm.dual_at);
		CHECK(!t.full && strstr(buf, "dual.a=0:0\ndual.b=2:1\n") != NULL && strstr(buf, at) != NULL);
	}
	CHECK(ui_svc_dual(NULL, 0) && sm.view.scan == SCAN_OFF && sm.view.zone_index == 2 && sm.view.zone_pos == 1);
	// The VFO's settings while on a channel, and the VFO as A
	req(RREQ_VFO, 1);
	req(RREQ_VFO, 0);
	channel_t v;
	CHECK(!sm.view.vfo && ui_svc_vfo_get(sm.vfo_idx, &v) && v.rx_hz == sm.vfo_ch.rx_hz && v.rx_hz != 0);
	const ui_dual_ref_t withvfo[2] = { vfo, a };
	CHECK(ui_svc_dual(withvfo, 0) && sm.view.scan == SCAN_DUAL);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.scan == SCAN_OFF && sm.view.vfo);
	// Saved while the watch is on B: powers up on the selected A all the same
	req(RREQ_VFO, 0);
	req(RREQ_ZONE_POS, 0);
	CHECK(ui_svc_dual(pair, 0) && sm.dual_sel == 0);
	for (int i = 0; i < 1000 && sm.dual_at != 1; i++) {
		poll_for(RADIO_POLL_MS);
	}
	CHECK(sm.view.scan == SCAN_DUAL && sm.dual_at == 1 && sm.view.zone_index == 2);
	CHECK(ui_svc_dual(pair, 0));		// a request saves the position
	boot();
	CHECK(!sm.view.vfo && sm.view.zone_index == 0 && sm.view.zone_pos == 0);
}

// Two VFOs: dual watch gives each channel its own; each keeps its frequency, both come back at power on, and Copy to
// VFO goes to the one the other channel isn't on
static void test_dual_two_vfos(void)
{
	setup_codeplug();
	start();
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 146550000);
	req(RREQ_VFO, 0);
	const ui_dual_ref_t v0 = { true, 0, 0 }, v1 = { true, 0, 1 }, a = { false, 0, 0 };
	const ui_dual_ref_t vfos[2] = { v0, v1 }, same[2] = { v1, v1 };
	CHECK(!ui_svc_dual(same, 0));
	CHECK(ui_svc_dual(vfos, 1) && sm.view.scan == SCAN_DUAL);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.vfo && sm.view.vfo_index == 1);	// stopped on B: the second VFO
	req(RREQ_VFO_SET, 446006250);
	channel_t c0, c1;
	CHECK(ui_svc_vfo_get(0, &c0) && c0.rx_hz == 146550000 && ui_svc_vfo_get(1, &c1) && c1.rx_hz == 446006250);
	// The watch hops between them: each hop tunes that VFO's own frequency
	CHECK(ui_svc_dual(vfos, 0));
	bool heard0 = false, heard1 = false;
	for (int i = 0; i < 400 && !(heard0 && heard1); i++) {
		poll_for(RADIO_POLL_MS);
		if (sm.view.vfo && sm.vfo_idx == 0 && hw.rx_hz == 146550000)
			heard0 = true;
		if (sm.view.vfo && sm.vfo_idx == 1 && hw.rx_hz == 446006250)
			heard1 = true;
	}
	CHECK(heard0 && heard1);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.vfo && sm.view.vfo_index == 0 && sm.view.ch.rx_hz == 146550000);
	// Both saved, and the one in use
	CHECK(ui_svc_dual(vfos, 1));
	CHECK(ui_svc_dual(NULL, 0) && sm.view.vfo_index == 1);
	boot();
	CHECK(sm.view.vfo && sm.view.vfo_index == 1 && sm.view.ch.rx_hz == 446006250);
	CHECK(ui_svc_vfo_get(0, &c0) && c0.rx_hz == 146550000);
	// Copy to VFO with B on the first VFO: A's channel goes to the second, the first is untouched
	req(RREQ_VFO, 0);
	req(RREQ_ZONE_POS, 0);
	const ui_dual_ref_t chan_vfo[2] = { a, v0 };
	CHECK(ui_svc_dual(chan_vfo, 0) && sm.dual_sel == 0);
	uint32_t ch_hz = sm.view.ch.rx_hz;
	req(RREQ_VFO_COPY, 0);
	CHECK(sm.view.vfo && sm.view.vfo_index == 1 && sm.view.ch.rx_hz == ch_hz);
	CHECK(ui_svc_vfo_get(0, &c0) && c0.rx_hz == 146550000);
}
#endif

// The board GPS for everything that reports a position (#466): the Position screen, LRRP's answer and DMR GPS Info
// read the position service, and the stored position settings apply. radioCoreTest builds it without APRS,
// radioCoreTestAprs with
static void test_position(void)
{
	static const char gga[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
	static const char rmc[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
	lrrpPosition_t lp;
	ui_position_t up;
	int32_t lat, lon;
	uint32_t err;
	bool send = true;

	CHECK(CONFIG_POSITION);
	gps_here = false;
	dmr_start(true);
	CHECK(!(ui_svc_feat_radio() & UI_FEAT_GPS) && !ui_svc_position(&up));
	CHECK(!dmrDataServiceOwnPosition(&lp));
	gps_here = true;
	CHECK(ui_svc_feat_radio() & UI_FEAT_GPS);
	CHECK(!ui_svc_position(&up));		// nothing heard yet
	position_nmea(gga, strlen(gga), true, now);
	position_nmea(rmc, strlen(rmc), true, now);
	CHECK(ui_svc_position(&up) && up.fix && up.sats == 8 && up.lat_e7 == 481173000 && up.lon_e7 == 115166666);
	CHECK(up.alt_ok && up.alt_m == 545 && up.course == 84);
	run(10);
	CHECK(dmrDataServiceOwnPosition(&lp) && lp.location.fixValid && lp.location.north && lp.location.east);
	CHECK(lp.location.latDegrees == 48 && lp.location.latMinutes10000 == 70380 && lp.location.lonDegrees == 11 &&
	      lp.location.lonMinutes10000 == 310000);
	CHECK(lp.location.speedKnots == 22 && lp.hasCourse && lp.courseDegrees == 84 && lp.utcYear == 2094 &&
	      lp.utcMonth == 3 && lp.utcDay == 23 && lp.location.utcHours == 12 && lp.location.utcMinutes == 35);
	dmrDataSettings_t ds;
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.hasGps);			// LRRP takes triggered requests: a GPS is there

	// DMR GPS Info: off until Settings > Location > Send in DMR calls says so, then our position goes out in the
	// call. The setting is the position service's own: saved without APRS, back after a restart
	CHECK(position_now(&lat, &lon, &err, &send, now) && !send && lat == 481173000 && err == 20);
	CHECK(ui_svc_setting_get(UI_SET_POS_DMR_CALLS) == 0 && ui_svc_setting_set(UI_SET_POS_DMR_CALLS, 1));
	position_cfg_t pc;
	position_get_cfg(&pc);
	pc.ambiguity = 2;			// whole minutes
	position_set_cfg(&pc, now);
	settings_t sr;
	settings_read(&sr);
	CHECK(sr.aprs_stored != 1 && (sr.aprs_misc & SETTINGS_APRS_MISC_POSITION));	// its own marker, not APRS's
	boot();
	CHECK(ui_svc_setting_get(UI_SET_POS_DMR_CALLS) == 1);
	position_nmea(rmc, strlen(rmc), true, now);
	CHECK(position_now(&lat, &lon, &err, &send, now) && send && err == 1852);
	CHECK(lat == 481250002);		// 48 07.038 -> the middle of its minute, 48 07.5
	position_get_cfg(&pc);
	pc.ambiguity = 0;
	position_set_cfg(&pc, now);
	req(RREQ_ZONE_POS, 3);			// the DMR channel again (dmr_start would start from new settings)
	run(50);
	position_nmea(rmc, strlen(rmc), true, now);
	dmr_call_stats_t cs0, cs1;
	dmr_call_get_stats(&cs0);
	req(RREQ_PTT, 1);
	run(360 * 4);
	req(RREQ_PTT, 0);
	run(1000);
	dmr_call_get_stats(&cs1);
	CHECK(cs1.tx_gps_lcs - cs0.tx_gps_lcs >= 1);
	// ... and with the setting off again, none
	CHECK(ui_svc_setting_set(UI_SET_POS_DMR_CALLS, 0));
	req(RREQ_PTT, 1);
	run(360 * 4);
	req(RREQ_PTT, 0);
	run(1000);
	dmr_call_get_stats(&cs0);
	CHECK(cs0.tx_gps_lcs == cs1.tx_gps_lcs);
	// The codeplug can ask for it per channel and timeslot (what a CPS sets): sent with the radio's setting off
	chans[4].alias_tx = CODEPLUG_ALIAS_TS2_GPS;	// a TS1 channel: not on this one
	reload_channel();
	CHECK(!sm.view.dmr.gps_tx_channel);
	chans[4].alias_tx = CODEPLUG_ALIAS_TS1_GPS;
	reload_channel();
	CHECK(sm.view.dmr.gps_tx_channel);
	position_nmea(rmc, strlen(rmc), true, now);
	req(RREQ_PTT, 1);
	run(360 * 4);
	req(RREQ_PTT, 0);
	run(1000);
	dmr_call_get_stats(&cs1);
	CHECK(cs1.tx_gps_lcs - cs0.tx_gps_lcs >= 1);
	chans[4].alias_tx = 0;
	reload_channel();
	CHECK(ui_svc_setting_set(UI_SET_POS_DMR_CALLS, 1));

	// Settings > Location > GPS off: the fix goes at once
	position_gps_enable(false);
	CHECK(!ui_svc_position(&up) && !dmrDataServiceOwnPosition(&lp));
	position_gps_enable(true);

	// A radio without a GPS: the fixed position typed in Settings > Location is what LRRP answers with and what
	// goes out in calls, after a restart too
	gps_here = false;
	CHECK(!dmrDataServiceOwnPosition(&lp));
	CHECK(ui_svc_setting_set(UI_SET_POS_FIXED_LAT, 337490000) && ui_svc_setting_set(UI_SET_POS_FIXED_LON, -843880000));
	CHECK(!ui_svc_setting_set(UI_SET_POS_FIXED_LAT, 900000001) && !ui_svc_setting_set(UI_SET_POS_SOURCE, 9));
	boot();
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(ui_svc_setting_get(UI_SET_POS_FIXED_LAT) == 337490000 && ui_svc_setting_get(UI_SET_POS_FIXED_LON) == -843880000);
	CHECK(dmrDataServiceOwnPosition(&lp) && lp.location.fixValid && lp.location.north && !lp.location.east);
	CHECK(lp.location.latDegrees == 33 && lp.location.latMinutes10000 == 449400 && lp.location.lonDegrees == 84 &&
	      lp.location.lonMinutes10000 == 232800 && lp.utcYear == 0 && !lp.hasCourse);
	CHECK(position_now(&lat, &lon, &err, &send, now) && send && lat == 337490000 && lon == -843880000 && err == 200);
	dmr_call_get_stats(&cs0);
	req(RREQ_PTT, 1);
	run(360 * 4);
	req(RREQ_PTT, 0);
	run(1000);
	dmr_call_get_stats(&cs1);
	CHECK(cs1.tx_gps_lcs - cs0.tx_gps_lcs >= 1);
	// The source set to GPS only: the fixed position doesn't count
	CHECK(ui_svc_setting_set(UI_SET_POS_SOURCE, POSITION_GPS) && ui_svc_setting_get(UI_SET_POS_SOURCE) == POSITION_GPS);
	CHECK(!dmrDataServiceOwnPosition(&lp) && !position_now(&lat, &lon, &err, &send, now));
	CHECK(ui_svc_setting_set(UI_SET_POS_SOURCE, POSITION_FIXED) && dmrDataServiceOwnPosition(&lp));
#if !CONFIG_POSITION_HOST
	// "Computer" left by a build that takes a computer's NMEA: here it counts as Auto, not as no position
	CHECK(ui_svc_setting_set(UI_SET_POS_SOURCE, POSITION_HOST) && dmrDataServiceOwnPosition(&lp));
	CHECK(ui_svc_setting_set(UI_SET_POS_SOURCE, POSITION_FIXED));
#endif
	// A record APRS stored before the position service had its own marker still counts
	settings_t *st = settings_begin();
	st->aprs_misc &= (uint8_t)~SETTINGS_APRS_MISC_POSITION;
	st->aprs_stored = 1;
	settings_end(now);
	boot();
	CHECK(ui_svc_setting_get(UI_SET_POS_FIXED_LAT) == 337490000 && ui_svc_setting_get(UI_SET_POS_DMR_CALLS) == 1);
	// ... and with neither, the defaults
	st = settings_begin();
	st->aprs_stored = 0;
	settings_end(now);
	boot();
	CHECK(ui_svc_setting_get(UI_SET_POS_FIXED_LAT) == 0 && ui_svc_setting_get(UI_SET_POS_DMR_CALLS) == 0 &&
	      ui_svc_setting_get(UI_SET_POS_SOURCE) == POSITION_AUTO);
	settings_init();
	boot();
}

static void test_ui_glue(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;
	settings_t *st;

	dmr_start(true);
	uint32_t f = ui_svc_feat_voice();
	CHECK((f & UI_FEAT_TIMESLOT) && (f & UI_FEAT_COLOR_CODE) && (f & UI_FEAT_HEAR_ALL) && (f & UI_FEAT_MODE) &&
	      (f & UI_FEAT_PRIVACY) && (f & UI_FEAT_MONITOR_LATCH) && (f & UI_FEAT_TALK_TARGET));
	CHECK(ui_svc_feat_storage() & UI_FEAT_FACTORY_RESET);
	// The focus goes to the radio only when it changes
	int posts = focus_posts;
	ui_svc_focus_target(9991234, true);
	ui_svc_focus_target(9991234, true);
	CHECK(focus_posts == posts + 1 && d->focus_id == 9991234 && d->focus_private);
	ui_svc_focus_target(0, false);
	CHECK(d->focus_id == 0);
	// Temporary channel changes and latches
	CHECK(ui_svc_dmr_timeslot(2) && !ui_svc_dmr_timeslot(3));
	CHECK(ui_svc_dmr_color_code(9) && !ui_svc_dmr_color_code(16));
	run(20);
	CHECK(d->ts == 2 && d->cc == 9);
	CHECK(ui_svc_hear_all(true) && d->hear_all_latch);
	CHECK(ui_svc_hear_all(false) && !d->hear_all_latch);
	CHECK(ui_svc_monitor(true) && !sm.view.monitor && !d->hear_all_latch);	// DMR: a press, toggled on release
	CHECK(ui_svc_monitor(false) && d->hear_all_latch);
	CHECK(ui_svc_monitor(true) && ui_svc_monitor(false) && !d->hear_all_latch);
	// Save to channel: nothing written by the call itself, the housekeeping job writes timeslot and color code
	int w = cp_writes;
	CHECK(ui_svc_save_channel());
	CHECK(cp_writes == w && storage_job_busy() && !ui_svc_save_channel());
	storage_job_poll();
	CHECK(cp_writes == w + 1 && chans[4].timeslot == 2 && chans[4].color_code == 9 && !storage_job_busy());
	run(100);			// the radio core re-reads the channel (codeplug serial)
	CHECK(!sm.view.temporary && d->ts == 2 && d->cc == 9);
	chans[4].timeslot = 1;
	chans[4].color_code = 2;
	reload_channel();
	// The channel editor's whole channel (ui_svc_write_channel): numbers out of range are refused, a write in
	// progress refuses the next, the channel in use drops its temporary changes. So does a write of another channel:
	// any codeplug write reloads the channel in use (ui.md 2.1)
	channel_t ed = chans[4];
	CHECK(!ui_svc_write_channel(0, &ed, 0) && !ui_svc_write_channel(CODEPLUG_CHANNELS_MAX + 1, &ed, 0));
	CHECK(ui_svc_dmr_color_code(9));
	run(20);
	CHECK(sm.view.temporary);
	ed.timeslot = 2;
	ed.color_code = 5;
	w = cp_writes;
	CHECK(ui_svc_write_channel(4, &ed, 0) && cp_writes == w && !ui_svc_write_channel(6, &ed, 0));
	storage_job_poll();
	CHECK(cp_writes == w + 1 && chans[4].timeslot == 2 && chans[4].color_code == 5);
	run(100);
	CHECK(!sm.view.temporary && d->ts == 2 && d->cc == 5);
	CHECK(ui_svc_dmr_color_code(9));
	run(20);
	ed = chans[6];
	ed.color_code = 7;
	CHECK(ui_svc_write_channel(6, &ed, 0));
	storage_job_poll();
	run(100);
	CHECK(chans[6].color_code == 7 && !sm.view.temporary && d->cc == 5);
	// A failed write: the event, and nothing reloaded
	CHECK(ui_svc_dmr_color_code(9));
	run(20);
	cp_write_fails = true;
	n_ev = 0;
	CHECK(ui_svc_write_channel(6, &ed, 0));
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && sm.view.temporary);
	cp_write_fails = false;
	req(RREQ_CLEAR_TEMP, 0);
	chans[4].timeslot = 1;
	chans[4].color_code = 2;
	reload_channel();
	// A failed write comes back as an event
	CHECK(ui_svc_dmr_color_code(4));
	cp_write_fails = true;
	n_ev = 0;
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(tr_en(last_ev.why), "not saved") != NULL);
	CHECK(sm.view.temporary);
	cp_write_fails = false;
	req(RREQ_CLEAR_TEMP, 0);
	// The permanent talk target: at once for now, then the channel's contact once the job ran
	CHECK(ui_svc_talk_to(9, false, UI_TALK_PERMANENT));
	run(20);
	CHECK(d->target_id == 9);
	storage_job_poll();
	CHECK(chans[4].contact == 3);
	run(100);
	CHECK(d->target_id == 9 && !d->target_manual);
	n_ev = 0;
	CHECK(ui_svc_talk_to(9999999, true, UI_TALK_PERMANENT));	// not a stored contact: stays for now
	storage_job_poll();
	run(20);
	CHECK(n_ev == 1 && strstr(tr_en(last_ev.why), "for now only") != NULL && d->target_manual && d->target_id == 9999999);
	chans[4].contact = 1;
	reload_channel();
	// Another firmware's codeplug: the write is held and the UI asked; nothing written until the answer
	cp_foreign = true;
	ed = chans[6];
	ed.color_code = 3;
	w = cp_writes;
	n_ev = 0;
	CHECK(ui_svc_write_channel(6, &ed, 0) && n_ev == 1 && last_ev.kind == UI_EV_CODEPLUG_FOREIGN);
	CHECK(!ui_svc_write_channel(6, &ed, 0) && !ui_svc_save_channel() && n_ev == 1);	// one question at a time
	storage_job_poll();
	CHECK(cp_writes == w && !storage_job_busy());
	// Cancel: nothing written, nothing cleared
	CHECK(ui_svc_codeplug_answer(UI_CP_CANCEL) && !ui_svc_codeplug_answer(UI_CP_CANCEL));
	storage_job_poll();
	CHECK(cp_writes == w && cp_new == 0 && !cp_accepted && chans[6].color_code != 3);
	// Use anyway: accepted and shown, the write made on the built-in set dropped, nothing cleared
	CHECK(ui_svc_write_channel(6, &ed, 0) && n_ev == 2);
	int reloads = cp_reloads;
	CHECK(ui_svc_codeplug_answer(UI_CP_USE) && storage_job_busy());
	storage_job_poll();
	CHECK(cp_writes == w && cp_new == 0 && cp_accepted && cp_reloads == reloads + 1 && !codeplug_foreign());
	CHECK(ui_svc_write_channel(6, &ed, 0) && n_ev == 2);	// ours now: written at once
	storage_job_poll();
	CHECK(cp_writes == w + 1 && chans[6].color_code == 3);
	// Start new: the old one cleared, then the write
	cp_accepted = false;
	ed.color_code = 4;
	CHECK(ui_svc_write_channel(6, &ed, 0) && n_ev == 3 && ui_svc_codeplug_answer(UI_CP_NEW));
	storage_job_poll();
	CHECK(cp_new == 1 && cp_writes == w + 2 && chans[6].color_code == 4 && !codeplug_foreign());
	// A keyed setting isn't a codeplug write that asks (docs/spec/codeplug-settings.md 5): with another firmware's
	// codeplug the area can't be written and holds no key, so the setting is saved as the radio's own, no question
	cp_foreign = true;
	kv_ars_present = false;
	kv_held = true;
	CHECK(ui_svc_setting_set(UI_SET_ARS_SERVER, 3100123) && n_ev == 3);
	storage_job_poll();
	run(100);
	CHECK(!kv_ars_present && cp_new == 1 && n_ev == 3);
	CHECK(ui_svc_setting_get(UI_SET_ARS_SERVER) == 3100123);
	{
		settings_t own;
		settings_read(&own);
		CHECK(own.ds_ars_server == 3100123);
	}
	cp_foreign = false;
	// A key that is present and can't be updated (a CPS session holds the flash) refuses the change: it would put
	// the old value back
	kv_ars_present = true;
	kv_ars = 3100123;
	CHECK(ui_svc_setting_set(UI_SET_ARS_SERVER, 3100124));
	storage_job_poll();
	run(100);
	CHECK(kv_ars == 3100123 && ui_svc_setting_get(UI_SET_ARS_SERVER) == 3100123 && last_ev.kind == UI_EV_SAVE_FAILED);
	kv_held = false;
	kv_ars_present = false;
	n_ev = 0;
	run(100);
	req(RREQ_CLEAR_TEMP, 0);
	reload_channel();
	// Mode
	CHECK(ui_svc_mode(RADIO_MODE_FM) && sm.view.ch.mode == RADIO_MODE_FM && !ui_svc_mode(7));
	req(RREQ_CLEAR_TEMP, 0);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR);
#if CONFIG_ZONE_EDIT
	{
		// The zone editor's queue (ui_svc_zone_edit, ui.md 2.2): nothing is written by the call; what is queued runs in
		// order in one job; a refusal drops what was queued after it; a full queue refuses
		zone_t keep[3];
		uint16_t nkeep = nzones;
		memcpy(keep, zones, sizeof keep);
		zone_op_t op, q[4];
		memset(q, 0, sizeof q);
		uint16_t num = sm.view.channel_number, pos = sm.view.zone_pos;
		CHECK(sm.view.zone_index == 0 && pos > 0);
		q[0].kind = ZONE_OP_MOVE;	// up
		q[0].pos = pos;
		q[0].dir = -1;
		q[1] = q[0];			// and back down
		q[1].pos = (uint16_t)(pos - 1u);
		q[1].dir = 1;
		q[2].kind = ZONE_OP_INSERT;	// refused: it is in the zone
		q[2].channel = num;
		q[3] = q[0];			// dropped
		int zw = zone_writes;
		n_ev = 0;
		for (int i = 0; i < 4; i++) {
			CHECK(ui_svc_zone_edit(&q[i]));
		}
		CHECK(!ui_svc_zone_edit(&q[0]) && zone_writes == zw && ui_svc_zone_pending());
		storage_job_poll();
		run(100);
		CHECK(zone_writes == zw + 2 && zones[0].channels[pos] == num && !ui_svc_zone_pending());
		CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(tr_en(last_ev.why), "Already") != NULL);
		uint32_t applied;
		uint8_t last;
		zone_edit_stats(&applied, &last);
		CHECK(applied >= 2 && last == ZONE_EDIT_IN_ZONE);
		// A move in the zone in use keeps the radio on its channel at its new position
		CHECK(ui_svc_zone_edit(&q[0]));
		storage_job_poll();
		run(100);
		CHECK(zones[0].channels[pos - 1u] == num);
		CHECK(sm.view.channel_number == num && sm.view.zone_pos == pos - 1u && sm.view.zone_index == 0);
		// Removing the channel in use: the radio goes to the one that took its place (the next usable one)
		memset(&op, 0, sizeof op);
		op.kind = ZONE_OP_REMOVE;
		op.pos = (uint16_t)(pos - 1u);
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(sm.view.zone_index == 0 && sm.view.channel_number != num && sm.view.zone_pos >= pos - 1u &&
		      zones[0].channels[sm.view.zone_pos] == sm.view.channel_number);
		memcpy(zones, keep, sizeof keep);
		cp_serial++;
		run(100);

		// Deleting a zone ahead of the zone in use: the radio follows its zone to the index it moved to, and so do dual
		// watch's channels and the roaming zones; a deleted roaming zone loses its flag
		req(RREQ_ZONE, 2);
		run(50);
		CHECK(sm.view.zone_index == 2 && sm.view.channel_number == 7);
		req(RREQ_ZONE_POS, 1);
		run(50);
#if CONFIG_SCAN
		radio_dual_ref_t dual_keep[2] = { sm.dual[0], sm.dual[1] };
		sm.dual[0] = (radio_dual_ref_t){ .vfo = false, .zone = 2, .pos = 0 };
		sm.dual[1] = (radio_dual_ref_t){ .vfo = false, .zone = 0, .pos = 1 };
#endif
#if CONFIG_DMR_ROAMING
		st = settings_begin();
		st->roam_zones[0] = 0x05;	// zones 0 and 2
		settings_end(0);
#endif
		memset(&op, 0, sizeof op);
		op.kind = ZONE_OP_DELETE;
		op.zone = 0;
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(nzones == 2 && sm.view.zone_index == 1 && sm.view.channel_number == 8 && sm.view.zone_pos == 1);
#if CONFIG_SCAN
		CHECK(sm.dual[0].zone == 1 && sm.dual[1].zone == 0);	// the deleted one's index: the zone that took it
		sm.dual[0] = dual_keep[0];
		sm.dual[1] = dual_keep[1];
#endif
#if CONFIG_DMR_ROAMING
		settings_t saved;
		settings_read(&saved);
		CHECK(sm.roam_zones[0] == 0x02 && saved.roam_zones[0] == 0x02);	// zone 2 is zone 1; zone 0's flag is gone
		st = settings_begin();
		st->roam_zones[0] = 0;
		settings_end(0);
#endif
		// Deleting the zone in use, the last one: the radio goes to the zone before it, on its first channel (it
		// doesn't hold channel 8)
		op.zone = 1;
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(nzones == 1 && sm.view.zone_index == 0 && sm.view.channel_number == 6 && sm.view.zone_pos == 0);
		// An outside writer's entries (an import), seen only when its write is done: zone_edit_busy holds the reload
		memcpy(zones, keep, sizeof keep);
		nzones = nkeep;
		zone_shift_busy(true);
		zone_shift_note(false, 0);	// as if zone 0 (Local) had come in ahead of Two and Tones
		cp_serial++;
		run(100);
		CHECK(sm.view.zone_index == 0 && sm.view.channel_number == 6);	// held
		zone_shift_busy(false);
		run(100);
		CHECK(sm.view.zone_index == 1 && sm.view.channel_number == 6);	// Two is zone 1
		CHECK(sm.view.zone_index == 1);
#if CONFIG_SCAN
		// A zone scan ends when the zone in use's list changes under it (its skips are by position); a rename doesn't
		req(RREQ_ZONE, 0);
		run(50);
		req(RREQ_SCAN, RSCAN_ZONE);
		run(200);
		CHECK(sm.scan.kind == SCAN_ZONE);
		memset(&op, 0, sizeof op);
		op.kind = ZONE_OP_RENAME;
		strcpy(op.name, "Renamed");
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(sm.scan.kind == SCAN_ZONE && strcmp(zones[0].name, "Renamed") == 0);
		op.kind = ZONE_OP_MOVE;
		op.pos = 0;
		op.dir = 1;
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(sm.scan.kind == SCAN_OFF);
#endif
		memcpy(zones, keep, sizeof keep);
		nzones = nkeep;
		cp_serial++;
		run(100);
		req(RREQ_ZONE, 0);
		run(50);
		req(RREQ_ZONE_POS, pos);
		run(50);
		CHECK(sm.view.zone_index == 0 && sm.view.channel_number == num);
	}
#endif

	// Privacy: no key stored, nothing to turn on; then the first valid slot is used
	CHECK(!ui_svc_privacy(true) && !d->privacy);
	st = settings_begin();
	st->privacy_type[1] = 1;	// Motorola basic privacy, key 7, slot 2 (none active)
	st->privacy_key[1] = 7;
	settings_end(now);
	CHECK(ui_svc_privacy(true) && d->privacy);
	CHECK(ui_svc_privacy(false) && !d->privacy);

	// Settings with a store field now: kept across "power cycles" (settings_t), the reply window in the radio core
	CHECK(ui_svc_setting_set(UI_SET_AUTO_LOCK_S, 60) && ui_svc_setting_get(UI_SET_AUTO_LOCK_S) == 60);
	CHECK(ui_svc_setting_set(UI_SET_MIC_GAIN, 8) && ui_svc_setting_get(UI_SET_MIC_GAIN) == 8);
	CHECK(ui_svc_setting_set(UI_SET_LONG_PRESS_MS, 900) && ui_svc_setting_get(UI_SET_LONG_PRESS_MS) == 900);
	CHECK(ui_svc_setting_set(UI_SET_GPS, 1) && ui_svc_setting_get(UI_SET_GPS) == 1);
	CHECK(ui_svc_setting_set(UI_SET_POS_SEND_S, 300) && ui_svc_setting_get(UI_SET_POS_SEND_S) == 300);
	settings_t sv;
	settings_read(&sv);
	CHECK(sv.auto_lock_s == 60 && sv.mic_gain == 8 && sv.long_press_ms == 900 && sv.gps == 1 && sv.pos_send_s == 300);
	// The reference trim (radio-features.md 6.1): stored rounded to 0.1 ppm, and applying it reaches the board
	CHECK(ui_svc_setting_set(UI_SET_REF_TRIM, -2140) && ui_svc_setting_get(UI_SET_REF_TRIM) == -2100);
	settings_read(&sv);
	CHECK(sv.ref_trim_ppb == -2100);
	ui_svc_apply(UI_SET_REF_TRIM, ui_svc_setting_get(UI_SET_REF_TRIM));
	CHECK(ref_trim_ppb == -2100);
	CHECK(ui_svc_setting_set(UI_SET_REF_TRIM, 30000) && ui_svc_setting_get(UI_SET_REF_TRIM) == 10000);
	CHECK(ui_svc_setting_set(UI_SET_REF_TRIM, -2100));
#if CONFIG_DUAL_WATCH
	// Dual watch's B, and whether the watch ran and which channel was selected (it comes back at power on)
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, 37) && ui_svc_setting_get(UI_SET_DUAL_B) == 37);
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, UI_DUAL_B_VFO | UI_DUAL_ON));
	CHECK(ui_svc_setting_get(UI_SET_DUAL_B) == (UI_DUAL_B_VFO | UI_DUAL_ON));
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, 1024 | UI_DUAL_ON | UI_DUAL_B_SEL));
	CHECK(ui_svc_setting_get(UI_SET_DUAL_B) == (1024 | UI_DUAL_ON | UI_DUAL_B_SEL));
	settings_read(&sv);
	CHECK(sv.dual_b == 1024 && sv.dual_state == (SETTINGS_DUAL_ON | SETTINGS_DUAL_B_SEL) && sv.mic_gain == 8);
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, UI_DUAL_B_VFO2) && ui_svc_setting_get(UI_SET_DUAL_B) == UI_DUAL_B_VFO2);
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, 0) && ui_svc_setting_get(UI_SET_DUAL_B) == 0);
#endif
	// Mic noise reduction: on by default, kept in accent bits 6-7, handed to the vocoder's encode options (tone frames
	// stay on)
	CHECK(ui_svc_setting_get(UI_SET_NOISE_REDUCTION) == 1 && (sv.accent & SETTINGS_AUDIO_NO_NOISE_REDUCTION) == 0);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT);
	CHECK(ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 0) && ui_svc_setting_get(UI_SET_NOISE_REDUCTION) == 0);
	ui_svc_apply(UI_SET_NOISE_REDUCTION, 0);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_TONE_FRAMES);
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == SETTINGS_AUDIO_NO_NOISE_REDUCTION);
	CHECK(ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 7) && ui_svc_setting_get(UI_SET_NOISE_REDUCTION) == 1);
	ui_svc_apply(UI_SET_NOISE_REDUCTION, 1);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT);
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == 0);
	// Mic AGC: off by default, its own bit, added to the options in force
	CHECK(ui_svc_setting_get(UI_SET_MIC_AGC) == 0);
	CHECK(ui_svc_setting_set(UI_SET_MIC_AGC, 1) && ui_svc_setting_get(UI_SET_MIC_AGC) == 1);
	ui_svc_apply(UI_SET_MIC_AGC, 1);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC));
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == SETTINGS_AUDIO_AGC);
	// ... and kept when noise reduction changes
	CHECK(ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 0));
	ui_svc_apply(UI_SET_NOISE_REDUCTION, 0);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_TONE_FRAMES | VOCODER_OPT_AGC));
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == (SETTINGS_AUDIO_AGC | SETTINGS_AUDIO_NO_NOISE_REDUCTION));
	CHECK(ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 1) && ui_svc_setting_set(UI_SET_MIC_AGC, 0));
	ui_svc_apply(UI_SET_MIC_AGC, 0);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT);
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == 0);
	// The debug link's SETTING 31 sets the options in RAM, any combination (0 = the codec's plain 0x800 word); the
	// store is not touched, and the next settings change brings theirs back
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 31, 0, 0 }, 3) == DL_ST_OK);
	CHECK(ambeVoiceEncodeOptions() == 0);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 31, 7, 0 }, 3) == DL_ST_OK);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC));
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 31, 8, 0 }, 3) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 31, 0, 1 }, 3) == DL_ST_BAD_ARG);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC));
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == 0);
	ui_svc_apply(UI_SET_NOISE_REDUCTION, ui_svc_setting_get(UI_SET_NOISE_REDUCTION));
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT);
	// Settings > Location > GPS reaches the board: the stored value at start-up (ui_app applies every setting), then
	// each change (the setting store keeps it either way)
	ui_svc_apply(UI_SET_GPS, ui_svc_setting_get(UI_SET_GPS));
	CHECK(gps_enable_calls == 1 && gps_enabled);
	CHECK(ui_svc_setting_set(UI_SET_GPS, 0));
	ui_svc_apply(UI_SET_GPS, ui_svc_setting_get(UI_SET_GPS));
	CHECK(gps_enable_calls == 2 && !gps_enabled);
	CHECK(ui_svc_setting_set(UI_SET_GPS, 1));
	ui_svc_apply(UI_SET_GPS, ui_svc_setting_get(UI_SET_GPS));
	CHECK(gps_enable_calls == 3 && gps_enabled);
	// The debug link's SETTING 37 stores and applies it as the menu does, on a radio whose module was heard only
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 37, 0, 0 }, 3) ==
	      DL_ST_UNAVAILABLE);
	CHECK(gps_enable_calls == 3);
	gps_here = true;
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 37, 2, 0 }, 3) == DL_ST_BAD_ARG);
	if (CONFIG_GPS) {
		CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 37, 0, 0 }, 3) == DL_ST_OK);
		CHECK(gps_enable_calls == 4 && !gps_enabled && ui_svc_setting_get(UI_SET_GPS) == 0);
		CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 37, 1, 0 }, 3) == DL_ST_OK);
		CHECK(gps_enable_calls == 5 && gps_enabled && ui_svc_setting_get(UI_SET_GPS) == 1);
	}
	gps_here = false;
	ui_svc_apply(UI_SET_LONG_PRESS_MS, 900);
	ui_svc_apply(UI_SET_REPEAT_MS, 200);
	CHECK(input_long == 900 && input_repeat == 200);
	// The UI's own settings (settings_t.ui_flags and the fields after it)
	CHECK(ui_svc_setting_get(UI_SET_HINTS) == 1 && ui_svc_setting_get(UI_SET_LARGE_TEXT) == 0);
	CHECK(ui_svc_setting_get(UI_SET_PTT_LOCKED) == 1 && ui_svc_setting_get(UI_SET_LOW_BATTERY) == 15);
	CHECK(ui_svc_setting_set(UI_SET_LARGE_TEXT, 1) && ui_svc_setting_set(UI_SET_HINTS, 0));
	CHECK(ui_svc_setting_set(UI_SET_PTT_LOCKED, 0) && ui_svc_setting_set(UI_SET_HEAR_ALL_DEFAULT, 1));
	CHECK(ui_svc_setting_set(UI_SET_THEME, 2) && ui_svc_setting_set(UI_SET_UPDOWN_HOME, 1));
	CHECK(ui_svc_setting_set(UI_SET_AUTO_OFF_MIN, 30) && ui_svc_setting_set(UI_SET_LOW_BATTERY, 20));
	settings_read(&sv);
	CHECK(sv.ui_flags == (SETTINGS_UI_LARGE_TEXT | SETTINGS_UI_MESSAGE_TONE | SETTINGS_UI_ALIAS | SETTINGS_UI_HEAR_ALL));
	CHECK(sv.theme == 2 && sv.updown_home == 1 && sv.auto_off_min == 30 && sv.low_batt_pct == 20);
	// Vibrate: on by default, kept as an "off" bit so records saved before it keep it on
	CHECK(ui_svc_setting_get(UI_SET_VIBRATE) == 1);
	CHECK(ui_svc_setting_set(UI_SET_VIBRATE, 0) && ui_svc_setting_get(UI_SET_VIBRATE) == 0);
	settings_read(&sv);
	CHECK(sv.ui_flags == (SETTINGS_UI_LARGE_TEXT | SETTINGS_UI_MESSAGE_TONE | SETTINGS_UI_ALIAS |
			      SETTINGS_UI_HEAR_ALL | SETTINGS_UI_NO_VIBRATE));
	CHECK(ui_svc_setting_set(UI_SET_VIBRATE, 1) && ui_svc_setting_get(UI_SET_VIBRATE) == 1);
	settings_read(&sv);
	CHECK(!(sv.ui_flags & SETTINGS_UI_NO_VIBRATE));
	// The setting is usable only on a board with a motor; ui_svc_vibrate reaches it through the radio core
	CHECK(!(ui_svc_feat_radio() & UI_FEAT_VIBRATE));
	vibrations = 0;
	ui_svc_vibrate(UI_VIBE_CALL);
	run(100);
	CHECK(vibrations == 0 && !vibe_busy());
	fake_power.vibrate = p_vibrate;
	CHECK(ui_svc_feat_radio() & UI_FEAT_VIBRATE);
	ui_svc_vibrate(UI_VIBE_MESSAGE);
	ui_svc_vibrate(200);		// not a pattern: nothing
	run(1000);
	CHECK(vibrations == 2 && !vibe_busy());
	ui_svc_vibrate(UI_VIBE_EMERGENCY);
	run(3000);
	CHECK(vibrations == 6);
	fake_power.vibrate = NULL;
	ui_svc_ptt_block(true);
	CHECK(ptt_blocked);
	ui_svc_ptt_block(false);
	CHECK(!ptt_blocked);
	ui_svc_dtmf_block(true);
	CHECK(dtmf_blocked);
	ui_svc_dtmf_block(false);
	CHECK(!dtmf_blocked);
	// Talk permit tone: the radio core's, saved with its settings
	CHECK(ui_svc_setting_get(UI_SET_TALK_PERMIT) == 1);
	ui_svc_apply(UI_SET_TALK_PERMIT, 0);
	run(20);
	CHECK(!sm.set.talk_permit && ui_svc_setting_get(UI_SET_TALK_PERMIT) == 0);
	settings_read(&sv);
	CHECK(sv.rs_flags & SETTINGS_RS_NO_TALK_PERMIT);
	ui_svc_apply(UI_SET_TALK_PERMIT, 1);
	run(20);
	CHECK(sm.set.talk_permit);
	// Settings > Sound tones. The four keyed ones (docs/spec/codeplug-settings.md 5.2): on while absent; a set writes
	// a one-byte bool key with no codeplug reload, and the radio core saves the tone as the radio's own
	static const uint8_t tone_ids[] = { UI_SET_CALL_END_TONE, UI_SET_TOT_WARN_TONE, UI_SET_BATTERY_TONE,
					    UI_SET_TX_DENY_TONE };
	for (size_t i = 0; i < sizeof(tone_ids); i++) {
		uint32_t cp_was = cp_serial;
		CHECK(ui_svc_setting_get(tone_ids[i]) == 1);
		CHECK(ui_svc_setting_set(tone_ids[i], 0) && storage_job_busy());
		storage_job_poll();
		CHECK(kv_tone[i] == 0);
		run(20);
		CHECK(sm.set.tones_off == (1u << i) && cp_serial == cp_was && ui_svc_setting_get(tone_ids[i]) == 0);
		settings_read(&sv);
		CHECK(settings_tones_off(&sv) == (1u << i));
		// A settings change sent by the UI (its copy of the bits may be older) doesn't undo the key
		ui_svc_apply(UI_SET_KEY_TONES, 0);
		run(20);
		CHECK(sm.set.tones_off == (1u << i));
		ui_svc_apply(UI_SET_KEY_TONES, 1);
		CHECK(ui_svc_setting_set(tone_ids[i], 1));
		storage_job_poll();
		run(20);
		CHECK(kv_tone[i] == 1 && ui_svc_setting_get(tone_ids[i]) == 1 && sm.set.tones_off == 0);
		kv_tone[i] = -1;
	}
	// A key written by a CPS holds over the radio's own copy, which follows it; once the key is gone the radio keeps
	// the key's last value
	kv_tone[1] = 0;
	kv_serial++;
	run(20);
	settings_read(&sv);
	CHECK(sm.set.tones_off == 0x02u && settings_tones_off(&sv) == 0x02u && ui_svc_setting_get(UI_SET_TOT_WARN_TONE) == 0);
	kv_tone[1] = -1;
	kv_serial++;
	run(20);
	CHECK(sm.set.tones_off == 0x02u);
	// With another firmware's codeplug the area can't be written and holds no key: the tone is saved as the radio's
	// own, with no "Old codeplug" question
	cp_foreign = true;
	kv_held = true;
	n_ev = 0;
	CHECK(ui_svc_setting_set(UI_SET_TOT_WARN_TONE, 1));
	storage_job_poll();
	run(20);
	settings_read(&sv);
	CHECK(n_ev == 0 && kv_tone[1] == -1 && sm.set.tones_off == 0 && settings_tones_off(&sv) == 0);
	cp_foreign = false;
	// A key that is present and can't be updated (a CPS session holds the flash) refuses the change
	kv_tone[2] = 1;
	CHECK(ui_svc_setting_set(UI_SET_BATTERY_TONE, 0));
	storage_job_poll();
	run(20);
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && kv_tone[2] == 1 && sm.set.tones_off == 0);
	// The debug link's SETTING 36 stores as the menu does: with the call end tone's key present, the key is written
	// too (else the key would put the radio's copy back); a present key that can't be updated is busy
	kv_tone[2] = -1;
	kv_tone[0] = 1;
	kv_serial++;
	run(20);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 36, 1, 0 }, 3) == DL_ST_BUSY);
	run(20);
	CHECK(kv_tone[0] == 1 && sm.set.tones_off == 0);
	kv_held = false;
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 36, 1, 0 }, 3) == DL_ST_OK);
	run(20);
	CHECK(kv_tone[0] == 0 && sm.set.tones_off == 0x01u);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 36, 0, 0 }, 3) == DL_ST_OK);
	run(20);
	CHECK(kv_tone[0] == 1 && sm.set.tones_off == 0);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 36, 0x10, 0 }, 3) == DL_ST_BAD_ARG);
	kv_tone[0] = -1;
	n_ev = 0;
	// The scan stop tone is the radio core's (rs_flags), the emergency tone the emergency service's (em_flags)
	CHECK(ui_svc_setting_get(UI_SET_SCAN_TONE) == 0);
	CHECK(ui_svc_setting_set(UI_SET_SCAN_TONE, 1));
	ui_svc_apply(UI_SET_SCAN_TONE, 1);
	run(20);
	CHECK(sm.set.scan_beep && ui_svc_setting_get(UI_SET_SCAN_TONE) == 1);
	settings_read(&sv);
	CHECK(sv.rs_flags & SETTINGS_RS_SCAN_BEEP);
	ui_svc_apply(UI_SET_SCAN_TONE, 0);
	run(20);
	CHECK(!sm.set.scan_beep);
	CHECK(ui_svc_setting_get(UI_SET_EMERGENCY_TONE) == 1);
	CHECK(ui_svc_setting_set(UI_SET_EMERGENCY_TONE, 0));
	run(20);
	CHECK(ui_svc_setting_get(UI_SET_EMERGENCY_TONE) == 0);
	settings_read(&sv);
	CHECK(sv.em_flags & SETTINGS_EM_NO_TONE);
	CHECK(ui_svc_setting_set(UI_SET_EMERGENCY_TONE, 1));
	run(20);
	CHECK(ui_svc_setting_get(UI_SET_EMERGENCY_TONE) == 1);
	// Reply window: the value shown at start-up for "the codeplug's" keeps it; another value is the radio's own
	CHECK(ui_svc_setting_get(UI_SET_REPLY_S) == 10);
	ui_svc_apply(UI_SET_REPLY_S, 10);
	CHECK(sm.set.reply_s == RADIO_REPLY_CODEPLUG);
	ui_svc_apply(UI_SET_REPLY_S, 4);
	CHECK(sm.set.reply_s == 4 && ui_svc_setting_get(UI_SET_REPLY_S) == 4);
	ui_svc_apply(UI_SET_REPLY_S, 10);
	CHECK(sm.set.reply_s == 10);
	// VOX: refused on a radio that can't measure the mic (the UI says so instead of "VOX on"); with a mic level, on
	// every FM channel, off again (the radio's settings)
	CHECK(!ui_svc_vox(true));
	run(5);
	CHECK(sm.set.vox == RADIO_VOX_OFF);
	fake_power.mic_level = p_mic;
	CHECK(ui_svc_vox(true));
	run(5);
	CHECK(sm.set.vox == RADIO_VOX_ALL);
	// A change made elsewhere (the debug link's SETTING op, a host, the radio core) survives the UI's next one: the
	// UI changes its own field only (issue 178: the VOX toggle put back a TX inhibit cleared over the debug link)
	CHECK(!sm.set.tx_inhibit && sm.set.tot_s != 77);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 5, 1, 0 }, 3) == DL_ST_OK);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 1, 77, 0 }, 3) == DL_ST_OK);
	CHECK(sm.set.tx_inhibit && sm.set.tot_s == 77);
	ui_svc_vox(false);
	run(5);
	CHECK(sm.set.vox == RADIO_VOX_OFF && sm.set.tx_inhibit && sm.set.tot_s == 77);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 5, 0, 0 }, 3) == DL_ST_OK);
	ui_svc_apply(UI_SET_KEY_TONES, 1);
	CHECK(!sm.set.tx_inhibit && sm.set.tot_s == 77 && sm.set.key_beep);
	ui_svc_apply(UI_SET_TOT_S, 180);
	CHECK(!sm.set.tx_inhibit && sm.set.tot_s == 180 && sm.set.key_beep);
	fake_power.mic_level = NULL;

	// Messages: delete through the data service
	CHECK(!ui_svc_msg_delete(0));

	// Factory reset: a job; the settings are the defaults afterwards (no flash here: RAM only, no restart)
	CHECK(ui_svc_factory_reset() && !ui_svc_factory_reset());
	storage_job_poll();
	settings_read(&sv);
	CHECK(sv.auto_lock_s == 0 && sv.mic_gain == 5 && sv.privacy_type[1] == 0 && sv.rs_stored == 0);
	CHECK(sv.ref_trim_ppb == 0);
	run(100);
	CHECK(sm.set.reply_s == RADIO_REPLY_CODEPLUG && sm.set.vox == RADIO_VOX_OFF);
}

// Every setting ui_svc_storage.c answers: where it is read from, the clamps on the way in, the privacy key slot,
// and the codeplug jobs' refusals (no channel, a job still pending, the channel unreadable)
static void test_ui_settings(void)
{
	settings_t sv;
	radio_settings_t rs;

	dmr_start(true);
	// The radio core's own settings, read back through radio_get_settings
	radio_get_settings(&rs);
	rs.key_beep = true;
	rs.beep_level = 65;		// shown in tenths, rounded: 7
	rs.tot_s = 90;
	rs.backlight_dim_s = 20;
	rs.rx_save = 2;
	rs.roam_survey = 2;
	rs.roam_beacon_s = 120;
	rs.roam_beacon_len_s = 6;
	rs.roam_probe = 1;
	radio_set_settings(&rs);
	CHECK(ui_svc_setting_get(UI_SET_KEY_TONES) == 1 && ui_svc_setting_get(UI_SET_TONE_VOLUME) == 7);
	CHECK(ui_svc_setting_get(UI_SET_TOT_S) == 90 && ui_svc_setting_get(UI_SET_BACKLIGHT_S) == 20);
	CHECK(ui_svc_setting_get(UI_SET_BATTERY_SAVER) == 1);
	// Backlight (EP): > 0 dims then goes off at 4x, 0 always on, < 0 dims after -value and never goes off
	static const struct { int32_t v; uint16_t dim, off; int32_t back; } bl[] = {
		{ 30, 30, 120, 30 }, { 0, 0, 0, 0 }, { -30, 30, 0, -30 },
		{ 1, 1, 4, 1 }, { -1, 1, 0, -1 },				// BVA: the smallest times
		{ 0x3FFF, 0x3FFF, 0xFFFC, 0x3FFF }, { 0x4000, 0x4000, 0xFFFF, 0x4000 },	// off saturates
		{ -0xFFFF, 0xFFFF, 0, -0xFFFF }, { -0x10000, 0xFFFF, 0, -0xFFFF },	// dim saturates
	};
	for (size_t i = 0; i < sizeof bl / sizeof bl[0]; i++) {
		ui_svc_apply(UI_SET_BACKLIGHT_S, bl[i].v);
		radio_get_settings(&rs);
		CHECK(rs.backlight_dim_s == bl[i].dim && rs.backlight_off_s == bl[i].off);
		CHECK(ui_svc_setting_get(UI_SET_BACKLIGHT_S) == bl[i].back);
	}
	// PBT: every value in the menu's range reads back as set, and a negative one never turns the backlight off
	fuzz_t bf;
	fuzz_init(&bf, 0xB1);
	for (long i = 0; i < fuzz_iters(500); i++) {
		int32_t v = (int32_t)fuzz_below(&bf, 2 * 0x3FFF + 1) - 0x3FFF;
		ui_svc_apply(UI_SET_BACKLIGHT_S, v);
		radio_get_settings(&rs);
		CHECK(ui_svc_setting_get(UI_SET_BACKLIGHT_S) == v);
		CHECK(v >= 0 || batt_backlight_pct(100, rs.backlight_dim_s, rs.backlight_off_s, 0xFFFFFFFFu, BATT_OK) == 20);
	}
	ui_svc_apply(UI_SET_BACKLIGHT_S, 20);
	CHECK(ui_svc_setting_get(UI_SET_ROAM_SURVEY) == 2 && ui_svc_setting_get(UI_SET_ROAM_BEACON_S) == 120 &&
	      ui_svc_setting_get(UI_SET_ROAM_BEACON_LEN) == 6 && ui_svc_setting_get(UI_SET_ROAM_PROBE) == 1);
	// ... which the store leaves to ui_svc_apply (nothing written here)
	settings_read(&sv);
	CHECK(ui_svc_setting_set(UI_SET_KEY_TONES, 0) && ui_svc_setting_set(UI_SET_ROAM_PROBE, 0));
	settings_t sv2;
	settings_read(&sv2);
	CHECK(memcmp(&sv, &sv2, sizeof(sv)) == 0);

	// Preferences in settings_t, with their clamps and the fallbacks for out of range stored values
	CHECK(ui_svc_setting_set(UI_SET_BRIGHTNESS, 3) && ui_svc_setting_get(UI_SET_BRIGHTNESS) == 10);
	CHECK(ui_svc_setting_set(UI_SET_BRIGHTNESS, 250) && ui_svc_setting_get(UI_SET_BRIGHTNESS) == 100);
	CHECK(ui_svc_setting_set(UI_SET_SQUELCH, -4) && ui_svc_setting_get(UI_SET_SQUELCH) == 0);
	CHECK(ui_svc_setting_set(UI_SET_SQUELCH, 12) && ui_svc_setting_get(UI_SET_SQUELCH) == 9);
	CHECK(ui_svc_setting_set(UI_SET_HOTSPOT_AUTO, 0) && ui_svc_setting_get(UI_SET_HOTSPOT_AUTO) == 0);
	CHECK(ui_svc_setting_set(UI_SET_HOTSPOT_AUTO, 5) && ui_svc_setting_get(UI_SET_HOTSPOT_AUTO) == 1);
	CHECK(ui_svc_setting_set(UI_SET_MIC_GAIN, 40) && ui_svc_setting_get(UI_SET_MIC_GAIN) == 10);
	CHECK(ui_svc_setting_set(UI_SET_AUTO_LOCK_S, 99999) && ui_svc_setting_get(UI_SET_AUTO_LOCK_S) == 3600);
	CHECK(ui_svc_setting_set(UI_SET_LONG_PRESS_MS, 10) && ui_svc_setting_get(UI_SET_LONG_PRESS_MS) == 200);
	CHECK(ui_svc_setting_set(UI_SET_REPEAT_MS, 5000) && ui_svc_setting_get(UI_SET_REPEAT_MS) == 1000);
	CHECK(ui_svc_setting_set(UI_SET_POS_FORMAT, 9) && ui_svc_setting_get(UI_SET_POS_FORMAT) == 2);
	CHECK(ui_svc_setting_set(UI_SET_POS_SEND_S, -1) && ui_svc_setting_get(UI_SET_POS_SEND_S) == 0);
	CHECK(ui_svc_setting_set(UI_SET_GPS, 0) && ui_svc_setting_get(UI_SET_GPS) == 0);
	CHECK(ui_svc_setting_set(UI_SET_THEME, -3) && ui_svc_setting_get(UI_SET_THEME) == 0);
	// The highlight color and the filled strip share settings_t.accent: setting one keeps the other. A zero byte (new
	// or older records) is the defaults: the theme's highlight, the strip filled
	CHECK(ui_svc_setting_get(UI_SET_ACCENT) == 0 && ui_svc_setting_get(UI_SET_STRIP) == 1);
	CHECK(ui_svc_setting_set(UI_SET_ACCENT, 2) && ui_svc_setting_set(UI_SET_STRIP, 0));
	CHECK(ui_svc_setting_set(UI_SET_ACCENT, 9) && ui_svc_setting_get(UI_SET_ACCENT) == 7 &&
	      ui_svc_setting_get(UI_SET_STRIP) == 0);
	settings_read(&sv);
	CHECK(sv.accent == (7 | SETTINGS_ACCENT_PLAIN));
	CHECK(ui_svc_setting_set(UI_SET_STRIP, 1) && ui_svc_setting_get(UI_SET_ACCENT) == 7);
	CHECK(ui_svc_setting_set(UI_SET_ACCENT, 0) && ui_svc_setting_get(UI_SET_STRIP) == 1);
	settings_read(&sv);
	CHECK(sv.accent == 0);
	CHECK(ui_svc_setting_set(UI_SET_UPDOWN_HOME, 7) && ui_svc_setting_get(UI_SET_UPDOWN_HOME) == 2);
#if CONFIG_CRASH_REPORT
	// The crash report's mode and target share theme, updown_home and accent (settings.h): each keeps the others
	CHECK(ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_OFF && ui_svc_setting_get(UI_SET_CRASH_CONTACT) == 0);
	CHECK(ui_svc_setting_set(UI_SET_THEME, 1) && ui_svc_setting_set(UI_SET_ACCENT, 3) && ui_svc_setting_set(UI_SET_STRIP, 0));
	CHECK(ui_svc_setting_set(UI_SET_CRASH_MODE, 9) && ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_AUTO);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_MODE, -1) && ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_OFF);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_MODE, UI_CRASH_SEND_ASK));
	CHECK(ui_svc_setting_set(UI_SET_CRASH_CONTACT, CODEPLUG_CONTACTS_MAX + 5) &&
	      ui_svc_setting_get(UI_SET_CRASH_CONTACT) == CODEPLUG_CONTACTS_MAX);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_CONTACT, -3) && ui_svc_setting_get(UI_SET_CRASH_CONTACT) == 0);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_CONTACT, 1000));
	CHECK(ui_svc_setting_get(UI_SET_THEME) == 1 && ui_svc_setting_get(UI_SET_UPDOWN_HOME) == 2 &&
	      ui_svc_setting_get(UI_SET_ACCENT) == 3 && ui_svc_setting_get(UI_SET_STRIP) == 0);
	CHECK(ui_svc_setting_set(UI_SET_THEME, 2) && ui_svc_setting_set(UI_SET_UPDOWN_HOME, 1) &&
	      ui_svc_setting_set(UI_SET_ACCENT, 5) && ui_svc_setting_set(UI_SET_STRIP, 1));
	CHECK(ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_ASK && ui_svc_setting_get(UI_SET_CRASH_CONTACT) == 1000);
	// A stored mode of 3 (no such mode): off
	settings_t *cs = settings_begin();
	settings_set_crash_mode(cs, 3);
	settings_end(now);
	CHECK(ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_OFF);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_MODE, UI_CRASH_SEND_OFF) && ui_svc_setting_set(UI_SET_CRASH_CONTACT, 0) &&
	      ui_svc_setting_set(UI_SET_THEME, 0) && ui_svc_setting_set(UI_SET_ACCENT, 0) &&
	      ui_svc_setting_set(UI_SET_UPDOWN_HOME, 2));
#endif
	CHECK(ui_svc_setting_set(UI_SET_LOW_BATTERY, 80) && ui_svc_setting_get(UI_SET_LOW_BATTERY) == 50);
	CHECK(ui_svc_setting_set(UI_SET_AUTO_OFF_MIN, 100000) && ui_svc_setting_get(UI_SET_AUTO_OFF_MIN) == 1440);
	CHECK(ui_svc_setting_set(UI_SET_MESSAGE_TONE, 0) && ui_svc_setting_get(UI_SET_MESSAGE_TONE) == 0);
	CHECK(ui_svc_setting_set(UI_SET_ALIAS_SHOW, 0) && ui_svc_setting_get(UI_SET_ALIAS_SHOW) == 0);
	CHECK(ui_svc_setting_set(UI_SET_TFTP_WRITE, 0) && ui_svc_setting_get(UI_SET_TFTP_WRITE) == 0);
	settings_read(&sv);
	CHECK(sv.net_flags & SETTINGS_NET_TFTP_NO_WRITE);
	CHECK(ui_svc_setting_set(UI_SET_TFTP_WRITE, 1) && ui_svc_setting_get(UI_SET_TFTP_WRITE) == 1);
	// Stored values another firmware (or a damaged record) left out of range: the defaults shown
	settings_t *st = settings_begin();
	st->brightness = 3;
	st->squelch = 0xEE;
	st->mic_gain = 77;
	st->long_press_ms = 0;
	st->repeat_ms = 0;
	st->pos_format = 9;
	st->theme = 7;		// bits 0-1 out of range (the bits above hold the crash report's target)
	st->updown_home = 7;
	st->privacy_type[0] = 99;
	settings_end(now);
	ram[UI_SET_SQUELCH] = 3;
	ram[UI_SET_LONG_PRESS_MS] = 777;
	ram[UI_SET_REPEAT_MS] = 111;
	CHECK(ui_svc_setting_get(UI_SET_BRIGHTNESS) == 100 && ui_svc_setting_get(UI_SET_SQUELCH) == 3 &&
	      ui_svc_setting_get(UI_SET_MIC_GAIN) == 5);
	CHECK(ui_svc_setting_get(UI_SET_LONG_PRESS_MS) == 777 && ui_svc_setting_get(UI_SET_REPEAT_MS) == 111);
	CHECK(ui_svc_setting_get(UI_SET_POS_FORMAT) == 0 && ui_svc_setting_get(UI_SET_THEME) == 0 &&
	      ui_svc_setting_get(UI_SET_UPDOWN_HOME) == 0);
	CHECK(ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == DMR_PRIVACY_OFF);
	// Settings nobody stores: the RAM table
	CHECK(ui_svc_setting_set(UI_SET_VOICE_PROMPTS, 1) && ram[UI_SET_VOICE_PROMPTS] == 1 &&
	      ui_svc_setting_get(UI_SET_VOICE_PROMPTS) == 1);

	// The data service's answers and message format
	CHECK(ui_svc_setting_set(UI_SET_CHECK_REPLY, 0) && ui_svc_setting_get(UI_SET_CHECK_REPLY) == 0);
	CHECK(ui_svc_setting_set(UI_SET_ALERT_ANSWER, 0) && ui_svc_setting_get(UI_SET_ALERT_ANSWER) == 0);
	CHECK(ui_svc_setting_set(UI_SET_POS_REPLY, 1) && ui_svc_setting_get(UI_SET_POS_REPLY) == 1);
	CHECK(ui_svc_setting_set(UI_SET_MSG_FORMAT, DMR_SMS_FORMAT_ETSI) &&
	      ui_svc_setting_get(UI_SET_MSG_FORMAT) == DMR_SMS_FORMAT_ETSI);
	CHECK(!ui_svc_setting_set(UI_SET_MSG_FORMAT, 9) && ui_svc_setting_get(UI_SET_MSG_FORMAT) == DMR_SMS_FORMAT_ETSI);
	CHECK(ui_svc_setting_set(UI_SET_MSG_FORMAT, DMR_SMS_FORMAT_ETSI_IP));

	// USB personality and the control station switches (no control station in this build: those stay off)
	CHECK(ui_svc_setting_set(UI_SET_USB_BOOT_NET, 1) && ui_svc_setting_get(UI_SET_USB_BOOT_NET) == 1);
	CHECK(ui_svc_setting_set(UI_SET_PC_ENDPOINT, 1) && ui_svc_setting_set(UI_SET_USB_FIXED_LINK, 1));
	CHECK(ui_svc_setting_get(UI_SET_PC_ENDPOINT) == CONFIG_DMR_CONTROL_STATION &&
	      ui_svc_setting_get(UI_SET_USB_FIXED_LINK) == CONFIG_DMR_CONTROL_STATION);
	CHECK(ui_svc_setting_set(UI_SET_USB_BOOT_NET, 0) && ui_svc_setting_get(UI_SET_USB_BOOT_NET) == 0);

	// Privacy key slot 1: the type and the key, kept in range; off clears the key and turns privacy off when
	// slot 1 was the active one; with privacy on, the radio core gets the new key at once
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_TYPE, DMR_PRIVACY_MOTOROLA_BP));
	settings_read(&sv);
	CHECK(sv.privacy_type[0] == DMR_PRIVACY_MOTOROLA_BP && sv.privacy_key[0] == 1);	// key 0: the lowest valid
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_KEY, 70000) && ui_svc_setting_get(UI_SET_PRIVACY_KEY) == 255);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_KEY, 42) && ui_svc_setting_get(UI_SET_PRIVACY_KEY) == 42);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_TYPE, 50) &&
	      ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == DMR_PRIVACY_NUM_TYPES - 1);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_TYPE, DMR_PRIVACY_MOTOROLA_BP));
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_KEY, 42));
	st = settings_begin();
	st->privacy_active = 1;
	settings_end(now);
	CHECK(ui_svc_privacy(true));
	run(20);
	CHECK(sm.view.dmr.privacy);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_KEY, 43));
	run(20);
	settings_read(&sv);
	CHECK(sv.privacy_key[0] == 43 && sm.view.dmr.privacy);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_TYPE, DMR_PRIVACY_OFF));
	run(20);
	settings_read(&sv);
	CHECK(sv.privacy_type[0] == DMR_PRIVACY_OFF && sv.privacy_key[0] == 0 && sv.privacy_active == 0);
	CHECK(!sm.view.dmr.privacy);

	// Codeplug jobs: refused off a stored channel and while one is pending; a channel that can't be read is
	// reported, nothing written
	int w = cp_writes;
	req(RREQ_VFO, 1);
	run(50);
	CHECK(sm.view.vfo && !ui_svc_save_channel());
#if CONFIG_VFO_DMR
	CHECK(ui_svc_save_talk_target(3100, false) && cp_writes == w);	// the VFO's own target: nothing to write
	run(20);
	CHECK(sm.vfo_talk_id == 3100 && !sm.vfo_talk_private);
#else
	CHECK(!ui_svc_save_talk_target(3100, false));
#endif
	sm.vfo_talk_id = 0;
	req(RREQ_VFO, 0);
	run(50);
	CHECK(!sm.view.vfo && sm.view.channel_number == 4);
	CHECK(!ui_svc_save_talk_target(0, false));
	CHECK(storage_job_post(test_job, NULL));
	CHECK(!ui_svc_save_channel() && !ui_svc_save_talk_target(9, false) && !ui_svc_factory_reset());
	storage_job_poll();
	used[4] = false;			// the channel is gone from the codeplug before the job runs
	n_ev = 0;
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(tr_en(last_ev.why), "read the channel") != NULL);
	n_ev = 0;
	CHECK(ui_svc_save_talk_target(9, false));
	storage_job_poll();
	CHECK(n_ev == 1 && strstr(tr_en(last_ev.why), "for now only") != NULL);
	used[4] = true;
	cp_write_fails = true;			// the contact found, the write fails
	n_ev = 0;
	CHECK(ui_svc_save_talk_target(9, false));
	storage_job_poll();
	CHECK(n_ev == 1 && strcmp(tr_en(last_ev.why), "Codeplug not written: for now only") == 0);
	cp_write_fails = false;
	CHECK(cp_writes == w);
	// The squelch saved with the channel goes to the default squelch preference
	ui_svc_setting_set(UI_SET_SQUELCH, 2);
	req(RREQ_SQUELCH, 1);
	run(20);
	CHECK(sm.view.temp & RADIO_TEMP_SQUELCH);
	uint8_t sq = sm.view.squelch;
	CHECK(sq != 2 && ui_svc_save_channel());
	storage_job_poll();
	settings_read(&sv);
	CHECK(sv.squelch == sq && cp_writes == w + 1);
	req(RREQ_CLEAR_TEMP, 0);
	run(20);
	// Every temporary change goes into the channel: power, bandwidth, both tones, the mode
	req(RREQ_ZONE_POS, 0);			// channel 1, FM simplex
	run(50);
	req(RREQ_POWER, 1);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_CTCSS, 1000 }));
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_RX, (subtone_t){ TONE_CTCSS, 1230 }));
	run(20);
	uint8_t pw = sm.view.power;
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(chans[1].power == pw && chans[1].bw == RADIO_BW_12K5 && chans[1].tx_tone.type == TONE_CTCSS &&
	      chans[1].tx_tone.value == 1000 && chans[1].rx_tone.value == 1230);
	run(100);				// the radio core re-reads the channel (codeplug serial)
	CHECK(ui_svc_mode(RADIO_MODE_DMR) && (sm.view.temp & RADIO_TEMP_MODE));
	run(20);
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(chans[1].mode == RADIO_MODE_DMR);
	setup_codeplug();
	char l1[17], l2[17];
	ui_svc_boot_text(l1, l2);
	CHECK(l1[0] == 0 && l2[0] == 0);
	req(RREQ_CLEAR_TEMP, 0);
	run(20);
}

// ---- The DMR data service on the radio core (radio_sm.c's data glue): what it lets out, keyed like PTT, and the
// answers it hands back ----

static void csbk_rx(uint8_t opcode, uint32_t target, uint32_t from, uint8_t cc)
{
	uint8_t c[10] = { (uint8_t)(0x80 | opcode), DMR_FID_ETSI, 0, 0, (uint8_t)(target >> 16), (uint8_t)(target >> 8),
			  (uint8_t)target, (uint8_t)(from >> 16), (uint8_t)(from >> 8), (uint8_t)from };
	dmrBurst_t d;
	CHECK(dmrDataBuildCSBK(c, &d) == 1);
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_CSBK;
	b.sync = DMR_SYNC_MS;
	b.cc = cc;
	b.crc_ok = true;
	b.in_step = true;
	b.len = (uint8_t)d.length;
	memcpy(b.payload, d.payload, d.length);
	sim_rx(&b);
}

// Bursts on air since n0 of this kind, with this CSBK opcode (0xFF: any)
static int air_count(unsigned n0, uint8_t kind, uint8_t opcode)
{
	int n = 0;
	for (unsigned i = n0; i < sim.air_count; i++) {
		if (sim.air[i].kind == kind && (opcode == 0xFF || (sim.air[i].payload[0] & 0x3F) == opcode)) {
			n++;
		}
	}
	return n;
}

// The side keys (ui_svc_storage.c over core/input/keyassign.c): the codeplug's slots give the key map; a change in
// Settings takes effect at once and goes to the codeplug through a storage job; a CPS write is taken
// The speaker volume of a radio without a knob (radio-features.md 4.2): the stored step reaches hal_radio_t.volume() as a
// percent at start-up and at each change; a knob radio, and a radio without the hook, are left alone
static int vol_knob = -1;
static int vol_calls, vol_last = -1;
static void r_volume(uint8_t percent) { vol_calls++; vol_last = percent; }
static int8_t p_volume(void) { return (int8_t)vol_knob; }

static void test_soft_volume(void)
{
	settings_t st;
	fake_radio.volume = NULL;
	// A power HAL that doesn't report volume at all: not soft, nothing sent
	start();
	run(100);
	CHECK(!radio_volume_soft() && vol_calls == 0 && !(ui_svc_feat_radio() & UI_FEAT_VOLUME));
	fake_radio.volume = r_volume;
	// No knob (-1): the default step goes out at start-up as 50 %
	fake_power.volume = p_volume;
	vol_knob = -1;
	start();
	run(100);
	CHECK(radio_volume_soft() && (ui_svc_feat_radio() & UI_FEAT_VOLUME));
	CHECK(vol_calls == 1 && vol_last == 50 && sm.view.volume == 50 && sm.view.volume_src == RVOL_SOFT);
	CHECK(ui_svc_setting_get(UI_SET_VOLUME) == RADIO_VOLUME_DEFAULT);
	// A change reaches the hook at once as round(step * 100 / RADIO_VOLUME_MAX) and is stored with bit 7 and step + 1
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, 13));
	run(100);
	CHECK(vol_calls == 2 && vol_last == 81 && sm.view.volume == 81 && ui_svc_setting_get(UI_SET_VOLUME) == 13);
	settings_read(&st);
	CHECK(st.volume == 0x8E);
	// The ends: mute (0) is stored as 1, the maximum is 100 %; out-of-range values are clamped
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, 0));
	run(100);
	CHECK(vol_last == 0 && ui_svc_setting_get(UI_SET_VOLUME) == 0);
	settings_read(&st);
	CHECK(st.volume == 0x81);
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, 99));
	run(100);
	CHECK(vol_last == 100 && ui_svc_setting_get(UI_SET_VOLUME) == RADIO_VOLUME_MAX);
	// The same step again sends nothing
	int calls = vol_calls;
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, RADIO_VOLUME_MAX));
	run(100);
	CHECK(vol_calls == calls);
	// A boot with the step stored: it goes out at once, the settings' value rather than the default
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, 4));
	run(100);
	vol_calls = 0;
	boot();
	run(100);
	CHECK(vol_calls == 1 && vol_last == 25);
	// A byte stored by the 0..10 version (1..11) is scaled to the new range: step s -> round(s * 16 / 10)
	settings_begin()->volume = 6;		// old step 5
	settings_end(now);
	CHECK(ui_svc_setting_get(UI_SET_VOLUME) == 8);
	CHECK(radio_volume_step(1) == 0 && radio_volume_step(2) == 2 && radio_volume_step(4) == 5 &&
	      radio_volume_step(11) == RADIO_VOLUME_MAX && radio_volume_step(0x80) == RADIO_VOLUME_MAX &&
	      radio_volume_step(radio_volume_stored(7)) == 7);
	// A knob that reads (0..100) wins: the stored step is not used
	vol_knob = 64;
	boot();
	run(100);
	CHECK(!radio_volume_soft() && vol_last == 64 && !(ui_svc_feat_radio() & UI_FEAT_VOLUME));
	CHECK(sm.view.volume_src == RVOL_KNOB && radio_volume_settable());
	CHECK(radio_volume_step(0) == RADIO_VOLUME_DEFAULT && radio_volume_step(200) == RADIO_VOLUME_MAX);
	// The debug link's override (RADIO op 30) takes the knob's place until it is given back; RAM only
	vol_calls = 0;
	CHECK(debuglink_is_feature_op(DL_OP_VOLUME));	// the dispatcher routes it here
	// VERSION's codec=: a codec stopped for a stub upload (present() false) still reports its profile from flash
	{
		const hal_vocoder_t pv = { v_present, v_reset, v_encode, v_decode, v_profile, NULL, NULL, NULL, NULL };
		bool was = codec_present;
		CHECK(strcmp(debuglink_codec_id(NULL), "none") == 0);
		codec_present = false;
		codec_profile = "db25d-2501";
		CHECK(strcmp(debuglink_codec_id(&pv), "db25d-2501") == 0);
		codec_profile = NULL;
		CHECK(strcmp(debuglink_codec_id(&pv), "absent") == 0);
		CHECK(strcmp(debuglink_codec_id(&fake_vocoder), "absent") == 0);
		codec_present = true;
		CHECK(strcmp(debuglink_codec_id(&fake_vocoder), "present") == 0);
		codec_present = was;
	}
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 4 }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_calls == 1 && vol_last == 25 && sm.view.volume_src == RVOL_OVERRIDE && sm.view.volume_override == 4);
	vol_knob = 90;		// the knob moves: no effect
	run(100);
	CHECK(vol_last == 25 && vol_calls == 1);
	{
		static char buf[2048];
		dl_text_t t;
		dl_text_init(&t, buf, sizeof buf);
		debuglink_view_radio(&sm.view, &t);
		CHECK(strstr(buf, "volume.src=override") != NULL && strstr(buf, "volume.override=4") != NULL &&
		      strstr(buf, "volume.max=16") != NULL);
	}
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 17 }, 1) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ RADIO_VOLUME_KNOB }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_last == 90 && sm.view.volume_src == RVOL_KNOB && sm.view.volume_override == -1);
	// The knob lock (keys locked, Settings > Key lock > Knob when locked Off): the level stays while the knob moves, the view
	// still tracks the knob, and letting go takes the knob's level at once
	CHECK(radio_volume_knob() && (ui_svc_feat_radio() & UI_FEAT_VOLUME_KNOB) && sm.view.volume_knob == 90);
	radio_req_volume_hold(true);
	run(100);
	vol_calls = 0;
	vol_knob = 30;
	run(100);
	CHECK(vol_calls == 0 && vol_last == 90 && sm.view.volume_src == RVOL_HOLD && sm.view.volume_knob == 30);
	radio_req_volume_hold(false);
	run(100);
	CHECK(vol_calls == 1 && vol_last == 30 && sm.view.volume_src == RVOL_KNOB);
	// A 1 % wobble is not a move
	vol_knob = 31;
	run(100);
	CHECK(sm.view.volume_knob == 30);
	vol_knob = 90;
	run(100);
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 0 }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_last == 0);
	debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ RADIO_VOLUME_KNOB }, 1);
	run(100);
	// No knob: the op stores the step like a UI change; 0xFF gives the default back
	vol_knob = -1;
	boot();
	run(100);
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 3 }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_last == 19 && sm.view.volume_src == RVOL_SOFT && ui_svc_setting_get(UI_SET_VOLUME) == 3);
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ RADIO_VOLUME_KNOB }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_last == 50 && ui_svc_setting_get(UI_SET_VOLUME) == RADIO_VOLUME_DEFAULT);
	// No hook at all (the volume works in analog hardware): not supported
	fake_radio.volume = NULL;
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 3 }, 1) == DL_ST_UNAVAILABLE);
	fake_radio.volume = r_volume;
	vol_knob = 64;
	fake_power.volume = NULL;
	fake_radio.volume = NULL;
}

static void test_side_keys(void)
{
	memset(cp_quick, 0, sizeof(cp_quick));
	keymap_sets = 0;
	ui_svc_keys_poll();
	CHECK(keymap_sets == 1 && got_keymap[1].raw_edges && got_keymap[1].short_action == ACT_MONITOR);
	CHECK(ui_svc_setting_get(UI_SET_KEY_FIRST) == KEYFN_DEFAULT);
	CHECK(ui_svc_setting_set(UI_SET_KEY_FIRST, KEYFN_POWER));
	CHECK(keymap_sets == 2 && got_keymap[1].short_action == ACT_POWER && !got_keymap[1].raw_edges);
	CHECK(ui_svc_setting_get(UI_SET_KEY_FIRST) == KEYFN_POWER);
	CHECK(cp_quick[0] == 0);		// not yet: the job does it
	CHECK(!ui_svc_setting_set(UI_SET_KEY_FIRST + 6, KEYFN_SCAN));	// two side keys and one home key
	// The home key (role 2): not Monitor; a function makes it send ACT_HOME_KEY_1, the UI's to turn back
	CHECK(!ui_svc_key_offered(UI_SET_KEY_FIRST + 4, KEYFN_MONITOR) && ui_svc_key_offered(UI_SET_KEY_FIRST + 2, KEYFN_MONITOR));
	CHECK(!ui_svc_setting_set(UI_SET_KEY_FIRST + 4, KEYFN_MONITOR));
	CHECK(ui_svc_home_key(ACT_NAV_LEFT, GESTURE_PRESS, true) == ACT_NAV_LEFT);
	CHECK(ui_svc_setting_set(UI_SET_KEY_FIRST + 4, KEYFN_POWER));
	CHECK(got_keymap[3].short_action == ACT_HOME_KEY_1 && got_keymap[3].long_action == ACT_HOME_KEY_1);
	CHECK(ui_svc_home_key(ACT_HOME_KEY_1, GESTURE_PRESS, true) == ACT_POWER);
	CHECK(ui_svc_home_key(ACT_HOME_KEY_1, GESTURE_PRESS, false) == ACT_NAV_LEFT);
	CHECK(ui_svc_home_key(ACT_HOME_KEY_1, GESTURE_LONG, true) == ACT_USER_1);
	CHECK(ui_svc_setting_set(UI_SET_KEY_FIRST + 4, KEYFN_DEFAULT));
	CHECK(got_keymap[3].short_action == ACT_NAV_LEFT);
	CHECK(!ui_svc_setting_set(UI_SET_KEY_FIRST + 2, KEYFN_COUNT));
	uint32_t ser = cp_serial;
	ui_svc_keys_poll();
	storage_job_poll();
	CHECK(cp_quick[0] == KEYASSIGN_MARKER && cp_quick[1] == KEYFN_POWER && cp_serial == ser + 1u);
	ui_svc_keys_poll();			// our own write read back: the same
	CHECK(ui_svc_setting_get(UI_SET_KEY_FIRST) == KEYFN_POWER && got_keymap[1].short_action == ACT_POWER);
	// A CPS wrote the slots: role 0's press, role 1's long press
	cp_quick[1] = KEYFN_SCAN;
	cp_quick[2] = KEYFN_ZONE_UP << 8;
	cp_serial++;
	ui_svc_keys_poll();
	CHECK(ui_svc_setting_get(UI_SET_KEY_FIRST) == KEYFN_SCAN && got_keymap[1].short_action == ACT_SCAN);
	CHECK(got_keymap[2].short_action == ACT_TALK_TARGET && got_keymap[2].long_action == ACT_ZONE_UP);
	// A write that fails is reported; the key works for now
	cp_quick_fail = true;
	n_ev = 0;
	CHECK(ui_svc_setting_set(UI_SET_KEY_FIRST + 3, KEYFN_ZONE_DOWN));
	ui_svc_keys_poll();
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(tr_en(last_ev.why), "save the keys") != NULL);
	CHECK(got_keymap[2].long_action == ACT_ZONE_DOWN && cp_quick[2] == KEYFN_ZONE_UP << 8);
	cp_quick_fail = false;
	// Layout 1 (three roles, a slot per gesture), as an older build wrote it: read as it is
	memset(cp_quick, 0, sizeof(cp_quick));
	cp_quick[0] = KEYASSIGN_MARKER_V1;
	cp_quick[1] = KEYFN_SCAN;
	cp_quick[4] = KEYFN_ZONE_UP;
	cp_serial++;
	ui_svc_keys_poll();
	CHECK(ui_svc_setting_get(UI_SET_KEY_FIRST) == KEYFN_SCAN && ui_svc_setting_get(UI_SET_KEY_FIRST + 3) == KEYFN_ZONE_UP);
	CHECK(got_keymap[2].long_action == ACT_ZONE_UP);
	// A CPS put Monitor on the home key (role 2): it acts as Default, and Settings says so
	memset(cp_quick, 0, sizeof(cp_quick));
	cp_quick[0] = KEYASSIGN_MARKER;
	cp_quick[3] = KEYFN_MONITOR;
	cp_serial++;
	ui_svc_keys_poll();
	CHECK(got_keymap[3].short_action == ACT_NAV_LEFT && ui_svc_setting_get(UI_SET_KEY_FIRST + 4) == KEYFN_DEFAULT);
	memset(cp_quick, 0, sizeof(cp_quick));	// leave the board's key map for what follows
	cp_serial++;
	ui_svc_keys_poll();
}

// Switching off with ARS on (gotcha 7.4): the radio core holds the power off until the deregistration is on air, as
// app.c's power_off() waits on radio_power_off_busy() before it drops the latch (the DM-1701 and MD-380 knob)
static void test_power_off_ars(void)
{
	dmrDataSettings_t ds;

	dmr_start(true);
	dmrDataServiceGetSettings(&ds);
	ds.arsServer = 9998100;
	dmr_data_set_settings(&ds);
	unsigned n0 = sim.air_count;
	run(20000);			// the power-on registration, 5-15 s on (no server here to acknowledge it)
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 1);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REGISTERING);

	// Power off: busy until the preambles, the header and the blocks have all gone out, and nothing after
	n0 = sim.air_count;
	req(RREQ_POWER_OFF, 0);
	run(50);
	CHECK(sm.power_off_req && sm.power_off_busy);
	unsigned ms = 50;
	while (sm.power_off_busy && ms < 3000) {
		run(50);
		ms += 50;
	}
	CHECK(!sm.power_off_busy && ms < 1000);
	unsigned aired = sim.air_count - n0;
	CHECK(air_count(n0, DMR_BURST_CSBK, DMR_CSBKO_PREAMBLE) >= 1 && air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 1);
	CHECK(air_count(n0, DMR_BURST_RATE_12_DATA, 0xFF) >= 1 && !sm.dmr_keyed);
	run(5000);
	CHECK(sim.air_count - n0 == aired);	// no registration while switching off

	// Still on after all (auto power off with the knob on): canceled, and registered again
	req(RREQ_POWER_OFF, 1);
	run(50);
	CHECK(!sm.power_off_req && !sm.power_off_busy);
	n0 = sim.air_count;
	run(2000);
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 1);

	// ARS off for what follows: the data service keeps its state from test to test
	ds.arsServer = 0;
	dmr_data_set_settings(&ds);
	run(10000);
	CHECK(!dmrDataServiceIsBusy() && dmrDataServiceArsState() == DMR_ARS_STATE_OFF);
}

#if CONFIG_VFO_DMR
// Data from a DMR VFO (docs/spec/dmr-data.md 6): ARS, an IP packet as the IP gateway's air path sends it and a CSBK
// answer go out as on a DMR channel, with the VFO's own color code, and an emergency starts. A retune re-registers once
// it has settled, not on every step. Nothing goes out from an FM VFO, nor on a frequency outside the transmit bands
static void test_vfo_dmr_data(void)
{
	dmrDataSettings_t ds;
	uint8_t kind;
	uint32_t dst;

	dmr_start(true);
	req(RREQ_VFO_SET, 446000000);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && sm.view.dmr.cc == 1);

	// ARS: the power-on registration (5-15 s on)
	dmrDataServiceGetSettings(&ds);
	ds.arsServer = 9998100;
	dmr_data_set_settings(&ds);
	unsigned n0 = sim.air_count;
	run(20000);
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 1);
	// Turning the knob: nothing while the frequency keeps changing, one registration once it has held still
	n0 = sim.air_count;
	for (int i = 0; i < 3; i++) {
		req(RREQ_VFO_STEP, 1);
		run(1000);
	}
	CHECK(sm.view.ch.rx_hz == 446037500 && air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 0);
	run(6000);
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 1);
	ds.arsServer = 0;
	dmr_data_set_settings(&ds);
	run(10000);
	CHECK(!dmrDataServiceIsBusy());

	// An IP packet through the IP gateway's air path (dmrDataServiceSendIP): on air from the VFO
	static const uint8_t l4[12] = { 0x13, 0x88, 0x13, 0x88, 0, 12, 0, 0, 'p', 'i', 'n', 'g' };
	n0 = sim.air_count;
	int tx0 = hw.tx_calls;
	CHECK(dmrDataServiceSendIP(false, 9991234, DMR_DATA_IP_OURS, DMR_DATA_IP_OURS, DMR_IP_TTL_DEFAULT, DMR_IP_PROTO_UDP, l4,
				   sizeof(l4)));
	run(3000);
	CHECK(!dmrDataServiceIpBusy() && dmrDataServiceIpResult() == DMR_REQ_OK);
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) >= 1 && hw.tx_calls > tx0 && hw.tx_hz == 446037500);

	// A CSBK answer: a call alert to us on the VFO's color code is acknowledged
	n0 = sim.air_count;
	csbk_rx(DMR_CSBKO_CALL_ALERT, 9996543, 9999876, 1);
	run(1000);
	CHECK(air_count(n0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT_ACK) == 1);
	// ... and our own call alert goes out
	n0 = sim.air_count;
	CHECK(dmr_signal_call_alert(9991234));
	run(600);
	CHECK(air_count(n0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT) >= 1);
	csbk_rx(DMR_CSBKO_CALL_ALERT_ACK, 9996543, 9991234, 1);
	run(1000);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_OK);

	// An emergency starts here too (it is gated as data is): the hot mic keys the VFO's talk target (TG 9)
	dmr_emergency_settings_t es, es0;
	dmr_emergency_get_settings_now(&es0);
	dmr_emergency_settings_default(&es);
	es.mode = DMR_EMERGENCY_MODE_CALL;
	es.hot_mic_s = 1;
	dmr_emergency_set_settings(&es);
	run(20);
	dmr_lc_t l;
	n0 = sim.air_count;
	CHECK(dmr_emergency_start());
	run(600);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_HOT_MIC && sm.em_keyed && hw.tx_hz == 446037500);
	air_lc(n0, &l);
	CHECK(sim.air[n0].kind == DMR_BURST_VOICE_LC_HEADER && (l.svc & DMR_SVC_EMERGENCY) && l.dst == 9);
	dmr_emergency_cancel();
	run(2000);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_IDLE && !sm.em_keyed && sm.view.state == RS_IDLE);
	dmr_emergency_set_settings(&es0);
	run(20);

	// Outside the transmit bands (a receive only band): nothing on air, the transmitter never keyed
	req(RREQ_VFO_SET, 125000000);
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sm.view.ch.tx_hz == 125000000);
	n0 = sim.air_count;
	tx0 = hw.tx_calls;
	dmr_signal_call_alert(9991234);
	csbk_rx(DMR_CSBKO_CALL_ALERT, 9996543, 9999876, 1);
	run(3000);
	CHECK(air_count(n0, DMR_BURST_CSBK, 0xFF) == 0 && air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 0 &&
	      hw.tx_calls == tx0);

	// An FM VFO: no data at all
	req(RREQ_VFO_SET, 446000000);
	req(RREQ_MODE, RADIO_MODE_FM);
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started);
	n0 = sim.air_count;
	tx0 = hw.tx_calls;
	dmr_signal_call_alert(9991234);
	CHECK(!dmrDataServiceSendIP(false, 9991234, DMR_DATA_IP_OURS, DMR_DATA_IP_OURS, DMR_IP_TTL_DEFAULT,
				    DMR_IP_PROTO_UDP, l4, sizeof(l4)));
	run(3000);
	dmr_emergency_start();		// queued, then refused by the tick
	run(3000);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_FAILED && !sm.em_keyed);
	CHECK(sim.air_count == n0 && hw.tx_calls == tx0);
	dmr_emergency_cancel();

	// Leave the VFO the way the other tests expect it
	req(RREQ_VFO, 0);
	run(30000);	// any request left fails out
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}
#endif

static void test_dmr_data_glue(void)
{
	uint8_t kind;
	uint32_t dst;

	// A call alert: listen before talk, keyed like PTT, one CSBK with the MCU's CRC; the ack ends it
	dmr_start(true);
	unsigned n0 = sim.air_count;
	int tx0 = hw.tx_calls;
	CHECK(dmr_signal_call_alert(9991234));
	// On air without PTT, the view says TX like a call (#148): the strip's pill, the red LED, Home's activity line
	bool shown_tx = false, red = false, unkeyed = true, dark = true;
	uint32_t tx_ms = sm.view.last_tx_ms;
	uint8_t ts0 = sm.view.ch.timeslot, ts_asked = 0;
	for (int i = 0; i < 60; i++) {
		run(10);
		if (sm.view.state == RS_TX && sm.view.dmr.state == RDMR_TX_DATA) {
			if (ts_asked == 0) {	// a slot change while it is out waits for its end, not lost
				ts_asked = (uint8_t)(3 - ts0);
				req(RREQ_DMR_TS, ts_asked);
				CHECK(sm.view.ch.timeslot == ts0);
			}
			shown_tx = true;
			red = red || hw.led[LED_RED];
#if CONFIG_APRS
			unkeyed = unkeyed && sm.tx_no_ptt && !sm.aprs_was_tx;	// not the user's PTT: no "beacon after PTT"
#endif
			// ... but the radio's own: the screen stays as it is
			dark = dark && sm.view.tx_auto && !radio_view_wakes(&sm.view);
		}
	}
	CHECK(shown_tx && red && unkeyed && dark);
	CHECK(sm.view.last_tx_ms != 0 && sm.view.last_tx_ms != tx_ms);	// when it went out, for the debug link
	CHECK(!sm.tx_no_ptt);
	CHECK(air_count(n0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT) >= 1 && hw.tx_calls > tx0);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_SENDING && dst == 9991234);
	CHECK(sm.view.state != RS_TX && !hw.led[LED_RED]);	// back to receive once the list is out
	CHECK(ts_asked != 0 && sm.view.ch.timeslot == ts_asked);
	req(RREQ_DMR_TS, ts0);
	CHECK(sm.view.ch.timeslot == ts0);
	for (unsigned i = n0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_CSBK) {
			CHECK(sim.air[i].host_crc);
		}
	}
	csbk_rx(DMR_CSBKO_CALL_ALERT_ACK, 9996543, 9991234, 2);
	run(50);
	CHECK(sm.view.state == RS_RX && sm.view.dmr.state == RDMR_IDLE);	// the ack heard: RX, though no call
	CHECK(!hw.led[LED_GREEN]);	// green is for a call being played
	CHECK(!radio_view_wakes(&sm.view));	// nor the screen: the radio takes the ack on its own
	run(150);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_OK);
	CHECK(!(sm.view.state == RS_TX));
	run(1000);
	CHECK(sm.view.state == RS_IDLE);	// faded

	// No answer: the retries, then failed; the keying released each time
	n0 = sim.air_count;
	CHECK(dmr_signal_radio_check(9995555));
	run(30000);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_FAILED);
	CHECK(air_count(n0, DMR_BURST_CSBK, 0xFF) >= 2 && !sm.dmr_keyed);
	CHECK(sm.view.state == RS_IDLE && !sm.tx_no_ptt && !hw.led[LED_RED]);	// a failed data send leaves no TX or deny behind

	// A call alert to us: alerted, and the ack goes out through the same glue
	n0 = sim.air_count;
	csbk_rx(DMR_CSBKO_CALL_ALERT, 9996543, 9999876, 2);
	run(1000);
	CHECK(air_count(n0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT_ACK) == 1);
	// ... not to us: nothing
	n0 = sim.air_count;
	csbk_rx(DMR_CSBKO_CALL_ALERT, 9990001, 9999876, 2);
	run(50);
	CHECK(sm.view.state == RS_RX && sm.view.dmr.state == RDMR_IDLE && !hw.led[LED_GREEN]);	// others' data: RX, no call
	CHECK(!radio_view_wakes(&sm.view));
	run(1000);
	CHECK(sim.air_count == n0);
	CHECK(sm.view.state == RS_IDLE);	// faded
	// ... on another color code: not our channel, no RX
	csbk_rx(DMR_CSBKO_CALL_ALERT, 9990001, 9999876, 5);
	run(50);
	CHECK(sm.view.state == RS_IDLE);
	run(1000);

	// A group SMS: a whole packet on air (preamble, header, blocks), sent once heard clear
	n0 = sim.air_count;
	CHECK(sms_send(9, true, "hello group", false, DMR_SMS_FORMAT_ETSI_IP));
	shown_tx = red = false;
	for (int i = 0; i < 300; i++) {	// the packet shows as a transmission, red LED included, from start to end
		run(10);
		if (sm.view.state == RS_TX && sm.view.dmr.state == RDMR_TX_DATA) {
			shown_tx = true;
			red = red || hw.led[LED_RED];
		}
	}
	CHECK(shown_tx && red);
	CHECK(sms_send_state() == DMR_REQ_OK);
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) >= 1 && sim.air_count - n0 >= 3);
	CHECK(sm.view.state != RS_TX && !sm.tx_no_ptt);

	// While the user transmits voice, data waits: nothing goes out in the middle of the call
	req(RREQ_PTT, 1);
	run(300);
	CHECK((sm.view.state == RS_TX));
	CHECK(!sm.tx_no_ptt);	// the user's own voice transmission counts for the after-PTT beacon
	CHECK(!sm.view.tx_auto && radio_view_wakes(&sm.view));	// and lights the screen
	unsigned voice0 = sim.air_count;
	CHECK(dmr_signal_call_alert(9991234));
	run(600);
	CHECK(air_count(voice0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT) == 0);
	req(RREQ_PTT, 0);
	run(3000);
	CHECK(air_count(voice0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT) >= 1);
	run(30000);

	// An RX only DMR channel and an FM channel: refused, the request fails instead of keying
	chans[4].rx_only = true;
	reload_channel();
	tx0 = hw.tx_calls;
	CHECK(!dmr_signal_call_alert(9991234) || (run(3000), dmr_signal_state(&kind, &dst) == DMR_REQ_FAILED));
	CHECK(hw.tx_calls == tx0);
	chans[4].rx_only = false;
	reload_channel();
	req(RREQ_ZONE_POS, 0);		// channel 1, FM
	run(50);
	tx0 = hw.tx_calls;
	CHECK(!sms_send(9991234, false, "x", true, DMR_SMS_FORMAT_ETSI) || (run(5000), sms_send_state() == DMR_REQ_FAILED));
	CHECK(hw.tx_calls == tx0);
}

// Receive past the band table: the VFO tunes there only while the extension is on, and it never transmits
static void test_rx_ext(void)
{
	setup_codeplug();
	start();
	req(RREQ_VFO, 1);
	run(20);
	req(RREQ_VFO_SET, 500000000);
	run(20);
	CHECK(sm.view.ch.rx_hz != 500000000);	// off: refused like any frequency outside the table
	board_rx_ext_set(true);
	req(RREQ_VFO_SET, 500000000);
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 500000000);
	int tx0 = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BAND && hw.tx_calls == tx0);
	req(RREQ_PTT, 0);
	run(20);
	board_rx_ext_set(false);
	CHECK(!board_in_band(500000000, false));
	req(RREQ_RELOAD, 0);	// what turning it off in Settings does: a VFO left past the table returns to a band
	run(20);
	CHECK(sm.view.vfo && board_in_band(sm.view.ch.rx_hz, false));
}

// ---- Error paths: the radio refuses to key, a channel without a TX frequency, retuning or scanning while
// transmitting, the stack's own refusals, targets that end with their channel, stored emergency settings ----
static void test_error_paths(void)
{
	// The squelch threshold's ends
	CHECK(radio_sm_squelch_open_below(0) == 255);
	CHECK(radio_sm_squelch_open_below(200) == radio_sm_squelch_open_below(RADIO_SQUELCH_MAX));

	// FM: no TX frequency, and a transmitter that won't key
	setup_codeplug();
	chans[1].tx_hz = 0;
	start();
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_FREQ && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE);
	chans[1].tx_hz = 146520000;
	reload_channel();
	req(RREQ_ZONE_POS, 0);
	run(20);
	tx_fail = true;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REFUSED);
	run(RADIO_LED_DENY_STEPS * RADIO_LED_DENY_STEP_MS);
	CHECK(!hw.led[LED_RED]);	// after the deny flash: dark, nothing on air
	req(RREQ_PTT, 0);
	// A hardware option still unset or not applied: the refusal names it, never the band
	for (uint8_t st = HWOPT_ST_ASK; st <= HWOPT_ST_RESTART; st++) {
		fake_hwopt_st = st;
		req(RREQ_PTT, 1);
		CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_HWOPT);
		req(RREQ_PTT, 0);
	}
	// The core refuses it itself, even where the board would key (a restart due, the old choice still confirmed)
	tx_fail = false;
	for (uint8_t st = HWOPT_ST_ASK; st <= HWOPT_ST_RESTART; st++) {
		fake_hwopt_st = st;
		int calls = hw.tx_calls;
		req(RREQ_PTT, 1);
		CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_HWOPT && hw.tx_calls == calls);
		req(RREQ_PTT, 0);
	}
	fake_hwopt_st = HWOPT_ST_HIDDEN;
	tx_fail = false;

	// No retuning while transmitting: channel, zone, VFO and scan requests wait for the release
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	uint16_t pos = sm.view.zone_pos;
	uint16_t zone = sm.view.zone_index;
	req(RREQ_CH_STEP, 1);
	req(RREQ_ZONE_POS, 3);
	req(RREQ_ZONE, 1);
	req(RREQ_VFO, 1);
	req(RREQ_SCAN, RSCAN_ZONE);
	run(50);
	CHECK(sm.view.state == RS_TX && sm.view.zone_pos == pos && sm.view.zone_index == zone && !sm.view.vfo);
	CHECK(!(sm.view.scan != 0));
	CHECK(hw.tx_calls == 1 && hw.tx_hz == 146520000);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);
	run(20);
	CHECK(sm.view.zone_pos != pos);
	// PTT while scanning: the scan stops where it is and the radio transmits there
	req(RREQ_ZONE_POS, 0);
	run(20);
	req(RREQ_SCAN, RSCAN_ZONE);
	run(400);
	CHECK(sm.view.scan != 0);
	req(RREQ_PTT, 1);
	run(20);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == sm.view.ch.tx_hz);
	req(RREQ_PTT, 0);
	run(20);
	req(RREQ_SCAN, RSCAN_STOP);
	run(20);
	CHECK(sm.view.scan == 0);

	// DMR: the transmitter won't key; the stack reports a rejection
	dmr_start(true);
	tx_fail = true;
	req(RREQ_PTT, 1);
	run(100);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REFUSED && sim.tx_begins == 0);
	req(RREQ_PTT, 0);
	run(100);
	tx_fail = false;	// the core refuses a pending option even where the board would key
	for (uint8_t st = HWOPT_ST_ASK; st <= HWOPT_ST_RESTART; st++) {
		fake_hwopt_st = st;
		req(RREQ_PTT, 1);
		run(100);
		CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_HWOPT && sim.tx_begins == 0);
		req(RREQ_PTT, 0);
		run(100);
	}
	fake_hwopt_st = HWOPT_ST_HIDDEN;
	sim.reject_next = true;
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REJECTED);
	req(RREQ_PTT, 0);
	run(1000);
	sim.reject_next = false;
	CHECK(sm.view.state != RS_TX && !sm.dmr_keyed);

	// A talk target, the hear all latch and the channel's own target: chosen ones end with their channel
	req(RREQ_DMR_TARGET, 9991234 | (1 << 24));
	req(RREQ_DMR_HEAR_ALL, 1);
	run(20);
	CHECK(sm.view.dmr.target_manual && sm.view.dmr.hear_all_latch);
	req(RREQ_ZONE_POS, 0);		// FM
	run(50);
	req(RREQ_ZONE_POS, 3);		// back on the DMR channel
	run(50);
	CHECK(!sm.view.dmr.target_manual && !sm.view.dmr.hear_all_latch && sm.view.dmr.target_id == 201);
	// Targets out of range or off DMR: ignored
	req(RREQ_DMR_TARGET, 0);
	run(20);
	CHECK(!sm.view.dmr.target_manual);
	req(RREQ_ZONE_POS, 0);
	run(50);
	req(RREQ_DMR_TARGET, 9);
	run(20);
	CHECK(!sm.view.dmr.target_manual);

	// Stored emergency settings are what the emergency engine starts with
	settings_t *st = settings_begin();
	st->em_stored = 1;
	st->em_mode = DMR_EMERGENCY_MODE_ALARM_CALL;
	st->em_dst = 9999999;
	st->em_flags = SETTINGS_EM_DST_PRIVATE | SETTINGS_EM_ETSI_FID | SETTINGS_EM_NO_TONE;
	st->em_alarm_tries = 3;
	st->em_hot_mic_s = 15;
	settings_end(now);
	boot();				// (start() would load the defaults: no flash here)
	run(50);
	dmr_emergency_settings_t es;
	dmr_emergency_get_settings_now(&es);
	CHECK(es.dst == 9999999 && es.dst_private && es.etsi_fid && !es.rx_tone && es.alarm_tries == 3 &&
	      es.hot_mic_s == 15);
	st = settings_begin();
	st->em_stored = 0;
	settings_end(now);
}

// Factory reset on a flash: the defaults saved at once, the native area emptied when ours, a toast when either
// fails (the settings can't be written; the native area won't erase), never the open-firmware codeplug or calibration
static const hal_flash_map_t reset_map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x30000u,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
	.native_addr = 0x30000u, .native_size = 0x4000u,
};

static void test_factory_reset(void)
{
	settings_t sv;
	sim_flash_create(0x40000u, &reset_map);
	CHECK(storage_init());
	settings_init();
	dmr_start(true);
	memset(&sim_flash_data[0x20000u], 0x42, 64);		// an open-firmware codeplug byte or two
	memset(&sim_flash_data[0x10000u], 0x24, 64);		// the calibration copy
	static const uint8_t magic[4] = { 0x4F, 0x44, 0x52, 0x4E };
	CHECK(sim_flash.write(reset_map.native_addr, magic, 4));
	settings_begin()->squelch = 2;
	settings_end(now);
	CHECK(settings_flush());
	n_ev = 0;
	CHECK(ui_svc_factory_reset());
	storage_job_poll();
	settings_read(&sv);
	CHECK(n_ev == 0 && sv.squelch == SETTINGS_UNSET8 && sim_flash_data[reset_map.native_addr] == 0xFF);
	CHECK(sim_flash_data[0x20000u] == 0x42 && sim_flash_data[0x10000u] == 0x24);
	settings_init();				// what the next boot finds
	settings_read(&sv);
	CHECK(sv.squelch == SETTINGS_UNSET8);
	// The settings can't be written (a worn ring): the toast says so
	settings_begin()->squelch = 4;
	settings_end(now);
	CHECK(settings_flush());		// stored: the reset has something to write
	sim_flash_bad_range(reset_map.settings_addr, reset_map.settings_size);
	CHECK(ui_svc_factory_reset());
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(tr_en(last_ev.why), "settings") != NULL);
	sim_flash_bad_range(0, 0);
	// The native area won't erase (ours, then the flash cut out under it)
	CHECK(sim_flash.write(reset_map.native_addr, magic, 4));
	CHECK(sim_flash.write(reset_map.native_addr + 0x2000u, magic, 4));
	settings_begin()->squelch = 5;
	settings_end(now);
	CHECK(settings_flush());
	n_ev = 0;
	CHECK(ui_svc_factory_reset());
	sim_flash_cut_after(440);	// the settings records (page 1: 180 + 4 bytes, page 0: 252 + 4) go in, the native erase fails
	storage_job_poll();
	sim_flash_power_on();
	CHECK(n_ev == 1 && strstr(tr_en(last_ev.why), "native") != NULL);
	sim_flash.map = NULL;
	storage_init();			// back to no flash for anything after
	settings_init();
	sim_flash_free();
}

// DMR is TDMA: the radio's reading alternates between the signal and the floor. The published level holds the peak
static void test_dmr_rssi_hold(void)
{
	dmr_start(false);
	const radio_dmr_view_t *d = &sm.view.dmr;
	burst_rssi = 0;				// a modem that reports no level per burst
	rssi_alt = true;
	int16_t lo = 0;
	int16_t hi = -200;
	uint8_t lc[9];
	mk_lc(lc, true, 201, 9991234);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 2, false);
		run(60);
	}
	for (int sf = 0; sf < 3; sf++) {
		for (int i = 0; i < 6; i++) {
			burst_voice((uint8_t)i, 2, i == 5 ? lc : NULL);
			for (int k = 0; k < 12; k++) {	// 60 ms in 5 ms steps, sampling the published value
				run(5);
				if (sf > 0) {
					lo = sm.view.rssi_dbm < lo ? sm.view.rssi_dbm : lo;
					hi = sm.view.rssi_dbm > hi ? sm.view.rssi_dbm : hi;
				}
			}
		}
	}
	CHECK(d->state == RDMR_RX);
	CHECK(lo >= -70 - RADIO_RSSI_DEADBAND_DB && hi <= -70);	// never the floor while the call runs
	CHECK(d->rssi_dbm >= -70 - RADIO_RSSI_DEADBAND_DB && d->rssi_dbm <= -70);	// the call's level is the held one
	burst_lc(lc, 2, true);
	run(60);
	CHECK(d->state == RDMR_IDLE && d->rssi_dbm <= -70 && d->rssi_dbm != 0);	// the last call keeps its level
	run(RADIO_RSSI_HOLD_MS + 2 * RADIO_POLL_MS);
	CHECK(sm.view.rssi_dbm == -70 || sm.view.rssi_dbm == -125);
	rssi_alt = false;
	hw.st.rssi_dbm = -125;			// the signal is gone: the floor shows within the window
	run(RADIO_RSSI_HOLD_MS + RADIO_POLL_MS);
	CHECK(sm.view.rssi_dbm == -125);
	burst_rssi = -85;

	// A modem that reports a level: its value wins for the call
	rssi_alt = true;
	hear_call(true, 201, 9991234, 2, 1, false);
	CHECK(d->state == RDMR_RX && d->rssi_dbm == -85);
	rssi_alt = false;
}

// FM keeps the raw reading: no hold
static void test_fm_rssi_raw(void)
{
	setup_codeplug();
	start();
	for (int i = 0; i < 20; i++) {
		hw.st.rssi_dbm = i % 2 ? -125 : -70;
		poll_for(RADIO_POLL_MS);
		CHECK(sm.view.rssi_dbm == hw.st.rssi_dbm);
	}
}

#if CONFIG_SFR
// Standalone SFR (docs/spec/sfr.md 4): a channel's SFR role in the codeplug. Repeater: the runner takes the modem
// instead of the call stack and repeats the inbound slot on the other one. User: the call stack on the inbound slot
static void test_sfr_roles(void)
{
	dmr_start(true);
	CHECK(sm.view.dmr.active && !sm.sfr_rpt && sm.view.dmr.sfr_role == SFR_ROLE_OFF);

	// Repeater, inbound TS1, TS framing (Auto): the keying armed on the channel's frequency, the stack stopped
	int tx0 = hw.tx_calls, off0 = hw.tx_off_calls;
	chans[4].sfr_flag1 = SFR_CHANNEL_FLAG1_ENABLED;
	chans[4].sfr_a = SFR_ROLE_RPT_STANDARD;
	reload_channel();
	CHECK(sm.sfr_rpt && sm.view.dmr.sfr_rpt && !sm.view.dmr.active && !sm.dmr_running);
	CHECK(sm.view.dmr.sfr_role == SFR_ROLE_RPT_STANDARD);
	CHECK(sim.started && sim.cb != NULL && sim.locked_ts == 0 && sim.cc_filter == 2 && !sim.cfg.repeater);
	CHECK(hw.tx_calls > tx0 && hw.tx_hz == 438500000 && sm.dmr_keyed);

	// A call on the inbound slot: repeated on TS2 with the TS2 direct mode sync; the radio shows it as receiving
	unsigned air0 = sim.air_count;
	uint8_t lc[9];
	mk_lc(lc, true, 201, 9991999);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 2, false);
		run(60);
	}
	CHECK(sm.view.dmr.sfr_busy && sm.view.state == RS_RX);
	for (int i = 0; i < 6; i++) {
		burst_voice((uint8_t)i, 2, i == 5 ? lc : NULL);
		run(60);
	}
	CHECK(sm.view.dmr.sfr_keyed && sim.tx_begins >= 1 && sim.tx_listens >= 1);
	burst_lc(lc, 2, true);
	run(400);
	unsigned voice = 0;
	for (unsigned i = air0; i < sim.air_count; i++) {
		CHECK(sim.air[i].sync == DMR_SYNC_TS2);
		if (sim.air[i].kind == DMR_BURST_VOICE) {
			voice++;
		}
	}
	CHECK(voice == 6);
	radio_sfr_view_t v;
	radio_sfr_get_view(&v);
	CHECK(v.on && v.stats.rx_repeated >= 10 && v.inbound_slot == 0 && v.framing == SFR_FRAMING_TS);
	run(3000);		// the hang ends
	CHECK(!sm.view.dmr.sfr_keyed && !sm.view.dmr.sfr_busy && sm.view.state == RS_IDLE);

	// The RSSI shown is the held peak, as with the call stack: half the readings fall in empty slots or our own
	rssi_alt = true;
	run(600);
	CHECK(sm.view.rssi_dbm == -70);
	rssi_alt = false;

	// PTT does nothing on the repeater
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_SFR_RPT);
	req(RREQ_PTT, 0);
	run(50);
	CHECK(sm.view.state == RS_IDLE && sm.sfr_rpt);

	// Hotspot mode takes the modem from the role; leaving it starts the role again
	req(RREQ_HS_ENTER, 0);
	CHECK(sm.hotspot && !sm.sfr_rpt && !sm.dmr_keyed && !sim.started);
	req(RREQ_HS_LEAVE, 0);
	run(50);
	CHECK(!sm.hotspot && sm.sfr_rpt && sm.dmr_keyed);

	// A scan visits the channel to listen: the role waits, and starts again once the scan ends there
	req(RREQ_SCAN, RSCAN_ZONE);
	bool visited = false;
	for (int i = 0; i < 300 && sm.view.scan != 0; i++) {
		run(20);
		CHECK(!sm.sfr_rpt);
		visited |= sm.view.zone_pos == 3;
	}
	CHECK(sm.view.scan != 0 && visited);
	req(RREQ_SCAN, RSCAN_CANCEL);
	run(50);
	CHECK(sm.view.scan == 0 && sm.view.zone_pos == 3 && sm.sfr_rpt && sm.dmr_keyed);

	// The board won't key: an ordinary channel, not a repeater that can't send
	tx_fail = true;
	reload_channel();
	CHECK(!sm.sfr_rpt && !sm.dmr_keyed && sm.view.dmr.active);
	tx_fail = false;
	reload_channel();
	CHECK(sm.sfr_rpt && sm.dmr_keyed);

	// TX inhibit: an ordinary channel again (the keying released, the stack back)
	radio_settings_t set = sm.set;
	set.tx_inhibit = true;
	radio_sm_set_settings(&sm, &set, now);
	run(50);
	CHECK(!sm.sfr_rpt && sm.view.dmr.active && hw.tx_off_calls > off0 && !sm.dmr_keyed);
	run(500);
	CHECK(!sm.sfr_rpt && sm.sfr_denied);
	set.tx_inhibit = false;	// the role back by itself, no channel change
	radio_sm_set_settings(&sm, &set, now);
	run(50);
	CHECK(sm.sfr_rpt && !sm.sfr_denied && sm.dmr_keyed && !sm.view.dmr.active);
	set.tx_inhibit = true;	// a repeater channel selected while inhibited waits, then starts
	radio_sm_set_settings(&sm, &set, now);
	run(50);
	reload_channel();
	CHECK(!sm.sfr_rpt && sm.sfr_denied && sm.view.dmr.active);
	set.tx_inhibit = false;
	radio_sm_set_settings(&sm, &set, now);
	run(50);
	CHECK(sm.sfr_rpt);

	// A repeater role on a two-frequency channel is refused: the channel is an ordinary repeater channel
	chans[4].tx_hz = 433500000;
	reload_channel();
	CHECK(!sm.sfr_rpt && sm.view.dmr.active && sm.view.dmr.sfr_role == SFR_ROLE_OFF && sm.view.dmr.rmo);
	chans[4].tx_hz = 438500000;

	// User, inbound TS2 (the channel says TS1): PTT on TS2 with the TS2 direct mode sync, both slots heard
	chans[4].sfr_a = SFR_ROLE_USER_STANDARD | 0x20;
	reload_channel();
	CHECK(!sm.sfr_rpt && sm.view.dmr.active && sm.view.dmr.sfr_role == SFR_ROLE_USER_STANDARD);
	CHECK(dmr_call_view()->ts == 2);
	air0 = sim.air_count;
	tx_once();
	CHECK(sim.air_count > air0 + 4 && sim.last_ts == 2);
	for (unsigned i = air0; i < sim.air_count; i++) {
		CHECK(sim.air[i].sync == DMR_SYNC_TS2);
	}

	// No SFR flag: the bytes are not read
	chans[4].sfr_flag1 = 0;
	reload_channel();
	CHECK(sm.view.dmr.sfr_role == SFR_ROLE_OFF && dmr_call_view()->ts == 1);
	chans[4].sfr_a = 0;
}

#if CONFIG_VFO_DMR
// The VFO's SFR role (RREQ_VFO_SFR, sfr.md 4): a channel's SFR bytes, taken as a channel's and saved with the VFO
static void test_sfr_vfo(void)
{
	dmr_start(true);
	req(RREQ_VFO_SFR, SFR_ROLE_RPT_STANDARD);	// on a channel: refused (its role is the codeplug's)
	run(50);
	CHECK(!sm.sfr_rpt && sm.view.dmr.sfr_role == SFR_ROLE_OFF);
	req(RREQ_VFO_COPY, 0);				// channel 4: 438.5 MHz simplex, DMR, CC 2; no role
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sm.view.dmr.sfr_role == SFR_ROLE_OFF);

	// Repeater: the runner takes the modem on the VFO's frequency, the stack stops; saved
	req(RREQ_VFO_SFR, SFR_ROLE_RPT_STANDARD);
	run(50);
	CHECK(sm.sfr_rpt && sm.dmr_keyed && !sm.view.dmr.active && sm.view.dmr.sfr_role == SFR_ROLE_RPT_STANDARD);
	CHECK(hw.tx_hz == 438500000 && sim.cc_filter == 2);
	settings_t st;
	settings_read(&st);
	CHECK(st.vfo_sfr == SFR_ROLE_RPT_STANDARD);

	// A new color code: the repeater takes it
	req(RREQ_DMR_CC, 5);
	run(50);
	CHECK(sm.sfr_rpt && sim.cc_filter == 5);
	// A split: an ordinary repeater channel, the stack back; one frequency again: the role again
	req(RREQ_VFO_TX, 433500000);
	run(50);
	CHECK(!sm.sfr_rpt && !sm.dmr_keyed && sm.view.dmr.active && sm.view.dmr.sfr_role == SFR_ROLE_OFF);
	req(RREQ_VFO_TX, 0);
	run(50);
	CHECK(sm.sfr_rpt && sm.dmr_keyed && hw.tx_hz == 438500000);

	// Kept over a restart
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.sfr_rpt && sm.view.dmr.sfr_role == SFR_ROLE_RPT_STANDARD);
	// The same role again: no retune, the repeater keeps the modem
	int offs = hw.tx_off_calls;
	req(RREQ_VFO_SFR, SFR_ROLE_RPT_STANDARD);
	run(50);
	CHECK(sm.sfr_rpt && hw.tx_off_calls == offs);
#if CONFIG_VFO_FM
	// FM: no role in force, the stored one kept for DMR
	req(RREQ_MODE, RADIO_MODE_FM);
	run(50);
	CHECK(!sm.sfr_rpt && sm.view.dmr.sfr_role == SFR_ROLE_OFF && sm.vfo_ch.sfr_a == SFR_ROLE_RPT_STANDARD);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sm.sfr_rpt && sm.view.dmr.sfr_role == SFR_ROLE_RPT_STANDARD);
#endif

	// User, inbound TS2: the call stack on TS2
	req(RREQ_VFO_SFR, SFR_ROLE_USER_STANDARD | 0x20);
	run(50);
	CHECK(!sm.sfr_rpt && sm.view.dmr.active && sm.view.dmr.sfr_role == SFR_ROLE_USER_STANDARD);
	CHECK(dmr_call_view()->ts == 2);
	req(RREQ_VFO_SFR, SFR_ROLE_NUM_STORED);	// not a stored role: refused
	run(50);
	CHECK(sm.view.dmr.sfr_role == SFR_ROLE_USER_STANDARD);

	// Off: the VFO's own slot again, saved
	req(RREQ_VFO_SFR, 0);
	run(50);
	CHECK(sm.view.dmr.sfr_role == SFR_ROLE_OFF && sm.vfo_ch.sfr_flag1 == 0 && dmr_call_view()->ts == 1);
	settings_read(&st);
	CHECK(st.vfo_sfr == 0);

	// Copy to VFO brings the channel's role
	req(RREQ_VFO, 0);
	chans[4].sfr_flag1 = SFR_CHANNEL_FLAG1_ENABLED;
	chans[4].sfr_a = SFR_ROLE_RPT_STANDARD;
	reload_channel();
	CHECK(sm.sfr_rpt && !sm.view.vfo);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && sm.sfr_rpt && sm.vfo_ch.sfr_a == SFR_ROLE_RPT_STANDARD);
	chans[4].sfr_flag1 = 0;
	chans[4].sfr_a = 0;
	req(RREQ_VFO_SFR, 0);
	run(50);
	CHECK(!sm.sfr_rpt);
}
#endif
#if CONFIG_SFR_CWID
// A call through the repeater role: the repeater transmits, its hang ends and it goes quiet
static void sfr_call_through(void)
{
	uint8_t lc[9];
	mk_lc(lc, true, 201, 9991999);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 2, false);
		run(60);
	}
	for (int i = 0; i < 6; i++) {
		burst_voice((uint8_t)i, 2, i == 5 ? lc : NULL);
		run(60);
	}
	burst_lc(lc, 2, true);
	run(60);
}

// The standalone SFR repeater's CW ID (docs/spec/sfr.md 4.6): the schedule, the callsign, and through the radio core
// an ID after traffic, in FM on the channel's frequency, then the repeater role again
static void test_sfr_cwid(void)
{
	// The schedule: the first ID once the repeater has transmitted and gone quiet, then at most one per interval,
	// and none without new traffic
	radio_cwid_t c;
	memset(&c, 0, sizeof(c));
	CHECK(radio_cwid_interval_ms(0) == 0 && radio_cwid_interval_ms(1) == 300000u);
	CHECK(radio_cwid_interval_ms(4) == 1800000u && radio_cwid_interval_ms(RADIO_CWID_CODES) == 0);
	CHECK(!radio_cwid_due(&c, 600000u, false, 1000));	// no traffic yet
	CHECK(!radio_cwid_due(&c, 600000u, true, 2000));	// transmitting
	CHECK(!radio_cwid_due(&c, 600000u, false, 3999));	// not quiet long enough
	CHECK(!radio_cwid_due(&c, 0, false, 4000));		// off
	CHECK(radio_cwid_due(&c, 600000u, false, 4000));	// the first one at once
	radio_cwid_sent(&c, 4000);
	CHECK(!radio_cwid_due(&c, 600000u, false, 700000));	// no traffic since
	CHECK(!radio_cwid_due(&c, 600000u, true, 10000));
	CHECK(!radio_cwid_due(&c, 600000u, false, 600000));	// traffic, the interval not over
	CHECK(radio_cwid_due(&c, 600000u, false, 604000));

	// The callsign: the name's first word, an SSID dropped, upper case, cut to fit
	char cs[8];
	CHECK(radio_cwid_callsign("  q4abc-7 Base", cs, sizeof(cs)) == 5 && strcmp(cs, "Q4ABC") == 0);
	CHECK(radio_cwid_callsign("W1AW/P", cs, sizeof(cs)) == 4 && strcmp(cs, "W1AW") == 0);
	CHECK(radio_cwid_callsign("QE3ABC", cs, sizeof(cs)) == 6 && strcmp(cs, "QE3ABC") == 0);
	// Only something that clearly is one: 4 to 7 letters and digits, at least one of each
	CHECK(radio_cwid_callsign("Test Radio", cs, sizeof(cs)) == 0 && cs[0] == '\0');	// no digit
	CHECK(radio_cwid_callsign("1234", cs, sizeof(cs)) == 0 && cs[0] == '\0');		// no letter
	CHECK(radio_cwid_callsign("Q1A", cs, sizeof(cs)) == 0);				// too short
	CHECK(radio_cwid_callsign("QD2ABCD", cs, sizeof(cs)) == 7 && strcmp(cs, "QD2ABCD") == 0);
	CHECK(radio_cwid_callsign("QD2ABCDE", cs, sizeof(cs)) == 0 && cs[0] == '\0');	// too long
	CHECK(radio_cwid_callsign("Q4.ABC", cs, sizeof(cs)) == 0);				// not letters and digits
	CHECK(radio_cwid_callsign("Q4ABC", cs, 5) == 0);					// no room
	CHECK(radio_cwid_callsign("", cs, sizeof(cs)) == 0 && cs[0] == '\0');

	// The radio core: a repeater role with the ID every 5 min and the name "q0bis"
	dmr_start(true);
	chans[4].sfr_flag1 = SFR_CHANNEL_FLAG1_ENABLED;
	chans[4].sfr_a = SFR_ROLE_RPT_STANDARD;
	reload_channel();
	char name0[sizeof(sm.radio_name)];
	memcpy(name0, sm.radio_name, sizeof(name0));
	strcpy(sm.radio_name, "q0bis");
	radio_settings_t set = sm.set;
	set.sfr_cwid = 1;
	radio_sm_set_settings(&sm, &set, now);
	CHECK(sm.set.sfr_cwid == 1);
	CHECK(sm.sfr_rpt && sm.dmr_keyed);
	run(5000);
	CHECK(sm.cw_phase == 0 && sm.view.dmr.sfr_cwid_sent == 0);	// nothing transmitted, no ID

	// A call, the hang, two quiet seconds: the ID in FM on the channel's frequency, the runner stopped
	sfr_call_through();
	memset(&tone, 0, sizeof(tone));
	int modes = hw.set_mode_calls;
	for (int i = 0; i < 2000 && sm.cw_phase == 0; i++) {
		run(5);
	}
	radio_sfr_view_t v;
	radio_sfr_get_view(&v);
	CHECK(v.stats.rx_repeated >= 10);	// (the runner's counters, kept while it is stopped)
	CHECK(sm.cw_phase != 0 && sm.view.dmr.sfr_cw && sm.sfr_rpt && !v.on && !sm.dmr_keyed);
	CHECK(hw_mode == RADIO_MODE_FM && hw.set_mode_calls == modes + 1 && hw.tx_hz == 438500000 && tone.hz == 800);
	req(RREQ_PTT, 1);		// PTT still refused
	CHECK(sm.view.tx_deny == TXDENY_SFR_RPT && sm.cw_phase != 0);
	req(RREQ_PTT, 0);
	// "Q0BIS": 18 elements; 61 units at 20 wpm (3660 ms) plus the lead and tail
	run(4500);
	CHECK(tone.changes == 36 && tone.hz == 0);
	CHECK(sm.cw_phase == 0 && !sm.view.dmr.sfr_cw && sm.view.dmr.sfr_cwid_sent == 1 && sm.view.dmr.sfr_cwid_cut == 0);
	radio_sfr_view_t v0 = v;
	radio_sfr_get_view(&v);
	CHECK(hw_mode == RADIO_MODE_DMR && hw.set_mode_calls == modes + 2 && v.on && sm.sfr_rpt && sm.dmr_keyed);
	CHECK(v.stats.rx_repeated >= 10 && v.stats.rx_repeated == v0.stats.rx_repeated);	// the counters carried on
	run(100);
	CHECK(sm.view.state == RS_IDLE);

	// No traffic: no ID. Traffic inside the interval: the ID waits for its end
	run(20000);
	CHECK(sm.view.dmr.sfr_cwid_sent == 1 && sm.cw_phase == 0);
	sfr_call_through();
	run(10000);
	CHECK(sm.view.dmr.sfr_cwid_sent == 1 && sm.cw_phase == 0);
	for (int i = 0; i < 300 && sm.cw_phase == 0; i++) {
		run(1000);
	}
	CHECK(sm.cw_phase != 0 && hw_mode == RADIO_MODE_FM);

	// The channel retuned in the middle: the ID cut, an ordinary DMR channel
	chans[4].sfr_flag1 = 0;
	reload_channel();
	CHECK(sm.cw_phase == 0 && !sm.view.dmr.sfr_cw && tone.hz == 0 && sm.view.dmr.sfr_cwid_cut == 1);
	CHECK(!sm.sfr_rpt && sm.view.dmr.active && hw_mode == RADIO_MODE_DMR);

	// No callsign in the radio name (a personal name): nothing keyed, counted as skipped, the role goes on
	strcpy(sm.radio_name, "Base 1");
	chans[4].sfr_flag1 = SFR_CHANNEL_FLAG1_ENABLED;
	reload_channel();
	CHECK(sm.sfr_rpt);
	sfr_call_through();
	run(6000);
	CHECK(sm.cw_phase == 0 && sm.view.dmr.sfr_cwid_cut == 2 && sm.sfr_rpt && hw_mode == RADIO_MODE_DMR);

	// A radio with a second transmitter (tx_split): on with the role, off for the ID, on again after it, and off
	// when the role ends. Left on, every later DMR transmission would go to the second transmitter
	chans[4].sfr_flag1 = 0;
	reload_channel();
	strcpy(sm.radio_name, "q0bis");
	sim.split_offer = true;
	chans[4].sfr_flag1 = SFR_CHANNEL_FLAG1_ENABLED;
	reload_channel();
	CHECK(sm.sfr_rpt && sim.split);
	sfr_call_through();
	for (int i = 0; i < 2000 && sm.cw_phase == 0; i++) {
		run(5);
	}
	CHECK(sm.cw_phase != 0 && !sim.split);
	run(4500);
	CHECK(sm.cw_phase == 0 && sm.sfr_rpt && sim.split);
	chans[4].sfr_flag1 = 0;
	reload_channel();
	CHECK(!sm.sfr_rpt && !sim.split);
	sim.split_offer = false;

	memcpy(sm.radio_name, name0, sizeof(name0));
	set = sm.set;
	set.sfr_cwid = 0;
	radio_sm_set_settings(&sm, &set, now);
	chans[4].sfr_flag1 = 0;
	chans[4].sfr_a = 0;
	reload_channel();
	CHECK(!sm.sfr_rpt);
}
#endif
#endif

// RADIO op 35, the test TX limit (tx_limit.h): flags, a count, the frequencies; bad arguments change nothing
static void test_tx_limit_op(void)
{
	tx_limit_info_t l;
	uint32_t hz[TX_LIMIT_MAX];
	CHECK(debuglink_is_feature_op(DL_OP_TX_LIMIT));
	CHECK(debuglink_radio_feature_op(DL_OP_TX_LIMIT, (const uint8_t[]){ 1, 0 }, 2) == DL_ST_OK);
	tx_limit_get(&l, NULL, 0);
	CHECK(l.on && l.block && l.n == 0);
	CHECK(debuglink_radio_feature_op(DL_OP_TX_LIMIT, (const uint8_t[]){ 0, 2, 0xE4, 0x5B, 0x81, 0x1B, 0xD0, 0x2E,
									  0x96, 0x1A }, 10) == DL_ST_OK);
	tx_limit_get(&l, hz, TX_LIMIT_MAX);
	CHECK(l.on && !l.block && l.n == 2 && hz[0] == 461462500u && hz[1] == 446050000u);
	CHECK(tx_limit_allows(461462500u) && !tx_limit_allows(461475000u));
	CHECK(debuglink_radio_feature_op(DL_OP_TX_LIMIT, (const uint8_t[]){ 0, 2, 0, 0, 0, 0 }, 6) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_TX_LIMIT, (const uint8_t[]){ 2, 0 }, 2) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_TX_LIMIT, (const uint8_t[]){ 0, TX_LIMIT_MAX + 1 }, 2) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_TX_LIMIT, (const uint8_t[]){ 0 }, 1) == DL_ST_BAD_ARG);
	tx_limit_get(&l, NULL, 0);
	CHECK(l.n == 2 && !l.block);
	CHECK(debuglink_radio_feature_op(DL_OP_TX_LIMIT, (const uint8_t[]){ 0, 0 }, 2) == DL_ST_OK);
	tx_limit_get(&l, NULL, 0);
	CHECK(!l.on && tx_limit_allows(461475000u));
}

// The knob's percent as a step of the volume scale (the bar the UI shows when the knob turns): 0 only when silent,
// the ends exact, every step round-trips, and never a step back as the knob goes up
static void test_volume_step_of(void)
{
	static const struct { uint8_t pct, step; } ep[] = {
		{ 0, 0 }, { 1, 1 }, { 3, 1 }, { 9, 1 }, { 10, 2 }, { 50, 8 }, { 96, 15 }, { 97, 16 }, { 100, 16 }, { 101, 16 },
		{ 255, 16 },
	};
	for (size_t i = 0; i < sizeof ep / sizeof ep[0]; i++)
		CHECK(radio_volume_step_of(ep[i].pct) == ep[i].step);
	for (uint8_t s = 0; s <= RADIO_VOLUME_MAX; s++)
		CHECK(radio_volume_step_of(radio_volume_percent(s)) == s);
	for (int p = 1; p <= 255; p++) {
		uint8_t a = radio_volume_step_of((uint8_t)(p - 1)), b = radio_volume_step_of((uint8_t)p);
		CHECK(b >= a && b - a <= 1 && b >= 1 && b <= RADIO_VOLUME_MAX);
	}
}

#if CONFIG_AIR && CONFIG_SCAN
// Run the scan (or dual watch) for up to ms: the stops it makes on channel number (visits), and whether it holds there.
// Returns at the hold
static bool scan_holds_on(uint16_t number, uint32_t ms, uint16_t *visits)
{
	bool was = false;
	*visits = 0;
	for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
		run(RADIO_POLL_MS);
		bool here = !sm.view.vfo && sm.view.channel_number == number;
		if (here && !was) {
			(*visits)++;
		}
		was = here;
		if (here && sm.view.scan_phase == SCAN_PH_HOLD) {
			return true;
		}
	}
	return false;
}
#endif

#if CONFIG_P25
// ---- P25 conventional in the radio core (docs/spec/p25.md 7): the far radio is a P25 engine on the other fake
// provider; it transmits calls the radio under test hears, and hears what the radio sends ----
#include "p25/p25Voice.h"

static p25_engine_t far_eng;
static bool far_running, far_tx;
static struct {
	uint8_t starts, ends;
	p25_call_t call;
	uint16_t voice;
} far_heard;
static void far_start(const p25_call_t *c, void *ctx) { (void)ctx; far_heard.starts++; far_heard.call = *c; }
static void far_update(const p25_call_t *c, void *ctx) { (void)ctx; far_heard.call = *c; }
static void far_voice(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx) { (void)imbe; (void)errs; (void)ctx; far_heard.voice++; }
static void far_end(void *ctx) { (void)ctx; far_heard.ends++; }
static const p25_engine_hooks_t far_hooks = { .rx_start = far_start, .rx_update = far_update, .rx_voice = far_voice,
						.rx_end = far_end };

static void far_step(void)
{
	if (far_tx && p25_engine_tx_state(&far_eng) == AIR_TX_DONE) {
		p25_engine_tx_clear(&far_eng);
		far_tx = false;
	}
}

// The far radio on the channel: it sends on nac, as unit, to a talk group or a unit (dst); it hears every NAC
static void far_begin(uint16_t nac, uint32_t unit, bool unit_call, uint32_t dst)
{
	p25_engine_cfg_t c = { .rx_nac = P25_NAC_ANY, .tx_nac = nac, .unit_id = unit, .silence = imbe_silence,
			       .talkaround = true };
	if (unit_call) {
		c.tx_lc.lcf = P25_LCF_UNIT;
		c.tx_lc.dst = dst;
	} else {
		c.tx_lc.lcf = P25_LCF_GROUP;
		c.tx_lc.tgid = (uint16_t)dst;
	}
	if (far_running) {
		p25_engine_end(&far_eng);
	}
	memset(&far_heard, 0, sizeof(far_heard));
	far_tx = false;
	far_running = p25_engine_begin(&far_eng, &fake_air1, &c, &far_hooks, NULL);
	CHECK(far_running);
}

static void far_stop(void)
{
	if (far_running) {
		p25_engine_end(&far_eng);
		far_running = far_tx = false;
	}
}

// The far radio keys, holds for ms, lets go and runs until its terminator is out
static void far_call(uint32_t ms)
{
	CHECK(p25_engine_tx_prepare(&far_eng));
	CHECK(p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(ms);
	p25_engine_tx_stop(&far_eng);
	for (int i = 0; i < 100 && far_tx; i++) {
		run(10);
	}
	CHECK(!far_tx);
}

static void p25_view_text(char *buf, size_t cap)
{
	dl_text_t t;
	dl_text_init(&t, buf, cap);
	debuglink_view_p25(&sm.view, &t);
}

static void setup_codeplug_p25(void)
{
	setup_codeplug();
	add(3, "P25 simp", 446100000, RADIO_MODE_P25, false, 2);	// zone Local, position 2
	radio_p25_reset_stats();
	p25VoiceResetStats();
	air_nofit = false;
	lend_refuse = false;
	memset(fair, 0, sizeof(fair));
}

// The radio's settings back to the defaults (they are RAM only and outlive radio_sm_init here)
static void p25_defaults(void)
{
	radio_p25_settings_t s = { .rx_nac = P25_NAC_DEFAULT, .tx_nac = P25_NAC_DEFAULT, .talkaround = RADIO_P25_TA_AUTO,
				   .tx_dst = 1 };
	radio_req_p25_settings(&s);
}

static void test_p25_rx(void)
{
	static char buf[4096];
	radio_p25_stats_t st;
	p25_voice_stats_t vs;
	const radio_p25_view_t *v = &sm.view.p25;

	setup_codeplug_p25();
	start();
	p25_defaults();
	aud.feed = true;
	uint32_t lends0 = lends;
	req(RREQ_ZONE_POS, 2);
	CHECK(sm.view.ch.mode == RADIO_MODE_P25 && sm.view.channel_number == 3);
	// The board in its digital mode at 12.5 kHz without a tone, the DMR stack stopped, the engine on board.air in
	// the memory the vocoder lends
	CHECK(hw_mode == RADIO_MODE_DMR && hw.rx_hz == 446100000 && hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_NONE);
	CHECK(!sm.dmr_running && !sim.started);
	CHECK(radio_p25_running() && fair[0].open && lends == lends0 + LENDS);
	CHECK(v->flags == RADIO_P25_ON && v->tx == AIR_TX_IDLE);
	radio_p25_stats(&st);
	CHECK(st.begins == 1 && st.begin_fails == 0 && st.mem_lent == LENDS);
	p25_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25.built=1\np25.on=1\np25.rx=0\n") != NULL && strstr(buf, "p25.rx_nac=659\n") != NULL);
	CHECK(strstr(buf, "p25.tx=0\np25.rx_work_us=1000\np25.rx_span_us=20000\n") != NULL);	// the provider's load
	run(300);				// noise: no call
	CHECK(sm.view.state == RS_IDLE && !hw.led[LED_GREEN] && !aud.started);

	// A group call on our NAC: received and played (rx_tgid 0: every call of the NAC)
	far_begin(P25_NAC_DEFAULT, 1001, false, 77);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK(sm.view.state == RS_RX && (v->flags & (RADIO_P25_RX | RADIO_P25_PLAYED)) == (RADIO_P25_RX | RADIO_P25_PLAYED));
	CHECK(!(v->flags & (RADIO_P25_ENC | RADIO_P25_UNIT | RADIO_P25_LATE)));
	CHECK(v->nac == P25_NAC_DEFAULT && v->src == 1001 && v->dst == 77);
	CHECK(hw.led[LED_GREEN] && !hw.audio && radio_view_wakes(&sm.view));	// voice is PCM, not the FM speaker path
	CHECK(aud.started);			// the voice stream plays it
	p25_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25.rx=1\np25.played=1\n") != NULL && strstr(buf, "p25.src=1001\np25.dst=77\n") != NULL);
	p25_engine_tx_stop(&far_eng);
	run(600);
	CHECK(!far_tx && sm.view.state == RS_IDLE && !hw.led[LED_GREEN]);
	CHECK(v->flags == RADIO_P25_ON && v->src == 1001 && v->dst == 77);	// the last call heard stays
	CHECK(sm.view.last_rx_ms != 0);
	radio_p25_stats(&st);
	p25VoiceGetStats(&vs);
	CHECK(st.rx_calls == 1 && st.voice >= 9 && st.voice_bad == 0 && st.rx_chunks > 0);
	CHECK(vs.rx_in == st.voice && vs.rx_frames > 0 && vs.rx_calls == 1);

	// Another NAC: another system on the channel, nothing heard
	far_begin(0x123, 1002, false, 77);
	far_call(400);
	CHECK(sm.view.state == RS_IDLE && v->src == 1001);
	radio_p25_stats(&st);
	CHECK(st.rx_calls == 1);
	// ... unless the receiver takes every NAC
	radio_p25_settings_t s;
	radio_p25_settings(&s);
	s.rx_nac = P25_NAC_ANY;
	radio_req_p25_settings(&s);
	far_begin(0x123, 1002, false, 77);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK(sm.view.state == RS_RX && v->nac == 0x123 && v->src == 1002);
	p25_engine_tx_stop(&far_eng);
	run(600);
	CHECK(sm.view.state == RS_IDLE);

	// The selective squelch: only talk group 5 and calls to us. A call to 77 is shown, not played
	s.rx_nac = P25_NAC_DEFAULT;
	s.rx_tgid = 5;
	radio_req_p25_settings(&s);
	far_begin(P25_NAC_DEFAULT, 1003, false, 77);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK(sm.view.state == RS_RX && (v->flags & RADIO_P25_RX) && !(v->flags & RADIO_P25_PLAYED) && v->dst == 77);
	CHECK(!hw.led[LED_GREEN] && !radio_view_wakes(&sm.view));
	p25_engine_tx_stop(&far_eng);
	run(600);
	// Talk group 5: played
	far_begin(P25_NAC_DEFAULT, 1003, false, 5);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK((v->flags & RADIO_P25_PLAYED) && v->dst == 5 && hw.led[LED_GREEN]);
	p25_engine_tx_stop(&far_eng);
	run(600);
	// A unit call to us (unit_id 0: the radio's DMR ID): played, shown as a unit call
	far_begin(P25_NAC_DEFAULT, 1004, true, own_dmr_id);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK((v->flags & (RADIO_P25_PLAYED | RADIO_P25_UNIT)) == (RADIO_P25_PLAYED | RADIO_P25_UNIT));
	CHECK(v->dst == own_dmr_id && v->src == 1004);
	p25_engine_tx_stop(&far_eng);
	run(600);
	// ... and to someone else: not played
	far_begin(P25_NAC_DEFAULT, 1004, true, 4242);
	far_call(600);
	CHECK(v->dst == 4242 && sm.view.state == RS_IDLE);
	radio_p25_stats(&st);
	CHECK(st.rx_calls == 6);

	// The counter reset clears the engine's and the voice path's counters
	debuglink_p25_reset();
	radio_p25_stats(&st);
	p25VoiceGetStats(&vs);
	CHECK(st.rx_calls == 0 && st.begins == 0 && vs.rx_in == 0);
	p25_defaults();
	far_stop();
	aud.feed = false;
}

static void test_p25_tx(void)
{
	radio_p25_stats_t st;
	const radio_p25_view_t *v = &sm.view.p25;

	setup_codeplug_p25();
	start();
	p25_defaults();
	req(RREQ_ZONE_POS, 2);
	far_begin(P25_NAC_DEFAULT, 1001, false, 1);
	run(100);

	// PTT: keyed on the channel's TX frequency at 12.5 kHz, its power, no tone; the provider starts inside the key
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && hw.keyed && fair[0].on && fair[0].begins == 1);
	CHECK(hw.tx_hz == 446100000 && hw.tx_bw == RADIO_BW_12K5 && hw.tx_tone.type == TONE_NONE && hw.tx_power == 2);
	CHECK(hw.led[LED_RED] && !hw.led[LED_GREEN] && !sm.view.tx_auto && v->tx != AIR_TX_IDLE);
	req(RREQ_CH_STEP, 1);			// no retuning while transmitting
	CHECK(sm.view.channel_number == 3);
	run(1000);
	CHECK(sm.view.state == RS_TX && sm.view.tx_elapsed_ms >= 975 && fair[0].underruns == 0);
	// The far radio hears the call: our NAC, our unit ID (the DMR ID), talk group tx_dst
	CHECK(far_heard.starts == 1 && far_heard.ends == 0);
	CHECK(far_heard.call.nac == P25_NAC_DEFAULT && far_heard.call.lc.src == own_dmr_id && far_heard.call.lc.tgid == 1);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && hw.keyed);	// the LDU on air is finished first
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed && hw.tx_off_calls == 1 && !hw.led[LED_RED]);
	CHECK(v->tx == AIR_TX_IDLE && far_heard.ends == 1 && far_heard.voice % 9 == 0 && far_heard.voice >= 45);
	CHECK(fair[0].keyed_violations == 0 && fair[0].underruns == 0);
	radio_p25_stats(&st);
	CHECK(st.tx_calls == 1 && st.tx_underruns == 0 && st.tx_syms > 0);

	// Pressed again while the last one ends: keyed again once it is done
	req(RREQ_PTT, 1);
	run(400);
	req(RREQ_PTT, 0);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_calls == 2);
	run(500);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 3 && hw.tx_off_calls == 2 && hw.keyed);
	req(RREQ_PTT, 0);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed && hw.tx_off_calls == 3 && far_heard.starts == 3 && far_heard.ends == 3);

	// Settings sent while transmitting: in force for the next transmission
	req(RREQ_PTT, 1);
	run(100);
	radio_p25_settings_t s;
	radio_p25_settings(&s);
	s.unit_call = true;
	s.tx_dst = 4321;
	s.unit_id = 777;
	s.tx_nac = 0x2A0;
	radio_req_p25_settings(&s);
	run(300);
	CHECK(far_heard.call.lc.src == own_dmr_id);	// this one goes on as it began
	req(RREQ_PTT, 0);
	run(500);
	req(RREQ_PTT, 1);
	run(400);
	CHECK(far_heard.call.nac == 0x2A0 && far_heard.call.lc.lcf == P25_LCF_UNIT && far_heard.call.lc.dst == 4321 &&
	      far_heard.call.lc.src == 777);
	req(RREQ_PTT, 0);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed);
	p25_defaults();

	// The time-out: cut at once, the transmitter off, denied until PTT is released
	uint32_t offs = (uint32_t)hw.tx_off_calls;
	req(RREQ_PTT, 1);
	run(179000);
	CHECK(sm.view.state == RS_TX && hw.keyed);
	run(1100);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TIMEOUT && !hw.keyed && hw.tx_off_calls == (int)offs + 1);
	CHECK(v->tx == AIR_TX_IDLE && fair[0].keyed_violations == 0);
	run(2000);
	CHECK(sm.view.state == RS_TX_DENIED && !hw.keyed);
	req(RREQ_PTT, 0);
	run(100);
	CHECK(sm.view.state == RS_IDLE);
	req(RREQ_PTT, 1);			// a fresh press transmits again
	run(100);
	CHECK(sm.view.state == RS_TX && hw.keyed);
	req(RREQ_PTT, 0);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed);
	far_stop();
}

static void test_p25_denied(void)
{
	radio_p25_stats_t st;

	// The board refuses to key: denied, the engine's transmission dropped, nothing on air
	setup_codeplug_p25();
	start();
	p25_defaults();
	req(RREQ_ZONE_POS, 2);
	tx_fail = true;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REFUSED && !hw.keyed && !fair[0].on);
	run(100);
	CHECK(sm.view.p25.tx == AIR_TX_IDLE && fair[0].qn == 0);
	req(RREQ_PTT, 0);
	tx_fail = false;
	run(50);
	CHECK(sm.view.state == RS_IDLE);
	req(RREQ_PTT, 1);			// and then keys normally
	CHECK(sm.view.state == RS_TX && hw.keyed);
	req(RREQ_PTT, 0);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed && fair[0].keyed_violations == 0);

	// Transmit inhibit
	sm.set.tx_inhibit = true;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_INHIBIT && !hw.keyed);
	req(RREQ_PTT, 0);
	sm.set.tx_inhibit = false;

	// Receive only
	chans[3].rx_only = true;
	req(RREQ_RELOAD, 0);
	run(50);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_RX_ONLY && !hw.keyed);
	req(RREQ_PTT, 0);
	chans[3].rx_only = false;

	// No engine: the provider can't carry P25. Nothing received, PTT refused
	air_nofit = true;
	req(RREQ_CH_STEP, 1);
	req(RREQ_CH_STEP, -1);
	CHECK(sm.view.channel_number == 3 && !radio_p25_running() && !fair[0].open);
	CHECK(sm.view.p25.flags == 0);
	radio_p25_stats(&st);
	CHECK(st.begin_fails >= 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REFUSED && !hw.keyed);
	req(RREQ_PTT, 0);
	air_nofit = false;
#ifdef HAL_VOCODER_LEND_BYTES
	// ... or the vocoder lends nothing
	lend_refuse = true;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 2);
	CHECK(!radio_p25_running());
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REFUSED && !hw.keyed);
	req(RREQ_PTT, 0);
	lend_refuse = false;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 2);
	CHECK(radio_p25_running());
#endif

	// In a listen band (the air band with CONFIG_AM_RX) P25 is refused as a mode, as DMR is
	req(RREQ_ZONE_POS, 4);			// "Air", 121.5 MHz
	req(RREQ_MODE, RADIO_MODE_P25);
	CHECK(sm.view.ch.mode == (radio_listen_band(121500000) ? RADIO_MODE_FM : RADIO_MODE_P25));
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !radio_p25_running());
}

// Leaving P25 for FM and DMR, and back: P25 stops, the provider closes, the other mode starts on the modem
static void test_p25_leave(void)
{
	setup_codeplug_p25();
	start();
	p25_defaults();
	req(RREQ_ZONE_POS, 2);
	CHECK(radio_p25_running() && fair[0].open);

	// Temporary mode change on the channel (the debug link's DMR_FIELDS mode): FM
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ RADIO_MODE_FM, 0xFF, 0 }, 3) == DL_ST_OK);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && hw_mode == RADIO_MODE_FM && !radio_p25_running() && !fair[0].open);
	CHECK(fair[0].closes == 1 && sm.view.p25.flags == 0 && (sm.view.temp & RADIO_TEMP_MODE));
	{
		char buf[1024];
		p25_view_text(buf, sizeof buf);
		CHECK(strstr(buf, "p25.on=0\n") != NULL && strstr(buf, "rx_work_us") == NULL);	// no load off P25
	}
	// ... P25 again
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ RADIO_MODE_P25, 0xFF, 0 }, 3) == DL_ST_OK);
	CHECK(sm.view.ch.mode == RADIO_MODE_P25 && hw_mode == RADIO_MODE_DMR && radio_p25_running() && fair[0].open);
	CHECK(hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_NONE);
	// ... DMR: the engine stops before the DMR stack starts (the vocoder's lent memory comes back first)
	req(RREQ_MODE, RADIO_MODE_DMR);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && !radio_p25_running() && !fair[0].open && sm.dmr_running && sim.started);
	CHECK(sm.view.p25.flags == 0);
	run(100);
	CHECK(!radio_p25_running() && sm.dmr_running);
	// ... and from DMR to P25: the DMR stack stops
	req(RREQ_MODE, RADIO_MODE_P25);
	CHECK(radio_p25_running() && fair[0].open && !sm.dmr_running);
	// A transmission running when the mode changes is refused (no retune while transmitting)
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_MODE, RADIO_MODE_FM);
	CHECK(sm.view.ch.mode == RADIO_MODE_P25 && radio_p25_running());
	req(RREQ_PTT, 0);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed);
	// The channel's own mode back (clear temporary)
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(sm.view.ch.mode == RADIO_MODE_P25 && radio_p25_running());

	// To an FM channel and a DMR channel by channel step
	req(RREQ_ZONE_POS, 1);			// Weather, FM
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !radio_p25_running() && hw_mode == RADIO_MODE_FM);
	req(RREQ_ZONE_POS, 2);
	CHECK(radio_p25_running());
	req(RREQ_ZONE_POS, 3);			// DMR rptr
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && !radio_p25_running() && sm.dmr_running && !fair[0].open);
	req(RREQ_ZONE_POS, 2);
	CHECK(radio_p25_running() && !sm.dmr_running);
	// DMR and FM receive work after P25
	req(RREQ_ZONE_POS, 3);
	CHECK(sm.dmr_running && sim.started && hw_mode == RADIO_MODE_DMR);
	req(RREQ_ZONE_POS, 0);
	rx_signal(10, true);
	poll_for(50);
	CHECK(sm.view.state == RS_RX && hw.audio);
	rx_signal(120, true);
	poll_for(500);
	CHECK(sm.view.state == RS_IDLE);

	// The settings request with P25 off: taken, in force at the next P25 start
	radio_p25_settings_t s;
	radio_p25_settings(&s);
	s.rx_tgid = 9;
	radio_req_p25_settings(&s);
	radio_p25_settings(&s);
	CHECK(s.rx_tgid == 9);
	// Out of range values are clipped or refused
	s.rx_nac = 0xFFFF;
	s.talkaround = 7;
	radio_p25_set_settings(&s);
	radio_p25_settings(&s);
	CHECK(s.rx_nac == 0xFFF && s.talkaround == RADIO_P25_TA_AUTO);
	static const uint8_t bad_ta[16] = { 0x93, 0x02, 0x93, 0x02, 0, 0, 3 };
	CHECK(debuglink_radio_feature_op(DL_OP_P25, bad_ta, 16) == DL_ST_BAD_ARG);
	static const uint8_t bad_nac[16] = { 0x00, 0x10, 0x93, 0x02 };
	CHECK(debuglink_radio_feature_op(DL_OP_P25, bad_nac, 16) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_P25, bad_nac, 15) == DL_ST_BAD_ARG);
	static const uint8_t ok[16] = { 0x93, 0x02, 0x94, 0x02, 5, 0, 1, 1, 0x39, 0x30, 0, 0, 0x10, 0x27, 0, 0 };
	CHECK(debuglink_radio_feature_op(DL_OP_P25, ok, 16) == DL_ST_OK);
	radio_p25_settings(&s);
	CHECK(s.rx_nac == 0x293 && s.tx_nac == 0x294 && s.rx_tgid == 5 && s.talkaround == RADIO_P25_TA_ON && s.unit_call &&
	      s.tx_dst == 12345 && s.unit_id == 10000);
	p25_defaults();
}

// The VFO in P25: its own mode, every retune starts the engine over, saved as DMR (the saved VFO has no P25)
static void test_p25_vfo(void)
{
	radio_p25_stats_t st;
	settings_t set;

	setup_codeplug_p25();
	start();
	p25_defaults();
	req(RREQ_VFO, 1);
	CHECK(sm.view.vfo);
	req(RREQ_VFO_SET, 446100000);
	req(RREQ_MODE, RADIO_MODE_P25);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_P25 && radio_p25_running() && hw_mode == RADIO_MODE_DMR);
	CHECK(!(sm.view.temp & RADIO_TEMP_MODE));	// the VFO's own mode, not a temporary change
	radio_p25_stats(&st);
	uint32_t begins = st.begins;
	req(RREQ_VFO_STEP, 1);
	CHECK(hw.rx_hz == sm.view.ch.rx_hz && radio_p25_running());
	radio_p25_stats(&st);
	CHECK(st.begins == begins + 1);
	// PTT in the VFO keys on its TX frequency
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == sm.view.ch.tx_hz && hw.tx_bw == RADIO_BW_12K5);
	req(RREQ_PTT, 0);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed);
	// Saved as P25
	run(5000);
	settings_read(&set);
	CHECK(settings_vfo_mode(&set) == RADIO_MODE_P25);
	CHECK(sm.view.ch.mode == RADIO_MODE_P25);
	// Out to a channel and back: the VFO is P25 still (RAM), the engine restarts
	req(RREQ_VFO, 0);
	CHECK(!sm.view.vfo && !radio_p25_running());
	req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_P25 && radio_p25_running());
	// After a restart the saved VFO comes back in P25, the engine running
	boot();
	if (!sm.view.vfo)
		req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_P25 && radio_p25_running());
	CHECK(sm.view.vfo_drop == 0);
	// A board that can't run P25: the VFO comes back in its default mode, and vfo_drop says why
	req(RREQ_VFO, 0);
	run(5000);
	air_nofit = true;
	boot();
	if (!sm.view.vfo)
		req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && sm.view.ch.mode != RADIO_MODE_P25 && !radio_p25_running());
	CHECK(sm.view.vfo_drop == RADIO_MODE_P25 + 1);
	air_nofit = false;
	req(RREQ_VFO, 0);
}

// The air interface bench (CONFIG_AIR_RAW): it holds the modem and keys at the lowest power (debuglink.md 3.28)
static void test_air_bench(void)
{
	setup_codeplug_p25();
	start();
	req(RREQ_ZONE_POS, 3);			// DMR rptr, power 2
	CHECK(sm.dmr_running);
	radio_req_t r = { RREQ_AIR_BENCH, 1 | (5 << 8), 0 };
	radio_sm_request(&sm, &r, now);
	CHECK(sm.air_bench && sm.bench_seq == 5 && !sm.dmr_running && hw_mode == RADIO_MODE_DMR && hw.rx_bw == RADIO_BW_12K5);
	req(RREQ_PTT, 1);			// PTT gives way to the bench
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BENCH && !hw.keyed);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);			// no retuning
	CHECK(sm.view.channel_number == 4);
	// The bench opened the provider and queued its first chunk; the core keys
	CHECK(fake_air0.open(NULL, NULL, NULL));
	air_tx_chunk_t c = { .nsyms = AIR_CHUNK };
	for (int i = 0; i < AIR_DEPTH; i++) {
		CHECK(fake_air0.tx_queue(&c));
	}
	r = (radio_req_t){ RREQ_AIR_TX, 1 | (6 << 8), 200 };
	radio_sm_request(&sm, &r, now);
	CHECK(sm.bench_keyed && hw.keyed && fair[0].on && sm.view.state == RS_TX && sm.view.tx_auto);
	CHECK(hw.tx_hz == 438500000 && hw.tx_bw == RADIO_BW_12K5 && hw.tx_tone.type == TONE_NONE);
	CHECK(hw.tx_power == 0);		// the lowest power, whatever the channel's
	run(300);				// the cap (200 ms) unkeys it
	CHECK(!sm.bench_keyed && !hw.keyed && !fair[0].on && fair[0].aborts == 1 && sm.view.state == RS_IDLE);
	// Refused: inhibit
	sm.set.tx_inhibit = true;
	fake_air0.tx_queue(&c);
	radio_sm_request(&sm, &r, now);
	CHECK(!sm.bench_keyed && !hw.keyed);
	sm.set.tx_inhibit = false;
	fake_air0.close();
	// Leave: the channel again, DMR takes the modem back
	r = (radio_req_t){ RREQ_AIR_BENCH, 0, 0 };
	radio_sm_request(&sm, &r, now);
	CHECK(!sm.air_bench && sm.dmr_running);
	// From a P25 channel: the engine stops for the bench and comes back after
	req(RREQ_ZONE_POS, 2);
	CHECK(radio_p25_running());
	r = (radio_req_t){ RREQ_AIR_BENCH, 1, 0 };
	radio_sm_request(&sm, &r, now);
	CHECK(sm.air_bench && !radio_p25_running());
	r = (radio_req_t){ RREQ_AIR_BENCH, 0, 0 };
	radio_sm_request(&sm, &r, now);
	CHECK(!sm.air_bench && radio_p25_running());
}

// The codec's side (core/p25/p25_codeplug.c, tested in p25CodeplugTest): a stored channel's set-up, by number, and
// the radio-wide unit ID
static p25_conv_cfg_t stored_p25[NCH + 1];
static uint32_t p25_radio_id;		// the kind's radio-wide record, 0 = none
bool p25_conv_load(uint16_t number, const channel_t *ch, p25_conv_cfg_t *out)
{
	if (ch->kind != CODEPLUG_KIND_P25 || number < 1 || number > NCH) {
		return false;
	}
	*out = stored_p25[number];
	return true;
}
uint32_t p25_radio_unit_id(uint32_t dmr_id) { return p25_radio_id != 0 ? p25_radio_id : dmr_id; }
// A plain channel in P25 saved as a stored one: the record and block it was given
static struct { uint16_t number; channel_t ch; p25_conv_t conv; } p25_saved;
bool p25_conv_save(uint16_t number, const channel_t *ch, const p25_conv_t *conv)
{
	p25_saved.number = number;
	p25_saved.ch = *ch;
	p25_saved.conv = *conv;
	return codeplug_set_channel(number, ch);
}

#if CONFIG_SCAN
// A zone scan over a stored P25 channel (docs/spec/p25.md 7.1): it stops there, starts the engine, holds on a call
// that plays and moves on from one that doesn't; a board that can't receive P25 and a trunked channel are left out
static void test_p25_scan(void)
{
	const radio_p25_view_t *v = &sm.view.p25;
	uint16_t visits;

	setup_codeplug_p25();
	add(3, "P25 rptr", 446100000, RADIO_MODE_DMR, false, 2);	// zone Local, position 2
	chans[3].kind = CODEPLUG_KIND_P25;
	chans[3].system = 0xFF;
	stored_p25[3] = (p25_conv_cfg_t){ .conv = { 0x2A0, 0x2A0, P25_SQUELCH_SELECTIVE }, .unit_id = 1234567,
					  .target = 201, .target_unit = false, .talkaround = true };
	start();
	p25_defaults();
	far_hz = 446100000;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.scan == SCAN_ZONE);
	// Nothing on the air: the scan stops on the P25 channel (the engine runs there) and goes on after the dwell
	radio_p25_stats_t st;
	radio_p25_stats(&st);
	uint32_t begins0 = st.begins;
	CHECK(!scan_holds_on(3, 3000, &visits) && visits >= 2 && sm.view.scan == SCAN_ZONE);
	radio_p25_stats(&st);
	CHECK(st.begins >= begins0 + 2);
	// The channel's talk group, a call already on when the scan comes round (a late entry, its talk group from the
	// link control): it holds there, the call playing, for as long as the call lasts
	far_begin(0x2A0, 1001, false, 201);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	CHECK(scan_holds_on(3, 4000, &visits));
	CHECK(radio_p25_running() && (v->flags & RADIO_P25_PLAYED) && v->dst == 201 && v->src == 1001);
	run(2000);
	CHECK(sm.view.channel_number == 3 && sm.view.scan_phase == SCAN_PH_HOLD && hw.led[LED_GREEN]);
	// The call ends: the resume delay, then the scan moves on
	p25_engine_tx_stop(&far_eng);
	run(600);
	CHECK(!far_tx && sm.view.channel_number == 3 && sm.view.scan_phase == SCAN_PH_RESUME);
	run(RADIO_SCAN_DELAY_MS + 600);
	CHECK(sm.view.channel_number != 3 && sm.view.scan == SCAN_ZONE);
	// Another talk group of the NAC (the channel's selective squelch): heard, not played, and the scan goes on
	far_begin(0x2A0, 1002, false, 7);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	CHECK(!scan_holds_on(3, 4000, &visits) && visits >= 1 && sm.view.scan == SCAN_ZONE);
	// Another NAC: a carrier, probed and left
	uint16_t probes = sm.view.scan_probes;
	far_begin(0x123, 1003, false, 201);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	CHECK(!scan_holds_on(3, 4000, &visits) && visits >= 1 && sm.view.scan_probes > probes);
	p25_engine_tx_stop(&far_eng);
	run(600);
	req(RREQ_SCAN, RSCAN_STOP);
	CHECK(sm.view.scan == SCAN_OFF);

	// A board whose air interface can't receive P25: the channel is parked and never a stop
	air_nofit = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.scan == SCAN_ZONE && !scan_holds_on(3, 3000, &visits) && visits == 0);
	req(RREQ_SCAN, RSCAN_STOP);
	air_nofit = false;
	// A trunked channel: left out (it follows its system's control channel)
	chans[3].kind = CODEPLUG_KIND_P25_TRUNK;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.scan == SCAN_ZONE && !scan_holds_on(3, 3000, &visits) && visits == 0);
	req(RREQ_SCAN, RSCAN_STOP);
	far_hz = 0;
	far_stop();
	p25_defaults();
}
#if CONFIG_DUAL_WATCH
// Dual watch with a stored P25 channel on one side (docs/spec/radio-features.md 2.1): the receiver alternates onto it
// and the engine runs there for each look; a call that plays holds the watch there until it ends and the delay
// passes. A trunked channel and one the board can't receive are refused
static void test_p25_dual(void)
{
	const radio_p25_view_t *v = &sm.view.p25;
	uint16_t visits;

	setup_codeplug_p25();
	add(3, "P25 rptr", 446100000, RADIO_MODE_DMR, false, 2);	// zone Local, position 2
	chans[3].kind = CODEPLUG_KIND_P25;
	chans[3].system = 0xFF;
	stored_p25[3] = (p25_conv_cfg_t){ .conv = { 0x2A0, 0x2A0, P25_SQUELCH_SELECTIVE }, .unit_id = 1234567,
					  .target = 201, .target_unit = false, .talkaround = true };
	start();
	p25_defaults();
	far_hz = 446100000;
	const ui_dual_ref_t a = { false, 0, 0 }, b = { false, 0, 2 }, pair[2] = { a, b };
	CHECK(ui_svc_dual(pair, 0) && sm.view.scan == SCAN_DUAL);
	// Nothing on the air: it goes back and forth, the engine starting at each look at B
	radio_p25_stats_t st;
	radio_p25_stats(&st);
	uint32_t begins0 = st.begins;
	CHECK(!scan_holds_on(3, 3000, &visits) && visits >= 2 && sm.view.scan == SCAN_DUAL);
	radio_p25_stats(&st);
	CHECK(st.begins >= begins0 + 2);
	// A call to the channel's talk group: the watch holds on B while it lasts
	far_begin(0x2A0, 1001, false, 201);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	CHECK(scan_holds_on(3, 4000, &visits));
	CHECK(radio_p25_running() && (v->flags & RADIO_P25_PLAYED) && v->dst == 201);
	run(2000);
	CHECK(sm.view.channel_number == 3 && sm.view.scan_phase == SCAN_PH_HOLD);
	p25_engine_tx_stop(&far_eng);
	run(600 + RADIO_DUAL_DELAY_MS + 600);
	CHECK(sm.view.scan == SCAN_DUAL && sm.view.channel_number == 1);	// back on A
	// Another talk group: heard, not played, and the watch goes on
	far_begin(0x2A0, 1002, false, 7);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	CHECK(!scan_holds_on(3, 4000, &visits) && visits >= 1 && sm.view.scan == SCAN_DUAL);
	p25_engine_tx_stop(&far_eng);
	run(600);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.scan == SCAN_OFF && sm.view.channel_number == 1);
	// B selected: the watch stops there, P25 running
	CHECK(ui_svc_dual(pair, 1) && sm.view.scan == SCAN_DUAL);
	run(500);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.channel_number == 3 && radio_p25_running());
	req(RREQ_ZONE_POS, 0);
	// Refused: no fit on the board (by the radio core), a trunked channel
	air_nofit = true;
	ui_svc_dual(pair, 0);
	CHECK(sm.view.scan == SCAN_OFF);
	air_nofit = false;
	chans[3].kind = CODEPLUG_KIND_P25_TRUNK;
	CHECK(!ui_svc_dual(pair, 0) && sm.view.scan == SCAN_OFF);	// refused at once
	far_hz = 0;
	far_stop();
	p25_defaults();
}
#endif
#endif

// A P25 conventional channel in the codeplug (kind 2, docs/spec/p25-codeplug.md 2): the radio runs it on its own
// parameters, the debug link's unit ID overrides for a session, and it is parked where the board can't carry P25
static void test_p25_stored(void)
{
	static char buf[4096];
	const radio_p25_view_t *v = &sm.view.p25;

	setup_codeplug_p25();
	// The safe stand-in as the codeplug reads it: DMR, the tag's kind and transmit allowed
	add(3, "P25 rptr", 446100000, RADIO_MODE_DMR, false, 2);
	chans[3].tx_hz = 451100000;
	chans[3].kind = CODEPLUG_KIND_P25;
	chans[3].system = 0xFF;	// no system
	stored_p25[3] = (p25_conv_cfg_t){ .conv = { 0x2A0, 0x2A1, P25_SQUELCH_SELECTIVE }, .unit_id = 1234567,
					  .target = 201, .target_unit = false, .talkaround = false };
	start();
	p25_defaults();
	req(RREQ_ZONE_POS, 2);
	CHECK(sm.view.channel_number == 3 && sm.view.ch.mode == RADIO_MODE_P25 && sm.view.ch.kind == CODEPLUG_KIND_P25);
	CHECK(hw_mode == RADIO_MODE_DMR && hw.rx_bw == RADIO_BW_12K5 && radio_p25_running() && !sm.dmr_running);
	CHECK(v->rx_nac == 0x2A0 && v->talk == 201 && !(v->flags & RADIO_P25_TALK_UNIT));
	p25_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25.rx_nac=659\n") != NULL);	// the settings are not what runs
	CHECK(strstr(buf, "p25.run.rx_nac=672\np25.run.tx_nac=673\np25.run.rx_tgid=201\np25.run.talkaround=2\n") != NULL);
	CHECK(strstr(buf, "p25.run.unit_call=0\np25.run.tx_dst=201\np25.run.unit_id=1234567\n") != NULL);

	// Selective: the channel's talk group plays, another is shown and not, everyone and a unit call to us play
	far_begin(0x2A0, 1001, false, 201);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK(sm.view.state == RS_RX && (v->flags & RADIO_P25_PLAYED) && v->dst == 201);
	p25_engine_tx_stop(&far_eng);
	run(600);
	far_begin(0x2A0, 1001, false, 7);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK((v->flags & RADIO_P25_RX) && !(v->flags & RADIO_P25_PLAYED) && v->dst == 7 && !hw.led[LED_GREEN]);
	p25_engine_tx_stop(&far_eng);
	run(600);
	far_begin(0x2A0, 1001, false, P25_TGID_EVERYONE);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK((v->flags & RADIO_P25_PLAYED) && v->dst == P25_TGID_EVERYONE);
	p25_engine_tx_stop(&far_eng);
	run(600);
	far_begin(0x2A0, 1001, true, 1234567);
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(600);
	CHECK((v->flags & (RADIO_P25_PLAYED | RADIO_P25_UNIT)) == (RADIO_P25_PLAYED | RADIO_P25_UNIT));
	p25_engine_tx_stop(&far_eng);
	run(600);
	CHECK(sm.view.state == RS_IDLE);

	// PTT: the channel's TX NAC, our unit ID and its talk group, on its TX frequency (a repeater: no talkaround)
	far_begin(0x2A1, 1001, false, 1);
	run(100);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 451100000 && hw.tx_bw == RADIO_BW_12K5);
	run(1000);
	CHECK(far_heard.starts == 1 && far_heard.call.nac == 0x2A1 && far_heard.call.lc.src == 1234567 &&
	      far_heard.call.lc.tgid == 201);
	req(RREQ_PTT, 0);
	run(600);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed);

	// The debug link's settings: only the unit ID reaches a stored channel, for this session
	radio_p25_settings_t s;
	radio_p25_settings(&s);
	s.rx_nac = 0x555;
	s.unit_id = 7777;
	radio_req_p25_settings(&s);
	p25_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25.run.rx_nac=672\n") != NULL && strstr(buf, "p25.run.unit_id=7777\n") != NULL);
	p25_defaults();

	// A private contact: PTT calls the unit; selective keeps everyone and calls to us. Direct mode: talkaround
	stored_p25[3].target = 9991234;
	stored_p25[3].target_unit = true;
	stored_p25[3].talkaround = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	p25_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25.run.rx_tgid=65535\np25.run.talkaround=1\np25.run.unit_call=1\np25.run.tx_dst=9991234\n"));
	CHECK(v->talk == 9991234 && (v->flags & RADIO_P25_TALK_UNIT));
	// Normal squelch: every call of the NAC
	stored_p25[3].conv.squelch = P25_SQUELCH_NORMAL;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	p25_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25.run.rx_tgid=0\n") != NULL);

	// No mode change for now on a channel of a kind: its mode is the kind's
	req(RREQ_MODE, RADIO_MODE_FM);
	CHECK(sm.view.ch.mode == RADIO_MODE_P25 && radio_p25_running() && !(sm.view.temp & RADIO_TEMP_MODE));
	// Receive only from the tag: PTT refused as on any channel
	chans[3].rx_only = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	int keyed = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == keyed);
	req(RREQ_PTT, 0);
	chans[3].rx_only = false;
	// The editor's feature list says P25 here
	CHECK(strncmp(build_modes_now(), "P25,", 4) == 0);

	// A board that can't carry P25: parked (DMR modem off, never keyed), and no P25 in the feature list
	air_nofit = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && !radio_p25_running() && !sm.dmr_running && hw_mode == RADIO_MODE_FM);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_MODE && hw.tx_calls == keyed);
	req(RREQ_PTT, 0);
	CHECK(strstr(build_modes_now(), "P25,") == NULL && build_features_now()[0] != '\0');
	air_nofit = false;

	// The VFO takes the radio-wide unit ID, then the DMR ID
	p25_radio_id = 5555;
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 446100000);
	req(RREQ_MODE, RADIO_MODE_P25);
	p25_view_text(buf, sizeof buf);
	CHECK(sm.view.vfo && strstr(buf, "p25.run.rx_nac=659\n") != NULL && strstr(buf, "p25.run.unit_id=5555\n") != NULL);
	p25_radio_id = 0;

	// Saved as a channel (the editor's New from current): a stored P25 channel from the P25 settings, its contact
	// the stored one of the talk target, talkaround on kept
	radio_p25_settings(&s);
	s.rx_nac = 0x2B0;
	s.tx_nac = 0x2B1;
	s.rx_tgid = 9;
	s.tx_dst = 9;
	s.unit_call = false;
	s.talkaround = RADIO_P25_TA_ON;
	radio_req_p25_settings(&s);
	run(20);
	memset(&p25_saved, 0, sizeof p25_saved);
	channel_t ed = sm.view.ch;
	CHECK(ed.mode == RADIO_MODE_P25 && ed.kind == CODEPLUG_KIND_NONE);
	CHECK(ui_svc_write_channel(6, &ed, 0));
	storage_job_poll();
	CHECK(p25_saved.number == 6 && p25_saved.conv.rx_nac == 0x2B0 && p25_saved.conv.tx_nac == 0x2B1);
	CHECK(p25_saved.conv.squelch == P25_SQUELCH_SELECTIVE && p25_saved.ch.contact == 3 && p25_saved.ch.dmo);
	// No stored contact for the talk group: none (talk group 1); automatic talkaround, normal squelch
	s.tx_dst = 4242;
	s.rx_tgid = 0;
	s.talkaround = RADIO_P25_TA_AUTO;
	radio_req_p25_settings(&s);
	run(20);
	CHECK(ui_svc_write_channel(6, &ed, 0));
	storage_job_poll();
	CHECK(p25_saved.ch.contact == 0 && !p25_saved.ch.dmo && p25_saved.conv.squelch == P25_SQUELCH_NORMAL);
	p25_defaults();
	// A copy of the stored P25 channel at another number takes its tag; edited in place, the tag follows by itself
	ed = chans[3];
	sim_tag_copy_from = 0;
	p25_saved.number = 0;
	CHECK(ui_svc_write_channel(7, &ed, 3));
	storage_job_poll();
	CHECK(sim_tag_copy_from == 3 && chans[7].kind == CODEPLUG_KIND_P25 && p25_saved.number == 0);
	sim_tag_copy_from = 0;
	CHECK(ui_svc_write_channel(3, &ed, 3));
	storage_job_poll();
	CHECK(sim_tag_copy_from == 0 && p25_saved.number == 0);
	// The channel editor's NAC (ui_svc_write_channel_air): the stored channel written again with its block, in place
	// or as a copy, which takes the source's tag first; a plain channel in P25 takes the block given and the rest
	// from the P25 settings
	ui_air_conv_t air = { .p25 = { 0x1A5, 0x1A6, P25_SQUELCH_SELECTIVE } };
	CHECK(ui_svc_write_channel_air(3, &ed, 3, &air));
	storage_job_poll();
	CHECK(p25_saved.number == 3 && p25_saved.conv.rx_nac == 0x1A5 && p25_saved.conv.tx_nac == 0x1A6 &&
	      p25_saved.ch.kind == CODEPLUG_KIND_P25 && sim_tag_copy_from == 0);
	p25_saved.number = 0;
	CHECK(ui_svc_write_channel_air(8, &ed, 3, &air));
	storage_job_poll();
	CHECK(p25_saved.number == 8 && p25_saved.conv.rx_nac == 0x1A5 && sim_tag_copy_from == 3);
	s.tx_dst = 9;
	radio_req_p25_settings(&s);
	run(20);
	air.p25.rx_nac = air.p25.tx_nac = 0x3C3;
	ed = sm.view.ch;
	CHECK(ui_svc_write_channel_air(6, &ed, 0, &air));
	storage_job_poll();
	CHECK(p25_saved.number == 6 && p25_saved.conv.rx_nac == 0x3C3 && p25_saved.ch.contact == 3);
	// A write that is no P25 channel takes no block
	ed = chans[1];
	p25_saved.number = 0;
	CHECK(ui_svc_write_channel_air(1, &ed, 1, &air));
	storage_job_poll();
	CHECK(p25_saved.number == 0);
	p25_defaults();
	req(RREQ_MODE, RADIO_MODE_FM);
	req(RREQ_VFO, 0);
	far_stop();
	memset(stored_p25, 0, sizeof(stored_p25));
}

#if CONFIG_P25_TRUNK
// ---- P25 trunked (docs/spec/p25-trunking.md 7): a site on the fake air (site above), the far radio its voice ----
#define TS_WACN		0xBEE00u
#define TS_SYSID	0x2F3u
#define TS_NAC		0x293u
#define TS_BASE		451000000u	// identifier 1: 12.5 kHz channels from here
#define TS_CC1		451012500u	// channel 0x1001, the primary control channel
#define TS_CC2		451025000u	// channel 0x1002, programmed by number only
#define TS_VOICE	451125000u	// channel 0x100A
#define TS_FAR_VOICE	851068750u	// identifier 2 (off air), channel 0x200A: outside the radio's bands

static void site_unit(void)
{
	uint8_t b[3][P25_TSBK_BYTES];
	unsigned n = 0;

	site.units++;
	if (site.grant_after != 0 && --site.grant_after == 0) {
		p25_tsbk_t m = { .kind = P25_TSBK_GRANT, .opcode = P25_OP_GRP_V_GRANT };
		m.u.grant.ch = 0x100A;
		m.u.grant.tg = site.last.tg;
		m.u.grant.src = site.last.src;
		p25_tsbk_make(&m, site.out);
		site.out_reps = 3;
	}
	if (site.out_reps != 0) {
		site.out_reps--;
		memcpy(b[n++], site.out, P25_TSBK_BYTES);
	}
	if (site.grant_tg != 0) {
		p25_tsbk_t m = { .kind = P25_TSBK_GRANT, .opcode = P25_OP_GRP_V_GRANT };
		m.u.grant.svc = site.grant_enc ? P25_SVC_PROTECTED : 0;
		m.u.grant.ch = site.grant_ch;
		m.u.grant.tg = site.grant_tg;
		m.u.grant.src = 1001;
		p25_tsbk_make(&m, b[n++]);
	}
	while (n < 2) {
		p25_tsbk_t m = { 0 };
		switch (site.rot++ % 4) {
		case 0:
			m.kind = P25_TSBK_NET;
			m.u.net.lra = 1;
			m.u.net.wacn = TS_WACN;
			m.u.net.sysid = TS_SYSID;
			m.u.net.ch = 0x1001;
			break;
		case 1:
			m.kind = P25_TSBK_RFSS;
			m.u.site.lra = 1;
			m.u.site.sysid = TS_SYSID;
			m.u.site.rfss = 1;
			m.u.site.site = 7;
			m.u.site.ch = 0x1001;
			break;
		case 2:
			if (!site.iden2) {
				continue;
			}
			m.kind = P25_TSBK_IDEN;
			m.u.iden.id = 2;
			m.u.iden.form = P25_OP_IDEN_UP;
			m.u.iden.base = 851006250u / 5;
			m.u.iden.spacing = 50;
			m.u.iden.bw_khz8 = 100;
			break;
		default:
			m.kind = P25_TSBK_SYS_SRV;
			m.u.svc.avail = m.u.svc.supp = 0x1F;
			break;
		}
		p25_tsbk_make(&m, b[n++]);
	}
	for (unsigned k = 0; k < n; k++) {
		b[k][0] = (uint8_t)((b[k][0] & 0x7F) | (k == n - 1 ? P25_TSBK_LB : 0));
		p25_tsbk_seal(b[k]);
	}
	site.unit_syms = p25_blocks_syms((uint8_t)n);
	p25_unit_begin(site.unit, site.unit_syms, TS_NAC, P25_DUID_TSDU, P25_SS_IDLE);
	for (unsigned k = 0; k < n; k++) {
		p25_block_put(site.unit, (uint8_t)k, b[k]);
	}
	site.unit_at = 0;
}

// The site hears the radio's inbound burst (on cc_in_hz): its TSDU found, decoded and answered as ans_* say
static void site_answer(const p25_tsbk_t *m)
{
	p25_tsbk_make(m, site.out);
	site.out_reps = 3;
}
static void site_inbound(const uint8_t *syms, uint16_t n, bool last)
{
	for (uint16_t i = 0; i < n && site.in_n < P25_UNIT_SYMS_MAX; i++) {
		air_sym_put(site.in, site.in_n++, air_sym_get(syms, i));
	}
	if (!last) {
		return;
	}
	uint16_t got = site.in_n;
	site.in_n = 0;
	uint8_t blk[P25_TSBK_BYTES];
	uint16_t nac;
	uint8_t duid;
	p25_isp_t q;
	if (got < 180 || p25_nid_get(site.in, &nac, &duid) < 0 || duid != P25_DUID_TSDU || nac != TS_NAC ||
	    p25_block_get(site.in, 0, blk) < 0 || !p25_isp_parse(blk, &q)) {
		site.bad_rx++;
		return;
	}
	site.last = q;
	p25_tsbk_t m = { 0 };
	if (q.opcode == P25_ISP_U_REG_REQ) {
		site.reg_rx++;
		if (site.ans_reg == SITE_ANS_NONE) {
			return;
		}
		m.kind = P25_TSBK_REG_RSP;
		m.u.reg = (typeof(m.u.reg)){ .rv = site.ans_reg == SITE_ANS_DENY ? P25_RV_DENY : P25_RV_ACCEPT, .sysid = TS_SYSID,
					     .id = q.src, .addr = q.src };
	} else if (q.opcode == P25_ISP_GRP_AFF_REQ) {
		site.aff_rx++;
		if (site.ans_aff == SITE_ANS_NONE) {
			return;
		}
		m.kind = P25_TSBK_AFF_RSP;
		m.u.aff = (typeof(m.u.aff)){ .gav = site.ans_aff == SITE_ANS_DENY ? P25_RV_DENY : P25_RV_ACCEPT, .ann = q.tg,
					     .tg = q.tg, .dst = q.src };
	} else {
		site.req_rx++;
		if (site.ans_req == SITE_ANS_NONE) {
			return;
		}
		if (site.ans_req == SITE_ANS_ACCEPT) {
			site.grant_after = 1;
			return;
		}
		m.kind = site.ans_req == SITE_ANS_DENY ? P25_TSBK_DENY : P25_TSBK_QUEUED;
		m.u.rsp = (typeof(m.u.rsp)){ .svc = q.opcode, .reason = site.ans_req == SITE_ANS_DENY ? 0x59 : 0x2F, .dst = q.src };
		if (site.ans_req == SITE_ANS_QUEUE) {
			site.grant_after = 40;	// the grant 2.4 s later
		}
	}
	site_answer(&m);
}

// One chunk of the control channel: its units back to back
static void site_chunk(uint8_t *syms)
{
	for (uint16_t i = 0; i < AIR_CHUNK; i++) {
		if (site.unit_at >= site.unit_syms) {
			site_unit();
		}
		air_sym_put(syms, i, air_sym_get(site.unit, site.unit_at++));
	}
}

// The codec's side (p25_codeplug.c, tested in p25CodeplugTest): channel 3's system
static p25_trunk_cfg_t stored_trunk;
static int trunk_loads;
static bool trunk_load_fails;
bool p25_trunk_load(uint16_t number, const channel_t *ch, p25_trunk_cfg_t *out)
{
	if (ch->kind != CODEPLUG_KIND_P25_TRUNK || number != 3 || trunk_load_fails) {
		return false;
	}
	trunk_loads++;
	*out = stored_trunk;
	return true;
}

static void p25t_view_text(char *buf, size_t cap)
{
	dl_text_t t;
	dl_text_init(&t, buf, cap);
	debuglink_view_p25_trunk(&t);
}

static void test_p25_trunk(void)
{
	static char buf[2048];
	const radio_p25_view_t *v = &sm.view.p25;
	radio_p25_stats_t st;
	p25_voice_stats_t vs;

	setup_codeplug_p25();
	// The record as the codeplug reads it: the safe stand-in (DMR, receive only) on the primary control channel
	add(3, "P25T sys", TS_CC1, RADIO_MODE_DMR, true, 2);
	chans[3].kind = CODEPLUG_KIND_P25_TRUNK;
	chans[3].system = 0;
	memset(&stored_trunk, 0, sizeof stored_trunk);
	stored_trunk.sys = (p25_tsys_t){ .wacn = TS_WACN, .sysid = TS_SYSID, .cc_nac = TS_NAC };
	stored_trunk.unit_id = 4321;
	stored_trunk.ctl_count = 2;
	stored_trunk.ctl[0] = (p25_ctl_t){ .chan = 0x1001, .rx_hz = TS_CC1 };
	stored_trunk.ctl[1] = (p25_ctl_t){ .chan = 0x1002 };
	stored_trunk.ctl_site[0] = stored_trunk.ctl_site[1] = 7;
	stored_trunk.iden_set = 1u << 1;
	stored_trunk.iden[1] = (p25_iden_t){ .base_hz = TS_BASE, .spacing_hz = 12500, .type = P25_IDEN_FDMA };
	stored_trunk.talk_group = 201;
	memset(&site, 0, sizeof site);
	site.cc_hz = TS_CC1;
	site.voice_hz = TS_VOICE;
	site.on = true;
	start();
	p25_defaults();
	aud.feed = true;
	CHECK(strncmp(build_modes_now(), "P25,P25_TRUNK,", 14) == 0);

	// The channel runs P25, receive only; the follower finds the system on the primary
	req(RREQ_ZONE_POS, 2);
	CHECK(sm.view.channel_number == 3 && sm.view.ch.mode == RADIO_MODE_P25 && trunk_loads == 1);
	CHECK(radio_p25_running() && radio_p25_trunked() && !sm.dmr_running && hw_mode == RADIO_MODE_DMR);
	CHECK(hw.rx_hz == TS_CC1 && hw.rx_bw == RADIO_BW_12K5);
	run(1500);
	CHECK(v->trunk == RADIO_P25_TRUNK_CONTROL && hw.rx_hz == TS_CC1 && sm.view.state == RS_IDLE);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.built=1\np25t.on=1\np25t.state=2\np25t.cc=0\n") != NULL);
	CHECK(strstr(buf, "p25t.nac=659\np25t.wacn=781824\np25t.sysid=755\np25t.rfss=1\np25t.site=7\n") != NULL);
	CHECK(strstr(buf, "p25t.blocks=0\n") == NULL && strstr(buf, "p25t.crc_fail=0\n") != NULL);
	p25_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25.run.rx_nac=659\n") != NULL && strstr(buf, "p25.run.unit_id=4321\n") != NULL);

	// PTT on a trunked channel: refused, nothing keyed
	int keyed = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == keyed);
	req(RREQ_PTT, 0);
	run(100);
	CHECK(!radio_p25_tx_prepare() && !fair[0].on);
	// The same with the record's (the tag's) receive only cleared on a listen-only system
	chans[3].rx_only = false;
	stored_trunk.listen_only = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == keyed);
	req(RREQ_PTT, 0);
	run(1500);
	CHECK(v->trunk == RADIO_P25_TRUNK_CONTROL);

	// A grant for our talk group: the receiver goes to the voice channel, the call plays, then back
	radio_p25_reset_stats();
	p25VoiceResetStats();
	far_begin(TS_NAC, 1001, false, 201);
	site.grant_tg = 201;
	site.grant_ch = 0x100A;
	CHECK(p25_engine_tx_prepare(&far_eng) && p25_engine_tx_go(&far_eng));
	far_tx = true;
	run(800);
	CHECK(hw.rx_hz == TS_VOICE && v->trunk == RADIO_P25_TRUNK_VOICE && sm.view.state == RS_RX);
	CHECK((v->flags & RADIO_P25_PLAYED) && v->dst == 201 && v->src == 1001 && hw.led[LED_GREEN]);
	run(600);
	p25_engine_tx_stop(&far_eng);
	site.grant_tg = 0;
	run(600);
	CHECK(!far_tx && hw.rx_hz == TS_CC1 && v->trunk == RADIO_P25_TRUNK_CONTROL && sm.view.state == RS_IDLE);
	p25VoiceGetStats(&vs);
	radio_p25_stats(&st);
	CHECK(vs.rx_calls == 1 && vs.rx_in > 0 && st.trunk_retunes >= 2 && st.trunk_untuned == 0 && st.tx_calls == 0);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.tg=201\np25t.src=1001\n") != NULL && strstr(buf, "p25t.follows=1\n") != NULL);
	CHECK(fair[0].begins == 0 && fair[0].keyed_violations == 0);

	// Another talk group's grant: heard on the control channel, not followed
	site.grant_tg = 300;
	run(600);
	CHECK(hw.rx_hz == TS_CC1 && v->trunk == RADIO_P25_TRUNK_CONTROL);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.follows=1\n") != NULL);
	// An encrypted grant with the system's skip on: refused
	site.grant_tg = 201;
	site.grant_enc = true;
	stored_trunk.sys.options = P25_TSYS_SKIP_ENC;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	run(1500);
	CHECK(hw.rx_hz == TS_CC1 && v->trunk == RADIO_P25_TRUNK_CONTROL);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.refused=1\np25t.refused_tg=201\n") != NULL && strstr(buf, "p25t.follows=0\n") != NULL);
	site.grant_enc = false;
	stored_trunk.sys.options = 0;

	// A grant on a channel the radio can't receive: not tuned; with no voice there the follower comes back
	site.iden2 = true;
	site.grant_ch = 0x200A;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	radio_p25_reset_stats();
	run(3000);
	radio_p25_stats(&st);
	CHECK(st.trunk_untuned >= 1 && hw.rx_hz == TS_CC1);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.voice_lost=0\n") == NULL);
	site.grant_tg = 0;
	site.iden2 = false;
	run(1500);
	CHECK(v->trunk == RADIO_P25_TRUNK_CONTROL);

	// The primary goes quiet and the site moves to its other control channel (programmed by number): found
	site.cc_hz = TS_CC2;
	run(5000);
	CHECK(hw.rx_hz == TS_CC2 && v->trunk == RADIO_P25_TRUNK_CONTROL);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.cc=1\n") != NULL && strstr(buf, "p25t.cc_lost=0\n") == NULL);

	// Leave for an FM channel: the follower stops with the engine; back, it hunts from the start
	req(RREQ_ZONE_POS, 0);
	CHECK(!radio_p25_running() && !radio_p25_trunked() && v->trunk == RADIO_P25_TRUNK_OFF);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && hw.rx_hz == chans[1].rx_hz);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.on=0\np25t.state=0\n") != NULL);
	req(RREQ_ZONE_POS, 2);
	run(3000);
	CHECK(radio_p25_trunked() && hw.rx_hz == TS_CC2 && v->trunk == RADIO_P25_TRUNK_CONTROL);

	// No control channel programmed: the follower hunts and finds none
	stored_trunk.ctl_count = 0;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	run(1000);
	CHECK(radio_p25_trunked() && v->trunk == RADIO_P25_TRUNK_HUNT && hw.rx_hz == TS_CC1);
	// The system missing (the codeplug can't load it): no engine, PTT still refused
	trunk_load_fails = true;
	radio_p25_reset_stats();
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	radio_p25_stats(&st);
	CHECK(!radio_p25_running() && st.begin_fails == 1 && sm.view.ch.mode == RADIO_MODE_P25);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == keyed);
	req(RREQ_PTT, 0);
	trunk_load_fails = false;
	req(RREQ_ZONE_POS, 0);
	far_stop();
	site.on = false;
}
#endif

#if CONFIG_P25_TRUNK
// The transmit side on a trunked channel (docs/spec/p25-trunking.md 5, 7): registration and affiliation through the
// site's inbound receiver, PTT's request, the grant, the call on the voice channel heard by the far radio, the return;
// queued, denied and unanswered requests
#define TS_OFFSET	5000000		// identifier 1's transmit offset
static void test_p25_trunk_tx(void)
{
	static char buf[4096];
	const radio_p25_view_t *v = &sm.view.p25;
	radio_p25_stats_t st;

	setup_codeplug_p25();
	add(3, "P25T sys", TS_CC1, RADIO_MODE_DMR, false, 2);
	chans[3].kind = CODEPLUG_KIND_P25_TRUNK;
	chans[3].system = 0;
	memset(&stored_trunk, 0, sizeof stored_trunk);
	stored_trunk.sys = (p25_tsys_t){ .wacn = TS_WACN, .sysid = TS_SYSID, .cc_nac = TS_NAC };
	stored_trunk.unit_id = 4321;
	stored_trunk.ctl_count = 1;
	stored_trunk.ctl[0] = (p25_ctl_t){ .chan = 0x1001, .rx_hz = TS_CC1 };
	stored_trunk.ctl_site[0] = 7;
	stored_trunk.iden_set = 1u << 1;
	stored_trunk.iden[1] = (p25_iden_t){ .base_hz = TS_BASE, .spacing_hz = 12500, .tx_offset_hz = TS_OFFSET,
					     .type = P25_IDEN_FDMA };
	stored_trunk.talk_group = 201;
	memset(&site, 0, sizeof site);
	site.cc_hz = TS_CC1;
	site.cc_in_hz = TS_CC1 + TS_OFFSET;
	site.voice_hz = TS_VOICE;
	site.ans_reg = site.ans_aff = SITE_ANS_NONE;
	site.on = true;
	start();
	p25_defaults();
	aud.feed = true;
	far_begin(TS_NAC, 1001, false, 201);	// the far radio listens on the voice channel

	// PTT before the system is found: no control channel
	req(RREQ_ZONE_POS, 2);
	CHECK(radio_p25_trunked() && radio_p25_trunk_tx() && (v->flags & RADIO_P25_TRUNK_TX));
	int keyed = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TRUNK && v->twhy == RADIO_P25T_WHY_NO_CC);
	req(RREQ_PTT, 0);
	// Registration, unanswered: sent on the control channel's inbound frequency, retried
	run(4000);
	CHECK(site.reg_rx >= 2 && site.bad_rx == 0 && site.last.src == 4321 && site.last.wacn == TS_WACN && hw.tx_hz == TS_CC1 + TS_OFFSET);
	CHECK(v->trunk == RADIO_P25_TRUNK_CONTROL && v->treg == P25_REG_RETRY && !hw.keyed && hw.rx_hz == TS_CC1);
	CHECK(sm.view.state == RS_IDLE && fair[0].keyed_violations == 0);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_TRUNK && v->twhy == RADIO_P25T_WHY_NOT_REG && hw.tx_calls == keyed + site.reg_rx);
	req(RREQ_PTT, 0);
	// Answered (the next round, after the pause): affiliation to the talk group, then ready
	site.ans_reg = site.ans_aff = SITE_ANS_ACCEPT;
	run(P25_ACC_RETRY_MS + 1000);
	CHECK(v->treg == P25_REG_READY && site.aff_rx == 1 && site.last.tg == 201 && site.last.sysid == TS_SYSID);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.tx=1\np25t.reg=4\np25t.req=0\np25t.aff_tg=201\n") != NULL);
	CHECK(strstr(buf, "p25t.aff_tx=1\np25t.aff_ok=1\n") != NULL && strstr(buf, "p25t.reg_ok=1\n") != NULL);
	radio_p25_stats(&st);
	CHECK(st.trunk_bursts == site.reg_rx + site.aff_rx && st.trunk_burst_refused == 0 && st.tx_calls == 0);

	// PTT: the request, the grant, the call on the voice channel's transmit frequency, then back
	site.ans_req = SITE_ANS_ACCEPT;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_IDLE && sm.p25_req);
	run(600);
	CHECK(site.req_rx == 1 && site.last.opcode == P25_ISP_GRP_V_REQ && site.last.tg == 201 && site.last.src == 4321);
	CHECK(sm.view.state == RS_TX && hw.keyed && hw.tx_hz == TS_VOICE + TS_OFFSET && hw.rx_hz == TS_VOICE);
	CHECK(v->trunk == RADIO_P25_TRUNK_VOICE && v->treq == P25_REQ_TALK);
	run(1500);
	CHECK(sm.view.state == RS_TX && far_heard.starts == 1 && far_heard.call.lc.tgid == 201 && far_heard.call.lc.src == 4321);
	req(RREQ_PTT, 0);
	run(600);
	CHECK(!hw.keyed && sm.view.state != RS_TX && far_heard.ends == 1 && hw.rx_hz == TS_CC1 && v->trunk == RADIO_P25_TRUNK_CONTROL);
	CHECK(v->treq == P25_REQ_IDLE && fair[0].keyed_violations == 0);
	radio_p25_stats(&st);
	CHECK(st.tx_calls == 1);
	run(1500);

	// Queued, then granted
	site.ans_req = SITE_ANS_QUEUE;
	req(RREQ_PTT, 1);
	run(800);
	CHECK(v->treq == P25_REQ_QUEUED && sm.view.state == RS_IDLE && !hw.keyed);
	run(2500);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == TS_VOICE + TS_OFFSET && site.req_rx == 2);
	req(RREQ_PTT, 0);
	run(1500);
	CHECK(!hw.keyed && hw.rx_hz == TS_CC1 && v->trunk == RADIO_P25_TRUNK_CONTROL);

	// Denied
	site.ans_req = SITE_ANS_DENY;
	req(RREQ_PTT, 1);
	run(800);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TRUNK && v->twhy == RADIO_P25T_WHY_DENIED);
	p25t_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "p25t.deny_svc=0\np25t.deny_reason=89\n") != NULL);
	req(RREQ_PTT, 0);
	run(500);
	// Unanswered: three sends, then refused
	site.ans_req = SITE_ANS_NONE;
	uint16_t sent = site.req_rx;
	req(RREQ_PTT, 1);
	run(5000);
	CHECK(sm.view.tx_deny == TXDENY_TRUNK && v->twhy == RADIO_P25T_WHY_NO_ANSWER && site.req_rx == sent + 3);
	req(RREQ_PTT, 0);
	// Released while waiting: the grant that comes later is not used
	site.ans_req = SITE_ANS_QUEUE;
	req(RREQ_PTT, 1);
	run(500);
	req(RREQ_PTT, 0);
	keyed = hw.tx_calls;
	run(3000);
	CHECK(!hw.keyed && hw.tx_calls == keyed && v->treq == P25_REQ_IDLE);
	run(1500);

	// The transmit inhibit: PTT refused, and no burst goes out
	sm.set.tx_inhibit = true;
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_INHIBIT);
	req(RREQ_PTT, 0);
	sm.set.tx_inhibit = false;

	// A listen-only system: receive only as before, nothing sent
	stored_trunk.listen_only = true;
	req(RREQ_ZONE_POS, 0);
	sent = site.reg_rx;
	keyed = hw.tx_calls;
	req(RREQ_ZONE_POS, 2);
	run(4000);
	CHECK(radio_p25_trunked() && !radio_p25_trunk_tx() && v->treg == P25_REG_OFF && site.reg_rx == sent && hw.tx_calls == keyed);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == keyed);
	req(RREQ_PTT, 0);
	stored_trunk.listen_only = false;
	req(RREQ_ZONE_POS, 0);
	far_stop();
	site.on = false;
}

#endif

static void test_p25(void)
{
#if CONFIG_P25_TRUNK
	test_p25_trunk();
	test_p25_trunk_tx();
#endif
	test_p25_rx();
	test_p25_stored();
#if CONFIG_SCAN
	test_p25_scan();
#if CONFIG_DUAL_WATCH
	test_p25_dual();
#endif
#endif
	test_p25_tx();
	test_p25_denied();
	test_p25_leave();
	test_p25_vfo();
	test_air_bench();
	// Off P25 for the tests after: an FM channel, nothing of P25 left running
	setup_codeplug();
	req(RREQ_RELOAD, 0);
	run(50);
	if (radio_p25_running()) {
		radio_p25_end();
	}
}
#endif

#if CONFIG_NXDN
// ---- NXDN conventional in the radio core (docs/spec/nxdn.md 6): the far radio is an NXDN engine on the other fake
// provider. Voice is made-up 72-bit frames through the fake vocoder: nothing here encodes or decodes speech ----
#include "nxdn/nxdn_engine.h"
#include "nxdn/nxdn_codeplug.h"
#include "dmr/ambe/ambeFec.h"

static nxdn_engine_t nfar;
static bool nfar_running, nfar_tx;
static struct {
	uint8_t starts, ends;
	nxdn_call_t call;
	uint16_t voice;		// voice frames heard
	uint16_t enc;		// ... that are what the radio's encoder made (the fake encoder's all-zero bits)
} nfar_heard;
static uint8_t nfar_next;	// the far radio's next made-up frame
static uint8_t zero_frame[NXDN_EHR_BYTES];	// the fake encoder's frame with its FEC

// Frame k of a made-up sequence: 49 bits from k (not speech), and with the voice frame's FEC
static void made_up_bits(uint8_t k, uint8_t bits[VOCODER_BITS])
{
	for (uint8_t i = 0; i < VOCODER_BITS; i++) {
		bits[i] = (uint8_t)((((uint32_t)k * 2654435761u) >> (i % 29)) & 1u);
	}
	bits[0] = (uint8_t)(k & 1u);
}

static void nf_start(const nxdn_call_t *c, void *ctx) { (void)ctx; nfar_heard.starts++; nfar_heard.call = *c; }
static void nf_update(const nxdn_call_t *c, void *ctx) { (void)ctx; nfar_heard.call = *c; }
static void nf_voice(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	(void)ctx;
	nfar_heard.voice++;
	if (!memcmp(ehr, zero_frame, NXDN_EHR_BYTES)) {
		nfar_heard.enc++;
	}
}
static void nf_end(void *ctx) { (void)ctx; nfar_heard.ends++; }
static bool nf_tx(uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	uint8_t bits[VOCODER_BITS];

	(void)ctx;
	made_up_bits(nfar_next++, bits);
	ambeFecEncode(bits, ehr);
	return true;
}
static const nxdn_engine_hooks_t nfar_hooks = { nf_start, nf_update, nf_voice, nf_end, nf_tx };

// The decoder's frames: how many came in the far radio's order (a repeat to conceal a gap is not counted)
static uint8_t dec_next;
static uint32_t dec_in_order;
static void dec_hook(const uint8_t *bits)
{
	uint8_t want[VOCODER_BITS];

	for (uint8_t k = 0; k < 8; k++) {
		made_up_bits((uint8_t)(dec_next + k), want);
		if (!memcmp(bits, want, VOCODER_BITS)) {
			dec_in_order += k == 0;
			dec_next = (uint8_t)(dec_next + k + 1);
			return;
		}
	}
}

static void nfar_step(void)
{
	if (nfar_tx && nxdn_engine_tx_state(&nfar) == AIR_TX_DONE) {
		nxdn_engine_tx_clear(&nfar);
		nfar_tx = false;
	}
}

// The far radio on the channel: it sends at the rate on ran, as unit, to a group or a unit (dst); it hears every RAN
static void nfar_begin(bool rate9600, uint8_t ran, uint16_t unit, bool unit_call, uint16_t dst)
{
	nxdn_engine_cfg_t c = { .rate9600 = rate9600, .rx_ran = NXDN_RAN_ANY, .tx_ran = ran, .unit_id = unit,
				.tx_call_type = unit_call ? NXDN_CALL_INDIVIDUAL : NXDN_CALL_GROUP, .tx_dst = dst,
				.silence = dmr_ambe_silence };
	uint8_t zero[VOCODER_BITS] = { 0 };

	ambeFecEncode(zero, zero_frame);
	if (nfar_running) {
		nxdn_engine_end(&nfar);
	}
	memset(&nfar_heard, 0, sizeof(nfar_heard));
	nfar_tx = false;
	nfar_running = nxdn_engine_begin(&nfar, &fake_air1, &c, &nfar_hooks, NULL);
	CHECK(nfar_running);
}

static void nfar_stop(void)
{
	if (nfar_running) {
		nxdn_engine_end(&nfar);
		nfar_running = nfar_tx = false;
	}
}

// The far radio keys, holds for ms, lets go and runs until its release frame is out
static void nfar_call(uint32_t ms)
{
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	run(ms);
	nxdn_engine_tx_stop(&nfar);
	for (int i = 0; i < 100 && nfar_tx; i++) {
		run(10);
	}
	CHECK(!nfar_tx);
}

static void nxdn_view_text(char *buf, size_t cap)
{
	dl_text_t t;
	dl_text_init(&t, buf, cap);
	debuglink_view_nxdn(&sm.view, &t);
}

static void setup_codeplug_nxdn(void)
{
	setup_codeplug();
	add(3, "NXDN", 446100000, RADIO_MODE_NXDN, false, 2);	// zone Local, position 2
	radio_nxdn_reset_stats();
	air_nofit = false;
	air_fit2400[0] = air_fit2400[1] = AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT;
	memset(fair, 0, sizeof(fair));
	v_decode_hook = NULL;
}

static const radio_nxdn_settings_t nxdn_default_set = { .rate9600 = true, .rx_ran = 0, .tx_ran = 1, .unit_id = 1,
							 .tx_dst = 1 };
static void nxdn_defaults(void)
{
	radio_req_nxdn_settings(&nxdn_default_set);
}

static void test_nxdn_rx(void)
{
	static char buf[4096];
	radio_nxdn_stats_t st;
	const radio_nxdn_view_t *v = &sm.view.nxdn;
	radio_nxdn_settings_t s = nxdn_default_set;

	setup_codeplug_nxdn();
	start();
	nxdn_defaults();
	aud.feed = true;
	req(RREQ_ZONE_POS, 2);
	CHECK(sm.view.ch.mode == RADIO_MODE_NXDN && sm.view.channel_number == 3);
	// The board in its digital mode at 12.5 kHz without a tone, the DMR stack stopped, the engine on board.air, the
	// DMR side's audio stream running for its voice
	CHECK(hw_mode == RADIO_MODE_DMR && hw.rx_hz == 446100000 && hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_NONE);
	CHECK(!sm.dmr_running && !sim.started && aud.started);
	CHECK(radio_nxdn_running() && fair[0].open);
	CHECK(v->flags == (RADIO_NXDN_ON | RADIO_NXDN_9600) && v->tx == AIR_TX_IDLE);
	CHECK(v->fit == (AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT) && v->rx_ran == 0 && v->tx_ran == 1 && v->talk == 1);
	radio_nxdn_stats(&st);
	CHECK(st.begins == 1 && st.begin_fails == 0);
	nxdn_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "nxdn.built=1\nnxdn.on=1\nnxdn.rx=0\n") != NULL && strstr(buf, "nxdn.rate=9600\n") != NULL);
	CHECK(strstr(buf, "nxdn.fit=7\n") != NULL && strstr(buf, "nxdn.unit_id=1\n") != NULL);
	run(300);				// noise: no call
	CHECK(sm.view.state == RS_IDLE && !hw.led[LED_GREEN]);

	// A group call on RAN 1: received (rx_ran 0 takes every RAN) and played; its voice frames reach the decoder in
	// order through the DMR side's voice path
	dec_next = 0;
	dec_in_order = 0;
	v_decode_hook = dec_hook;
	nfar_next = 0;
	nfar_begin(true, 1, 2001, false, 20);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	run(1000);
	CHECK(sm.view.state == RS_RX && hw.led[LED_GREEN]);
	CHECK((v->flags & (RADIO_NXDN_RX | RADIO_NXDN_PLAYED)) == (RADIO_NXDN_RX | RADIO_NXDN_PLAYED));
	CHECK(v->src == 2001 && v->dst == 20 && v->ran == 1 && !(v->flags & (RADIO_NXDN_UNIT | RADIO_NXDN_ENC)));
	CHECK(dec_in_order >= 30);
	nxdn_engine_tx_stop(&nfar);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !(v->flags & RADIO_NXDN_RX) && v->src == 2001);	// the last call stays
	radio_nxdn_stats(&st);
	CHECK(st.rx_calls == 1 && st.voice >= 40 && st.sacch_fail == 0 && st.facch_fail == 0 && st.voice_held == 0);
	v_decode_hook = NULL;
	nxdn_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "nxdn.src=2001\nnxdn.dst=20\n") != NULL && strstr(buf, "nxdn.rx_calls=1\n") != NULL);

	// Another RAN: not taken, counted
	s.rx_ran = 5;
	radio_req_nxdn_settings(&s);
	CHECK(v->rx_ran == 5);
	nfar_call(600);
	CHECK(sm.view.state == RS_IDLE);
	radio_nxdn_stats(&st);
	CHECK(st.rx_calls == 1 && st.ran_rejects > 0);

	// Group squelch: a call to another group shows (RS_RX), its voice does not play
	s.rx_ran = 1;
	s.rx_gid = 30;
	radio_req_nxdn_settings(&s);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	run(600);
	CHECK(sm.view.state == RS_RX && (v->flags & RADIO_NXDN_RX) && !(v->flags & RADIO_NXDN_PLAYED) && !hw.led[LED_GREEN]);
	nxdn_engine_tx_stop(&nfar);
	run(500);
	// ... and one to our group plays
	nfar_begin(true, 1, 2002, false, 30);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	run(600);
	CHECK((v->flags & RADIO_NXDN_PLAYED) && v->src == 2002 && v->dst == 30);
	nxdn_engine_tx_stop(&nfar);
	run(500);
	nfar_stop();
	nxdn_defaults();
}

static void test_nxdn_tx(void)
{
	radio_nxdn_stats_t st;
	const radio_nxdn_view_t *v = &sm.view.nxdn;
	radio_nxdn_settings_t s = nxdn_default_set;

	setup_codeplug_nxdn();
	start();
	nxdn_defaults();
	aud.feed = true;
	req(RREQ_ZONE_POS, 2);
	nfar_begin(true, 1, 2001, false, 1);
	run(100);

	// PTT: keyed on the channel's TX frequency at 12.5 kHz, its power, no tone; the provider starts inside the key
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && hw.keyed && fair[0].on && fair[0].begins == 1);
	CHECK(hw.tx_hz == 446100000 && hw.tx_bw == RADIO_BW_12K5 && hw.tx_tone.type == TONE_NONE && hw.tx_power == 2);
	CHECK(hw.led[LED_RED] && v->tx != AIR_TX_IDLE);
	run(1000);
	CHECK(sm.view.state == RS_TX && fair[0].underruns == 0);
	// The far radio hears the call: RAN 1, unit 1, group 1; its voice is the encoder's frames
	CHECK(nfar_heard.starts == 1 && nfar_heard.ends == 0 && nfar_heard.call.ran == 1);
	CHECK(nfar_heard.call.vcall.src == 1 && nfar_heard.call.vcall.dst == 1 &&
	      nfar_heard.call.vcall.call_type == NXDN_CALL_GROUP && !(nfar_heard.call.vcall.cc_opt & NXDN_CC_EMERGENCY));
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && hw.keyed);	// the frame on air is finished first
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed && hw.tx_off_calls == 1 && !hw.led[LED_RED]);
	CHECK(v->tx == AIR_TX_IDLE && nfar_heard.ends == 1 && nfar_heard.voice >= 40 && nfar_heard.enc >= 30);
	CHECK(fair[0].keyed_violations == 0 && fair[0].underruns == 0);
	radio_nxdn_stats(&st);
	CHECK(st.tx_calls == 1 && st.tx_underruns == 0 && st.tx_syms > 0);

	// Settings sent while transmitting: in force for the next transmission
	req(RREQ_PTT, 1);
	run(100);
	s.unit_call = true;
	s.tx_dst = 4321;
	s.unit_id = 777;
	s.tx_ran = 9;
	s.emergency = true;
	radio_req_nxdn_settings(&s);
	run(300);
	CHECK(nfar_heard.call.vcall.src == 1);	// this one goes on as it began
	req(RREQ_PTT, 0);
	run(500);
	req(RREQ_PTT, 1);
	run(400);
	CHECK(nfar_heard.call.ran == 9 && nfar_heard.call.vcall.call_type == NXDN_CALL_INDIVIDUAL &&
	      nfar_heard.call.vcall.dst == 4321 && nfar_heard.call.vcall.src == 777 &&
	      (nfar_heard.call.vcall.cc_opt & NXDN_CC_EMERGENCY));
	CHECK(v->flags & RADIO_NXDN_TALK_UNIT);
	req(RREQ_PTT, 0);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed);

	// 4800 bit/s: a new session at 2400 symbols/s
	radio_nxdn_stats(&st);
	uint32_t begins = st.begins;
	s = nxdn_default_set;
	s.rate9600 = false;
	radio_req_nxdn_settings(&s);
	radio_nxdn_stats(&st);
	CHECK(radio_nxdn_running() && st.begins == begins + 1 && !(v->flags & RADIO_NXDN_9600));
	nfar_begin(false, 1, 2001, false, 1);
	req(RREQ_PTT, 1);
	run(600);
	req(RREQ_PTT, 0);
	run(800);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed && nfar_heard.starts == 1 && nfar_heard.ends == 1);

	// A provider that transmits the rate only (no receive): the engine runs, PTT keys, the far radio hears it
	air_fit2400[0] = AIR_FIT_TX;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 2);
	CHECK(radio_nxdn_running() && v->fit == AIR_FIT_TX);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.keyed);
	run(600);
	req(RREQ_PTT, 0);
	run(800);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed && nfar_heard.starts == 2 && nfar_heard.ends == 2);

	// One that receives it only: PTT refused before anything is queued
	air_fit2400[0] = AIR_FIT_RX | AIR_FIT_SOFT;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 2);
	uint32_t keyed = (uint32_t)hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_AIR_NO_TX && !hw.keyed &&
	      hw.tx_calls == (int)keyed && fair[0].qn == 0);
	req(RREQ_PTT, 0);
	run(50);
	// ... and one that gives the rate nothing: no engine, refused
	air_fit2400[0] = 0;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 2);
	CHECK(!radio_nxdn_running() && v->fit == 0);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REFUSED && !hw.keyed);
	req(RREQ_PTT, 0);
	run(50);
	// Back to 9600 bit/s, which the board carries: the engine starts again
	nxdn_defaults();
	CHECK(radio_nxdn_running() && (v->flags & RADIO_NXDN_9600));
	air_fit2400[0] = AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT;

	// The time-out: cut at once, the transmitter off, denied until PTT is released
	nfar_begin(true, 1, 2001, false, 1);
	uint32_t offs = (uint32_t)hw.tx_off_calls;
	req(RREQ_PTT, 1);
	run(181000);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TIMEOUT && !hw.keyed && hw.tx_off_calls == (int)offs + 1);
	req(RREQ_PTT, 0);
	run(100);
	CHECK(sm.view.state == RS_IDLE && v->tx == AIR_TX_IDLE && fair[0].keyed_violations == 0);
	nfar_stop();
}

static void test_nxdn_mode(void)
{
	settings_t set;
	radio_nxdn_settings_t s;

	setup_codeplug_nxdn();
	start();
	nxdn_defaults();
	// The VFO in NXDN through the mode request: its own mode, the engine running
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 446100000);
	req(RREQ_MODE, RADIO_MODE_NXDN);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_NXDN && radio_nxdn_running() && hw_mode == RADIO_MODE_DMR);
	CHECK(!(sm.view.temp & RADIO_TEMP_MODE));
	// The settings from the debug link: 4800 bit/s, RAN 7 both ways, unit 1234 calls group 55
	static const uint8_t op[] = { 0x00, 7, 7, 0, 0, 0xD2, 0x04, 55, 0 };
	CHECK(debuglink_radio_feature_op(DL_OP_NXDN, op, sizeof op) == DL_ST_OK);
	radio_nxdn_settings(&s);
	CHECK(!s.rate9600 && s.rx_ran == 7 && s.tx_ran == 7 && s.unit_id == 1234 && s.tx_dst == 55 && !s.unit_call);
	CHECK(radio_nxdn_running() && !(sm.view.nxdn.flags & RADIO_NXDN_9600) && sm.view.nxdn.rx_ran == 7);
	static const uint8_t bad_ran[] = { 0x01, 64, 1, 0, 0, 1, 0, 1, 0 }, bad_unit[] = { 0x01, 1, 1, 0, 0, 0, 0, 1, 0 },
			     bad_flag[] = { 0x08, 1, 1, 0, 0, 1, 0, 1, 0 };
	CHECK(debuglink_radio_feature_op(DL_OP_NXDN, bad_ran, sizeof bad_ran) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_NXDN, bad_unit, sizeof bad_unit) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_NXDN, bad_flag, sizeof bad_flag) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_NXDN, op, 8) == DL_ST_BAD_ARG);
	CHECK(debuglink_is_feature_op(DL_OP_NXDN));
	nxdn_defaults();
	// Saved with the VFO, back after a restart
	run(5000);
	settings_read(&set);
	CHECK(settings_vfo_mode(&set) == RADIO_MODE_NXDN);
	boot();
	if (!sm.view.vfo)
		req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_NXDN && radio_nxdn_running() && sm.view.vfo_drop == 0);
#if CONFIG_P25
	// To P25 and back: one engine at a time, in the same memory where P25 has its own
	req(RREQ_MODE, RADIO_MODE_P25);
	CHECK(sm.view.ch.mode == RADIO_MODE_P25 && radio_p25_running() && !radio_nxdn_running());
	req(RREQ_MODE, RADIO_MODE_NXDN);
	CHECK(sm.view.ch.mode == RADIO_MODE_NXDN && radio_nxdn_running() && !radio_p25_running());
#endif
	// To FM: the engine closes, the audio stream stops
	req(RREQ_MODE, RADIO_MODE_FM);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !radio_nxdn_running() && hw_mode == RADIO_MODE_FM && !aud.started);
	CHECK(!(sm.view.nxdn.flags & RADIO_NXDN_ON));
	// A board whose air interface carries no NXDN: the mode is refused
	air_nofit = true;
	req(RREQ_MODE, RADIO_MODE_NXDN);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !radio_nxdn_running());
	air_nofit = false;
	// On a channel: a temporary change
	req(RREQ_VFO, 0);
	req(RREQ_ZONE_POS, 0);
	req(RREQ_MODE, RADIO_MODE_NXDN);
	CHECK(!sm.view.vfo && sm.view.ch.mode == RADIO_MODE_NXDN && (sm.view.temp & RADIO_TEMP_MODE) && radio_nxdn_running());
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !radio_nxdn_running());
}

// The codec's side (core/nxdn/nxdn_codeplug.c, tested in nxdnCodeplugTest): a stored channel's set-up, by number
static nxdn_conv_cfg_t stored_nxdn[NCH + 1];
bool nxdn_conv_load(uint16_t number, const channel_t *ch, nxdn_conv_cfg_t *out)
{
	if (ch->kind != CODEPLUG_KIND_NXDN || number < 1 || number > NCH)
		return false;
	*out = stored_nxdn[number];
	return true;
}
// A plain channel in NXDN saved as a stored one: the record and block it was given
static struct { uint16_t number; channel_t ch; nxdn_conv_t conv; } nxdn_saved;
bool nxdn_conv_save(uint16_t number, const channel_t *ch, const nxdn_conv_t *conv)
{
	nxdn_saved.number = number;
	nxdn_saved.ch = *ch;
	nxdn_saved.conv = *conv;
	return codeplug_set_channel(number, ch);
}

#if CONFIG_SCAN
// A zone scan over a stored NXDN channel (docs/spec/nxdn.md 6.4): it stops there, holds on a call that plays (once its
// VCALL says so) and moves on from one that doesn't; a channel at a rate the board can't receive is left out
static void test_nxdn_scan(void)
{
	const radio_nxdn_view_t *v = &sm.view.nxdn;
	uint16_t visits;

	setup_codeplug_nxdn();
	add(3, "NXDN rptr", 446100000, RADIO_MODE_DMR, false, 2);	// zone Local, position 2
	chans[3].kind = CODEPLUG_KIND_NXDN;
	chans[3].system = 0xFF;
	stored_nxdn[3] = (nxdn_conv_cfg_t){ .conv = { .rate9600 = true, .rx_ran = 7, .tx_ran = 7,
						      .squelch = NXDN_SQUELCH_SELECTIVE },
					    .unit_id = 4321, .target = 20, .target_unit = false };
	start();
	nxdn_defaults();
	far_hz = 446100000;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.scan == SCAN_ZONE);
	// Nothing on the air: a stop on the NXDN channel (the engine runs there), then on
	CHECK(!scan_holds_on(3, 3000, &visits) && visits >= 2 && sm.view.scan == SCAN_ZONE);
	// The channel's group on its RAN, a call already on: held, playing, while it lasts
	nfar_begin(true, 7, 2001, false, 20);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	CHECK(scan_holds_on(3, 4000, &visits));
	CHECK(radio_nxdn_running() && (v->flags & RADIO_NXDN_PLAYED) && v->dst == 20 && v->src == 2001);
	run(1500);
	CHECK(sm.view.channel_number == 3 && sm.view.scan_phase == SCAN_PH_HOLD);
	nxdn_engine_tx_stop(&nfar);
	run(600);
	CHECK(!nfar_tx && sm.view.channel_number == 3 && sm.view.scan_phase == SCAN_PH_RESUME);
	run(RADIO_SCAN_DELAY_MS + 600);
	CHECK(sm.view.channel_number != 3 && sm.view.scan == SCAN_ZONE);
	// Another group on the RAN (selective squelch), and the channel's group on another RAN: not held
	nfar_begin(true, 7, 2002, false, 21);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	CHECK(!scan_holds_on(3, 4000, &visits) && visits >= 1 && sm.view.scan == SCAN_ZONE);
	nfar_begin(true, 9, 2003, false, 20);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	CHECK(!scan_holds_on(3, 4000, &visits) && visits >= 1 && sm.view.scan == SCAN_ZONE);
	nxdn_engine_tx_stop(&nfar);
	run(600);
	req(RREQ_SCAN, RSCAN_STOP);

	// A 4800 bit/s channel on a board that only transmits 2400 symbols/s: never a stop. At 9600 it is one again
	stored_nxdn[3].conv.rate9600 = false;
	air_fit2400[0] = AIR_FIT_TX;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.scan == SCAN_ZONE && !scan_holds_on(3, 3000, &visits) && visits == 0);
	req(RREQ_SCAN, RSCAN_STOP);
	stored_nxdn[3].conv.rate9600 = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(!scan_holds_on(3, 3000, &visits) && visits >= 1);
	req(RREQ_SCAN, RSCAN_STOP);
	air_fit2400[0] = AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT;
	far_hz = 0;
	nfar_stop();
	nxdn_defaults();
}
#if CONFIG_DUAL_WATCH
// Dual watch with a stored NXDN channel on one side: the receiver alternates onto it and a call that plays holds the
// watch there. With P25 built too, a P25 channel on the other side: one engine at a time, each started at its look
static void test_nxdn_dual(void)
{
	const radio_nxdn_view_t *v = &sm.view.nxdn;
	uint16_t visits;

	setup_codeplug_nxdn();
	add(3, "NXDN rptr", 446100000, RADIO_MODE_DMR, false, 2);	// zone Local, position 2
	chans[3].kind = CODEPLUG_KIND_NXDN;
	chans[3].system = 0xFF;
	stored_nxdn[3] = (nxdn_conv_cfg_t){ .conv = { .rate9600 = true, .rx_ran = 7, .tx_ran = 7,
						      .squelch = NXDN_SQUELCH_SELECTIVE },
					    .unit_id = 4321, .target = 20, .target_unit = false };
	start();
	nxdn_defaults();
	far_hz = 446100000;
	const ui_dual_ref_t a = { false, 0, 0 }, b = { false, 0, 2 }, pair[2] = { a, b };
	CHECK(ui_svc_dual(pair, 0) && sm.view.scan == SCAN_DUAL);
	CHECK(!scan_holds_on(3, 3000, &visits) && visits >= 2 && sm.view.scan == SCAN_DUAL);
	nfar_begin(true, 7, 2001, false, 20);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	CHECK(scan_holds_on(3, 4000, &visits));
	CHECK(radio_nxdn_running() && (v->flags & RADIO_NXDN_PLAYED) && v->dst == 20);
	run(1500);
	CHECK(sm.view.channel_number == 3 && sm.view.scan_phase == SCAN_PH_HOLD);
	nxdn_engine_tx_stop(&nfar);
	run(600 + RADIO_DUAL_DELAY_MS + 600);
	CHECK(sm.view.scan == SCAN_DUAL && sm.view.channel_number == 1);
	// Another group: not held
	nfar_begin(true, 7, 2002, false, 21);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	CHECK(!scan_holds_on(3, 4000, &visits) && visits >= 1 && sm.view.scan == SCAN_DUAL);
	nxdn_engine_tx_stop(&nfar);
	run(600);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.scan == SCAN_OFF);
	// A rate the board can't receive: refused by the radio core
	stored_nxdn[3].conv.rate9600 = false;
	air_fit2400[0] = AIR_FIT_TX;
	req(RREQ_ZONE_POS, 0);
	ui_svc_dual(pair, 0);
	CHECK(sm.view.scan == SCAN_OFF);
	stored_nxdn[3].conv.rate9600 = true;
	air_fit2400[0] = AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT;
#if CONFIG_P25
	// P25 on A, NXDN on B: each look starts its own engine, the other stopped first
	add(1, "P25 simp", 446200000, RADIO_MODE_P25, false, 2);
	start();
	radio_p25_stats_t p0, p1;
	radio_nxdn_stats_t n0, n1;
	radio_p25_stats(&p0);
	radio_nxdn_stats(&n0);
	CHECK(ui_svc_dual(pair, 0) && sm.view.scan == SCAN_DUAL);
	bool p25_seen = false;
	for (int i = 0; i < 3000 / RADIO_POLL_MS; i++) {
		run(RADIO_POLL_MS);
		CHECK(!(radio_p25_running() && radio_nxdn_running()));
		p25_seen = p25_seen || (sm.view.channel_number == 1 && radio_p25_running());
	}
	radio_p25_stats(&p1);
	radio_nxdn_stats(&n1);
	CHECK(p25_seen && p1.begins >= p0.begins + 2 && n1.begins >= n0.begins + 2);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.scan == SCAN_OFF);
	setup_codeplug_nxdn();
#endif
	far_hz = 0;
	nfar_stop();
	nxdn_defaults();
}
#endif
#endif

// An NXDN conventional channel in the codeplug (kind 4, docs/spec/nxdn-codeplug.md 2): the radio runs it on its own
// parameters, not the settings; saved from the VFO it becomes one; parked where the board can't carry NXDN
static void test_nxdn_stored(void)
{
	static char buf[4096];
	const radio_nxdn_view_t *v = &sm.view.nxdn;
	radio_nxdn_settings_t s = nxdn_default_set;
	radio_nxdn_stats_t st;

	setup_codeplug_nxdn();
	// The safe stand-in as the codeplug reads it: DMR, the tag's kind and transmit allowed
	add(3, "NXDN rptr", 446100000, RADIO_MODE_DMR, false, 2);
	chans[3].tx_hz = 451100000;
	chans[3].kind = CODEPLUG_KIND_NXDN;
	chans[3].system = 0xFF;
	stored_nxdn[3] = (nxdn_conv_cfg_t){ .conv = { .rate9600 = true, .rx_ran = 7, .tx_ran = 8,
						      .squelch = NXDN_SQUELCH_SELECTIVE },
					    .unit_id = 4321, .target = 20, .target_unit = false };
	start();
	nxdn_defaults();
	aud.feed = true;
	req(RREQ_ZONE_POS, 2);
	CHECK(sm.view.channel_number == 3 && sm.view.ch.mode == RADIO_MODE_NXDN && sm.view.ch.kind == CODEPLUG_KIND_NXDN);
	CHECK(hw_mode == RADIO_MODE_DMR && hw.rx_bw == RADIO_BW_12K5 && radio_nxdn_running() && !sm.dmr_running);
	CHECK(v->rx_ran == 7 && v->tx_ran == 8 && v->talk == 20 && !(v->flags & RADIO_NXDN_TALK_UNIT) &&
	      (v->flags & RADIO_NXDN_9600));
	nxdn_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "nxdn.rx_ran=0\n") != NULL);	// the settings are not what runs
	CHECK(strstr(buf, "nxdn.run.rate=9600\nnxdn.run.rx_ran=7\nnxdn.run.tx_ran=8\nnxdn.run.rx_gid=20\n") != NULL);
	CHECK(strstr(buf, "nxdn.run.unit_id=4321\nnxdn.run.unit_call=0\nnxdn.run.tx_dst=20\n") != NULL);

	// RAN squelch first: another RAN is not taken
	nfar_begin(true, 9, 2001, false, 20);
	nfar_call(600);
	CHECK(sm.view.state == RS_IDLE);
	radio_nxdn_stats(&st);
	CHECK(st.rx_calls == 0 && st.ran_rejects > 0);
	// Selective on RAN 7: the channel's group plays, another group shows and does not, all and a unit call to us play
	nfar_begin(true, 7, 2001, false, 20);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	run(800);
	CHECK(sm.view.state == RS_RX && (v->flags & RADIO_NXDN_PLAYED) && v->dst == 20 && v->ran == 7);
	nxdn_engine_tx_stop(&nfar);
	run(500);
	nfar_begin(true, 7, 2001, false, 21);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	run(800);
	CHECK((v->flags & RADIO_NXDN_RX) && !(v->flags & RADIO_NXDN_PLAYED) && v->dst == 21 && !hw.led[LED_GREEN]);
	nxdn_engine_tx_stop(&nfar);
	run(500);
	nfar_begin(true, 7, 2001, false, NXDN_ID_ALL);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	run(800);
	CHECK((v->flags & RADIO_NXDN_PLAYED) && v->dst == NXDN_ID_ALL);
	nxdn_engine_tx_stop(&nfar);
	run(500);
	nfar_begin(true, 7, 2001, true, 4321);
	CHECK(nxdn_engine_tx_prepare(&nfar) && nxdn_engine_tx_go(&nfar));
	nfar_tx = true;
	run(800);
	CHECK((v->flags & (RADIO_NXDN_PLAYED | RADIO_NXDN_UNIT)) == (RADIO_NXDN_PLAYED | RADIO_NXDN_UNIT));
	nxdn_engine_tx_stop(&nfar);
	run(500);
	CHECK(sm.view.state == RS_IDLE);

	// PTT: the channel's TX RAN, our unit ID and its group, on its TX frequency at 12.5 kHz
	nfar_begin(true, 8, 2001, false, 1);
	run(100);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 451100000 && hw.tx_bw == RADIO_BW_12K5);
	run(1000);
	CHECK(nfar_heard.starts == 1 && nfar_heard.call.ran == 8 && nfar_heard.call.vcall.src == 4321 &&
	      nfar_heard.call.vcall.dst == 20 && nfar_heard.call.vcall.call_type == NXDN_CALL_GROUP);
	req(RREQ_PTT, 0);
	run(600);
	CHECK(sm.view.state == RS_IDLE && !hw.keyed);

	// The debug link's settings don't reach a stored channel
	s.rx_ran = 33;
	s.tx_ran = 34;
	s.unit_id = 777;
	s.rate9600 = false;
	radio_req_nxdn_settings(&s);
	nxdn_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "nxdn.rx_ran=33\n") != NULL && strstr(buf, "nxdn.run.rx_ran=7\n") != NULL &&
	      strstr(buf, "nxdn.run.unit_id=4321\n") != NULL && strstr(buf, "nxdn.run.rate=9600\n") != NULL);
	nxdn_defaults();

	// 4800 bit/s, a unit target, RAN squelch: read again when the channel is
	stored_nxdn[3].conv.rate9600 = false;
	stored_nxdn[3].conv.squelch = NXDN_SQUELCH_RAN;
	stored_nxdn[3].target = 2001;
	stored_nxdn[3].target_unit = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	nxdn_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "nxdn.run.rate=4800\nnxdn.run.rx_ran=7\nnxdn.run.tx_ran=8\nnxdn.run.rx_gid=0\n") != NULL);
	CHECK(strstr(buf, "nxdn.run.unit_call=1\nnxdn.run.tx_dst=2001\n") != NULL);
	CHECK(v->talk == 2001 && (v->flags & RADIO_NXDN_TALK_UNIT) && !(v->flags & RADIO_NXDN_9600));
	// Selective with a unit target: all and unit calls to us only
	stored_nxdn[3].conv.squelch = NXDN_SQUELCH_SELECTIVE;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	nxdn_view_text(buf, sizeof buf);
	CHECK(strstr(buf, "nxdn.run.rx_gid=65535\n") != NULL);

	// No mode change for now on a channel of a kind: its mode is the kind's
	req(RREQ_MODE, RADIO_MODE_FM);
	CHECK(sm.view.ch.mode == RADIO_MODE_NXDN && radio_nxdn_running() && !(sm.view.temp & RADIO_TEMP_MODE));
	// Receive only from the tag: PTT refused as on any channel
	chans[3].rx_only = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	int keyed = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == keyed);
	req(RREQ_PTT, 0);
	chans[3].rx_only = false;
	// The editor's feature list says NXDN here
	CHECK(strstr(build_modes_now(), "NXDN,") != NULL);

	// A board that can't carry NXDN: parked (DMR modem off, never keyed), and no NXDN in the feature list
	air_nofit = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 2);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && !radio_nxdn_running() && !sm.dmr_running && hw_mode == RADIO_MODE_FM);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_MODE && hw.tx_calls == keyed);
	req(RREQ_PTT, 0);
	CHECK(strstr(build_modes_now(), "NXDN,") == NULL && build_features_now()[0] != '\0');
	air_nofit = false;

	// The VFO keeps the settings
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 446100000);
	req(RREQ_MODE, RADIO_MODE_NXDN);
	nxdn_view_text(buf, sizeof buf);
	CHECK(sm.view.vfo && strstr(buf, "nxdn.run.rx_ran=0\n") != NULL && strstr(buf, "nxdn.run.unit_id=1\n") != NULL);

	// Saved as a channel (the editor's New from current): a stored NXDN channel from the NXDN settings, its contact
	// the stored one of the talk target
	s = nxdn_default_set;
	s.rate9600 = false;
	s.rx_ran = 12;
	s.tx_ran = 13;
	s.rx_gid = 9;
	s.tx_dst = 9;
	radio_req_nxdn_settings(&s);
	run(20);
	memset(&nxdn_saved, 0, sizeof nxdn_saved);
	channel_t ed = sm.view.ch;
	CHECK(ed.mode == RADIO_MODE_NXDN && ed.kind == CODEPLUG_KIND_NONE);
	CHECK(ui_svc_write_channel(6, &ed, 0));
	storage_job_poll();
	CHECK(nxdn_saved.number == 6 && !nxdn_saved.conv.rate9600 && nxdn_saved.conv.rx_ran == 12 &&
	      nxdn_saved.conv.tx_ran == 13);
	CHECK(nxdn_saved.conv.squelch == NXDN_SQUELCH_SELECTIVE && nxdn_saved.ch.contact == 3);
	// No stored contact for the group: none (group 1); RAN squelch
	s.tx_dst = 4242;
	s.rx_gid = 0;
	radio_req_nxdn_settings(&s);
	run(20);
	CHECK(ui_svc_write_channel(6, &ed, 0));
	storage_job_poll();
	CHECK(nxdn_saved.ch.contact == 0 && nxdn_saved.conv.squelch == NXDN_SQUELCH_RAN);
	nxdn_defaults();
	// A copy of the stored NXDN channel at another number takes its tag; edited in place, the tag follows by itself
	ed = chans[3];
	sim_tag_copy_from = 0;
	nxdn_saved.number = 0;
	CHECK(ui_svc_write_channel(7, &ed, 3));
	storage_job_poll();
	CHECK(sim_tag_copy_from == 3 && chans[7].kind == CODEPLUG_KIND_NXDN && nxdn_saved.number == 0);
	// The channel editor's RAN and rate (ui_svc_write_channel_air): the stored channel written again with its block
	ui_air_conv_t air = { .nxdn = { .rate9600 = false, .rx_ran = 17, .tx_ran = 17, .squelch = NXDN_SQUELCH_RAN } };
	sim_tag_copy_from = 0;
	CHECK(ui_svc_write_channel_air(3, &ed, 3, &air));
	storage_job_poll();
	CHECK(nxdn_saved.number == 3 && nxdn_saved.conv.rx_ran == 17 && !nxdn_saved.conv.rate9600 && sim_tag_copy_from == 0);
	// As a copy: the source's tag first, then the block
	nxdn_saved.number = 0;
	CHECK(ui_svc_write_channel_air(8, &ed, 3, &air));
	storage_job_poll();
	CHECK(nxdn_saved.number == 8 && nxdn_saved.conv.rx_ran == 17 && sim_tag_copy_from == 3);
	sim_tag_copy_from = 0;
	nxdn_saved.number = 0;
	CHECK(ui_svc_write_channel(3, &ed, 3));
	storage_job_poll();
	CHECK(sim_tag_copy_from == 0 && nxdn_saved.number == 0);
	// A channel in NXDN for now, saved: a stored NXDN channel too
	req(RREQ_MODE, RADIO_MODE_FM);
	req(RREQ_VFO, 0);
	req(RREQ_ZONE_POS, 0);
	CHECK(sm.view.ch.kind == CODEPLUG_KIND_NONE && sm.view.channel_number != 0);
	uint16_t plain = sm.view.channel_number;
	req(RREQ_MODE, RADIO_MODE_NXDN);
	CHECK(sm.view.ch.mode == RADIO_MODE_NXDN && (sm.view.temp & RADIO_TEMP_MODE));
	nxdn_saved.number = 0;
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(nxdn_saved.number == plain);
	req(RREQ_MODE, RADIO_MODE_FM);
	nfar_stop();
	memset(stored_nxdn, 0, sizeof(stored_nxdn));
}

static void test_nxdn(void)
{
	test_nxdn_rx();
	test_nxdn_tx();
	test_nxdn_mode();
	test_nxdn_stored();
#if CONFIG_SCAN
	test_nxdn_scan();
#if CONFIG_DUAL_WATCH
	test_nxdn_dual();
#endif
#endif
	setup_codeplug();
	req(RREQ_RELOAD, 0);
	run(50);
	if (radio_nxdn_running()) {
		radio_nxdn_end();
	}
}
#endif

#if CONFIG_AIR
static void air_step(void)
{
	air_step1(0);
	air_step1(1);
#if CONFIG_P25
	far_step();
#endif
#if CONFIG_NXDN
	nfar_step();
#endif
}
#endif

#if CONFIG_DMR_TRUNK_TX
// A Tier III site on the sim: TS1 of 450.0125 is the TSCC (C_ALOHA, no registration asked), LCN 1 is that carrier
#define T3_OWN		2345679u
#define T3_TG		1u
#define T3_RX_HZ	450012500u
#define T3_TX_HZ	455012500u

// A site's PDU comes when the radio can hear it: not in the middle of the radio's own burst list
static void t3_quiet(void)
{
	for (int i = 0; i < 60 && dmr_call_data_busy(); i++) {
		run(5);
	}
}

static void t3_csbk_rx(const t3_csbk_t *p, int8_t ts)
{
	t3_quiet();
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_CSBK;
	b.sync = DMR_SYNC_BS;
	b.ts = ts;
	b.cc = 2;
	b.crc_ok = true;
	b.in_step = true;
	b.len = T3_CSBK_BYTES;
	CHECK(t3_csbk_build(p, T3_CRC_MASK_CSBK, b.payload));
	sim_rx(&b);
}

static uint16_t t3_code(void)
{
	t3_syscode_t c = { T3_MODEL_SMALL, 5, 1, T3_PAR_A };
	return t3_syscode_join(&c);
}

static bool t3_site_offset;	// the TSCC's Aloha says offset timing

// The TSCC for ms: an Aloha on TS1 every frame
static void t3_site_run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 60) {
		t3_csbk_t a = { .lb = true, .op = T3_OP_C_ALOHA };
		a.u.aloha.syscode = t3_code();
		a.u.aloha.nrand_wait = 6;
		a.u.aloha.backoff = 1;
		a.u.aloha.version = 2;
		a.u.aloha.offset = t3_site_offset;
		t3_csbk_rx(&a, 1);
		run(60);
	}
}

static unsigned t3_sent(uint8_t op)
{
	unsigned n = 0;
	for (unsigned i = 0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_CSBK && (sim.air[i].payload[0] & 0x3Fu) == op) {
			n++;
		}
	}
	return n;
}

// PTT on the TSCC asks for the call, held through the wait (the request is sent, not refused as talking), and on
// the grant the radio talks; with nothing heard while it talks, the call stays up (its own TX is the activity)
// Stage the test system and select it: the radio camps on its TSCC
static void t3_stage_with(uint16_t opts, const uint8_t *key)
{
	radio_t3_setup_t *s = radio_t3_staged();
	radio_t3_reset(s);
	s->cfg.own_id = T3_OWN;
	s->cfg.auth.model = T3_MODEL_SMALL;
	s->cfg.auth.net = 5;
	s->cfg.auth.category = T3_PAR_A;
	s->cfg.opts = opts;
	if (key != NULL) {
		s->cfg.auth_key_set = true;
		memcpy(s->cfg.auth_key, key, T3_AUTH_KEY_LEN);
	}
	s->cc = 2;
	s->cfg.ctrl[0] = (t3_chan_t){ .lcn = 1, .rx_hz = T3_RX_HZ, .tx_hz = T3_TX_HZ, .cc = 2 };
	s->cfg.nctrl = 1;
	s->chans[0] = s->cfg.ctrl[0];
	s->nchans = 1;
	s->cfg.tg[0] = (t3_tg_t){ T3_TG, T3_TG_LISTEN | T3_TG_TALK };
	s->cfg.ntg = 1;
	radio_t3_fix(s);
	req(RREQ_T3, RADIO_T3_START);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED);
}

static void t3_stage_start(void)
{
	t3_stage_with(T3_OPT_PRIVATE | T3_OPT_STUN, NULL);
}

static void t3_data_rx(uint8_t kind, const uint8_t *d, int8_t ts)
{
	if (kind == DMR_BURST_DATA_HEADER) {
		t3_quiet();	// a UDT's blocks follow its header in the next frames: wait only before the header
	}
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = kind;
	b.sync = DMR_SYNC_BS;
	b.ts = ts;
	b.cc = 2;
	b.crc_ok = true;
	b.in_step = true;
	b.len = T3_CSBK_BYTES;
	memcpy(b.payload, d, T3_CSBK_BYTES);
	sim_rx(&b);
}

// Random access on the TSCC (TS1) reads the AT bit of the CACH describing its inbound: with aligned timing that is the
// CACH naming slot 2 (361-1 table 5.1, 4.5), so a call on the other slot doesn't block it; with offset timing, slot 1's
static bool t3_at_case(bool offset, uint8_t busy)
{
	dmr_start(true);
	sim.repeater = true;
	sim.at_busy = busy;
	t3_site_offset = offset;
	t3_stage_start();
	unsigned before = t3_sent(T3_OP_C_RAND);
	req(RREQ_PTT, 1);
	t3_site_run(900);
	bool sent = t3_sent(T3_OP_C_RAND) > before;
	req(RREQ_PTT, 0);
	run(100);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
	sim.at_busy = 0;
	t3_site_offset = false;
	return sent;
}

static void test_t3_at(void)
{
	CHECK(t3_at_case(false, 1));	// aligned: slot 1's CACH (TS2's inbound) busy, the TSCC's free
	CHECK(!t3_at_case(false, 2));	// aligned: slot 2's CACH (the TSCC's inbound) busy
	CHECK(!t3_at_case(true, 1));	// offset: slot 1's own
	CHECK(t3_at_case(true, 2));
}

static void test_t3_talk(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	CHECK(sm.view.dmr.own_id == T3_OWN);

	unsigned before = t3_sent(T3_OP_C_RAND);
	req(RREQ_PTT, 1);
	t3_site_run(600);
	CHECK(radio_t3v.ptt_wait && t3_sent(T3_OP_C_RAND) > before);
	CHECK(radio_t3v.send_fail == 0);
	// The site's bursts go on after our C_RAND: the turnaround is measured, short on the simulated modem
	CHECK(radio_t3v.turns >= 1 && radio_t3v.turn_ms <= 90 && radio_t3v.turn_max_ms >= radio_t3v.turn_ms &&
	      radio_t3v.turn_slow == 0);

	t3_csbk_t g = { .lb = true, .op = T3_OP_TV_GRANT };
	g.u.grant.lpcn = 1;
	g.u.grant.slot = 2;
	g.u.grant.dst = T3_TG;
	g.u.grant.src = T3_OWN;
	t3_csbk_rx(&g, 1);
	run(200);
	CHECK(radio_t3v.state == T3_FOLLOW && radio_t3v.can_talk);
	CHECK(sm.view.state == RS_TX && sm.view.ch.timeslot == 2);

	run(4000);		// longer than TV_Inactive, nothing heard
	CHECK(radio_t3v.state == T3_FOLLOW && sm.view.state == RS_TX);
	CHECK(radio_t3v.returns == 0);

	req(RREQ_PTT, 0);
	// The call's hang, filled with idle bursts: a signal, so the call stays (6.6.2.3.2.9 a)
	static const uint8_t idle[T3_CSBK_BYTES];
	for (int i = 0; i < 70; i++) {
		t3_data_rx(DMR_BURST_IDLE, idle, 2);
		run(60);
	}
	CHECK(radio_t3v.state == T3_FOLLOW && radio_t3v.returns == 0);
	run(4000);		// then nothing: the inactivity timer ends it
	CHECK(radio_t3v.state != T3_FOLLOW && sm.view.state != RS_TX);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}

// Short data: the upload goes out as a UDT header and its blocks in consecutive frames of the TSCC's slot; a download
// to the radio is kept and answered
static void test_t3_sdata(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	radio_t3_setup_t *s = radio_t3_staged();
	s->sd_dst = 1000001u;
	s->sd_group = false;
	s->sd_format = T3_UDTF_ISO8;
	memcpy(s->sd_data, "hello trunk world", 17);
	s->sd_len = 17;
	unsigned before = t3_sent(T3_OP_C_RAND);
	req(RREQ_T3, RADIO_T3_SDATA);
	t3_site_run(600);
	CHECK(t3_sent(T3_OP_C_RAND) > before && radio_t3v.ax_svc == T3_SVC_SDATA);

	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_IND_UDT;
	h.u.ahoy.blocks = 1;
	h.u.ahoy.dst = T3_OWN;
	h.u.ahoy.src = T3_ID_SDMI;
	unsigned from = sim.air_count;
	t3_csbk_rx(&h, 1);
	t3_site_run(400);
	int hdr = -1;
	for (unsigned i = from; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_DATA_HEADER) {
			hdr = (int)i;
			break;
		}
	}
	CHECK(hdr >= 0 && (unsigned)hdr + 2u < sim.air_count);
	if (hdr >= 0 && (unsigned)hdr + 2u < sim.air_count) {
		const sim_air_t *a = &sim.air[hdr];
		t3_udt_t u;
		CHECK(t3_udt_parse(a->payload, &u) && u.op == T3_OP_C_UDTHU && u.dst == 1000001u && u.src == T3_OWN &&
		      u.blocks == 2 && u.format == T3_UDTF_ISO8);
		CHECK(sim.air[hdr + 1].kind == DMR_BURST_RATE_12_DATA && sim.air[hdr + 2].kind == DMR_BURST_RATE_12_DATA);
		// One frame apart each: consecutive inbound slots of the TSCC's slot
		CHECK(sim.air[hdr + 1].slot == a->slot + 2u && sim.air[hdr + 2].slot == a->slot + 4u);
		uint8_t blk[2][T3_UDT_BLOCK];
		memcpy(blk[0], sim.air[hdr + 1].payload, T3_UDT_BLOCK);
		memcpy(blk[1], sim.air[hdr + 2].payload, T3_UDT_BLOCK);
		CHECK(t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])blk, 2) && memcmp(blk, "hello trunk world", 17) == 0);
	}
	CHECK(radio_t3v.send_fail == 0);

	// A download to the radio
	uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK], pad, d[T3_CSBK_BYTES];
	t3_udt_t u = { .op = T3_OP_C_UDTHD, .a = true, .format = T3_UDTF_ISO8, .dst = T3_OWN, .src = 1000001u };
	u.blocks = t3_udt_pack((const uint8_t *)"hi there", 8, 0xFF, blk, &pad);
	u.pad = pad;
	t3_udt_build(&u, d);
	before = t3_sent(T3_OP_C_ACKU);
	t3_data_rx(DMR_BURST_DATA_HEADER, d, 1);
	run(60);
	t3_data_rx(DMR_BURST_RATE_12_DATA, blk[0], 1);
	t3_site_run(300);
	CHECK(radio_t3v.sd_count == 1 && radio_t3v.sd_len == 8 && memcmp(radio_t3v.sd_data, "hi there", 8) == 0 &&
	      radio_t3v.sd_src == 1000001u);
	CHECK(t3_sent(T3_OP_C_ACKU) > before);
	// ... into the message inbox, as a message from the sender
	int msgs = sms_inbox_count();
	CHECK(msgs >= 1);
	if (msgs >= 1) {
		const dmrDataMessage_t *m = dmrDataServiceMessage(0);
		CHECK(m->src == 1000001u && !m->group && strcmp(m->text, "hi there") == 0 && m->unread);
	}
	while (sms_inbox_count() > 0 && sms_inbox_delete(0)) {
	}
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}
// Packet data (6.6.3): the data service's message waits while the radio asks the TSCC for a data channel, then goes on
// the granted slot without preambles; data to the radio on a payload channel reaches the data service
static void test_t3_pdata(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	unsigned from = sim.air_count;
	CHECK(sms_send(1000001u, false, "hello trunk", false, DMR_SMS_FORMAT_ETSI_IP));
	t3_site_run(600);
	CHECK(radio_t3v.pd_wait && radio_t3v.ax_svc == T3_SVC_PDATA);
	int rand_at = -1;
	bool data_early = false;
	for (unsigned i = from; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_CSBK && (sim.air[i].payload[0] & 0x3Fu) == T3_OP_C_RAND) {
			rand_at = (int)i;
		}
		data_early |= sim.air[i].kind == DMR_BURST_DATA_HEADER;
	}
	CHECK(rand_at >= 0 && !data_early);
	if (rand_at >= 0) {
		t3_csbk_t q;
		CHECK(t3_csbk_parse(sim.air[rand_at].payload, &q) && q.u.rand.kind == T3_SK_IND_DATA &&
		      q.u.rand.dst == 1000001u && q.u.rand.src == T3_OWN);
	}

	// The grant: on TS2 of the channel, the data goes once settled
	t3_csbk_t g = { .lb = true, .op = T3_OP_PD_GRANT };
	g.u.grant.lpcn = 1;
	g.u.grant.slot = 2;
	g.u.grant.dst = 1000001u;
	g.u.grant.src = T3_OWN;
	t3_quiet();
	from = sim.air_count;
	t3_csbk_rx(&g, 1);
	run(1500);
	CHECK(radio_t3v.state == T3_FOLLOW && radio_t3v.call_data && !radio_t3v.can_talk && sm.view.ch.timeslot == 2);
	CHECK(!radio_t3v.pd_wait);
	int hdr = -1;
	unsigned csbks = 0;
	for (unsigned i = from; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_DATA_HEADER && hdr < 0) {
			hdr = (int)i;
		}
		csbks += hdr < 0 && sim.air[i].kind == DMR_BURST_CSBK;
	}
	CHECK(hdr >= 0 && csbks == 0);	// no preambles
	if (hdr >= 0) {
		const uint8_t *h = sim.air[hdr].payload;
		uint32_t dst = ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
		uint32_t src = ((uint32_t)h[5] << 16) | ((uint32_t)h[6] << 8) | h[7];
		CHECK(dst == 1000001u && src == T3_OWN && (h[0] & 0x80) == 0);
		CHECK((unsigned)hdr + 1u < sim.air_count && sim.air[hdr + 1].kind == DMR_BURST_RATE_12_DATA);
	}
	CHECK(sms_send_state() == DMR_REQ_OK);
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.dst = 1000001u;
	t3_csbk_rx(&c, 2);
	run(300);
	CHECK(radio_t3v.state == T3_CAMPED);

	// Data to the radio: the TSCC's check is answered, PD_GRANT followed, the text taken by the data service
	unsigned acks = t3_sent(T3_OP_C_ACKU);
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_IND_DATA;
	h.u.ahoy.dst = T3_OWN;
	h.u.ahoy.src = 1000001u;
	t3_csbk_rx(&h, 1);
	t3_site_run(300);
	CHECK(t3_sent(T3_OP_C_ACKU) > acks);
	g.u.grant.dst = T3_OWN;
	g.u.grant.src = 1000001u;
	t3_csbk_rx(&g, 1);
	run(500);
	CHECK(radio_t3v.state == T3_FOLLOW && radio_t3v.call_data);
	int inbox = sms_inbox_count();
	dmrBurst_t b[16];
	int n = dmrDataBuildIPText(false, T3_OWN, 1000001u, "hi radio", DMR_TYPE_RATE_1_2, 0, b, 16);
	CHECK(n >= 2);
	for (int i = 0; i < n; i++) {
		t3_data_rx(b[i].dataType, b[i].payload, 2);
		run(60);
	}
	run(500);
	CHECK(sms_inbox_count() == inbox + 1);
	dmrDataMessage_t m;
	CHECK(sms_inbox_get(0, &m) && m.src == 1000001u && strcmp(m.text, "hi radio") == 0);
	CHECK(sms_inbox_delete(0));	// the inbox as the later tests expect it
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}
// The last C_RAND sent at or after from with this Service_Kind; false when none
static bool t3_rand_since(unsigned from, uint8_t kind, t3_csbk_t *q)
{
	bool found = false;
	for (unsigned i = from; i < sim.air_count; i++) {
		t3_csbk_t c;
		if (sim.air[i].kind == DMR_BURST_CSBK && (sim.air[i].payload[0] & 0x3Fu) == T3_OP_C_RAND &&
		    t3_csbk_parse(sim.air[i].payload, &c) && c.u.rand.kind == kind) {
			*q = c;
			found = true;
		}
	}
	return found;
}

static uint8_t t3_rand_status(const t3_csbk_t *q)
{
	return (uint8_t)(((q->u.rand.options & 0x1Fu) << 2) | (q->u.rand.extra & 3u));
}

// A radio check on a trunked system goes as a status poll (6.6.6.2): the poll's acknowledgement is the check's answer;
// a refusal fails it at once, without the conventional retries
static void test_t3_radio_check(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	uint8_t kind;
	uint32_t dst;
	unsigned from = sim.air_count;
	CHECK(dmr_signal_radio_check(1000001u));
	t3_site_run(600);
	t3_csbk_t q;
	CHECK(t3_rand_since(from, T3_SK_STATUS, &q) && t3_rand_status(&q) == T3_STATUS_POLL && q.u.rand.dst == 1000001u &&
	      !(q.u.rand.options & 0x40u));
	CHECK(air_count(from, DMR_BURST_CSBK, DMR_CSBKO_RADIO_CHECK) == 0);	// no conventional check on the TSCC
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_SENDING);
	t3_csbk_t a = { .lb = true, .op = T3_OP_C_ACKD };
	a.u.ack.reason = T3_RC_MS_POLL_ACCEPTED;
	a.u.ack.info = 5;
	a.u.ack.dst = T3_OWN;
	a.u.ack.src = 1000001u;
	t3_csbk_rx(&a, 1);
	t3_site_run(300);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_OK && dst == 1000001u);

	// Refused (the polled MS isn't there)
	from = sim.air_count;
	CHECK(dmr_signal_radio_check(1000002u));
	t3_site_run(600);
	CHECK(t3_rand_since(from, T3_SK_STATUS, &q) && q.u.rand.dst == 1000002u);
	t3_csbk_t n = { .lb = true, .op = T3_OP_C_ACKD };
	n.u.ack.reason = T3_RC_MS_AWAY;
	n.u.ack.dst = T3_OWN;
	n.u.ack.src = 1000002u;
	t3_csbk_rx(&n, 1);
	t3_site_run(300);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_FAILED && dst == 1000002u);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}

#if CONFIG_DMR_TRUNK_GW
// Digits from the UI (RREQ_T3_DIAL) go to the system in use: PTT to the PSTN gateway asks for the call, and the
// digits go up when the gateway asks; an empty string drops them, and the call is refused again
static void test_t3_dial(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	uint32_t w[2];
	radio_t3_dial_args("5551234", w);
	CHECK(w[0] == 0x5551234Fu && w[1] == 0xFFFFFFFFu);
	radio_t3_dial_args("0123456789*#01234567", w);	// the first 16
	CHECK(w[0] == 0x01234567u && w[1] == 0x89AB0123u);
	radio_t3_dial_args("5551234", w);
	radio_req_t r = { RREQ_T3_DIAL, (int32_t)w[0], w[1] };
	radio_sm_request(&sm, &r, now);
	req(RREQ_DMR_TARGET, (int32_t)((1u << 24) | T3_ID_PSTNI));
	unsigned from = sim.air_count;
	req(RREQ_PTT, 1);
	t3_site_run(600);
	t3_csbk_t q;
	CHECK(t3_rand_since(from, T3_SK_IND_VOICE, &q) && q.u.rand.dst == T3_ID_PSTNI);
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_IND_VOICE;
	h.u.ahoy.dst = T3_OWN;
	h.u.ahoy.src = T3_ID_PSTNI;
	from = sim.air_count;
	t3_csbk_rx(&h, 1);
	t3_site_run(400);
	int hdr = -1;
	for (unsigned i = from; i < sim.air_count && hdr < 0; i++) {
		hdr = sim.air[i].kind == DMR_BURST_DATA_HEADER ? (int)i : -1;
	}
	CHECK(hdr >= 0 && (unsigned)hdr + 1u < sim.air_count);
	if (hdr >= 0 && (unsigned)hdr + 1u < sim.air_count) {
		t3_udt_t u;
		CHECK(t3_udt_parse(sim.air[hdr].payload, &u) && u.sf && u.dst == T3_ID_PSTNI && u.format == T3_UDTF_BCD);
		CHECK(memcmp(sim.air[hdr + 1].payload, "\x55\x51\x23\x4f", 4) == 0);
	}
	req(RREQ_PTT, 0);
	t3_site_run(4000);	// unanswered: NRand_NR tries of NWait + 2 frames and the backoff, then free again

	// The focused row's target (one call) goes before the one chosen for now
	req(RREQ_DMR_FOCUS, (int32_t)((1u << 24) | 1000001u));
	from = sim.air_count;
	req(RREQ_PTT, 1);
	t3_site_run(600);
	CHECK(t3_rand_since(from, T3_SK_IND_VOICE, &q) && q.u.rand.dst == 1000001u);
	req(RREQ_PTT, 0);
	req(RREQ_DMR_FOCUS, 0);
	t3_site_run(3000);

	// No digits: refused, nothing asked for
	radio_t3_dial_args("", w);
	r = (radio_req_t){ RREQ_T3_DIAL, (int32_t)w[0], w[1] };
	radio_sm_request(&sm, &r, now);
	from = sim.air_count;
	req(RREQ_PTT, 1);
	t3_site_run(600);
	CHECK(!t3_rand_since(from, T3_SK_IND_VOICE, &q));
	req(RREQ_PTT, 0);
	req(RREQ_DMR_TARGET_CLEAR, 0);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}
#endif

// A selective clear on a payload slot of the TSCC's own carrier (6.6.2.3.2.7): the P_ACKU goes out on that slot before
// the radio returns, and once back on the TSCC the radio ignores the clear's repeats on the payload slot
static void test_t3_clear_slot(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	t3_csbk_t g = { .lb = true, .op = T3_OP_TV_GRANT };
	g.u.grant.lpcn = 1;
	g.u.grant.slot = 2;
	g.u.grant.dst = T3_TG;
	g.u.grant.src = 1000001u;
	t3_csbk_rx(&g, 1);
	run(300);
	CHECK(radio_t3v.state == T3_FOLLOW && sm.view.ch.timeslot == 2);
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_CANCEL;
	h.u.ahoy.group = true;
	h.u.ahoy.dst = T3_TG;
	h.u.ahoy.src = T3_ID_TSI;
	unsigned from = sim.air_count;
	t3_csbk_rx(&h, 2);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED);
	unsigned acks = 0;
	int8_t ts = 0;
	for (unsigned i = from; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_CSBK && (sim.air[i].payload[0] & 0x3Fu) == T3_OP_P_ACKU) {
			acks++;
			ts = sim.air[i].ts;
		}
	}
	CHECK(acks == 1 && ts == 2);
	// The clear again, on the payload slot, while camped: not the TSCC's slot, not answered
	h.u.ahoy.group = false;
	h.u.ahoy.dst = T3_OWN;
	from = sim.air_count;
	t3_csbk_rx(&h, 2);
	t3_site_run(600);
	unsigned after = 0;
	for (unsigned i = from; i < sim.air_count; i++) {
		after += sim.air[i].kind == DMR_BURST_CSBK;
	}
	CHECK(after == 0);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}

// Emergency on a trunked system: the alarm is status 126 to the target, acknowledged once (no repeats); the call
// asks with the emergency option; the end of the emergency sends status 124; status 126 heard raises the alert
static void test_t3_emergency(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	dmr_emergency_settings_t es;
	dmr_emergency_settings_default(&es);
	es.mode = DMR_EMERGENCY_MODE_ALARM_CALL;
	es.hot_mic_s = 2;
	es.alarm_tries = 3;
	dmr_emergency_set_settings(&es);
	run(20);
	unsigned from = sim.air_count;
	CHECK(dmr_emergency_start());
	t3_site_run(600);
	t3_csbk_t q;
	CHECK(t3_rand_since(from, T3_SK_STATUS, &q) && t3_rand_status(&q) == T3_STATUS_EMERGENCY &&
	      (q.u.rand.options & 0x40u) && q.u.rand.dst == T3_TG);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_ALARM);
	t3_csbk_t a = { .lb = true, .op = T3_OP_C_ACKD };
	a.u.ack.reason = T3_RC_MESSAGE_ACCEPTED;
	a.u.ack.dst = T3_OWN;
	a.u.ack.src = T3_TG;
	t3_csbk_rx(&a, 1);
	from = sim.air_count;
	t3_site_run(600);
	// Acknowledged: no second alarm; the hot mic asks for an emergency call to the same group
	t3_csbk_t v;
	CHECK(!t3_rand_since(from, T3_SK_STATUS, &q));
	CHECK(t3_rand_since(from, T3_SK_TG_VOICE, &v) && (v.u.rand.options & T3_SO_EMERGENCY) && v.u.rand.dst == T3_TG);
	t3_csbk_t g = { .lb = true, .op = T3_OP_TV_GRANT };
	g.u.grant.lpcn = 1;
	g.u.grant.slot = 2;
	g.u.grant.dst = T3_TG;
	g.u.grant.src = T3_OWN;
	g.u.grant.emergency = true;
	t3_csbk_rx(&g, 1);
	run(500);
	CHECK(radio_t3v.state == T3_FOLLOW && sm.view.state == RS_TX);
	run(2500);	// the hot mic runs out
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_ACTIVE && sm.view.state != RS_TX);
	req(RREQ_T3, RADIO_T3_END);
	run(400);
	CHECK(radio_t3v.state == T3_CAMPED);

	// Canceled: status 124 to the alarm's target
	from = sim.air_count;
	dmr_emergency_cancel();
	t3_site_run(600);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_IDLE);
	CHECK(t3_rand_since(from, T3_SK_STATUS, &q) && t3_rand_status(&q) == T3_STATUS_EMERGENCY_CANCEL &&
	      q.u.rand.dst == T3_TG);
	t3_csbk_rx(&a, 1);
	from = sim.air_count;
	t3_site_run(600);
	CHECK(!t3_rand_since(from, T3_SK_STATUS, &q));	// once

	// Heard: another MS's status 126 to our group raises the alarm alert
	dmr_emergency_stats_t s0, s1;
	dmr_emergency_get_stats(&s0);
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_STATUS;
	h.u.ahoy.group = true;
	h.u.ahoy.options = T3_STATUS_EMERGENCY >> 2;
	h.u.ahoy.blocks = T3_STATUS_EMERGENCY & 3;
	h.u.ahoy.dst = T3_TG;
	h.u.ahoy.src = 1000002u;
	t3_csbk_rx(&h, 1);
	t3_site_run(300);
	dmr_emergency_get_stats(&s1);
	dmr_emergency_alert_t al;
	CHECK(s1.alerts_alarm == s0.alerts_alarm + 1 && dmr_emergency_alert(&al) && al.src == 1000002u && al.dst == T3_TG);
	dmr_emergency_dismiss();
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	dmr_emergency_settings_default(&es);
	dmr_emergency_set_settings(&es);
	run(20);
	sim.repeater = false;
}
// A stun outlasts leaving the system and coming back; PTT is refused while stunned; the revive lifts it
// A broadcast talkgroup: PTT asks with the broadcast option; on the BTV_GRANT the voice LC carries the broadcast
// service option (361-2 5.3.3.1)
static void test_t3_broadcast(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	radio_t3_staged()->cfg.tg[0].flags |= T3_TG_BROADCAST;
	req(RREQ_T3, RADIO_T3_START);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED);
	unsigned from = sim.air_count;
	req(RREQ_PTT, 1);
	t3_site_run(600);
	t3_csbk_t q;
	CHECK(t3_rand_since(from, T3_SK_TG_VOICE, &q) && (q.u.rand.options & T3_SO_BROADCAST) && q.u.rand.dst == T3_TG);
	t3_csbk_t g = { .lb = true, .op = T3_OP_BTV_GRANT };
	g.u.grant.lpcn = 1;
	g.u.grant.slot = 2;
	g.u.grant.dst = T3_TG;
	g.u.grant.src = T3_OWN;
	from = sim.air_count;
	t3_csbk_rx(&g, 1);
	run(600);
	CHECK(radio_t3v.state == T3_FOLLOW && sm.view.state == RS_TX);
	bool found = false;
	for (unsigned i = from; i < sim.air_count && !found; i++) {
		if (sim.air[i].kind == DMR_BURST_VOICE_LC_HEADER) {
			dmr_lc_t l;
			air_lc(i, &l);
			CHECK((l.svc & DMR_SVC_BROADCAST) && l.dst == T3_TG);
			found = true;
		}
	}
	CHECK(found);
	req(RREQ_PTT, 0);
	run(400);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}

// An ALS call to the radio (6.6.2.6): the radio check accepted, then on the caller's private grant the radio transmits
// with no PTT until T_ALS, sends its terminator and P_MAINT, and goes back to the TSCC
static void test_t3_als(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	radio_t3_staged()->cfg.opts |= T3_OPT_ALS;
	radio_t3_staged()->cfg.als_s = 2;
	req(RREQ_T3, RADIO_T3_START);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED);
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_IND_VOICE;
	h.u.ahoy.als = true;
	h.u.ahoy.dst = T3_OWN;
	h.u.ahoy.src = 1000001u;
	unsigned acks = t3_sent(T3_OP_C_ACKU);
	t3_csbk_rx(&h, 1);
	t3_site_run(300);
	CHECK(t3_sent(T3_OP_C_ACKU) == acks + 1u);
	t3_csbk_t g = { .lb = true, .op = T3_OP_PV_GRANT };
	g.u.grant.lpcn = 1;
	g.u.grant.slot = 2;
	g.u.grant.dst = T3_OWN;
	g.u.grant.src = 1000001u;
	t3_csbk_rx(&g, 1);
	run(600);
	CHECK(radio_t3v.state == T3_FOLLOW && radio_t3v.call_als && radio_t3v.als_calls == 1);
	CHECK(sm.view.state == RS_TX);
	unsigned maints = t3_sent(T3_OP_P_MAINT);
	run(2200);
	CHECK(sm.view.state != RS_TX);
	CHECK(t3_sent(T3_OP_P_MAINT) > maints);
	run(600);
	CHECK(radio_t3v.state == T3_CAMPED);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}

// Trunked channels in the codeplug (dmr-trunking-codeplug.md): system n's record and its control channel. opts: the
// block's options, plus two bits these tests give the header's flags with: 0x0800 the system needs a key,
// T3_OPT_LISTEN_ONLY no transmit side
#define T3_CP_NEEDS_KEY	0x0800u
#define SYS_IP_GW	0x1000u
static void t3_cp_system(uint8_t n, uint16_t opts, uint8_t hold_s, uint16_t fb_chan, const char *fb_name)
{
	uint8_t v[CODEPLUG_SYS_HDR + 39];
	uint8_t *b = &v[CODEPLUG_SYS_HDR];
	memset(v, 0, sizeof(v));
	v[0] = CODEPLUG_SYSTYPE_DMR_TRUNK;
	v[1] = (uint8_t)(((opts & T3_CP_NEEDS_KEY) != 0 ? CODEPLUG_SYS_NEEDS_KEY : 0u) |
			 ((opts & T3_OPT_LISTEN_ONLY) != 0 ? 0u : CODEPLUG_SYS_TX));
	v[3] = 4;			// the identity: model, NET, category
	v[8] = (uint8_t)fb_chan;
	v[9] = (uint8_t)(fb_chan >> 8);
	uint16_t check = fb_name != NULL ? codeplug_name_check(fb_name) : 0;
	v[10] = (uint8_t)check;
	v[11] = (uint8_t)(check >> 8);
	snprintf((char *)&v[12], 16, "Sys %u", n);
	opts &= (uint16_t)~(T3_CP_NEEDS_KEY | T3_OPT_LISTEN_ONLY);
	b[0] = T3_MODEL_SMALL;
	b[1] = 5;			// NET 5
	b[3] = T3_PAR_A;
	b[4] = 2;			// CC
	b[5] = (uint8_t)opts;
	b[6] = (uint8_t)(opts >> 8);
	b[7] = hold_s;
	kv_t3_set((uint16_t)(CODEPLUG_KEY_SYSTEM + n), v, sizeof(v));
	uint8_t c[CODEPLUG_SYS_ENTRY_HDR + 11] = { n, CODEPLUG_ENTRY_CHANNEL, CODEPLUG_GROUP_NONE, CODEPLUG_ENTRY_CONTROL, 1, 0 };
	for (int i = 0; i < 4; i++) {	// a control channel, LCN 1
		c[6 + i] = (uint8_t)(T3_RX_HZ >> (8 * i));
		c[10 + i] = (uint8_t)(T3_TX_HZ >> (8 * i));
	}
	c[14] = 2;
	kv_t3_set((uint16_t)(CODEPLUG_KEY_SYS_CHAN + n), c, sizeof(c));
}

// Channel c (named in the fake codeplug) tagged to system n, with these tag flags
static void t3_cp_tag_flags(uint16_t c, uint8_t n, uint8_t flags)
{
	uint16_t check = codeplug_name_check(chans[c].name);
	uint8_t t[CODEPLUG_TAG_HDR] = { CODEPLUG_KIND_DMR_TRUNK, n, (uint8_t)check, (uint8_t)(check >> 8), flags };
	kv_t3_set((uint16_t)(CODEPLUG_KEY_CH_TAG + c - 1u), t, sizeof(t));
}

// The same, transmit allowed (as an editor writes a new one)
static void t3_cp_tag(uint16_t c, uint8_t n)
{
	t3_cp_tag_flags(c, n, CODEPLUG_TAG_TX);
}

// Zone "Tones" (index 2) made two trunked channels of system 0, as an editor writes them: DMR, receive only, the
// control channel's frequencies, the talk target (TG 201, then TG 9)
static void t3_cp_zone(uint16_t opts, uint8_t hold_s, uint16_t fb_chan, const char *fb_name)
{
	dmr_start(true);
	sim.repeater = true;
	kv_t3_clear();
	add(7, "Ops A", T3_RX_HZ, RADIO_MODE_DMR, true, CODEPLUG_POWER_DEFAULT);
	chans[7].tx_hz = T3_TX_HZ;
	chans[7].contact = 1;
	add(8, "Ops B", T3_RX_HZ, RADIO_MODE_DMR, true, CODEPLUG_POWER_DEFAULT);
	chans[8].tx_hz = T3_TX_HZ;
	chans[8].contact = 3;
	t3_cp_system(0, opts, hold_s, fb_chan, fb_name);
	t3_cp_tag(7, 0);
	t3_cp_tag(8, 0);
}

static void t3_cp_done(void)
{
	req(RREQ_ZONE, 0);
	run(100);
	kv_t3_clear();
	sim.repeater = false;
	setup_codeplug();
}

// The cancel of an emergency alarm goes to the system that took it (another system's IDs are another network's): the
// emergency ended on another system, the cancel waits until the radio is back on the alarm's
static void test_t3_emergency_other_system(void)
{
	t3_cp_zone(T3_OPT_PRIVATE, 0, 0, NULL);
	t3_cp_system(1, T3_OPT_PRIVATE, 0, 0, NULL);
	t3_cp_tag(8, 1);			// "Ops B" on system 1
	req(RREQ_ZONE, 0);
	run(100);
	dmr_emergency_settings_t es;
	dmr_emergency_settings_default(&es);
	es.mode = DMR_EMERGENCY_MODE_ALARM;
	es.alarm_tries = 3;
	dmr_emergency_set_settings(&es);
	req(RREQ_ZONE, 2);			// "Ops A", system 0, talking to TG 201
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED && radio_t3v.sys == 1);
	unsigned from = sim.air_count;
	CHECK(dmr_emergency_start());
	t3_csbk_t q;
	for (int i = 0; i < 20 && !t3_rand_since(from, T3_SK_STATUS, &q); i++) {
		t3_site_run(60);
	}
	CHECK(t3_rand_since(from, T3_SK_STATUS, &q) && t3_rand_status(&q) == T3_STATUS_EMERGENCY && q.u.rand.dst == 201);
	t3_csbk_t a = { .lb = true, .op = T3_OP_C_ACKD };
	a.u.ack.reason = T3_RC_MESSAGE_ACCEPTED;
	a.u.ack.dst = own_dmr_id;
	a.u.ack.src = 201;
	t3_csbk_rx(&a, 1);
	from = sim.air_count;
	t3_site_run(1200);
	CHECK(!t3_rand_since(from, T3_SK_STATUS, &q));	// acknowledged: no repeats

	req(RREQ_ZONE_POS, 1);			// system 1
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED && radio_t3v.sys == 2);
	from = sim.air_count;
	dmr_emergency_cancel();
	t3_site_run(600);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_IDLE && !t3_rand_since(from, T3_SK_STATUS, &q));
	req(RREQ_ZONE_POS, 0);			// back on system 0: the cancel goes
	t3_site_run(1200);
	CHECK(t3_rand_since(from, T3_SK_STATUS, &q) && t3_rand_status(&q) == T3_STATUS_EMERGENCY_CANCEL &&
	      q.u.rand.dst == 201);
	t3_csbk_rx(&a, 1);
	run(100);
	t3_cp_done();
}

// No ID to register with (the system's record has none, nor the radio): the channel shows as unavailable, the engine
// never starts and nothing is sent
static void test_t3_no_id(void)
{
	own_dmr_id = 0;
	t3_cp_zone(T3_OPT_PRIVATE, 0, 0, NULL);
	unsigned air = sim.air_count;
	req(RREQ_ZONE, 2);
	t3_site_run(600);
	CHECK(radio_t3v.sys == 1 && radio_t3v.unavail && !radio_t3v.on && sm.view.ch.rx_only && sim.air_count == air);
	own_dmr_id = 9996543;
	t3_cp_done();
}

// Selecting trunked channels: the first starts its system and camps; another of the same system keeps the TSCC; an
// untagged channel leaves; a renamed channel (its name check no longer matches) is conventional; one whose system
// authenticates without a key is unavailable and refuses PTT; zone scan passes trunked channels by
static void test_t3_channels(void)
{
	t3_cp_zone(T3_OPT_PRIVATE, 0, 0, NULL);
	req(RREQ_ZONE, 2);
	CHECK(sm.view.channel_number == 7 && radio_t3v.sys == 1 && !radio_t3v.unavail);
	t3_site_run(600);
	CHECK(radio_t3v.on && radio_t3v.state == T3_CAMPED && !sm.view.ch.rx_only);
	CHECK(hw.rx_hz == T3_RX_HZ);
	unsigned hunts = radio_t3v.hunts;
	req(RREQ_ZONE_POS, 1);		// "Ops B": the same system
	t3_site_run(120);
	CHECK(sm.view.channel_number == 8 && radio_t3v.on && radio_t3v.state == T3_CAMPED);
	CHECK(radio_t3v.hunts == hunts);
	CHECK(strcmp(sm.view.ch.name, "Ops B") == 0);

	// Following a call on a payload slot, another channel of the system (the same talkgroup): the radio stays on the
	// call's slot, not the record's control channel
	chans[8].contact = 1;
	req(RREQ_ZONE_POS, 0);
	t3_site_run(120);
	t3_csbk_t g = { .lb = true, .op = T3_OP_TV_GRANT };
	g.u.grant.lpcn = 1;
	g.u.grant.slot = 2;
	g.u.grant.dst = 201;
	g.u.grant.src = 1000001u;
	t3_csbk_rx(&g, 1);
	run(300);
	CHECK(radio_t3v.state == T3_FOLLOW && sm.view.ch.timeslot == 2);
	req(RREQ_ZONE_POS, 1);
	run(20);
	CHECK(sm.view.channel_number == 8 && radio_t3v.state == T3_FOLLOW && sm.view.ch.timeslot == 2);
	CHECK(hw.rx_hz == T3_RX_HZ);
	chans[8].contact = 3;
	req(RREQ_ZONE_POS, 0);
	t3_site_run(3000);
	CHECK(radio_t3v.state == T3_CAMPED);

	// A zone scan visits only the conventional channel; canceled, the radio is back on its system
	zones[2].channels[2] = 6;
	zones[2].count = 3;
	req(RREQ_ZONE, 1);
	req(RREQ_ZONE, 2);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED);
	req(RREQ_SCAN, RSCAN_ZONE);
	bool on_t3 = false, on_6 = false;
	for (int i = 0; i < 400; i++) {
		run(5);
		on_t3 = on_t3 || (sm.view.scan != 0 && hw.rx_hz == T3_RX_HZ);
		on_6 = on_6 || hw.rx_hz == 145500000;
	}
	CHECK(sm.view.scan != 0 && on_6 && !on_t3);
	req(RREQ_SCAN, RSCAN_CANCEL);
	t3_site_run(600);
	CHECK(sm.view.scan == 0 && sm.view.channel_number == 7 && radio_t3v.state == T3_CAMPED);
	zones[2].count = 2;

	req(RREQ_ZONE, 0);		// a conventional channel: the system goes
	run(100);
	CHECK(!radio_t3v.on && radio_t3v.sys == 0 && hw.rx_hz == 146520000);

	strcpy(chans[7].name, "Renamed");	// a CPS that renumbered or renamed it: no longer the tagged channel
	req(RREQ_ZONE, 2);
	run(100);
	CHECK(radio_t3v.sys == 0 && !radio_t3v.on && sm.view.ch.rx_only);
	strcpy(chans[7].name, "Ops A");

	t3_cp_system(1, T3_OPT_PRIVATE | T3_CP_NEEDS_KEY, 0, 0, NULL);	// authenticates, no key in the codeplug
	t3_cp_tag(8, 1);
	req(RREQ_ZONE_POS, 1);
	run(100);
	CHECK(radio_t3v.sys == 2 && radio_t3v.unavail && !radio_t3v.on);
	req(RREQ_PTT, 1);
	run(100);
	CHECK(sm.view.state != RS_TX && t3_sent(T3_OP_C_RAND) == 0);
	req(RREQ_PTT, 0);
	run(100);
	// Stepping passes an unavailable channel by: from "Ops A" up, down, both land on "Ops A" (the zone's only other)
	req(RREQ_ZONE_POS, 0);
	run(50);
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 7);
	req(RREQ_CH_STEP, -1);
	CHECK(sm.view.channel_number == 7);
	t3_cp_done();
}

// A trunked channel transmits only when its tag allows it and its system is one the radio runs with a transmit side
// (channel-modes.md 3 and 4): a receive-only channel and a system without the transmit flag camp and listen, send
// nothing on the control channel and refuse PTT; a channel that names no system, or one with no record, is unavailable
static void t3_no_tx(void)
{
	unsigned air = sim.air_count;
	t3_site_run(600);
	req(RREQ_PTT, 1);
	t3_site_run(300);
	CHECK(sm.view.state != RS_TX && sim.air_count == air);
	req(RREQ_PTT, 0);
	t3_site_run(120);
	CHECK(sim.air_count == air);
}

static void test_t3_never_keys(void)
{
	t3_cp_zone(T3_OPT_PRIVATE, 0, 0, NULL);
	t3_cp_tag_flags(8, 0, 0);			// "Ops B": a zeroed flags byte, receive only
	req(RREQ_ZONE, 2);
	req(RREQ_ZONE_POS, 1);
	t3_no_tx();
	CHECK(sm.view.channel_number == 8 && radio_t3v.on && !radio_t3v.unavail && sm.view.ch.rx_only);
	CHECK(radio_t3v.state == T3_CAMPED);		// it listens to its system

	// "Ops A" may transmit; its system's header has no transmit flag: listen only, whatever the tag says
	t3_cp_system(0, T3_OPT_PRIVATE | T3_OPT_LISTEN_ONLY, 0, 0, NULL);
	req(RREQ_ZONE, 0);
	run(100);
	req(RREQ_ZONE, 2);
	req(RREQ_ZONE_POS, 0);
	t3_no_tx();
	CHECK(sm.view.channel_number == 7 && radio_t3v.on && radio_t3v.state == T3_CAMPED);

	// No system named, then a system with no record: unavailable, nothing started
	t3_cp_tag(7, CODEPLUG_SYSTEM_NONE);
	req(RREQ_ZONE, 0);
	run(100);
	req(RREQ_ZONE, 2);
	req(RREQ_ZONE_POS, 0);
	t3_no_tx();
	CHECK(radio_t3v.sys == 0xFF && radio_t3v.unavail && !radio_t3v.on && sm.view.ch.rx_only);
	t3_cp_tag(7, 5);
	req(RREQ_ZONE, 0);
	run(100);
	req(RREQ_ZONE, 2);
	req(RREQ_ZONE_POS, 0);
	t3_no_tx();
	CHECK(radio_t3v.sys == 6 && radio_t3v.unavail && !radio_t3v.on && sm.view.ch.rx_only);

	// The same channel with its system's transmit side back asks for its call: the checks above can see a transmit
	t3_cp_system(0, T3_OPT_PRIVATE, 0, 0, NULL);
	t3_cp_tag(7, 0);
	req(RREQ_ZONE, 0);
	run(100);
	req(RREQ_ZONE, 2);
	req(RREQ_ZONE_POS, 0);
	t3_site_run(600);
	unsigned air = sim.air_count;
	req(RREQ_PTT, 1);
	t3_site_run(300);
	CHECK(sim.air_count > air && t3_sent(T3_OP_C_RAND) > 0 && !sm.view.ch.rx_only);
	req(RREQ_PTT, 0);
	t3_site_run(300);
	t3_cp_done();
}

// Fallback (12.5): hunting past the hold time goes to the fallback channel, which works as any channel; a probe in a
// quiet moment that finds the TSCC goes back with the system-back tone, one that doesn't returns to the fallback
static void test_t3_fallback(void)
{
	t3_cp_zone(T3_OPT_PRIVATE, 3, 6, "Other");
	req(RREQ_ZONE, 2);
	run(2900);			// no TSCC heard
	CHECK(radio_t3v.on && radio_t3v.fallback == 0);
	run(200);
	CHECK(radio_t3v.fallback == 1 && radio_t3v.fallbacks == 1 && !radio_t3v.on);
	CHECK(hw.rx_hz == 145500000 && strcmp(sm.view.ch.name, "Other") == 0 && sm.view.channel_number == 7);

	run(RADIO_T3_PROBE_MS);		// a probe: nothing found in RADIO_T3_PROBE_HUNT_MS, back
	CHECK(radio_t3v.fallback == 2 && radio_t3v.on);
	run(RADIO_T3_PROBE_HUNT_MS + 100);
	CHECK(radio_t3v.fallback == 1 && !radio_t3v.on && hw.rx_hz == 145500000 && radio_t3v.fallbacks == 1);

	// A probe while the channel is busy waits for it to go quiet
	rx_signal(10, true);
	run(RADIO_T3_PROBE_MS);
	CHECK(radio_t3v.fallback == 1);
	rx_signal(120, true);
	run(RADIO_T3_PROBE_QUIET_MS + 500);
	CHECK(radio_t3v.fallback == 2);
	t3_site_run(600);		// found
	CHECK(radio_t3v.fallback == 0 && radio_t3v.backs == 1 && radio_t3v.state == T3_CAMPED);
	CHECK(strcmp(sm.view.ch.name, "Ops A") == 0 && !sm.view.ch.rx_only && hw.rx_hz == T3_RX_HZ);
	beep_stats_t bs;
	beep_get_stats(&bs);
	CHECK(bs.last == BEEP_SYSTEM_BACK);
	t3_cp_done();
}

// A stun on a codeplug system outlasts a restart: kept in the settings with the system's slot and network, lifted by
// the revive; a stun kept for another network doesn't apply
static void t3_stun_ahoy(bool revive)
{
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_SUPPLEMENTARY;
	h.u.ahoy.dst = sm.view.dmr.own_id;	// a codeplug system's ID is the radio's
	h.u.ahoy.src = T3_ID_STUNI;
	h.u.ahoy.kind_flag = revive;
	t3_csbk_rx(&h, 1);
	t3_site_run(300);
}

static void t3_set_stun(uint8_t w)
{
	settings_t *st = settings_begin();
	st->t3_stun = w;
	settings_end(now);
}

static void test_t3_stun_kept(void)
{
	settings_t st;
	t3_cp_zone(T3_OPT_PRIVATE | T3_OPT_STUN, 0, 0, NULL);
	req(RREQ_ZONE, 2);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED && !radio_t3v.stunned);
	settings_read(&st);
	CHECK(st.t3_stun == 0);
	t3_stun_ahoy(false);
	CHECK(radio_t3v.stunned);
	settings_read(&st);
	uint8_t w = st.t3_stun;
	CHECK((w & SETTINGS_T3_STUN) != 0 && ((w >> 4) & 7u) == 0);
	t3_stun_ahoy(true);
	CHECK(!radio_t3v.stunned);
	settings_read(&st);
	CHECK(st.t3_stun == 0);

	// A restart with the stun kept (the engine's own memory of it gone with the revive): stunned on the system again
	t3_set_stun(w);
	req(RREQ_ZONE, 0);
	run(100);
	boot();
	req(RREQ_ZONE, 2);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED && radio_t3v.stunned);
	unsigned rands = t3_sent(T3_OP_C_RAND);
	req(RREQ_PTT, 1);
	run(200);
	CHECK(sm.view.state == RS_TX_DENIED && t3_sent(T3_OP_C_RAND) == rands);
	req(RREQ_PTT, 0);
	run(200);
	t3_stun_ahoy(true);
	settings_read(&st);
	CHECK(!radio_t3v.stunned && st.t3_stun == 0);

	// Kept for another network in that slot (the system was replaced): not this one's
	t3_set_stun((uint8_t)(w ^ 0x01u));
	req(RREQ_ZONE, 0);
	run(100);
	req(RREQ_ZONE, 2);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED && !radio_t3v.stunned);
	settings_read(&st);
	CHECK(st.t3_stun == (uint8_t)(w ^ 0x01u));	// left for its own system
	t3_set_stun(0);
	t3_cp_done();
}

// A kill (6.4.10.0: the radio loses all DMR functionality) outlasts a restart: kept in the settings, the DMR stack
// stops at once and doesn't start again on a conventional DMR channel, PTT there is denied, FM still works; the
// defaults (a factory reset) clear it
static void test_t3_kill_kept(void)
{
	static const uint8_t key[T3_AUTH_KEY_LEN] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA };
	settings_t st;
	dmr_start(true);
	sim.repeater = true;
	t3_stage_with(T3_OPT_PRIVATE | T3_OPT_KILL, key);
	CHECK(sm.view.dmr.active && !sm.view.dmr.killed);
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_SUPPLEMENTARY;
	h.u.ahoy.dst = T3_OWN;
	h.u.ahoy.src = T3_ID_KILLI;
	t3_csbk_rx(&h, 1);
	t3_site_run(300);
	t3_csbk_t v = { 0 };
	bool challenged = false;
	for (unsigned i = 0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_CSBK && (sim.air[i].payload[0] & 0x3Fu) == T3_OP_C_ACKVIT) {
			challenged = t3_csbk_parse(sim.air[i].payload, &v);
		}
	}
	CHECK(challenged && !radio_t3v.killed);
	t3_csbk_t a = { .lb = true, .op = T3_OP_C_ACKD };
	a.u.ack.reason = T3_RC_AUTH_RESPONSE;
	a.u.ack.dst = T3_OWN;
	a.u.ack.src = t3_auth_response(key, v.u.ahoy.dst, 0);
	t3_csbk_rx(&a, 1);
	t3_site_run(300);
	CHECK(radio_t3v.killed && sm.view.dmr.killed && !sm.view.dmr.active);
	settings_read(&st);
	CHECK((st.t3_kill & SETTINGS_T3_KILLED) != 0);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;

	// A restart: a conventional DMR channel doesn't start the stack, PTT is denied, nothing goes on air
	boot();
	req(RREQ_ZONE_POS, 3);
	run(100);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sm.view.dmr.killed && !sm.view.dmr.active);
	req(RREQ_PTT, 1);
	run(200);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_DMR && hw.tx_calls == 0 &&
	      sim.air_count == 0);
	req(RREQ_PTT, 0);
	run(200);

	// FM still transmits
	req(RREQ_ZONE_POS, 0);
	run(100);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM);
	req(RREQ_PTT, 1);
	run(100);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	run(200);

	// The defaults clear it
	settings_t *w = settings_begin();
	settings_defaults(w);
	settings_end(now);
	boot();
	req(RREQ_ZONE_POS, 3);
	run(100);
	CHECK(!sm.view.dmr.killed && sm.view.dmr.active);
}

// Power save on a board whose rx_sleep works on DMR (dmr-trunking.md 7.13): registered with a wake frame and the
// CSC known, the receiver goes off between wake frames and comes back, restarting the modem's receive, for them
static int ps_sleeps, ps_wakes;
static void r_rx_sleep(bool sleep)
{
	if (sleep) {
		ps_sleeps++;
	} else {
		ps_wakes++;
	}
}

static void ps_site_run(uint32_t ms, bool slc)
{
	for (uint32_t t = 0; t < ms; t += 60) {
		t3_csbk_t a = { .lb = true, .op = T3_OP_C_ALOHA };
		a.u.aloha.syscode = t3_code();
		a.u.aloha.nrand_wait = 6;
		a.u.aloha.backoff = 1;
		a.u.aloha.version = 2;
		a.u.aloha.reg = true;
		if (!radio_t3v.rx_off) {
			t3_csbk_rx(&a, 1);	// asleep, nothing is heard
		}
		if (slc && t % 120 == 0) {
			t3_sysparms_t sp = { true, T3_MODEL_SMALL, (uint16_t)((t3_code() >> 2) & 0xFFFu), true,
					     (uint16_t)((now / 120u) & 0x1FFu) };
			uint32_t w = t3_sysparms_build(&sp);
			if (!radio_t3v.rx_off) {
				sim_short_lc(w, dmrShortLcCrc8(w));
			}
		}
		run(60);
	}
}

static void test_t3_power_save(void)
{
	fake_radio.rx_sleep = r_rx_sleep;
	fake_radio.rx_sleep_dmr = true;
	ps_sleeps = ps_wakes = 0;
	dmr_start(true);
	sim.repeater = true;
	radio_t3_setup_t *s = radio_t3_staged();
	radio_t3_reset(s);
	s->cfg.own_id = T3_OWN;
	s->cfg.auth.model = T3_MODEL_SMALL;
	s->cfg.auth.net = 5;
	s->cfg.auth.category = T3_PAR_A;
	s->cfg.opts = T3_OPT_PRIVATE;
	s->cfg.ps_rq = 2;	// 1:4
	s->cc = 2;
	s->cfg.ctrl[0] = (t3_chan_t){ .lcn = 1, .rx_hz = T3_RX_HZ, .tx_hz = T3_TX_HZ, .cc = 2 };
	s->cfg.nctrl = 1;
	s->chans[0] = s->cfg.ctrl[0];
	s->nchans = 1;
	radio_t3_fix(s);
	unsigned from = sim.air_count;
	req(RREQ_T3, RADIO_T3_START);
	ps_site_run(1200, false);
	t3_csbk_t q;
	CHECK(t3_rand_since(from, T3_SK_REGISTRATION, &q) && ((q.u.rand.options >> 1) & 7u) == 2u);
	t3_csbk_t ack = { .lb = true, .op = T3_OP_C_ACKD };
	ack.u.ack.reason = T3_RC_REG_ACCEPTED;
	ack.u.ack.info = 1;
	ack.u.ack.dst = T3_OWN;
	ack.u.ack.src = T3_ID_REGI;
	t3_csbk_rx(&ack, 1);
	ps_site_run(600, false);
	CHECK(radio_t3v.ps_offset == 1);
	ps_site_run(3000, false);	// no CSC: awake
	CHECK(ps_sleeps == 0 && !radio_t3v.rx_off);
	unsigned restarts = sim.rx_restarts;
	ps_site_run(6000, true);
	CHECK(radio_t3v.csc_valid && ps_sleeps > 2 && ps_wakes > 2 && radio_t3v.rx_offs == (uint32_t)ps_sleeps);
	CHECK(radio_t3v.rx_off_ms > 2000u && sim.rx_restarts - restarts >= (unsigned)ps_wakes - 1u);
	CHECK(radio_t3v.state == T3_CAMPED);	// T_Nosig counts only the time awake
	req(RREQ_T3, RADIO_T3_STOP);
	run(200);
	CHECK(!radio_t3v.rx_off && ps_wakes == ps_sleeps);	// leaving wakes the receiver
	fake_radio.rx_sleep = NULL;
	fake_radio.rx_sleep_dmr = false;
}

// IP Connection Advice (6.4.11): on a system with an IP gateway the registration carries IP_Inform, and the C_AHOY
// from IPI gets the radio's CAI address; the final C_ACKD from IPI shows on the link as ip_advised
static void test_t3_ip_advice(void)
{
	dmr_start(true);
	sim.repeater = true;
	radio_t3_setup_t *s = radio_t3_staged();
	radio_t3_reset(s);
	s->cfg.own_id = T3_OWN;
	s->cfg.auth.model = T3_MODEL_SMALL;
	s->cfg.auth.net = 5;
	s->cfg.auth.category = T3_PAR_A;
	s->cfg.opts = T3_OPT_PRIVATE;
	s->ip_gw = true;
	s->cc = 2;
	s->cfg.ctrl[0] = (t3_chan_t){ .lcn = 1, .rx_hz = T3_RX_HZ, .tx_hz = T3_TX_HZ, .cc = 2 };
	s->cfg.nctrl = 1;
	s->chans[0] = s->cfg.ctrl[0];
	s->nchans = 1;
	radio_t3_fix(s);
	unsigned from = sim.air_count;
	req(RREQ_T3, RADIO_T3_START);
	ps_site_run(1200, false);
	t3_csbk_t q;
	CHECK(t3_rand_since(from, T3_SK_REGISTRATION, &q) && (q.u.rand.options & T3_SO_REG_IP_INFORM));
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_IND_UDT;
	h.u.ahoy.dst = T3_OWN;
	h.u.ahoy.src = T3_ID_IPI;
	from = sim.air_count;
	t3_csbk_rx(&h, 1);
	ps_site_run(400, false);
	int hdr = -1;
	for (unsigned i = from; i < sim.air_count && hdr < 0; i++) {
		hdr = sim.air[i].kind == DMR_BURST_DATA_HEADER ? (int)i : -1;
	}
	CHECK(hdr >= 0 && (unsigned)hdr + 1u < sim.air_count);
	if (hdr >= 0 && (unsigned)hdr + 1u < sim.air_count) {
		t3_udt_t u;
		CHECK(t3_udt_parse(sim.air[hdr].payload, &u) && u.dst == T3_ID_IPI && u.format == T3_UDTF_IP);
		uint32_t ip = dmrDataIdAddress(false, T3_OWN);
		const uint8_t *b = sim.air[hdr + 1].payload;
		CHECK(b[0] == (uint8_t)(ip >> 24) && b[1] == (uint8_t)(ip >> 16) && b[2] == (uint8_t)(ip >> 8) &&
		      b[3] == (uint8_t)ip);
	}
	t3_csbk_t ack = { .lb = true, .op = T3_OP_C_ACKD };
	ack.u.ack.reason = T3_RC_REG_ACCEPTED;
	ack.u.ack.dst = T3_OWN;
	ack.u.ack.src = T3_ID_IPI;
	t3_csbk_rx(&ack, 1);
	ps_site_run(300, false);
	CHECK(radio_t3v.reg == T3_REG_OK && radio_t3v.ip_advised);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
}


// Supplementary user data (6.4.13): staged with the system, offered with a request and uploaded to SUPLI; one heard
// from SUPLI shows in the view, its sender named by the user data that follows
static void udt_down_rx(uint32_t src, bool sf, uint8_t format, const uint8_t *data, uint8_t len)
{
	uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK], pad, d[T3_CSBK_BYTES];
	t3_udt_t u = { .op = T3_OP_C_UDTHD, .a = true, .sf = sf, .format = format, .dst = T3_OWN, .src = src };
	u.blocks = t3_udt_pack(data, len, 0xFF, blk, &pad);
	u.pad = pad;
	t3_udt_build(&u, d);
	t3_data_rx(DMR_BURST_DATA_HEADER, d, 1);
	for (uint8_t i = 0; i < u.blocks; i++) {
		run(60);
		t3_data_rx(DMR_BURST_RATE_12_DATA, blk[i], 1);
	}
	t3_site_run(300);
}

static void test_t3_supl(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	radio_t3_setup_t *s = radio_t3_staged();
	memcpy(s->supl, "\x01\x02\x03\x04", 4);
	s->cfg.supl_len = 4;
	s->cfg.supl_format = T3_UDTF_BINARY;
	req(RREQ_T3, RADIO_T3_START);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED);
	s->sd_dst = 1000001u;
	s->sd_group = false;
	s->sd_format = T3_UDTF_ISO8;
	memcpy(s->sd_data, "hi", 2);
	s->sd_len = 2;
	unsigned from = sim.air_count;
	req(RREQ_T3, RADIO_T3_SDATA);
	t3_site_run(600);
	t3_csbk_t q;
	CHECK(t3_rand_since(from, T3_SK_IND_UDT, &q) && (q.u.rand.options & T3_SO_SUPPL_DATA));
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_IND_UDT;
	h.u.ahoy.dst = T3_OWN;
	h.u.ahoy.src = T3_ID_SUPLI;
	from = sim.air_count;
	t3_csbk_rx(&h, 1);
	t3_site_run(400);
	int hdr = -1;
	for (unsigned i = from; i < sim.air_count && hdr < 0; i++) {
		hdr = sim.air[i].kind == DMR_BURST_DATA_HEADER ? (int)i : -1;
	}
	CHECK(hdr >= 0 && (unsigned)hdr + 1u < sim.air_count);
	if (hdr >= 0 && (unsigned)hdr + 1u < sim.air_count) {
		t3_udt_t u;
		CHECK(t3_udt_parse(sim.air[hdr].payload, &u) && u.sf && u.dst == T3_ID_SUPLI && u.src == T3_OWN &&
		      u.format == T3_UDTF_BINARY && u.blocks == 1);
		CHECK(memcmp(sim.air[hdr + 1].payload, "\x01\x02\x03\x04", 4) == 0);
	}
	// Heard: in the view, sender unknown; the user data that follows names it
	udt_down_rx(T3_ID_SUPLI, true, T3_UDTF_BINARY, (const uint8_t *)"\x0a\x0b\x0c", 3);
	CHECK(radio_t3v.supl_count == 1 && radio_t3v.supl_len == 3 && radio_t3v.supl_src == 0 &&
	      memcmp(radio_t3v.supl_data, "\x0a\x0b\x0c", 3) == 0 && radio_t3v.supl_dst == T3_OWN);
	udt_down_rx(1000001u, false, T3_UDTF_ISO8, (const uint8_t *)"hello", 5);
	CHECK(radio_t3v.sd_count >= 1 && radio_t3v.supl_src == 1000001u);
	while (sms_inbox_count() > 0 && sms_inbox_delete(0)) {
	}
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}

// The CACH short LC from the modem (DMR_EV_SHORT_LC): a good CRC gives the engine the Common_Slot_Counter, a bad
// one is only counted
static void test_t3_short_lc(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	uint32_t ok0 = radio_t3v.slc_ok, bad0 = radio_t3v.slc_bad;
	CHECK(!radio_t3v.csc_valid);
	t3_syscode_t c;
	t3_syscode_split(t3_code(), &c);
	t3_sysparms_t sp = { true, c.model, (uint16_t)((t3_code() >> 2) & 0xFFFu), false, 77 };
	uint32_t slc = t3_sysparms_build(&sp);
	sim_short_lc(slc, (uint8_t)(dmrShortLcCrc8(slc) ^ 0x01u));
	t3_site_run(120);
	CHECK(!radio_t3v.csc_valid && radio_t3v.slc_bad == bad0 + 1u && radio_t3v.slc_ok == ok0);
	sim_short_lc(slc, dmrShortLcCrc8(slc));
	t3_site_run(120);
	CHECK(radio_t3v.csc_valid && radio_t3v.slc_ok == ok0 + 1u && radio_t3v.slc_bad == bad0 + 1u);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
}

// Off-net IP on a system with an IP gateway (dmr-trunking.md 7.7): the data service's packet to IPI asks for a
// multi-part call to IPI, uploads the packet's destination when the TSCC asks, and sends the data to IPI on the grant
static void test_t3_ip_gateway(void)
{
	t3_cp_zone(T3_OPT_PRIVATE, 0, 0, NULL);
	req(RREQ_ZONE, 2);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED && !radio_t3v.ip_gw);	// not set: conventional off-net routing stays
	t3_cp_done();
	t3_cp_zone(T3_OPT_PRIVATE | SYS_IP_GW, 0, 0, NULL);
	req(RREQ_ZONE, 2);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED && radio_t3v.ip_gw);

	static const uint8_t udp[12] = { 0x10, 0x00, 0x10, 0x01, 0x00, 0x0C, 0x00, 0x00, 'p', 'i', 'n', 'g' };
	unsigned from = sim.air_count;
	CHECK(dmrDataServiceSendIP(false, T3_ID_IPI, DMR_DATA_IP_OURS, 0x08080404u, 64, 17, udp, sizeof udp));
	t3_site_run(600);
	t3_csbk_t q;
	CHECK(radio_t3v.pd_wait && t3_rand_since(from, T3_SK_IND_DATA, &q) && q.u.rand.dst == T3_ID_IPI && !q.u.rand.proxy);

	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_IND_DATA;
	h.u.ahoy.dst = sm.view.dmr.own_id;
	h.u.ahoy.src = T3_ID_IPI;
	from = sim.air_count;
	t3_csbk_rx(&h, 1);
	t3_site_run(400);
	int hdr = -1;
	for (unsigned i = from; i < sim.air_count && hdr < 0; i++) {
		hdr = sim.air[i].kind == DMR_BURST_DATA_HEADER ? (int)i : -1;
	}
	CHECK(hdr >= 0 && (unsigned)hdr + 1u < sim.air_count);
	if (hdr >= 0 && (unsigned)hdr + 1u < sim.air_count) {
		t3_udt_t u;
		CHECK(t3_udt_parse(sim.air[hdr].payload, &u) && u.op == T3_OP_C_UDTHU && u.dst == T3_ID_IPI && u.sf &&
		      u.format == T3_UDTF_IP && u.blocks == 1);
		const uint8_t *b = sim.air[hdr + 1].payload;
		CHECK(b[0] == 8 && b[1] == 8 && b[2] == 4 && b[3] == 4 && b[4] == 0 && b[9] == 0);
	}

	// The grant: the data goes to IPI, the packet's full header carrying its destination
	t3_csbk_t g = { .lb = true, .op = T3_OP_PD_GRANT };
	g.u.grant.lpcn = 1;
	g.u.grant.slot = 2;
	g.u.grant.dst = T3_ID_IPI;
	g.u.grant.src = sm.view.dmr.own_id;
	from = sim.air_count;
	t3_csbk_rx(&g, 1);
	run(1500);
	CHECK(radio_t3v.state == T3_FOLLOW && radio_t3v.call_data && !radio_t3v.pd_wait);
	hdr = -1;
	for (unsigned i = from; i < sim.air_count && hdr < 0; i++) {
		hdr = sim.air[i].kind == DMR_BURST_DATA_HEADER ? (int)i : -1;
	}
	CHECK(hdr >= 0);
	if (hdr >= 0) {
		const uint8_t *p = sim.air[hdr].payload;
		CHECK((((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4]) == T3_ID_IPI && (p[0] & 0x80) == 0);
	}
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.dst = T3_ID_IPI;
	t3_csbk_rx(&c, 2);
	run(300);
	CHECK(radio_t3v.state == T3_CAMPED);
	t3_cp_done();
}

// A short data poll from the menus (RREQ_T3_POLL): the system's poll setting gives the format and size; the answer is
// a message in the inbox; a refusal counts in poll_fails. The radio's own reply to a poll comes from the same setting
static void test_t3_poll(void)
{
	t3_cp_zone(T3_OPT_PRIVATE, 0, 0, NULL);
	uint8_t p[2 + 5] = { T3_UDTF_ISO8, 2, 'r', 'e', 'a', 'd', 'y' };
	kv_t3_set((uint16_t)(CODEPLUG_KEY_T3_POLL + 0), p, sizeof p);
	static radio_t3_setup_t ld;	// the loader: the radio's own reply to a poll, and the size to ask for
	CHECK(radio_t3_load(0, &ld) && ld.cfg.poll_format == T3_UDTF_ISO8 && ld.cfg.poll_len == 5 && ld.poll_blocks == 2 &&
	      memcmp(ld.poll_reply, "ready", 5) == 0 && ld.cfg.poll_data == ld.poll_reply);
	req(RREQ_ZONE, 2);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED);
	unsigned from = sim.air_count;
	uint16_t fails = radio_t3v.poll_fails;
	radio_req_t r = { .type = RREQ_T3_POLL, .arg2 = 1000001u };
	radio_sm_request(&sm, &r, now);
	t3_site_run(600);
	t3_csbk_t q;
	CHECK(t3_rand_since(from, T3_SK_UDT_POLL, &q) && q.u.rand.dst == 1000001u && (q.u.rand.options & 0x0Fu) == T3_UDTF_ISO8 &&
	      (q.u.rand.extra & 3u) == 1u);
	// The polled MS's data: a C_UDTHD to the radio
	uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK], pad, d[T3_CSBK_BYTES];
	t3_udt_t u = { .op = T3_OP_C_UDTHD, .a = true, .format = T3_UDTF_ISO8, .dst = sm.view.dmr.own_id, .src = 1000001u };
	u.blocks = t3_udt_pack((const uint8_t *)"at gate 4", 9, 0xFF, blk, &pad);
	u.pad = pad;
	t3_udt_build(&u, d);
	t3_data_rx(DMR_BURST_DATA_HEADER, d, 1);
	run(60);
	t3_data_rx(DMR_BURST_RATE_12_DATA, blk[0], 1);
	t3_site_run(300);
	CHECK(sms_inbox_count() >= 1 && strcmp(dmrDataServiceMessage(0)->text, "at gate 4") == 0);
	CHECK(radio_t3v.polls == 1 && radio_t3v.poll_fails == fails);
	while (sms_inbox_count() > 0 && sms_inbox_delete(0)) {
	}

	// Refused: counted
	radio_sm_request(&sm, &r, now);
	t3_site_run(600);
	t3_csbk_t n = { .lb = true, .op = T3_OP_C_ACKD };
	n.u.ack.reason = T3_RC_MS_AWAY;
	n.u.ack.dst = sm.view.dmr.own_id;
	n.u.ack.src = 1000001u;
	t3_csbk_rx(&n, 1);
	t3_site_run(300);
	CHECK(radio_t3v.poll_fails == fails + 1u);
	t3_cp_done();
}

static void test_t3_stun(void)
{
	dmr_start(true);
	sim.repeater = true;
	t3_stage_start();
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_SUPPLEMENTARY;
	h.u.ahoy.dst = T3_OWN;
	h.u.ahoy.src = T3_ID_STUNI;
	t3_csbk_rx(&h, 1);
	t3_site_run(300);
	CHECK(radio_t3v.stunned && radio_t3v.stuns == 1);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	req(RREQ_T3, RADIO_T3_START);
	t3_site_run(600);
	CHECK(radio_t3v.state == T3_CAMPED && radio_t3v.stunned);
	unsigned rands = t3_sent(T3_OP_C_RAND);
	req(RREQ_PTT, 1);
	run(200);
	CHECK(sm.view.state == RS_TX_DENIED && t3_sent(T3_OP_C_RAND) == rands);
	req(RREQ_PTT, 0);
	run(200);
	h.u.ahoy.kind_flag = true;
	t3_csbk_rx(&h, 1);
	t3_site_run(300);
	CHECK(!radio_t3v.stunned && radio_t3v.revives == 1);
	req(RREQ_T3, RADIO_T3_STOP);
	run(100);
	sim.repeater = false;
}
#endif

int main(void)
{
	test_tx_limit_op();
	test_startup();
	test_persist();
	test_channel_step();
	test_vfo_step_auto();
	test_squelch();
	test_parked_kind();
	test_tx();
	test_deny_flash_then_tx();
	test_timeout();
	test_power();
	test_vfo();
	test_vfo_quick_settings_kept();
	test_no_codeplug();
	test_codeplug_tones();
	test_tone_squelch();
	test_temp_bw_tone();
	test_tone_lists();
	test_dmr_rx();
	test_dmr_channel_change_view();
	test_dmr_rssi_hold();
	test_fm_rssi_raw();
	test_dmr_tx();
	test_dmr_tx_end();
#if CONFIG_DMR_TRUNK_TX
	test_t3_at();
	test_t3_talk();
	test_t3_sdata();
	test_t3_radio_check();
	test_t3_clear_slot();
#if CONFIG_DMR_TRUNK_GW
	test_t3_dial();
#endif
	test_t3_pdata();
	test_t3_emergency();
#if CONFIG_DMR_EMERGENCY
	test_t3_emergency_other_system();
#endif
	test_t3_broadcast();
	test_t3_als();
	test_t3_stun();
	test_t3_stun_kept();
	test_t3_kill_kept();
	test_t3_poll();
	test_t3_ip_gateway();
	test_t3_ip_advice();
	test_t3_supl();
	test_t3_short_lc();
	test_t3_power_save();
	test_t3_channels();
	test_t3_never_keys();
	test_t3_no_id();
	test_t3_fallback();
#endif
	test_dmr_stalled();
	test_dmr_list_talk_target();
#if CONFIG_VFO_DMR
	test_vfo_dmr();
#else
	test_vfo_no_dmr();
#endif
	test_vfo_copy();
	test_vfo_copy_dmr();
	test_dmr_denied();
	test_dmr_wake_leds();
	test_dmr_voice_features();
	test_dmr_contact_added();
	test_ui_requests();
	test_ui_glue();
	test_position();
#if CONFIG_DUAL_WATCH
	test_ui_dual();
	test_dual_two_vfos();
#endif
	test_ui_settings();
	test_soft_volume();
	test_side_keys();
	test_rx_ext();
	test_error_paths();
	test_ptt_line_stuck();
	test_hotspot();
	test_hotspot_session();
	test_hotspot_cwid();
#if CONFIG_SFR
	test_sfr_roles();
#if CONFIG_VFO_DMR
	test_sfr_vfo();
#endif
#endif
#if CONFIG_SFR_CWID
	test_sfr_cwid();
#endif
	test_dmr_emergency();
#if CONFIG_APRS_DMR
	test_aprs_dmr_beacon();
#endif
#if CONFIG_APRS_RX
	test_dmr_aprs_position();
#endif
	test_power_off_ars();
#if CONFIG_VFO_DMR
	test_vfo_dmr_data();
#endif
#if CONFIG_P25
	test_p25();
#endif
#if CONFIG_NXDN
	test_nxdn();
#endif
	test_dmr_data_glue();	// (last: the data service's state carries over)
	test_factory_reset();
	test_volume_step_of();
	printf("# radioCoreTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
