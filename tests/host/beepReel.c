// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Records the beep reel, a video of the UI simulator in which every beep in core/radio/beep.h plays once, in the
// situation where a radio would play it. This program writes the pieces (PNG frames, the audio as a WAV, the captions
// as ASS subtitles, and a WAV of each beep on its own) and tools/beep_reel.py encodes them into an MP4 with ffmpeg.
//
// We treat beep.c as a black box and only call beep_init, beep_play and beep_service. The fake hal_audio_t below calls
// the audio callback every 20 ms like a board does, and we call beep_service every 25 ms like the radio task. Beep
// lengths and timings are measured from the audio that comes out, never read from beep.c, so beep.c can be rewritten
// any way you like as long as beep.h keeps its functions. BEEP_DIR in the Makefile builds against another commit's
// beep.c and beep.h, which is how CI makes the "before" reel.
//
// The sim has no real radio core, so nothing in it plays the radio core's beeps (talk permit, call end and so on).
// The scenes below play those by hand. The UI's own beeps (key, error, message, battery low) come from the real UI
// code through the sim's beep hook.
//
// Before any step that can beep, we wait until 1 s after the previous beep ended, so a longer beep pushes the rest of
// the reel later.
//
// Usage:
//   beepReel --out DIR [--geom 160x128] [--map K] [--scale 4] [--band 96] [--lenient] [--no-frames]
//
// Output:
//   DIR/frames/NNNNN.png   one frame per 50 ms sim tick, unscaled (skipped with --no-frames)
//   DIR/reel.wav           the reel's audio, 8 kHz 16-bit mono, starting at the first frame
//   DIR/reel.ass           the captions, sized for the video at --scale with a --band pixel strip under the screen
//   DIR/beeps/<name>.wav   each beep played on its own
//
// Exits with 1 if beep_names.h is missing a beep, a beep never stops or makes no sound, two beeps are less than 1 s
// apart (key beeps in a run of presses may be closer), there are more than EVENTS_MAX beeps, a beep has no scene (the
// speaker test's sweep needs none), or a scene uses a beep that beep.h doesn't have. With --lenient, which is for
// building against another commit's beep.h, the last two are only warnings.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include "sim/ui_sim.h"
#include "ui/ui_svc.h"
#include "ui/gfx.h"
#include "radio/beep.h"

#define LEVEL		50	// the radio's default tone volume
#define POLL_MS		25	// how often the radio task calls beep_service (RADIO_POLL_MS)
#define BLOCK_MS	20	// length of one audio block
#define TICK_MS		50	// one sim tick, one video frame
#define GAP_MS		1000	// minimum silence between beeps
#define PACE_MS		250	// time between key presses in a run
#define STOP_MS		5000	// give up on a beep still playing after this long
#define SAMPLES_PER_MS	(AUDIO_RATE_HZ / 1000)

static const struct {
	const char *name;	// "TX_PERMIT" for BEEP_TX_PERMIT
	beep_t b;
} beeps[] = {
#include "beep_names.h"
};
#define BEEPS	((int)(sizeof beeps / sizeof beeps[0]))

// The UI's beep codes and the beeps they play. This has to match ui_svc_beep() in core/ui/ui_svc_radio.c
static const char *const ui_beeps[] = {
	[UI_BEEP_KEY] = "KEY", [UI_BEEP_ERROR] = "ERROR", [UI_BEEP_MESSAGE] = "MESSAGE",
	[UI_BEEP_BATTERY_LOW] = "BATTERY_LOW",
};

// One step of a scene. A step waits out the gap unless it's quiet, plays its radio core beep if it has one, runs its
// script, then leaves the screen up for hold_ms.
typedef struct {
	const char *caption;	// starts a new scene with this caption; NULL continues the current one
	const char *script;	// sim script tokens, see ui_sim.c
	const char *beep;	// radio core beep to play as the script starts, like "CALL_END", or NULL
	bool keys;		// the script is a run of key presses, to be spaced PACE_MS apart
	bool quiet;		// the step makes no sound, so it doesn't wait for the gap
	uint16_t hold_ms;	// how long to leave the screen up afterwards
} step_t;

