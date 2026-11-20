// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 conventional on a channel, the radio core's side (radio_p25.h)
#include "config.h"
#if CONFIG_P25	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "radio/radio_p25.h"
#include "p25/p25_codeplug.h"
#include "codeplug/codeplug_modes.h"
#include "radio/radio_bands.h"
#if CONFIG_IMBE
#include "p25/p25Voice.h"
#endif
#if CONFIG_P25_TRUNK
#include "hal_time.h"
#include "p25/p25_follow.h"

// A trunked channel's follower (docs/spec/p25-trunking.md 7) and what it runs on. The provider's task hands it units
// and calls, the radio task ticks it and carries out its retunes; the two never run it at once (enter_* below)
typedef struct {
	p25_follow_t f;
	p25_follow_cfg_t cfg;
	p25_follow_cc_t cc[P25_CP_CTL_MAX];
	p25_follow_iden_t iden[P25_FOLLOW_IDENS];
	// The last retune the follower asked (its tune hook, from either task): want moves past done until the radio
	// task has carried it out
	volatile uint8_t want, done;
	uint8_t what;
	uint32_t hz, id;
	bool owed;			// the receiver is on the new frequency, the follower not told yet
	volatile uint8_t in_prov, in_radio;	// which task is inside the follower
	// The inbound burst the follower asked last (its send hook, from either task), as want / done above
	volatile uint8_t burst_want, burst_done;
	uint32_t burst_hz;
	uint8_t burst[P25_TSBK_BYTES];
	// PTT and the end of our transmission, for the follower at the radio task's next turn inside it
	bool ptt, talk_end;
	uint8_t ptt_seq, ptt_done;
} p25_trunk_t;
_Static_assert(P25_CP_IDEN_MAX == P25_FOLLOW_IDENS, "one identifier plan size");
_Static_assert(RADIO_P25_TRUNK_HUNT == P25_FOLLOW_HUNT && RADIO_P25_TRUNK_CONTROL == P25_FOLLOW_CONTROL &&
	       RADIO_P25_TRUNK_VOICE == P25_FOLLOW_VOICE, "the view's trunking states are the follower's");
#endif

// What runs while P25 is on: the engine and, with the decoder, the voice path's queue and decoder memory. On a
// trunked channel the follower too, and its set-up as the codeplug gives it, read before the voice path starts
typedef struct {
	p25_engine_t eng;
#if CONFIG_P25_TRUNK
	p25_trunk_t trunk;
	union {
#if CONFIG_IMBE
		p25_voice_mem_t voice;
#endif
		p25_trunk_cfg_t load;
	};
#elif CONFIG_IMBE
	p25_voice_mem_t voice;
#endif
} p25_mem_t;

#ifdef HAL_VOCODER_LEND_BYTES
// The platform's DMR vocoder lends its memory while P25 runs (hal_vocoder_t.lend): no copy here
_Static_assert(sizeof(p25_mem_t) <= HAL_VOCODER_LEND_BYTES, "P25 does not fit the memory the vocoder lends");
#else
static p25_mem_t own_mem;
#endif

#if CONFIG_IMBE
#define SILENCE		imbe_silence
#else
// No decoder in the build: the silence frame (pitch code 216) for what the engine sends
static const uint8_t silence_frame[P25_IMBE_BYTES] = { 0xD8 };
#define SILENCE		silence_frame
#endif

static const radio_p25_settings_t defaults = {
	.rx_nac = P25_NAC_DEFAULT, .tx_nac = P25_NAC_DEFAULT, .rx_tgid = 0, .talkaround = RADIO_P25_TA_AUTO,
	.unit_call = false, .tx_dst = 1, .unit_id = 0,
};
static radio_p25_settings_t set;
static p25_mem_t *mem;			// while running
static radio_p25_stats_t stats;
static p25_call_stats_t call_base;	// the call layer's counters when the engine started (they restart with it)
static air_session_stats_t air_base;
static uint32_t rx_calls_base, tx_calls_base, filler_base;
#if CONFIG_P25_TRUNK
static bool trunk;			// the engine runs a trunked channel
static radio_p25_trunk_view_t tview;	// the radio task's copy, for any task
static uint8_t twhy;			// RADIO_TRUNK_WHY_* of the last PTT refused or request failed
static bool ttx;			// the follower's transmit side is on
#endif

