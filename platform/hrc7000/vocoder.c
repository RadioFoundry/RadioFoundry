// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 vocoder: the radio's own stock AMBE+2 codec, run from RAM in the board's codec window
// (docs/spec/codec-placement.md, "HR_C7000 radios"; the codec itself: docs/spec/codec-dm32.md, "C. Codec state init
// tables"). Shared by the HR_C7000 radios that carry this codec build; what differs between them (the slot, the windows)
// comes from the board.
//
// tools/hrc7000_codec.py carves the codec code and its table window out of the user's stock file and writes them into
// the board's codec slot behind a small header (codec_slot.h); they are never part of this repository or a release.
// On first use the code is copied to the start of the window and the words the slot lists are moved with it, and the
// tables are copied to their stock modem SRAM address. Without a valid slot, present() is false: FM only.
//
// The board's linker script gives the slot (__codec_slot..__codec_slot_end) and the windows a slot may name
// (__codec_run_lo/hi, __codec_tab_lo/hi); the profile prefix is the board's id (ODR_BOARD_ID, from the Makefile; board.toml [codec] profile starts with it).
//
// Not reentrant: one task (the audio task, through the audio block callback) makes all calls. The codec runs on the
// caller's stack.
#include <string.h>
#include "config.h"
#include "vocoder.h"
#include "codec_slot.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"

#ifndef ODR_BOARD_ID
#error "the Makefile defines ODR_BOARD_ID (the board's id, the codec profile prefix)"
#endif

#define CODEC_SIG0		0x152514D8u	// first two words of the codec code (every build)
#define CODEC_SIG1		0x74C776CDu

// Where a codec may run: the RAM window the linker script leaves out and the modem SRAM table window
extern const uint8_t __codec_run_lo[], __codec_run_hi[], __codec_tab_lo[], __codec_tab_hi[];

extern const uint8_t __codec_slot[];
extern const uint8_t __codec_slot_end[];

// State sizes: at least D 0x1400 and E 0x1900 (codec-dm32.md C, exact sizes open); room to spare
#define DEC_STATE_BYTES		0x1800u
#define ENC_STATE_BYTES		0x1C00u
#define HALF			(VOCODER_FRAME_SAMPLES / 2)

static uint32_t dec_state[DEC_STATE_BYTES / 4] __attribute__((aligned(8)));
static uint32_t enc_state[ENC_STATE_BYTES / 4] __attribute__((aligned(8)));
static int16_t bitbuf[72] __attribute__((aligned(4)));	// one bit per halfword
static int16_t pcmbuf[VOCODER_FRAME_SAMPLES] __attribute__((aligned(4)));

typedef int (*fn2_t)(void *, int);
typedef int (*fn3_t)(void *, int, int);
// C-SKY V2 ABI: arguments 5.. on the stack at sp+0, sp+4, ... (codec-dm32.md "Calling convention")
typedef int (*decode_t)(int16_t *out, int n, int16_t *bits, int r3, int flags, int half, void *d);
typedef int (*encode_t)(int16_t *bits, int r1, const int16_t *in, int n, int flags, int half, int s8, void *e);

static const codec_slot_t *slot;
static uint32_t code_ram;
static int8_t presence = -1;	// the slot's check: -1 not run yet, 1 valid, 0 not
static volatile bool parked;	// stopped for a stub upload: the calls return at once, the slot's check still holds
static bool ready;
static bool enc_first;

static int16_t *h16(void *state, uint32_t off)
{
	return (int16_t *)((uint8_t *)state + off);
}

// The slot checked (codec_slot.h: header, windows, entries, relocations, CRC), the code copied to where it runs and
// moved there, the tables copied, then the signature
static bool load(void)
{
	const codec_windows_t win = {
		(uint32_t)(uintptr_t)__codec_run_lo, (uint32_t)(uintptr_t)__codec_run_hi,
		(uint32_t)(uintptr_t)__codec_tab_lo, (uint32_t)(uintptr_t)__codec_tab_hi,
	};
	uint32_t run;
	if (!codec_slot_check(__codec_slot, (uint32_t)(__codec_slot_end - __codec_slot), &win, &run)) {
		return false;
	}
	const codec_slot_t *s = (const codec_slot_t *)__codec_slot;
	memcpy((void *)(uintptr_t)run, __codec_slot + s->code_off, s->code_len);
	codec_slot_relocate(__codec_slot, (uint8_t *)(uintptr_t)run, run);
	memcpy((void *)(uintptr_t)s->tab_ram, __codec_slot + s->tab_off, s->tab_len);
	hrc_barrier();
	const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)run;
	if (w[0] != CODEC_SIG0 || w[1] != CODEC_SIG1) {
		return false;
	}
	slot = s;
	code_ram = run;
	return true;
}

static volatile bool busy;	// a call is running the codec (hrc_vocoder_park waits it out)

static bool present(void)
{
	if (presence < 0) {
		presence = load() ? 1 : 0;
	}
	return presence == 1 && !parked;
}

static void *entry(uint32_t off)
{
	return (void *)(uintptr_t)(code_ram + off);
}