// The scenes go from the beeps people hear most to the ones they hear least. The sim starts on key map K, zone 0,
// channel 1 (DMR), with key beep on. The TX timeout and the battery drain are sped up so the reel doesn't sit waiting.
//
// Nothing ties these steps to the radio core. If you change when core/radio/radio_sm.c plays a beep, update the
// matching step here. The calls are in dmr_follow() (TX_PERMIT, TX_PERMIT_SHORT, CALL_END), tx_timer() (TOT_WARN), deny() (TX_DENY),
// battery_poll() (BATTERY_CRIT), scan_service() (SCAN_HOLD) and emergency_tick() (EMERGENCY).
static const step_t steps[] = {
	{ "Browse the menu", "ok down down back", NULL, true, false, 0 },
	{ "Group call heard", "rx:9991234,91,g,Q1AAA_Joe", NULL, false, true, 700 },
	{ NULL, "rxend", "CALL_END", false, false, 0 },
	{ "Reply, held past the time-out (shortened)", "pttdown", "TX_PERMIT", false, false, 0 },
	{ NULL, NULL, "TOT_WARN", false, false, 0 },
	{ NULL, "timeout", "TX_DENY", false, false, 800 },
	{ NULL, "pttup", NULL, false, true, 300 },
	{ "Reply, permit before keying (radios with CAP_BEEP_NOT_ON_AIR)", "pttdown", "TX_PERMIT_SHORT", false, false, 600 },
	{ NULL, "pttup", NULL, false, true, 300 },
	{ "Text message", "msg:9991234,Net_starts_in_5_min", NULL, false, false, 0 },
	{ NULL, "back", NULL, false, false, 0 },
	{ "Battery runs down", "battery:14", NULL, false, false, 0 },
	{ NULL, "battery:3", "BATTERY_CRIT", false, false, 0 },
	{ NULL, "back", NULL, false, false, 0 },	// close the battery warning, or it covers the scan
	{ "Scan", "ok 6 1", NULL, true, false, 700 },
	{ NULL, "fmrx", "SCAN_HOLD", false, false, 0 },
	{ NULL, "fmrxend", NULL, false, true, 0 },
	{ NULL, "back", NULL, false, false, 0 },
	{ "PTT while locked", "pttlocked:off", NULL, false, true, 0 },
	{ NULL, "long:quick", NULL, false, false, 0 },
	{ NULL, "ptt:500", NULL, false, false, 0 },
	{ NULL, "long:quick", NULL, false, false, 0 },
	{ "Emergency heard", "emergency:3105678", "EMERGENCY", false, false, 0 },
	{ NULL, "back", NULL, false, false, 500 },
};
#define STEPS	((int)(sizeof steps / sizeof steps[0]))

static bool lenient, no_frames;
static int failures;

static void fail(bool hard, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "beepReel: %s", hard || !lenient ? "" : "warning: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	if (hard || !lenient)
		failures++;
}

static int find_beep(const char *name)
{
	for (int i = 0; i < BEEPS; i++)
		if (strcmp(beeps[i].name, name) == 0)
			return i;
	return -1;
}

// ---- Fake board audio and the recorded PCM ----

static audio_block_cb_t cb;
static void *cb_ctx;
static bool a_start(audio_block_cb_t c, void *ctx) { cb = c; cb_ctx = ctx; return true; }
static void a_stop(void) { cb = NULL; }
static void a_pcm(bool on) { (void)on; }
static const hal_audio_t audio = { .start = a_start, .stop = a_stop, .speaker_pcm = a_pcm };

static int16_t *pcm;
static uint32_t pcm_len, pcm_cap;
static uint32_t audio_ms, audio_t0;	// audio is rendered up to audio_ms; pcm[0] is at audio_t0

