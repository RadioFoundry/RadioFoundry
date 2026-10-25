// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Hamlib's network rig control protocol on TCP 4532 (docs/spec/tcp-services.md 6), from the rigctld(1) / rigctl(1)
// manual pages: one command per line, a one-letter short form or a backslash long name, answers as value lines for
// "get" commands and "RPRT <n>" (0, or a negative Hamlib error number) for the rest. A leading '+' asks for the
// extended answer ("get_freq:", "Frequency: ...", "RPRT 0"). The commands loggers, SDR programs and satellite trackers
// use: frequency, mode, PTT, VFO, split, the S meter, get_info, chk_vfo and a minimal dump_state. They map onto the
// radio core's requests; a frequency on a channel moves the radio to the VFO.
#include "config.h"
#if CONFIG_NET_RIGCTL	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "net/tcp.h"
#include "board.h"
#include "hal_time.h"
#include "radio/radio.h"
#include "dmr/ipGateway.h"
#include "debuglink/dl_fmt.h"

#define LINE_MAX		64
#define OUT_MAX			448	// the longest answer (dump_state, about 350 bytes) is built here, then written
#define REQ_GAP_MS		20u	// radio requests at most this often (the radio core's queue holds 16)
#define FREQ_HOLD_MS		1000u	// a frequency just set is reported until the radio shows it, at most this long
#define PTT_MAX_MS		180000u	// a PTT from the network is released after this long, whatever the client does
#define PTT_GAP_MS		1000u	// and may be keyed again only this long after it was released
#define S9_DBM			(-93)	// STRENGTH is dB over S9; the IARU's VHF / UHF S9 is -93 dBm

// Hamlib error numbers (answered negated)
#define RIG_EINVAL		1
#define RIG_ENIMPL		4
#define RIG_ERJCTED		9
#define RIG_ENAVAIL		11

typedef struct {
	bool used;
	bool too_long;		// the line under way didn't fit: answered -1 at its end, not run
	uint8_t ln;
	char line[LINE_MAX];
} rconn_t;

static rconn_t rc[TCP_CONNS];
static const radio_view_t *view;
static uint32_t pending_hz, pending_ms;	// a frequency set but not applied yet (the request gap) / not shown yet
static bool pending_apply;
static uint32_t last_req_ms;
static int ptt_owner = -1;	// the connection that keyed the radio
static uint32_t ptt_ms, ptt_up_ms;
static struct {
	uint32_t conns, commands, errors, freq_sets, ptt_downs;
} st;

static char out[OUT_MAX];
static dl_text_t ot;

static uint32_t freq_now(void)
{
	if ((pending_hz != 0) && ((hal_millis() - pending_ms) < FREQ_HOLD_MS) && (view->ch.rx_hz != pending_hz)) {
		return pending_hz;
	}
	pending_hz = 0;
	return view->ch.rx_hz;
}

// A decimal number with an optional fraction (Hamlib sends "145500000.000000"); false when it isn't one
static bool parse_num(const char *s, int32_t *v)
{
	int64_t x = 0;
	bool neg = (*s == '-'), digits = false;

	if (neg) {
		s++;
	}
	for (; (*s >= '0') && (*s <= '9'); s++) {
		x = x * 10 + (*s - '0');
		digits = true;
		if (x > 0xFFFFFFFFLL) {
			return false;
		}
	}
	if (*s == '.') {
		s++;
		if ((*s >= '5') && (*s <= '9')) {
			x++;
		}
		while ((*s >= '0') && (*s <= '9')) {
			s++;
		}
	}
	if (!digits || (*s != '\0')) {
		return false;
	}
	*v = (int32_t)(neg ? -x : x);
	return true;
}

static void apply_freq(uint32_t now)
{
	if (!view->vfo) {
		radio_req_vfo(true);	// from a channel to the VFO, then the frequency (the queue keeps the order)
	}
	radio_req_vfo_set(pending_hz);
	pending_apply = false;
	last_req_ms = now;
}

static int set_freq(const char *arg)
{
	int32_t hz;
	uint32_t now = hal_millis();

	if ((arg == NULL) || !parse_num(arg, &hz) || (hz <= 0) || !board_in_band((uint32_t)hz, false)) {
		return -RIG_EINVAL;
	}
	if (view->hotspot) {
		return -RIG_ERJCTED;
	}
	st.freq_sets++;
	pending_hz = (uint32_t)hz;
	pending_ms = now;
	pending_apply = true;
	if ((now - last_req_ms) >= REQ_GAP_MS) {
		apply_freq(now);	// else at the next poll: a tracker's stream of updates ends at its latest value
	}
	return 0;
}