void radio_p25_init(const hal_audio_t *audio)
{
	set = defaults;
	mem = NULL;
#if CONFIG_IMBE
	p25VoiceInit(audio);
#else
	(void)audio;
#endif
}

void radio_p25_settings(radio_p25_settings_t *out)
{
	*out = set;
}

void radio_p25_set_settings(const radio_p25_settings_t *s)
{
	set = *s;
	set.rx_nac &= 0xFFFu;
	set.tx_nac &= 0xFFFu;
	set.unit_id &= 0xFFFFFFu;
	set.tx_dst &= set.unit_call ? 0xFFFFFFu : 0xFFFFu;
	if (set.talkaround > RADIO_P25_TA_OFF)
		set.talkaround = RADIO_P25_TA_AUTO;
}

// What PTT calls: talk group or unit dst
static void talk_lc(p25_lc_t *lc, bool unit, uint32_t dst)
{
	lc->lcf = unit ? P25_LCF_UNIT : P25_LCF_GROUP;
	if (unit)
		lc->dst = dst & 0xFFFFFFu;
	else
		lc->tgid = (uint16_t)dst;
}

void radio_p25_params(const channel_t *ch, uint16_t number, uint32_t own_id, p25_engine_cfg_t *cfg)
{
	p25_conv_cfg_t cc;

	memset(cfg, 0, sizeof(*cfg));
	cfg->silence = SILENCE;
	if (number != 0 && p25_conv_load(number, ch, &cc)) {
		// A stored P25 channel (docs/spec/p25-codeplug.md 2). Selective: its talk group, everyone and unit calls to
		// us; a channel that calls a unit or everyone keeps only the last two
		cfg->rx_nac = cc.conv.rx_nac;
		cfg->tx_nac = cc.conv.tx_nac;
		if (cc.conv.squelch == P25_SQUELCH_SELECTIVE)
			cfg->rx_tgid = cc.target_unit ? (uint16_t)P25_TGID_EVERYONE : (uint16_t)cc.target;
		cfg->unit_id = (set.unit_id != 0 ? set.unit_id : cc.unit_id) & 0xFFFFFFu;
		talk_lc(&cfg->tx_lc, cc.target_unit, cc.target);
		cfg->talkaround = cc.talkaround;
		return;
	}
	cfg->rx_nac = set.rx_nac;
	cfg->tx_nac = set.tx_nac;
	cfg->rx_tgid = set.rx_tgid;
	cfg->unit_id = (set.unit_id != 0 ? set.unit_id : p25_radio_unit_id(own_id)) & 0xFFFFFFu;
	talk_lc(&cfg->tx_lc, set.unit_call, set.tx_dst);
	cfg->talkaround = set.talkaround == RADIO_P25_TA_ON ||
			  (set.talkaround == RADIO_P25_TA_AUTO && (ch->tx_hz == ch->rx_hz || ch->tx_hz == 0));
}

// The engine's configuration as settings: what the screen, the view and the debug link show
static void cfg_settings(const p25_engine_cfg_t *c, radio_p25_settings_t *out)
{
	out->rx_nac = c->rx_nac;
	out->tx_nac = c->tx_nac;
	out->rx_tgid = c->rx_tgid;
	out->talkaround = c->talkaround ? RADIO_P25_TA_ON : RADIO_P25_TA_OFF;
	out->unit_call = c->tx_lc.lcf == P25_LCF_UNIT;
	out->tx_dst = out->unit_call ? c->tx_lc.dst : c->tx_lc.tgid;
	out->unit_id = c->unit_id;
}