static void pump_to(uint32_t ms)
{
	while (audio_ms < ms) {
		uint32_t t = audio_ms - audio_t0;
		if (t % POLL_MS == 0) {
#ifdef BEEP_SERVICE_NOW
			beep_service(false, audio_ms);
#else
			beep_service(false);	// a base commit's beep.h from before the amplifier hang
#endif
		}
		if (t % BLOCK_MS == 0) {
			int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, spk[AUDIO_BLOCK_SAMPLES] = { 0 };
			if (cb != NULL)
				cb(mic, spk, cb_ctx);
			if (pcm_len + AUDIO_BLOCK_SAMPLES > pcm_cap) {
				pcm_cap = pcm_cap * 2 + AUDIO_RATE_HZ;
				pcm = realloc(pcm, pcm_cap * sizeof pcm[0]);
				if (pcm == NULL) {
					perror("beepReel");
					exit(1);
				}
			}
			memcpy(pcm + pcm_len, spk, sizeof spk);
			pcm_len += AUDIO_BLOCK_SAMPLES;
		}
		audio_ms += 5;
	}
}

static void audio_restart(uint32_t t0)
{
	pcm_len = 0;
	audio_ms = audio_t0 = t0;
}

// Finds the first and last nonzero sample between from and to, in ms from pcm[0]. False if it's all silence
static bool sound_in(uint32_t from, uint32_t to, uint32_t *first, uint32_t *last)
{
	uint32_t a = from * SAMPLES_PER_MS, b = to * SAMPLES_PER_MS;
	if (b > pcm_len)
		b = pcm_len;
	bool any = false;
	for (uint32_t i = a; i < b; i++) {
		if (pcm[i] != 0) {
			if (!any)
				*first = i;
			*last = i;
			any = true;
		}
	}
	return any;
}

static bool write_wav(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		perror(path);
		return false;
	}
	uint32_t bytes = pcm_len * 2, riff = 36 + bytes, fmt_len = 16, rate = AUDIO_RATE_HZ, byte_rate = AUDIO_RATE_HZ * 2;
	uint16_t pcm_fmt = 1, channels = 1, align = 2, bits = 16;
	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt_len, 4, 1, f);
	fwrite(&pcm_fmt, 2, 1, f);
	fwrite(&channels, 2, 1, f);
	fwrite(&rate, 4, 1, f);
	fwrite(&byte_rate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&bytes, 4, 1, f);
	fwrite(pcm, 2, pcm_len, f);	// WAV is little-endian, and so are the hosts we build on
	return fclose(f) == 0;
}

// ---- Measuring each beep on its own ----

static uint32_t beep_end_ms[sizeof beeps / sizeof beeps[0]];	// ms from beep_play to the beep's last sound

static void measure(const char *dir)
{
	for (int i = 0; i < BEEPS; i++) {
		audio_restart(0);
		cb = NULL;
		beep_play(beeps[i].b, LEVEL, false);
		while (cb != NULL || audio_ms < POLL_MS) {
			if (audio_ms >= STOP_MS) {
				fail(true, "BEEP_%s never stops", beeps[i].name);
				cb = NULL;
				break;
			}
			pump_to(audio_ms + 5);
		}
		uint32_t first, last;
		if (!sound_in(0, audio_ms, &first, &last)) {
			fail(true, "BEEP_%s makes no sound", beeps[i].name);
			last = 0;
		}
		beep_end_ms[i] = (last + 1 + SAMPLES_PER_MS - 1) / SAMPLES_PER_MS;
		char path[512], name[64];
		snprintf(name, sizeof name, "%s", beeps[i].name);
		for (char *p = name; *p; p++)
			*p = (char)tolower((unsigned char)*p);
		snprintf(path, sizeof path, "%s/beeps/%s.wav", dir, name);
		if (!write_wav(path))
			failures++;
	}
}

// ---- Recording the reel ----

#define EVENTS_MAX	64
static struct {
	int beep;		// index into beeps[]
	uint32_t at;		// sim time of the beep_play call
} events[EVENTS_MAX];
static int event_n;
static uint32_t scene_at[sizeof steps / sizeof steps[0]];	// sim time each captioned step started
static uint32_t frame_n;
static const char *out_dir;
static bool recording;
static bool sounded[sizeof beeps / sizeof beeps[0]];