// The stock init sequence (codec-dm32.md C), on our state blocks
static void reset_run(void)
{
	if (!present()) {
		return;
	}
	void *D = dec_state, *E = enc_state;
	memset(dec_state, 0, sizeof dec_state);
	memset(enc_state, 0, sizeof enc_state);
	memset(bitbuf, 0, sizeof bitbuf);
	static const struct { uint16_t off; int16_t v; } eparams[] = {
		{ 0x172C, 0x1840 }, { 0x173A, 0x2000 }, { 0x173C, 128 }, { 0x173E, -20 }, { 0x1740, -22 },
		{ 0x1742, 20 }, { 0x1744, -10 }, { 0x1746, -40 }, { 0x1748, 0x4000 }, { 0x174A, 3276 },
		{ 0x174C, 3276 }, { 0x174E, 1310 }, { 0x1750, 10 }, { 0x1752, 20 },
	};
	for (uint32_t i = 0; i < sizeof eparams / sizeof eparams[0]; i++) {
		*h16(E, eparams[i].off) = eparams[i].v;
	}
	*h16(D, 0x1152) = 0;
	*h16(D, 0x116A) = 128;
	*h16(D, 0x116C) = -20;

	((fn3_t)entry(slot->enc_init))(E, 1, 1);
	((fn2_t)entry(slot->fec_init))(h16(E, 0x16E0), 1);
	((fn3_t)entry(slot->enc_set))(E, *h16(E, 0x173C), *h16(E, 0x173E));
	((fn2_t)entry(slot->dec_init))(D, 1);
	((fn2_t)entry(slot->fec_init))(h16(D, 0xFD0), 1);
	((decode_t)entry(slot->decode))(h16(D, 0x12AE), HALF, h16(D, 0x1354), 0, 8, 0, D);
	((fn3_t)entry(slot->dec_set))(D, *h16(D, 0x116A), *h16(D, 0x116C));
	enc_first = true;
	ready = true;
}

// Two calls per 20 ms frame, 80 samples each, half 0 then 1. Flags 0x1840 on the first call of a transmission
// (E[0x172C] at init) and 0x800 after; r1 and the sp+8 argument are the state's own E[0x1736] / E[0x173A], as stock's
// wrapper passes them. The 49 parameter bits come back one per halfword.
static void encode_run(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	if (!present()) {
		memset(bits, 0, VOCODER_BITS);
		return;
	}
	if (!ready) {
		reset_run();
	}
	memcpy(pcmbuf, pcm, sizeof pcmbuf);
	encode_t enc = (encode_t)entry(slot->encode);
	for (int h = 0; h < 2; h++) {
		enc(bitbuf, *h16(enc_state, 0x1736), &pcmbuf[h * HALF], HALF, enc_first ? 0x1840 : 0x800, h,
		    *h16(enc_state, 0x173A), enc_state);
		enc_first = false;
	}
	for (int i = 0; i < VOCODER_BITS; i++) {
		bits[i] = (uint8_t)(bitbuf[i] & 1);
	}
}

// Two calls per frame on the same bit buffer; flags 0 (a good frame: the caller conceals bad ones, core/dmr/ambe);
// r3 = D[0x1166] as stock's wrapper passes it
static void decode_run(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
	if (!present()) {
		memset(pcm, 0, VOCODER_FRAME_SAMPLES * sizeof(int16_t));
		return;
	}
	if (!ready) {
		reset_run();
	}
	for (int i = 0; i < VOCODER_BITS; i++) {
		bitbuf[i] = bits[i] & 1;
	}
	decode_t dec = (decode_t)entry(slot->decode);
	for (int h = 0; h < 2; h++) {
		dec(&pcmbuf[h * HALF], HALF, bitbuf, *h16(dec_state, 0x1166), 0, h, dec_state);
	}
	memcpy(pcm, pcmbuf, sizeof pcmbuf);
}

// The calls mark themselves busy, so a park can wait for the one running
static void reset(void)
{
	busy = true;
	hrc_barrier();		// set before present() reads parked: a park sees one or the other
	reset_run();
	busy = false;
}

static void encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	busy = true;
	hrc_barrier();
	encode_run(pcm, bits);
	busy = false;
}

static void decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
	busy = true;
	hrc_barrier();
	decode_run(bits, pcm);
	busy = false;
}

// The slot is checked first, before the stub upload overwrites the codec RAM: profile() and the tag then keep
// reporting the codec in flash (a flasher's VERSION after the stub entry)
void hrc_vocoder_park(void)
{
	(void)present();
	parked = true;		// every later call returns at once (silence, zero bits)
	hrc_barrier();
	while (busy) {
		vTaskDelay(1);
	}
}

// The id tools/odrflash and the release manifests use: which stock file the slot was carved from, "<board>-<tag>" with
// the tag in at least three digits (dm32-049, db25d-2501)
static const char *profile(void)
{
	static char id[sizeof ODR_BOARD_ID + 8];
	(void)present();
	if (presence != 1) {
		return NULL;
	}
	uint32_t tag = slot->stock_tag;
	if (tag == 0 || tag > 99999u) {
		return ODR_BOARD_ID "-unknown";
	}
	char digits[6];
	uint32_t n = 0;
	for (uint32_t v = tag; v != 0; v /= 10) {
		digits[n++] = (char)('0' + v % 10);
	}
	uint32_t o = 0;
	for (const char *b = ODR_BOARD_ID; *b != '\0'; b++) {
		id[o++] = *b;
	}
	id[o++] = '-';
	for (uint32_t pad = n; pad < 3; pad++) {
		id[o++] = '0';
	}
	while (n > 0) {
		id[o++] = digits[--n];
	}
	id[o] = '\0';
	return id;
}

const hal_vocoder_t hrc_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
};

uint16_t hrc_vocoder_tag(void)
{
	(void)present();
	return presence == 1 ? (uint16_t)slot->stock_tag : 0;
}

#endif	// CONFIG_DMR