static int set_mode(const char *mode, const char *pb)
{
	int32_t passband = 0;

	if ((mode == NULL) || ((pb != NULL) && !parse_num(pb, &passband))) {
		return -RIG_EINVAL;
	}
	bool narrow = strcmp(mode, "FMN") == 0;

	if (!narrow && (strcmp(mode, "FM") != 0)) {
		return -RIG_EINVAL;	// FM is all the radio's analog side does
	}
#if CONFIG_DMR
	if (view->ch.mode == RADIO_MODE_DMR) {
		radio_req_mode(RADIO_MODE_FM);
	}
#endif
	if (narrow || (passband > 0)) {
		radio_req_bandwidth((narrow || (passband <= 12500)) ? RADIO_BW_12K5 : RADIO_BW_25K);
	}
	last_req_ms = hal_millis();
	return 0;
}

static int set_ptt(int c, const char *arg)
{
	int32_t v;
	uint32_t now = hal_millis();

	if ((arg == NULL) || !parse_num(arg, &v) || (v < 0) || (v > 3)) {
		return -RIG_EINVAL;
	}
	if (tcp_remote_ip(c) != ipGatewayHostIP()) {
		return -RIG_ERJCTED;	// transmit only from the computer the radio is plugged into
	}
	if (v != 0) {
		if ((ptt_owner < 0) && (ptt_up_ms != 0) && ((now - ptt_up_ms) < PTT_GAP_MS)) {
			return -RIG_ERJCTED;	// rate limit
		}
		if (ptt_owner < 0) {
			st.ptt_downs++;
			ptt_ms = now;
		}
		ptt_owner = c;
		radio_req_ptt(true);
	} else if (ptt_owner >= 0) {
		ptt_owner = -1;
		ptt_up_ms = now;
		radio_req_ptt(false);
	}
	return 0;
}

static void release_ptt(int c)
{
	if ((ptt_owner >= 0) && ((c < 0) || (ptt_owner == c))) {
		ptt_owner = -1;
		ptt_up_ms = hal_millis();
		radio_req_ptt(false);
	}
}

static void dump_state(void)
{
	dl_text(&ot, "0\n2\n2\n");	// protocol version 0, rig model 2 (NET rigctl), ITU region 2
	for (int tx = 0; tx < 2; tx++) {
		for (uint8_t i = 0; i < board.band_count; i++) {
			const band_t *b = &board.bands[i];

			if (!tx || b->tx) {
				dl_text(&ot, "%u.000000 %u.000000 0x20 %d %d 0x3 0x1\n", (unsigned)b->lo_hz, (unsigned)b->hi_hz,
					tx ? 1000 : -1, tx ? 5000 : -1);
			}
		}
		dl_text(&ot, "0 0 0 0 0 0 0\n");
	}
	dl_text(&ot, "0x20 2500\n0x20 5000\n0x20 6250\n0x20 12500\n0 0\n");	// tuning steps (FM)
	dl_text(&ot, "0x20 12500\n0x20 25000\n0 0\n");			// filters
	dl_text(&ot, "0\n0\n0\n0\n0\n0\n");	// max RIT, XIT, IF shift, announces; no preamp, no attenuator
	dl_text(&ot, "0x0\n0x0\n0x40000000\n0x0\n0x0\n0x0\n");	// functions, levels (STRENGTH), parameters
}