bool radio_p25_in_force(radio_p25_settings_t *out)
{
	p25_mem_t *m = mem;

	if (m == NULL)
		return false;
	cfg_settings(&m->eng.cfg, out);
	return true;
}

bool radio_p25_fits(void)
{
	return board.air != NULL && (p25_engine_fit(board.air) & AIR_FIT_RX) != 0;
}

bool radio_p25_take(channel_t *ch)
{
	bool kind = ch->kind == CODEPLUG_KIND_P25;
#if CONFIG_P25_TRUNK
	kind = kind || ch->kind == CODEPLUG_KIND_P25_TRUNK;
#endif
	if (!kind || !radio_p25_fits() || radio_listen_band(ch->rx_hz))
		return false;
	ch->mode = RADIO_MODE_P25;
	ch->bw = RADIO_BW_12K5;
	return true;
}

static p25_mem_t *mem_get(void)
{
#ifdef HAL_VOCODER_LEND_BYTES
	size_t len = 0;
	void *m = board.vocoder != NULL && board.vocoder->lend != NULL ? board.vocoder->lend(&len) : NULL;
	if (m == NULL || len < sizeof(p25_mem_t) || ((uintptr_t)m & 7u) != 0) {
		return NULL;
	}
	stats.mem_lent++;
	return m;
#else
	return &own_mem;
#endif
}

#if CONFIG_P25_TRUNK
// One task at a time inside the follower, without a lock: each says it is in, then looks for the other. The task that
// finds the other in leaves again: the provider's drops that unit (the follower takes the next), the radio task's tick
// waits for its next. On one core, whichever runs second sees the other's flag
static bool enter(volatile uint8_t *mine, const volatile uint8_t *other)
{
	*mine = 1;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	if (*other) {
		*mine = 0;
		return false;
	}
	return true;
}

static void leave(volatile uint8_t *mine)
{
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	*mine = 0;
}

// Engine hooks (provider's task): every unit and every call's fields to the follower
static void trunk_unit(const p25_rx_unit_t *u, void *ctx)
{
	p25_mem_t *m = mem;

	(void)ctx;
	if (m == NULL || !enter(&m->trunk.in_prov, &m->trunk.in_radio)) {
		stats.trunk_skips++;
		return;
	}
	p25_follow_unit(&m->trunk.f, u, hal_millis());
	leave(&m->trunk.in_prov);
}

static void trunk_call(const p25_call_t *c, void *ctx)
{
	p25_mem_t *m = mem;

	(void)ctx;
	if (m == NULL || !enter(&m->trunk.in_prov, &m->trunk.in_radio)) {
		stats.trunk_skips++;
		return;
	}
	p25_follow_call(&m->trunk.f, c, hal_millis());
	leave(&m->trunk.in_prov);
}

// The follower's tune hook (inside it, from either task): the request for the radio task, the frequency first
static void trunk_tune(uint32_t hz, uint8_t what, uint32_t id, void *ctx)
{
	p25_trunk_t *t = ctx;

	t->hz = hz;
	t->what = what;
	t->id = id;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	t->want++;
}

// The follower's send hook (inside it, from either task): the burst for the radio task, the block first. A burst not
// yet sent is replaced (the follower sends one block at a time)
static bool trunk_send(uint32_t hz, const uint8_t blk[P25_TSBK_BYTES], void *ctx)
{
	p25_trunk_t *t = ctx;

	memcpy(t->burst, blk, P25_TSBK_BYTES);
	t->burst_hz = hz;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	t->burst_want++;
	return true;
}

static const p25_follow_hooks_t follow_hooks = { trunk_tune, NULL, trunk_send };	// no fallback yet (p25-trunking.md 7)

