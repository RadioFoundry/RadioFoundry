// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV vocoder: the radio's own stock AMBE+2 codec (049 or 046), run from RAM at its stock address
// (docs/spec/codec-dm32.md, "Recommended hosting" and "C. Codec state init tables").
//
// tools/dm32_codec.py carves the codec code and its table window out of the user's stock file and writes them into
// the codec slot (0x030E8000..0x03100000) behind a small header; they are never part of this repository or a release.
// On first use the code is copied to its stock RAM address (inside the window dm32.ld reserves) and the tables to their
// stock modem SRAM address. Nothing is relocated or patched. Without a valid slot, present() is false: FM only.
//
// Not reentrant: one task (the audio task, through the audio block callback) makes all calls. The codec runs on the
// caller's stack.
#include <string.h>
#include "dm32.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "platform.h"

#define SLOT_MAGIC		0x43524444u	// "DDRC" little-endian: ODR DM-32 codec slot
#define SLOT_VERSION		1u
#define CODEC_SIG0		0x152514D8u	// first two words of the codec code (both versions)
#define CODEC_SIG1		0x74C776CDu

// Where a codec may run: the RAM window dm32.ld leaves out (049 and 046 code windows) and the modem SRAM table window
#define RUN_LO			0x00016CDCu
#define RUN_HI			0x0002F914u
#define TAB_LO			0x18000E48u
#define TAB_HI			0x18003E20u

// The slot header, written by tools/dm32_codec.py (keep the two in step). Code follows at +code_off, tables at +tab_off
typedef struct {
	uint32_t magic, version;
	uint32_t code_off, code_len, code_ram;	// run address
	uint32_t tab_off, tab_len, tab_ram;
	// Entry points, offsets from the code start
	uint32_t decode, encode, dec_init, dec_set, enc_init, enc_set, fec_init;
	uint32_t stock_tag;			// 49 or 46: which stock file
	uint32_t crc;				// CRC-32 of code then tables, as written to the slot
	uint32_t reserved[15];
} codec_slot_t;
_Static_assert(sizeof(codec_slot_t) == 128, "codec slot header layout");

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
static int8_t presence = -1;	// -1 not checked yet
static bool ready;
static bool enc_first;

static uint32_t crc32(const uint8_t *p, uint32_t len, uint32_t c)
{
	static const uint32_t nib[16] = {
		0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
		0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
	};
	c = ~c;
	while (len--) {
		c ^= *p++;
		c = (c >> 4) ^ nib[c & 15];
		c = (c >> 4) ^ nib[c & 15];
	}
	return ~c;
}

static int16_t *h16(void *state, uint32_t off)
{
	return (int16_t *)((uint8_t *)state + off);
}

static bool within(uint32_t a, uint32_t len, uint32_t lo, uint32_t hi)
{
	return a >= lo && a <= hi && len <= hi - a;
}

// Header sane, windows inside what the memory map reserves, CRC right; then the copy and the signature check
static bool load(void)
{
	const codec_slot_t *s = (const codec_slot_t *)__codec_slot;
	uint32_t room = (uint32_t)(__codec_slot_end - __codec_slot);
	if (s->magic != SLOT_MAGIC || s->version != SLOT_VERSION) {
		return false;
	}
	if (s->code_off < sizeof *s || s->code_off > room || s->code_len > room - s->code_off ||
		s->tab_off > room || s->tab_len > room - s->tab_off ||
		!within(s->code_ram, s->code_len, RUN_LO, RUN_HI) || !within(s->tab_ram, s->tab_len, TAB_LO, TAB_HI)) {
		return false;
	}
	uint32_t ep[7] = { s->decode, s->encode, s->dec_init, s->dec_set, s->enc_init, s->enc_set, s->fec_init };
	for (int i = 0; i < 7; i++) {
		if (ep[i] >= s->code_len || (ep[i] & 1u) != 0) {
			return false;
		}
	}
	const uint8_t *code = __codec_slot + s->code_off;
	const uint8_t *tab = __codec_slot + s->tab_off;
	if (crc32(tab, s->tab_len, crc32(code, s->code_len, 0)) != s->crc) {
		return false;
	}
	memcpy((void *)(uintptr_t)s->code_ram, code, s->code_len);
	memcpy((void *)(uintptr_t)s->tab_ram, tab, s->tab_len);
	hrc_barrier();
	const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)s->code_ram;
	if (w[0] != CODEC_SIG0 || w[1] != CODEC_SIG1) {
		return false;
	}
	slot = s;
	code_ram = s->code_ram;
	return true;
}

static bool present(void)
{
	if (presence < 0) {
		presence = load() ? 1 : 0;
	}
	return presence == 1;
}

static void *entry(uint32_t off)
{
	return (void *)(uintptr_t)(code_ram + off);
}

// The stock init sequence (codec-dm32.md C), on our state blocks
static void reset(void)
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
static void encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	if (!present()) {
		memset(bits, 0, VOCODER_BITS);
		return;
	}
	if (!ready) {
		reset();
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
static void decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
	if (!present()) {
		memset(pcm, 0, VOCODER_FRAME_SAMPLES * sizeof(int16_t));
		return;
	}
	if (!ready) {
		reset();
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

// The id tools/odrflash and the release manifests use: which stock file the slot was carved from
static const char *profile(void)
{
	if (!present()) {
		return NULL;
	}
	return slot->stock_tag == 49u ? "dm32-049" : (slot->stock_tag == 46u ? "dm32-046" : "dm32-unknown");
}

const hal_vocoder_t dm32_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
};

uint8_t dm32_vocoder_tag(void)
{
	return present() ? (uint8_t)slot->stock_tag : 0;
}

#endif	// CONFIG_DMR