// One command line; the answer goes into ot. Returns false for quit
static bool command(int c, char *line)
{
	bool ext = false;
	char *argv[4] = { NULL, NULL, NULL, NULL };
	int argc = 0;

	if ((*line == '+') || (*line == ';') || (*line == '|') || (*line == ',')) {
		ext = true;
		line++;
	}
	while ((argc < 4) && (*line != '\0')) {
		while (*line == ' ') {
			*line++ = '\0';
		}
		if (*line == '\0') {
			break;
		}
		argv[argc++] = line;
		while ((*line != ' ') && (*line != '\0')) {
			line++;
		}
	}
	if (argc == 0) {
		return true;
	}
	const char *cmd = argv[0];
	// A VFO argument first (clients started with --vfo send one): skipped, the radio has one VFO
	int a = ((argc > 1) && ((strncmp(argv[1], "VFO", 3) == 0) || (strcmp(argv[1], "currVFO") == 0) ||
				(strcmp(argv[1], "Main") == 0))) ? 2 : 1;
	const char *a1 = (a < argc) ? argv[a] : NULL;
	const char *a2 = (a + 1 < argc) ? argv[a + 1] : NULL;
	int rprt = 0;
	bool get = false;

	st.commands++;
	view = net_radio_view();
#define IS(s, l)	((strcmp(cmd, s) == 0) || (strcmp(cmd, "\\" l) == 0))
	if (IS("f", "get_freq")) {
		get = true;
		dl_text(&ot, ext ? "get_freq:\nFrequency: %u\n" : "%u\n", (unsigned)freq_now());
	} else if (IS("F", "set_freq")) {
		rprt = set_freq(a1);
		if (ext) {
			dl_text(&ot, "set_freq: %s\n", (a1 != NULL) ? a1 : "");
		}
	} else if (IS("m", "get_mode")) {
		get = true;
		unsigned pb = (view->ch.bw == RADIO_BW_25K) ? 25000u : 12500u;

		dl_text(&ot, ext ? "get_mode:\nMode: FM\nPassband: %u\n" : "FM\n%u\n", pb);	// DMR is 4FSK on FM too
	} else if (IS("M", "set_mode")) {
		rprt = set_mode(a1, a2);
		if (ext) {
			dl_text(&ot, "set_mode: %s %s\n", (a1 != NULL) ? a1 : "", (a2 != NULL) ? a2 : "");
		}
	} else if (IS("v", "get_vfo")) {
		get = true;
		dl_text(&ot, ext ? "get_vfo:\nVFO: VFOA\n" : "VFOA\n");
	} else if (IS("V", "set_vfo")) {
		rprt = ((argc > 1) && ((strcmp(argv[1], "VFOA") == 0) || (strcmp(argv[1], "currVFO") == 0) ||
				       (strcmp(argv[1], "VFO") == 0) || (strcmp(argv[1], "Main") == 0))) ? 0 : -RIG_EINVAL;
		if (ext) {
			dl_text(&ot, "set_vfo: %s\n", (argc > 1) ? argv[1] : "");
		}
	} else if (IS("t", "get_ptt")) {
		get = true;
		dl_text(&ot, ext ? "get_ptt:\nPTT: %u\n" : "%u\n", view->state == RS_TX);
	} else if (IS("T", "set_ptt")) {
		rprt = set_ptt(c, a1);
		if (ext) {
			dl_text(&ot, "set_ptt: %s\n", (a1 != NULL) ? a1 : "");
		}
	} else if (IS("\x8b", "get_dcd")) {
		get = true;
		dl_text(&ot, ext ? "get_dcd:\nDCD: %u\n" : "%u\n", view->state == RS_RX);
	} else if (IS("l", "get_level")) {
		if ((a1 != NULL) && (strcmp(a1, "STRENGTH") == 0)) {
			get = true;
			int s = (view->rssi_dbm != 0) ? (view->rssi_dbm - S9_DBM) : -54;

			dl_text(&ot, ext ? "get_level: STRENGTH\n%d\n" : "%d\n", s);
		} else if ((a1 != NULL) && (strcmp(a1, "RFPOWER") == 0)) {
			get = true;
			unsigned pct = (board.power_count > 1) ? (view->power * 100u / (board.power_count - 1u)) : 100u;

			dl_text(&ot, ext ? "get_level: RFPOWER\n%u.%02u\n" : "%u.%02u\n", pct / 100u, pct % 100u);
		} else {
			rprt = (a1 == NULL) ? -RIG_EINVAL : -RIG_ENAVAIL;
		}
	} else if (IS("s", "get_split_vfo")) {
		get = true;
		dl_text(&ot, ext ? "get_split_vfo:\nSplit: 0\nTX VFO: VFOA\n" : "0\nVFOA\n");
	} else if (IS("S", "set_split_vfo")) {
		rprt = ((a1 != NULL) && (strcmp(a1, "0") == 0)) ? 0 : -RIG_ENAVAIL;
	} else if (IS("i", "get_split_freq")) {
		get = true;
		dl_text(&ot, ext ? "get_split_freq:\nTX Frequency: %u\n" : "%u\n", (unsigned)view->ch.tx_hz);
	} else if (IS("_", "get_info")) {
		get = true;
		dl_text(&ot, ext ? "get_info:\nInfo: RadioFoundry %s\n" : "RadioFoundry %s\n", board.name);
	} else if (strcmp(cmd, "\\chk_vfo") == 0) {
		get = true;
		dl_text(&ot, ext ? "chk_vfo:\nCHKVFO: 0\n" : "0\n");
	} else if (strcmp(cmd, "\\get_powerstat") == 0) {
		get = true;
		dl_text(&ot, ext ? "get_powerstat:\nPower Status: 1\n" : "1\n");
	} else if (strcmp(cmd, "\\dump_state") == 0) {
		get = true;
		dump_state();
	} else if (IS("q", "quit") || (strcmp(cmd, "Q") == 0)) {
		return false;
	} else {
		rprt = -RIG_ENIMPL;
	}
#undef IS
	if (rprt != 0) {
		st.errors++;
	}
	if (!get || ext || (rprt != 0)) {
		dl_text(&ot, "RPRT %d\n", rprt);	// sets always answer; gets only in extended mode or on an error
	}
	return true;
}