// The follower's configuration from the codeplug's (p25-trunking.md 6), and the engine's: the system's NAC, our unit
// ID, trunking signaling on, what PTT calls. tx: the channel allows transmitting (its tag); the system's listen only
// and a missing unit ID or talk group keep the transmit side off
static void trunk_params(p25_trunk_t *t, const p25_trunk_cfg_t *l, bool tx, p25_engine_cfg_t *cfg)
{
	p25_follow_cfg_t *c = &t->cfg;

	memset(c, 0, sizeof(*c));
	c->wacn = l->sys.wacn;		// the codeplug's "any" values are the follower's
	c->sysid = l->sys.sysid;
	c->nac = l->sys.cc_nac;
	if (l->sys.options & P25_TSYS_SKIP_ENC)
		c->flags |= P25_FOLLOW_SKIP_ENC;
	if (l->sys.options & P25_TSYS_PLAN_ONLY)
		c->flags |= P25_FOLLOW_PLAN_ONLY;
	if (l->follow_all)
		c->flags |= P25_FOLLOW_ALL_GROUPS;
	c->unit_id = (set.unit_id != 0 ? set.unit_id : l->unit_id) & 0xFFFFFFu;
	c->talk_group = l->talk_group;
	if (tx && !l->listen_only && c->unit_id != 0 && c->talk_group != 0)
		c->flags |= P25_FOLLOW_TX;
	if (l->sys.options & P25_TSYS_NO_REG)
		c->flags |= P25_FOLLOW_NO_REG;
	c->ncc = l->ctl_count;
	for (uint8_t i = 0; i < l->ctl_count; i++)
		t->cc[i] = (p25_follow_cc_t){ .hz = l->ctl[i].rx_hz, .tx_hz = l->ctl[i].tx_hz, .chan = l->ctl[i].chan,
					      .site = l->ctl_site[i] };
	c->cc = t->cc;
	c->iden_set = l->iden_set;
	for (uint8_t i = 0; i < P25_FOLLOW_IDENS; i++)
		t->iden[i] = (p25_follow_iden_t){ .base_hz = l->iden[i].base_hz, .spacing_hz = l->iden[i].spacing_hz,
						  .tx_off_hz = l->iden[i].tx_offset_hz, .slots = l->iden[i].slots,
						  .tdma = l->iden[i].type == P25_IDEN_TDMA };
	c->iden = t->iden;
	c->hold_ms = (uint32_t)l->sys.hold_s * 1000u;	// 0: no hold

	memset(cfg, 0, sizeof(*cfg));
	cfg->silence = SILENCE;
	cfg->rx_nac = cfg->tx_nac = l->sys.cc_nac;
	cfg->unit_id = c->unit_id;
	talk_lc(&cfg->tx_lc, false, l->talk_group);
	cfg->trunk = true;
}

// Radio task: a retune the follower asked is carried out (the receiver, the engine's squelch for a group call, the
// engine told), then the follower is told and ticked. From begin and every tick
static void trunk_service(p25_mem_t *m)
{
	static const subtone_t no_tone = { TONE_NONE, 0 };
	p25_trunk_t *t = &m->trunk;
	uint32_t now = hal_millis();

	if (t->done != t->want) {
		uint8_t want = t->want;

		__atomic_signal_fence(__ATOMIC_SEQ_CST);
		uint32_t hz = t->hz;
		p25_engine_cfg_t c = m->eng.cfg;

		c.rx_tgid = t->what == P25_FOLLOW_TUNE_GROUP || t->what == P25_FOLLOW_TUNE_TALK ? (uint16_t)t->id : 0u;
		t->done = want;
		// A frequency the radio can't receive stays untuned: the follower hears nothing there and moves on
		if (hz != 0 && board_in_band(hz, false) && !radio_listen_band(hz)) {
			board.radio->rx(hz, RADIO_BW_12K5, &no_tone);
		} else {
			stats.trunk_untuned++;
		}
		stats.trunk_retunes++;
		p25_engine_set(&m->eng, &c);
		p25_engine_retuned(&m->eng);
		t->owed = true;
	}
	if (!enter(&t->in_radio, &t->in_prov))
		return;
	if (t->owed) {
		t->owed = false;
		p25_follow_retuned(&t->f, now);
	}
	if (t->talk_end) {
		t->talk_end = false;
		p25_follow_talk_end(&t->f, now);
	}
	if (t->ptt_done != t->ptt_seq) {
		t->ptt_done = t->ptt_seq;
		p25_follow_ptt(&t->f, t->ptt, now);
	}
	p25_follow_tick(&t->f, now);
	leave(&t->in_radio);
	// The copy other tasks read (radio_p25_trunk_view); the provider's task may be writing the follower's view, which
	// p25_follow_view waits out
	tview.seq++;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	p25_follow_view(&t->f, &tview.v);
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	tview.seq++;
}