static void sound(const char *name, uint32_t now)
{
	int i = find_beep(name);
	if (i < 0) {
		fail(false, "the scenes sound BEEP_%s, which beep.h doesn't have", name);
		return;
	}
	pump_to(now);
	beep_play(beeps[i].b, LEVEL, false);
	sounded[i] = true;
	if (event_n == EVENTS_MAX) {
		static bool said;
		if (!said)
			fail(true, "more than %d beeps in the reel: BEEP_%s and later ones aren't checked or captioned "
			     "(EVENTS_MAX)", EVENTS_MAX, name);
		said = true;
		return;
	}
	events[event_n].beep = i;
	events[event_n].at = now;
	event_n++;
}

static void on_ui_beep(uint8_t b, uint32_t now)
{
	if (recording && b < sizeof ui_beeps / sizeof ui_beeps[0] && ui_beeps[b] != NULL)
		sound(ui_beeps[b], now);
}

static void save_frame(void)
{
	char path[512];
	snprintf(path, sizeof path, "%s/frames/%05u.png", out_dir, (unsigned)frame_n++);
	if (!no_frames && !sim_save_png(path, 1)) {
		fprintf(stderr, "beepReel: can't write %s\n", path);
		exit(1);
	}
}

static void on_tick(uint32_t now)
{
	if (!recording)
		return;
	pump_to(now);
	save_frame();
}

static void run(const char *script)
{
	char err[256];
	if (!sim_run(script, err, sizeof err)) {
		fprintf(stderr, "beepReel: %s\n", err);
		exit(1);
	}
}

static void wait_ms(uint32_t ms)
{
	if (ms == 0)
		return;
	char t[32];
	snprintf(t, sizeof t, "wait:%u", (unsigned)((ms + TICK_MS - 1) / TICK_MS * TICK_MS));
	run(t);
}

// Waits until GAP_MS after the last beep ended
static void gap(void)
{
	if (event_n == 0)
		return;
	uint32_t free_at = events[event_n - 1].at + beep_end_ms[events[event_n - 1].beep] + GAP_MS, now = sim_now();
	if (now < free_at)
		wait_ms(free_at - now);
}

static void play_steps(void)
{
	for (int s = 0; s < STEPS; s++) {
		const step_t *st = &steps[s];
		if (!st->quiet)
			gap();
		if (st->caption != NULL)
			scene_at[s] = sim_now();
		if (st->beep != NULL)
			sound(st->beep, sim_now());
		if (st->script != NULL && st->keys) {
			char buf[256];
			snprintf(buf, sizeof buf, "%s", st->script);
			char *save = NULL;
			for (char *k = strtok_r(buf, " ", &save); k != NULL; k = strtok_r(NULL, " ", &save)) {
				uint32_t t = sim_now();
				run(k);
				uint32_t took = sim_now() - t;
				if (took < PACE_MS && save != NULL && *save != '\0')
					wait_ms(PACE_MS - took);
			}
		} else if (st->script != NULL) {
			run(st->script);
		}
		wait_ms(st->hold_ms);
	}
}

// Turns a beep's name into its caption, so TX_PERMIT becomes "TX permit"
static void beep_title(int i, char *out, size_t len)
{
	snprintf(out, len, "%s", beeps[i].name);
	for (char *p = out; *p; p++)
		*p = *p == '_' ? ' ' : (char)tolower((unsigned char)*p);
	static const char *const upper[] = { "tx", "tot", "dmr", "fm" };
	for (char *w = out; *w; ) {
		size_t n = strcspn(w, " ");
		for (size_t u = 0; u < sizeof upper / sizeof upper[0]; u++)
			if (strlen(upper[u]) == n && strncmp(w, upper[u], n) == 0)
				for (size_t c = 0; c < n; c++)
					w[c] = (char)toupper((unsigned char)w[c]);
		w += n;
		while (*w == ' ')
			w++;
	}
	out[0] = (char)toupper((unsigned char)out[0]);
}