static void r_opened(int c)
{
	memset(&rc[c], 0, sizeof(rc[c]));
	rc[c].used = true;
	st.conns++;
}

static void r_recv(int c, const uint8_t *d, int n)
{
	rconn_t *r = &rc[c];

	for (int i = 0; (i < n) && r->used; i++) {
		char ch = (char)d[i];

		if (ch == '\r') {
			continue;
		}
		if (ch != '\n') {
			if (r->ln < LINE_MAX - 1) {
				r->line[r->ln++] = ch;
			} else {
				r->too_long = true;
			}
			continue;
		}
		r->line[r->ln] = '\0';
		r->ln = 0;
		dl_text_init(&ot, out, sizeof(out));
		bool more = true;

		if (r->too_long) {
			r->too_long = false;	// never run cut short (a digit less is another frequency)
			st.errors++;
			dl_text(&ot, "RPRT -%d\n", RIG_EINVAL);
		} else {
			more = command(c, r->line);
		}

		if (ot.len > 0) {
			tcp_write(c, out, (int)ot.len);	// at most OUT_MAX: fits the transmit buffer of a client that reads
		}
		if (!more) {
			release_ptt(c);
			r->used = false;
			tcp_close(c);
		}
	}
}

// Room for one answer before a line is taken (a client that doesn't read its answers is slowed down, not fed garbage)
static int r_space(int c)
{
	int room = tcp_space(c) - OUT_MAX;

	return (room > 0) ? 4 * LINE_MAX : 0;
}

static void r_peer_closed(int c)
{
	release_ptt(c);
	rc[c].used = false;
	tcp_close(c);
}

static void r_closed(int c, bool error)
{
	(void)error;
	release_ptt(c);
	rc[c].used = false;
}

static const tcp_app_t app = {
	.opened = r_opened, .recv = r_recv, .space = r_space, .peer_closed = r_peer_closed, .closed = r_closed,
};

void rigctld_start(void)
{
	memset(rc, 0, sizeof(rc));
	ptt_owner = -1;
	tcp_listen(NET_RIGCTL_PORT, &app);
}

void rigctld_poll(uint32_t now_ms)
{
	if (pending_apply && ((now_ms - last_req_ms) >= REQ_GAP_MS)) {
		apply_freq(now_ms);
	}
	if ((ptt_owner >= 0) && ((now_ms - ptt_ms) >= PTT_MAX_MS)) {
		release_ptt(-1);	// the safety limit (the radio's own TX time-out may come first)
	}
}

void rigctld_view(dl_text_t *t)
{
	dl_text(t, "rigctl.conns=%u\nrigctl.commands=%u\nrigctl.errors=%u\nrigctl.freq_sets=%u\nrigctl.ptt=%u\n"
		"rigctl.ptt_downs=%u\n", (unsigned)st.conns, (unsigned)st.commands, (unsigned)st.errors,
		(unsigned)st.freq_sets, ptt_owner >= 0, (unsigned)st.ptt_downs);
}

#endif	// CONFIG_NET_RIGCTL