void radio_p25_trunk_view(radio_p25_trunk_view_t *out)
{
	uint8_t seq;

	do {
		seq = tview.seq;
		__atomic_signal_fence(__ATOMIC_SEQ_CST);
		out->v = tview.v;
		__atomic_signal_fence(__ATOMIC_SEQ_CST);
	} while ((seq & 1u) || seq != tview.seq);
	out->seq = seq;
	out->on = trunk;
	out->retunes = stats.trunk_retunes;
	out->untuned = stats.trunk_untuned;
	out->skips = stats.trunk_skips;
	out->tx = trunk && ttx;
	out->bursts = stats.trunk_bursts;
	out->burst_refused = stats.trunk_burst_refused;
}

bool radio_p25_trunk_tx(void)
{
	return radio_p25_trunked() && ttx;
}

uint8_t radio_p25_trunk_ptt(bool down)
{
	static const uint8_t WHY[] = { RADIO_TRUNK_WHY_NONE, RADIO_TRUNK_WHY_OFF, RADIO_TRUNK_WHY_NO_CC, RADIO_TRUNK_WHY_NOT_REG };
	p25_trunk_t *t;
	uint8_t why = RADIO_TRUNK_WHY_OFF;

	if (!radio_p25_trunked())
		return why;
	t = &mem->trunk;
	if (down) {
		why = WHY[p25_follow_can_talk(&tview.v, t->cfg.flags)];
		twhy = why;
		if (why != RADIO_TRUNK_WHY_NONE)
			return why;
	}
	// The follower takes it at the radio task's next turn inside it (trunk_service)
	t->ptt = down;
	t->ptt_seq++;
	return RADIO_TRUNK_WHY_NONE;
}

uint8_t radio_p25_trunk_talk(uint32_t *hz)
{
	p25_trunk_t *t;

	if (!radio_p25_trunked())
		return RADIO_P25T_FAIL;
	t = &mem->trunk;
	if (t->ptt_done != t->ptt_seq)
		return RADIO_P25T_WAIT;		// PTT not yet with the follower
	switch (tview.v.req) {
	case P25_REQ_WAIT:
	case P25_REQ_QUEUED:
		return RADIO_P25T_WAIT;
	case P25_REQ_TALK:
		if (t->done != t->want || t->owed)
			return RADIO_P25T_WAIT;	// the receiver not yet on the voice channel
		*hz = tview.v.talk_hz;
		return RADIO_P25T_GO;
	case P25_REQ_DENIED:
		twhy = RADIO_TRUNK_WHY_DENIED;
		break;
	case P25_REQ_NO_CHAN:
		twhy = RADIO_TRUNK_WHY_NO_CHAN;
		break;
	case P25_REQ_TIMEOUT:
		twhy = RADIO_TRUNK_WHY_NO_ANSWER;
		break;
	default:
		// Not taken (the follower lost the control channel or the registration meanwhile)
		twhy = tview.v.state == P25_FOLLOW_HUNT ? RADIO_TRUNK_WHY_NO_CC : RADIO_TRUNK_WHY_NOT_REG;
		break;
	}
	return RADIO_P25T_FAIL;
}

void radio_p25_trunk_talk_end(void)
{
	if (radio_p25_trunked())
		mem->trunk.talk_end = true;
}

uint32_t radio_p25_trunk_burst_hz(void)
{
	p25_trunk_t *t;

	if (!radio_p25_trunked())
		return 0;
	t = &mem->trunk;
	return t->burst_done != t->burst_want ? t->burst_hz : 0;
}