static void ass_time(FILE *f, uint32_t ms)
{
	fprintf(f, "%u:%02u:%02u.%02u", (unsigned)(ms / 3600000), (unsigned)(ms / 60000 % 60), (unsigned)(ms / 1000 % 60),
		(unsigned)(ms % 1000 / 10));
}

// Writes the captions shown in the strip under the screen: the scene's name on top, and below it the beep that just
// played and how long it lasted.
static bool write_captions(const char *path, int w, int h, int band, const uint32_t *start, const uint32_t *end,
			   uint32_t total)
{
	FILE *f = fopen(path, "w");
	if (f == NULL) {
		perror(path);
		return false;
	}
	int big = band * 3 / 10, small = band / 4, margin = band / 6;
	fprintf(f, "[Script Info]\nScriptType: v4.00+\nPlayResX: %d\nPlayResY: %d\nWrapStyle: 2\n\n", w, h + band);
	fprintf(f, "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, OutlineColour, BackColour, Bold, "
		"Alignment, MarginL, MarginR, MarginV, BorderStyle, Outline, Shadow\n");
	fprintf(f, "Style: Scene,DejaVu Sans,%d,&H00B0B0B0,&H00000000,&H00000000,0,1,%d,%d,%d,1,0,0\n", small, margin,
		margin, band - margin - small);
	fprintf(f, "Style: Beep,DejaVu Sans,%d,&H0000D8FF,&H00000000,&H00000000,1,1,%d,%d,%d,1,0,0\n\n", big, margin,
		margin, margin);
	fprintf(f, "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n");
	uint32_t t0 = audio_t0;
	for (int s = 0; s < STEPS; s++) {
		if (steps[s].caption == NULL)
			continue;
		uint32_t from = scene_at[s] - t0, to = total;
		for (int n = s + 1; n < STEPS; n++) {
			if (steps[n].caption != NULL) {
				to = scene_at[n] - t0;
				break;
			}
		}
		fputs("Dialogue: 0,", f);
		ass_time(f, from);
		fputc(',', f);
		ass_time(f, to);
		fprintf(f, ",Scene,,0,0,0,,%s\n", steps[s].caption);
	}
	for (int e = 0; e < event_n; e++) {
		char title[64];
		beep_title(events[e].beep, title, sizeof title);
		uint32_t to = start[e] + GAP_MS;
		if (e + 1 < event_n && start[e + 1] < to)
			to = start[e + 1];
		if (to > total)
			to = total;
		fputs("Dialogue: 0,", f);
		ass_time(f, start[e]);
		fputc(',', f);
		ass_time(f, to);
		fprintf(f, ",Beep,,0,0,0,,%s  %u ms\n", title, (unsigned)(end[e] - start[e]));
	}
	return fclose(f) == 0;
}

static int usage(void)
{
	fprintf(stderr, "usage: beepReel --out DIR [--geom 160x128] [--map K|M|V|F] [--scale 4] [--band 96] "
		"[--lenient] [--no-frames]\n");
	return 2;
}