// The NAC we send: the system's, or the one the control channel was heard on where the system's is any
static void trunk_tx_nac(p25_mem_t *m)
{
	if (m->eng.cfg.tx_nac == P25_NAC_ANY && tview.v.nac != 0 && tview.v.nac != P25_NAC_ANY) {
		p25_engine_cfg_t c = m->eng.cfg;

		c.tx_nac = tview.v.nac;
		p25_engine_set(&m->eng, &c);
	}
}

bool radio_p25_trunk_burst_start(void)
{
	p25_trunk_t *t;
	uint8_t want;

	if (!radio_p25_trunked())
		return false;
	t = &mem->trunk;
	want = t->burst_want;
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
	trunk_tx_nac(mem);
	bool ok = p25_engine_tx_burst(&mem->eng, t->burst);
	t->burst_done = want;
	if (ok)
		stats.trunk_bursts++;
	else
		stats.trunk_burst_refused++;
	return ok;
}

void radio_p25_trunk_burst_drop(void)
{
	if (radio_p25_trunked()) {
		mem->trunk.burst_done = mem->trunk.burst_want;
		stats.trunk_burst_refused++;
	}
}
#endif

bool radio_p25_begin(const channel_t *ch, uint16_t number, uint32_t own_id)
{
	p25_engine_cfg_t cfg;
	static const p25_engine_hooks_t no_voice;
	const p25_engine_hooks_t *hooks = &no_voice;

	radio_p25_end();
	stats.begins++;
	mem = mem_get();
	if (mem == NULL || board.air == NULL || p25_engine_fit(board.air) == 0) {
		mem = NULL;
		stats.begin_fails++;
		return false;
	}
#if CONFIG_P25_TRUNK
	p25_engine_hooks_t th;

	trunk = ch->kind == CODEPLUG_KIND_P25_TRUNK;
	if (trunk) {
		// The codeplug's set-up goes where the voice path's memory will be: read and turned into the follower's
		// before that starts
		if (!p25_trunk_load(number, ch, &mem->load)) {
			trunk = false;
			mem = NULL;
			stats.begin_fails++;
			return false;
		}
		trunk_params(&mem->trunk, &mem->load, !ch->rx_only, &cfg);
		ttx = (mem->trunk.cfg.flags & P25_FOLLOW_TX) != 0;
	} else
#endif
		radio_p25_params(ch, number, own_id, &cfg);
#if CONFIG_IMBE
	p25VoiceBegin(&mem->voice);
	hooks = p25VoiceHooks();
#endif
#if CONFIG_P25_TRUNK
	if (trunk) {
		p25_trunk_t *t = &mem->trunk;

		th = *hooks;
		th.rx_unit = trunk_unit;
		th.rx_call = trunk_call;
		hooks = &th;
		t->want = t->done = 0;
		t->burst_want = t->burst_done = 0;
		t->ptt = t->talk_end = false;
		t->ptt_seq = t->ptt_done = 0;
		twhy = RADIO_TRUNK_WHY_NONE;
		t->owed = false;
		t->in_prov = t->in_radio = 0;
		memset(&tview, 0, sizeof(tview));
		// Before the engine: its first unit may come at once. Its first tune is carried out below
		p25_follow_begin(&t->f, &t->cfg, &follow_hooks, t, hal_millis());
	}
#endif
	if (!p25_engine_begin(&mem->eng, board.air, &cfg, hooks, NULL)) {
#if CONFIG_P25_TRUNK
		if (trunk)
			p25_follow_end(&mem->trunk.f);
		trunk = false;
#endif
#if CONFIG_IMBE
		p25VoiceEnd();
#endif
		mem = NULL;
		stats.begin_fails++;
		return false;
	}
#if CONFIG_P25_TRUNK
	if (trunk)
		trunk_service(mem);
#endif
	memset(&call_base, 0, sizeof(call_base));
	memset(&air_base, 0, sizeof(air_base));
	rx_calls_base = tx_calls_base = filler_base = 0;
	return true;
}

// The engine's counters into ours; they restart with each engine, so ours add what moved since the last look
static void count(const p25_engine_view_t *ev)
{
	const p25_call_stats_t *c = &mem->eng.rx.stats;
	const air_session_stats_t *a = &mem->eng.session.stats;

	stats.rx_calls += ev->rx_calls - rx_calls_base;
	stats.tx_calls += ev->tx_calls - tx_calls_base;
	stats.tx_filler += ev->tx_filler - filler_base;
	rx_calls_base = ev->rx_calls;
	tx_calls_base = ev->tx_calls;
	filler_base = ev->tx_filler;

	stats.units += c->units - call_base.units;
	stats.voice += c->voice - call_base.voice;
	stats.voice_bad += c->voice_bad - call_base.voice_bad;
	stats.voice_bits += c->voice_bits - call_base.voice_bits;
	stats.lc_fail += c->lc_fail - call_base.lc_fail;
	stats.es_fail += c->es_fail - call_base.es_fail;
	stats.rx_chunks += a->rx_chunks - air_base.rx_chunks;
	stats.tx_underruns += a->tx_underruns - air_base.tx_underruns;
	stats.tx_syms += a->tx_syms - air_base.tx_syms;
	call_base = *c;
	air_base = *a;
}

void radio_p25_end(void)
{
	p25_engine_view_t ev;

	if (mem == NULL) {
		return;
	}
	p25_engine_end(&mem->eng);	// the provider calls nothing after its close
	p25_engine_view(&mem->eng, &ev);
	count(&ev);
#if CONFIG_P25_TRUNK
	if (trunk) {
		p25_follow_end(&mem->trunk.f);
		trunk = false;
		tview.seq++;
		tview.v.state = P25_FOLLOW_OFF;
		tview.seq++;
	}
#endif
#if CONFIG_IMBE
	p25VoiceEnd();
#endif
	mem = NULL;			// the memory goes back (a lending vocoder rebuilds its state at its next call)
}

bool radio_p25_running(void)
{
	return mem != NULL;
}

void radio_p25_retuned(void)
{
	if (mem != NULL) {
		p25_engine_retuned(&mem->eng);
	}
}

void radio_p25_apply(const channel_t *ch, uint16_t number, uint32_t own_id)
{
	p25_engine_cfg_t cfg;

	if (mem == NULL || p25_engine_tx_state(&mem->eng) != AIR_TX_IDLE || radio_p25_trunked()) {
		return;		// (a trunked channel's parameters are its system's, and the follower sets the squelch)
	}
	radio_p25_params(ch, number, own_id, &cfg);
	p25_engine_set(&mem->eng, &cfg);
}

// What the screen shows of the parameters: the NAC heard and what PTT calls. Those in force while the engine runs (a
// stored channel's own), else the settings
static void settings_view(radio_p25_view_t *n)
{
	radio_p25_settings_t s = set;

	if (mem != NULL)
		cfg_settings(&mem->eng.cfg, &s);
	n->rx_nac = s.rx_nac;
	n->talk = s.tx_dst;
	if (s.unit_call)
		n->flags |= RADIO_P25_TALK_UNIT;
}