int main(int argc, char **argv)
{
	const char *geom = "160x128";
	sim_map_t map = SIM_MAP_K;
	int scale = 4, band = 96;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (strcmp(a, "--lenient") == 0) {
			lenient = true;
			continue;
		}
		if (strcmp(a, "--no-frames") == 0) {
			no_frames = true;
			continue;
		}
		if (v == NULL)
			return usage();
		i++;
		if (strcmp(a, "--out") == 0)
			out_dir = v;
		else if (strcmp(a, "--geom") == 0)
			geom = v;
		else if (strcmp(a, "--map") == 0 && strlen(v) == 1 && strchr("KMVF", v[0]) != NULL)
			map = (sim_map_t)(strchr("KMVF", v[0]) - "KMVF");
		else if (strcmp(a, "--scale") == 0)
			scale = atoi(v);
		else if (strcmp(a, "--band") == 0)
			band = atoi(v);
		else
			return usage();
	}
	const sim_geom_t *g = sim_geom_find(geom);
	if (out_dir == NULL || g == NULL || scale < 1 || band < 0)
		return usage();

	// beep_names.h comes from a rough parse of beep.h (beep_names.awk). If it missed a beep or picked up something that
	// isn't one, stop now instead of quietly leaving a beep out of the reel.
	bool listed[BEEP_COUNT] = { false };
	for (int i = 0; i < BEEPS; i++) {
		if (beeps[i].b <= BEEP_NONE || beeps[i].b >= BEEP_COUNT || listed[beeps[i].b]) {
			fprintf(stderr, "beepReel: beep_names.h: BEEP_%s isn't a beep, or is listed twice\n", beeps[i].name);
			return 1;
		}
		listed[beeps[i].b] = true;
	}
	for (int b = BEEP_NONE + 1; b < BEEP_COUNT; b++)
		if (!listed[b]) {
			fprintf(stderr, "beepReel: beep %d of beep.h isn't in beep_names.h (the Makefile's awk missed it)\n", b);
			return 1;
		}

	char path[512];
	mkdir(out_dir, 0777);
	snprintf(path, sizeof path, "%s/frames", out_dir);
	if (!no_frames)
		mkdir(path, 0777);
	snprintf(path, sizeof path, "%s/beeps", out_dir);
	mkdir(path, 0777);

	beep_init(&audio);
	measure(out_dir);

	sim_reset(g, map);
	sim_set_tick_hook(on_tick);
	sim_set_beep_hook(on_ui_beep);
	run("wait:500");		// let the start screen settle
	audio_restart(sim_now());
	cb = NULL;
	recording = true;
	save_frame();
	play_steps();
	run("wait:500");
	recording = false;
	uint32_t total = sim_now() - audio_t0;
	pump_to(sim_now());

	// Work out from the recorded audio when each beep really started and stopped. A beep's window runs from its
	// beep_play to the next one's.
	uint32_t start[EVENTS_MAX], end[EVENTS_MAX];
	for (int e = 0; e < event_n; e++) {
		uint32_t from = events[e].at - audio_t0, to = e + 1 < event_n ? events[e + 1].at - audio_t0 : total;
		uint32_t a, b;
		if (!sound_in(from, to, &a, &b)) {
			fail(true, "BEEP_%s made no sound in the reel", beeps[events[e].beep].name);
			a = b = from * SAMPLES_PER_MS;
		}
		start[e] = a / SAMPLES_PER_MS;
		end[e] = (b + 1 + SAMPLES_PER_MS - 1) / SAMPLES_PER_MS;
		printf("%6u ms  %-14s %4u ms\n", (unsigned)start[e], beeps[events[e].beep].name, (unsigned)(end[e] - start[e]));
		if (e > 0 && start[e] < end[e - 1] + GAP_MS) {
			bool keys = strcmp(beeps[events[e].beep].name, "KEY") == 0 &&
				    strcmp(beeps[events[e - 1].beep].name, "KEY") == 0;
			if (!keys)
				fail(true, "BEEP_%s only %u ms after BEEP_%s", beeps[events[e].beep].name,
				     (unsigned)(start[e] - end[e - 1]), beeps[events[e - 1].beep].name);
		}
	}
	for (int i = 0; i < BEEPS; i++)
		if (!sounded[i] && beeps[i].b != BEEP_SWEEP)	// the speaker test's sweep: no radio user hears it
			fail(false, "BEEP_%s has no scene (tests/host/beepReel.c)", beeps[i].name);

	snprintf(path, sizeof path, "%s/reel.wav", out_dir);
	if (!write_wav(path))
		failures++;
	snprintf(path, sizeof path, "%s/reel.ass", out_dir);
	if (!write_captions(path, gfx_width() * scale, gfx_height() * scale, band, start, end, total))
		failures++;
	printf("%u frames, %u.%u s\n", (unsigned)frame_n, (unsigned)(total / 1000), (unsigned)(total % 1000 / 100));
	return failures ? 1 : 0;
}