bool radio_p25_poll(radio_p25_view_t *v)
{
	p25_engine_view_t ev;
	radio_p25_view_t n;

	if (mem == NULL) {
		n = *v;		// the last call heard stays
		n.flags = 0;
		n.tx = AIR_TX_IDLE;
		n.trunk = RADIO_P25_TRUNK_OFF;
		settings_view(&n);
		bool changed = memcmp(&n, v, sizeof(n)) != 0;
		*v = n;
		return changed;
	}
#if CONFIG_P25_TRUNK
	if (trunk)
		trunk_service(mem);
#endif
#if CONFIG_IMBE
	p25VoicePoll();
#endif
	p25_engine_view(&mem->eng, &ev);
	count(&ev);
	memset(&n, 0, sizeof(n));
	n.flags = RADIO_P25_ON;
	if (ev.rx_active) {
		n.flags |= RADIO_P25_RX;
		if (ev.rx_played)
			n.flags |= RADIO_P25_PLAYED;
	}
	if (ev.rx.have_alg && ev.rx.algid != P25_ALGID_CLEAR)
		n.flags |= RADIO_P25_ENC;
	if (ev.rx.late)
		n.flags |= RADIO_P25_LATE;
	bool unit = ev.rx.have_lc && ev.rx.lc.lcf == P25_LCF_UNIT;
	if (unit)
		n.flags |= RADIO_P25_UNIT;
	n.tx = ev.tx;
	n.nac = ev.rx.nac;
	n.src = ev.rx.have_lc ? ev.rx.lc.src : 0;
	n.dst = unit ? ev.rx.lc.dst : ev.rx.lc.tgid;
#if CONFIG_P25_TRUNK
	if (trunk) {
		n.trunk = tview.v.state;
		if (ttx)
			n.flags |= RADIO_P25_TRUNK_TX;
		n.treg = tview.v.reg;
		n.treq = tview.v.req;
		n.twhy = twhy;
		// On the voice channel before the call's fields are in: the grant's
		if (n.trunk == RADIO_P25_TRUNK_VOICE && !ev.rx_active) {
			n.dst = tview.v.unit_call ? tview.v.dst : tview.v.tg;
			if (tview.v.unit_call)
				n.flags |= RADIO_P25_UNIT;
		}
	}
#endif
	settings_view(&n);
	bool changed = memcmp(&n, v, sizeof(n)) != 0;
	*v = n;
	return changed;
}

// The voice path encodes the mic from the header on, so the first voice frames are ready when the engine asks
static void voice_tx(bool on)
{
#if CONFIG_IMBE
	p25VoiceTx(on);
#else
	(void)on;
#endif
}

bool radio_p25_trunked(void)
{
#if CONFIG_P25_TRUNK
	return mem != NULL && trunk;
#else
	return false;
#endif
}

bool radio_p25_tx_prepare(void)
{
	if (mem == NULL) {
		return false;
	}
#if CONFIG_P25_TRUNK
	// On a trunked channel only on the voice channel granted to us
	if (trunk) {
		if (tview.v.req != P25_REQ_TALK) {
			return false;
		}
		trunk_tx_nac(mem);
	}
#endif
	if (!p25_engine_tx_prepare(&mem->eng)) {
		return false;
	}
	voice_tx(true);
	return true;
}

bool radio_p25_tx_go(void)
{
	return mem != NULL && p25_engine_tx_go(&mem->eng);
}

void radio_p25_tx_stop(void)
{
	if (mem != NULL) {
		voice_tx(false);	// the engine fills the LDU on air with silence
		p25_engine_tx_stop(&mem->eng);
	}
}

void radio_p25_tx_abort(void)
{
	if (mem != NULL) {
		voice_tx(false);
		p25_engine_tx_abort(&mem->eng);
	}
}

uint8_t radio_p25_tx_state(void)
{
	return mem != NULL ? (uint8_t)p25_engine_tx_state(&mem->eng) : (uint8_t)AIR_TX_IDLE;
}

void radio_p25_tx_clear(void)
{
	if (mem != NULL) {
		voice_tx(false);
		p25_engine_tx_clear(&mem->eng);
	}
}

const radio_air_ops_t radio_p25_air = {
	radio_p25_running, NULL, radio_p25_tx_prepare, radio_p25_tx_go, radio_p25_tx_stop, radio_p25_tx_abort,
	radio_p25_tx_state, radio_p25_tx_clear,
};

#ifndef HAL_VOCODER_LEND_BYTES
void *radio_p25_idle_mem(size_t *len)
{
	*len = sizeof(own_mem);
	return mem == NULL ? &own_mem : NULL;
}
#endif

void radio_p25_stats(radio_p25_stats_t *out)
{
	*out = stats;
}

void radio_p25_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif	// CONFIG_P25
