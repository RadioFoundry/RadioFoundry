// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Host tests for the APRS protocol layers (core/aprs, docs/spec/aprs.md 13): the AX.25 FCS and addresses, HDLC bit
 * stuffing and NRZI, AFSK modulation into demodulation with noise, tilt and clock error, APRS positions (uncompressed,
 * compressed, Mic-E, objects, items), messages, NMEA, KISS, SmartBeaconing and the trigger rules, the iGate rules and
 * the digipeater. Vectors come from the specs (worked examples) or are computed by hand from their rules.
 *
 *   make -C tests/host            (aprsTest --wav DIR writes test WAV files for an external decoder)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "aprs/ax25.h"
#include "aprs/hdlc.h"
#include "aprs/afsk.h"
#include "aprs/kiss.h"
#include "aprs/aprs_fmt.h"
#include "aprs/aprs_nmea.h"
#include "aprs/aprs_beacon.h"
#include "aprs/aprs_igate.h"
#include "fuzz.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ax25_addr_t addr(const char *s)
{
	ax25_addr_t a;
	if (!ax25_addr_parse(s, strlen(s), &a)) {
		printf("bad test address %s\n", s);
		exit(2);
	}
	return a;
}

static size_t make_frame(const char *tnc2, uint8_t *out, size_t max)
{
	ax25_frame_t f;
	if (!ax25_from_tnc2(tnc2, strlen(tnc2), &f)) {
		printf("bad test frame %s\n", tnc2);
		exit(2);
	}
	f.dst.h = true;
	return ax25_encode(&f, out, max);
}

// --- AX.25 ---

static void test_fcs(void)
{
	// CRC-16/X-25 check value (ISO 3309 / X.25 FCS): "123456789" -> 0x906E
	CHECK(ax25_fcs((const uint8_t *)"123456789", 9) == 0x906E, "fcs %04X", ax25_fcs((const uint8_t *)"123456789", 9));
	uint8_t buf[11];
	memcpy(buf, "123456789", 9);
	uint16_t f = ax25_fcs(buf, 9);
	buf[9] = (uint8_t)f;
	buf[10] = (uint8_t)(f >> 8);
	// the FCS over data plus FCS gives the fixed residue 0xF0B8 before the final complement: ~0xF0B8 = 0x0F47
	CHECK(ax25_fcs(buf, 11) == 0x0F47, "residue %04X", ax25_fcs(buf, 11));
}

static void test_addresses(void)
{
	ax25_addr_t a;
	CHECK(ax25_addr_parse("n0call-15", 9, &a) && strcmp(a.call, "N0CALL") == 0 && a.ssid == 15, "parse");
	CHECK(!ax25_addr_parse("N0CALL-16", 9, &a), "ssid 16");
	CHECK(!ax25_addr_parse("TOOLONG1", 8, &a), "7 chars");
	CHECK(!ax25_addr_parse("-1", 2, &a), "empty call");
	CHECK(!ax25_addr_parse("N0CALL-", 7, &a), "dash only");
	char s[12];
	ax25_addr_parse("N0CALL-15", 9, &a);
	a.h = true;
	ax25_addr_format(&a, true, s);
	CHECK(strcmp(s, "N0CALL-15*") == 0, "format %s", s);

	// N0CALL>APRS: the address bytes of AX.25 2.2 3.12 (callsign shifted left, SSID byte 0b011SSSS0, C bit on
	// the destination for a command, the extension bit on the last address)
	uint8_t buf[64];
	size_t n = make_frame("N0CALL>APRS:>test", buf, sizeof(buf));
	static const uint8_t want[] = { 0x82, 0xA0, 0xA4, 0xA6, 0x40, 0x40, 0xE0, 0x9C, 0x60, 0x86, 0x82, 0x98, 0x98,
					0x61, 0x03, 0xF0, '>', 't', 'e', 's', 't' };
	CHECK(n == sizeof(want) && memcmp(buf, want, n) == 0, "encode n=%zu", n);
	ax25_frame_t f;
	CHECK(ax25_decode(buf, n, &f) && f.ndigi == 0 && strcmp(f.src.call, "N0CALL") == 0 && f.info_len == 5,
	      "decode");

	// TNC2 round trip with a used digipeater
	const char *line = "K1ABC-9>APZODR,W1XYZ-1*,WIDE2-1:!4903.50N/07201.75W-Test";
	n = make_frame(line, buf, sizeof(buf));
	CHECK(ax25_decode(buf, n, &f), "decode digi");
	CHECK(f.ndigi == 2 && f.digi[0].h && !f.digi[1].h, "h bits");
	char out[AX25_TNC2_MAX];
	ax25_to_tnc2(&f, out, sizeof(out));
	CHECK(strcmp(out, line) == 0, "tnc2 %s", out);
	// junk: no end bit, bad characters
	buf[13] &= (uint8_t)~1u;
	buf[20] &= (uint8_t)~1u;
	buf[27] &= (uint8_t)~1u;
	CHECK(!ax25_decode(buf, 21, &f), "no end bit");
	// APRS-IS line with a q construct and a long server name: the name is skipped
	CHECK(ax25_from_tnc2("K1ABC>APRS,TCPIP*,qAC,T2TEXAS99:>hi", 35, &f) && f.ndigi == 2 &&
	      strcmp(f.digi[1].call, "QAC") == 0, "is line");
}

// --- HDLC ---

static void test_hdlc(void)
{
	uint8_t frame[64], tones[256];
	// a frame full of 0xFF and 0x7E: the worst case for stuffing
	size_t n = make_frame("N0CALL>APRS:~~~~", frame, sizeof(frame));
	for (int i = 0; i < 4; i++) {
		frame[n - 4 + i] = (uint8_t)(i & 1 ? 0xFF : 0x7E);
	}
	hdlc_enc_t e;
	hdlc_enc_init(&e, tones, sizeof(tones));
	CHECK(hdlc_enc_flags(&e, 4) && hdlc_enc_frame(&e, frame, n) && hdlc_enc_flags(&e, 2), "encode");
	// between the flags no run of one tone is longer than 6 symbols (five 1s and the stuffed 0)
	int run = 1, maxrun = 0;
	for (uint32_t i = 33; i < e.nbits - 16; i++) {
		if (hdlc_tone_bit(tones, i) == hdlc_tone_bit(tones, i - 1)) {
			run++;
		} else {
			run = 1;
		}
		if (run > maxrun) {
			maxrun = run;
		}
	}
	CHECK(maxrun <= 6, "run %d", maxrun);
	// decode the tones back
	hdlc_dec_t d;
	hdlc_dec_init(&d);
	size_t got = 0;
	bool same = false;
	for (uint32_t i = 0; i < e.nbits; i++) {
		size_t k = hdlc_dec_bit(&d, hdlc_tone_bit(tones, i));
		if (k) {
			got = k;
			same = k == n && memcmp(d.buf, frame, n) == 0;
		}
	}
	CHECK(got == n && same, "decode got %zu", got);
	// a flipped bit fails the FCS
	hdlc_dec_init(&d);
	tones[10] ^= 0x10;
	got = 0;
	for (uint32_t i = 0; i < e.nbits; i++) {
		got += hdlc_dec_bit(&d, hdlc_tone_bit(tones, i));
	}
	CHECK(got == 0 && d.fcs_errors + d.aborts >= 1, "bad bit got %zu", got);
	CHECK(hdlc_flags_for_ms(300, 1200, 2) == 45, "flags 300 ms %u", hdlc_flags_for_ms(300, 1200, 2));
	// the first bits: a flag 0x7E LSB first is 0 1 1 1 1 1 1 0; from mark, a 0 changes the tone (NRZI)
	hdlc_enc_init(&e, tones, sizeof(tones));
	hdlc_enc_flags(&e, 1);
	CHECK(tones[0] == 0x80, "flag tones %02X", tones[0]);	// 0 -> space, six 1s stay space, 0 -> mark
}

// --- AFSK ---

static uint32_t rng = 12345;
static float gauss(void)
{
	// Box-Muller on a small LCG
	rng = rng * 1103515245u + 12345u;
	float u1 = ((rng >> 8) + 1.0f) / 16777217.0f;
	rng = rng * 1103515245u + 12345u;
	float u2 = (rng >> 8) / 16777216.0f;
	return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static int frames_found;
static size_t want_len;
static const uint8_t *want_frame;
static void on_frame(const uint8_t *f, size_t n, void *ctx)
{
	(void)ctx;
	if (n == want_len && memcmp(f, want_frame, n) == 0) {
		frames_found++;
	}
}

// Modulate count copies of a frame at mod_rate, filter / add noise, demodulate at demod_rate (the rates differing is
// a clock error). Returns the frames decoded
static int afsk_run(uint32_t mod_rate, uint32_t demod_rate, float snr_db, float tilt, int count)
{
	static uint8_t frame[AX25_FRAME_MAX], tones[1024];
	static int16_t pcm[400000];
	size_t n = make_frame("K1ABC-9>APZODR,WIDE1-1,WIDE2-1:=4903.50N/07201.75W>OpenDigitalRadio APRS test 0123456789",
			      frame, sizeof(frame));
	hdlc_enc_t e;
	hdlc_enc_init(&e, tones, sizeof(tones));
	hdlc_enc_flags(&e, 30);
	hdlc_enc_frame(&e, frame, n);
	hdlc_enc_flags(&e, 3);
	afsk_demod_t d;
	if (!afsk_demod_init(&d, demod_rate, on_frame, NULL)) {
		return -1;
	}
	frames_found = 0;
	want_frame = frame;
	want_len = n;
	float sigma = powf(10.0f, -snr_db / 20.0f) * 0.5f * 0.7071f;	// tone RMS 0.35 at amplitude 0.5
	float y = 0.0f;
	for (int c = 0; c < count; c++) {
		afsk_mod_t m;
		afsk_mod_init(&m, mod_rate);
		size_t k = afsk_mod(&m, tones, e.nbits, 16384, pcm, sizeof(pcm) / sizeof(pcm[0]));
		// a gap of noise with a random length before each frame (random bit phase)
		size_t gap = 50 + (size_t)(rng % 300u);
		rng = rng * 1103515245u + 12345u;
		static int16_t out[400000];
		size_t o = 0;
		for (size_t i = 0; i < gap + k; i++) {
			float x = i < gap ? 0.0f : pcm[i - gap] / 32768.0f;
			// tilt: a one pole low pass mixed in (tilt 0 = flat); it attenuates 2200 Hz more than 1200 Hz like the
			// de-emphasis of a receiver after a transmitter without pre-emphasis
			y += 0.35f * (x - y);
			x = (1.0f - tilt) * x + tilt * y * 1.6f;
			x += sigma * gauss();
			float v = x * 32768.0f;
			out[o++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		afsk_demod(&d, out, o);
	}
	return frames_found;
}

static void test_afsk(void)
{
	int n = afsk_run(8000, 8000, 40.0f, 0.0f, 20);
	CHECK(n == 20, "clean 8 kHz: %d of 20", n);
	n = afsk_run(8000, 8000, 12.0f, 0.0f, 50);
	CHECK(n >= 47, "noisy 8 kHz (12 dB): %d of 50", n);
	n = afsk_run(8000, 8000, 20.0f, 0.8f, 50);
	CHECK(n >= 47, "tilted 8 kHz: %d of 50", n);
	n = afsk_run(8016, 8000, 20.0f, 0.0f, 50);
	CHECK(n >= 47, "clock 0.2 %% fast: %d of 50", n);
	n = afsk_run(7984, 8000, 20.0f, 0.0f, 50);
	CHECK(n >= 47, "clock 0.2 %% slow: %d of 50", n);
	n = afsk_run(8040, 8000, 20.0f, 0.0f, 50);
	CHECK(n >= 47, "clock 0.5 %% fast: %d of 50", n);
	n = afsk_run(48000, 48000, 12.0f, 0.5f, 30);
	CHECK(n >= 28, "48 kHz: %d of 30", n);
	n = afsk_run(9600, 9600, 15.0f, 0.3f, 30);
	CHECK(n >= 28, "9.6 kHz: %d of 30", n);
	afsk_demod_t d;
	CHECK(!afsk_demod_init(&d, 6000, NULL, NULL), "rate too low");
}

// --- APRS formats ---

static void test_position_build(void)
{
	char out[128];
	aprs_report_t r;
	memset(&r, 0, sizeof(r));
	// APRS101 chapter 8 example: 49 03.50 N, 072 01.75 W, symbol /-
	r.pos.lat = 490583333;
	r.pos.lon = -720291667;
	r.pos.course = APRS_COURSE_NONE;
	r.pos.speed_kn = APRS_SPEED_NONE;
	r.pos.alt_m = APRS_ALT_NONE;
	r.pos.sym_table = '/';
	r.pos.sym_code = '-';
	aprs_build_position(&r, out, sizeof(out));
	CHECK(strcmp(out, "!4903.50N/07201.75W-") == 0, "uncompressed %s", out);
	r.messaging = true;
	r.pos.course = 88;
	r.pos.speed_kn = 36;
	r.pos.alt_m = 1000;	// 3281 ft
	r.comment = "Hello";
	aprs_build_position(&r, out, sizeof(out));
	CHECK(strcmp(out, "=4903.50N/07201.75W-088/036/A=003281Hello") == 0, "with extension %s", out);
	r.ambiguity = 2;
	r.pos.alt_m = APRS_ALT_NONE;
	r.comment = NULL;
	aprs_build_position(&r, out, sizeof(out));
	CHECK(strcmp(out, "=4903.  N/07201.  W-088/036") == 0, "ambiguity %s", out);
	// APRS101 chapter 9 worked example: 49 30 N, 072 45 W, course 88, speed 36.2 kn -> "/5L!!<*e7>7P"
	r.ambiguity = 0;
	r.compressed = true;
	r.pos.lat = 495000000;
	r.pos.lon = -727500000;
	r.pos.sym_code = '>';
	aprs_build_position(&r, out, sizeof(out));
	CHECK(strncmp(out, "=/5L!!<*e7>7P", 13) == 0 && strlen(out) == 14, "compressed %s", out);
	// south / east
	r.compressed = false;
	r.pos.lat = -338688000;		// 33 52.13 S (Sydney)
	r.pos.lon = 1512093000;		// 151 12.56 E
	r.pos.course = APRS_COURSE_NONE;
	aprs_build_position(&r, out, sizeof(out));
	CHECK(strcmp(out, "=3352.13S/15112.56E>") == 0, "south east %s", out);
	// status, message, ack
	aprs_build_status("On the air", out, sizeof(out));
	CHECK(strcmp(out, ">On the air") == 0, "status");
	aprs_build_message("K1ABC-9", "Hi {there}|", "42", out, sizeof(out));
	CHECK(strcmp(out, ":K1ABC-9  :Hi  there} {42") == 0, "message %s", out);
	aprs_build_ack("K1ABC-9", "42", false, out, sizeof(out));
	CHECK(strcmp(out, ":K1ABC-9  :ack42") == 0, "ack %s", out);
	CHECK(aprs_build_status("x", out, 2) == 0, "too small");
}

static void test_position_parse(void)
{
	aprs_parsed_t p;
	ax25_addr_t dst = addr("APRS");
	const char *s = "=4903.50N/07201.75W-088/036/A=003281Hello";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.type == APRS_T_POSITION && p.messaging, "type");
	CHECK(p.has_pos && llabs(p.pos.lat - 490583333) < 2000 && llabs(p.pos.lon + 720291667) < 2000, "lat lon %d %d",
	      p.pos.lat, p.pos.lon);
	CHECK(p.pos.course == 88 && p.pos.speed_kn == 36 && p.pos.alt_m == 1000, "ext %u %u %d", p.pos.course,
	      p.pos.speed_kn, p.pos.alt_m);
	CHECK(p.text_len == 14 && memcmp(p.text, "/A=003281Hello", 14) == 0, "comment");
	// with a timestamp
	s = "@092345z4903.50N/07201.75W>";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.has_pos && p.pos.sym_code == '>', "timestamp");
	// compressed (the chapter 9 example)
	s = "=/5L!!<*e7>7P[";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.has_pos, "compressed");
	CHECK(llabs(p.pos.lat - 495000000) < 300 && llabs(p.pos.lon + 727500000) < 300, "compressed pos %d %d",
	      p.pos.lat, p.pos.lon);
	CHECK(p.pos.course == 88 && p.pos.speed_kn == 36, "compressed cs %u %u", p.pos.course, p.pos.speed_kn);
	// compressed altitude (T with the GGA source): cs = "S]" -> 1.002^((50 * 91) + 60) ft = 10004 ft
	s = "!/5L!!<*e7>S]S";	// T = 'S' = 50: bits 4-3 = 10
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.pos.alt_m > 3040 && p.pos.alt_m < 3060,
	      "compressed altitude %d", p.pos.alt_m);
	// object and item
	s = ";LEADER   *092345z4903.50N/07201.75W>088/036";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.type == APRS_T_OBJECT &&
	      strcmp(p.name, "LEADER") == 0 && !p.killed && p.pos.course == 88, "object %s", p.name);
	s = ")AID #2!4903.50N/07201.75WA";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.type == APRS_T_ITEM &&
	      strcmp(p.name, "AID #2") == 0, "item %s", p.name);
	// weather: the extension is wind
	s = "!4903.50N/07201.75W_220/004g005t077";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.type == APRS_T_WEATHER &&
	      p.pos.course == APRS_COURSE_NONE, "weather");
	// junk
	s = "!49x3.50N/07201.75W-";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.type == APRS_T_UNKNOWN, "junk");
}

// Mic-E by hand from APRS101 chapter 10: 33 25.64 N, 112 07.74 W, 20 kn, course 251, M0 (off duty), symbol /j.
// Destination: digits 3 3 2 5 6 4; message bits 1 1 1 (standard: P-Y), north (P-Y), longitude offset (P-Y), west (P-Y)
// -> "SSRUVT". Information: '`', lon 112 - 100 = 12 + 28 = '(', minutes 7 + 88 = '_', hundredths 74 + 28 = 'f',
// speed tens 2 + 80 + 28 = 'n', units 0 x 10 + course hundreds 2 + 4 + 28 = '"', course 51 + 28 = 'O', 'j', '/'
static void test_mice(void)
{
	aprs_parsed_t p;
	ax25_addr_t dst = addr("SSRUVT");
	const char *s = "`(_fn\"Oj/";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.type == APRS_T_MICE && p.has_pos, "mic-e");
	int32_t lat = 330000000 + (int32_t)(2564LL * APRS_DEG / 6000), lon = -(1120000000 + (int32_t)(774LL * APRS_DEG / 6000));
	CHECK(llabs(p.pos.lat - lat) < 100 && llabs(p.pos.lon - lon) < 100, "mic-e pos %d %d", p.pos.lat, p.pos.lon);
	CHECK(p.pos.speed_kn == 20 && p.pos.course == 251, "mic-e speed %u course %u", p.pos.speed_kn, p.pos.course);
	CHECK(p.pos.sym_code == 'j' && p.pos.sym_table == '/' && p.mice_msg == 0, "mic-e sym / msg %u", p.mice_msg);
	// south, east, no offset (lon 12), emergency (000 = all 0-9), altitude "4T}" with a ']' radio type
	dst = addr("332564");
	s = "`(_fn\"Oj/]\"4T}hello";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, strlen(s), &p) && p.has_pos, "mic-e 2");
	CHECK(p.pos.lat < 0 && p.pos.lon > 0 && p.pos.lon / APRS_DEG == 12, "mic-e hemispheres %d %d", p.pos.lat,
	      p.pos.lon);
	CHECK(p.mice_msg == 7, "emergency %u", p.mice_msg);
	// '"' = 1, '4' = 19, 'T' = 51: (1 x 91 + 19) x 91 + 51 - 10000 = 61 m
	CHECK(p.pos.alt_m == (1 * 91 + 19) * 91 + 51 - 10000, "mic-e altitude %d", p.pos.alt_m);
	CHECK(p.text_len == 5 && memcmp(p.text, "hello", 5) == 0, "mic-e comment");
	// custom message (A-K in the first three)
	dst = addr("A32U6T");
	CHECK(aprs_parse(&dst, (const uint8_t *)"`(_fn\"Oj/", 9, &p) && p.mice_msg >= 8, "custom %u", p.mice_msg);
	dst = addr("APRS");
	CHECK(aprs_parse(&dst, (const uint8_t *)"`(_fn\"Oj/", 9, &p) && p.type == APRS_T_UNKNOWN, "bad mic-e dest");
}

static void test_messages(void)
{
	aprs_parsed_t p;
	const char *s = ":N0CALL-7 :Hello there{123";
	CHECK(aprs_parse(NULL, (const uint8_t *)s, strlen(s), &p) && p.type == APRS_T_MESSAGE, "message");
	CHECK(strcmp(p.addressee, "N0CALL-7") == 0 && strcmp(p.msgno, "123") == 0 && p.text_len == 11 &&
	      memcmp(p.text, "Hello there", 11) == 0 && !p.ack, "fields");
	s = ":N0CALL   :ack123";
	CHECK(aprs_parse(NULL, (const uint8_t *)s, strlen(s), &p) && p.ack && strcmp(p.msgno, "123") == 0, "ack");
	s = ":N0CALL   :rej7";
	CHECK(aprs_parse(NULL, (const uint8_t *)s, strlen(s), &p) && p.rej && strcmp(p.msgno, "7") == 0, "rej");
	// 1.1 reply-ack: "{MM}AA"
	s = ":N0CALL   :Hi{AB}CD";
	CHECK(aprs_parse(NULL, (const uint8_t *)s, strlen(s), &p) && strcmp(p.msgno, "AB") == 0 && p.text_len == 2,
	      "reply-ack %s", p.msgno);
	s = ":BLN1     :Bulletin";
	CHECK(aprs_parse(NULL, (const uint8_t *)s, strlen(s), &p) && strcmp(p.addressee, "BLN1") == 0 &&
	      p.msgno[0] == '\0', "bulletin");
	s = ">Status text";
	CHECK(aprs_parse(NULL, (const uint8_t *)s, strlen(s), &p) && p.type == APRS_T_STATUS && p.text_len == 11,
	      "status");
}

static void test_geo(void)
{
	// one degree of latitude: 111.2 km
	uint32_t d = aprs_distance_m(0, 0, APRS_DEG, 0);
	CHECK(d > 111100 && d < 111300, "1 deg %u", d);
	CHECK(aprs_bearing_deg(0, 0, APRS_DEG, 0) == 0, "north");
	CHECK(aprs_bearing_deg(0, 0, 0, APRS_DEG) == 90, "east");
	CHECK(aprs_bearing_deg(0, 0, -APRS_DEG, 0) == 180, "south");
	CHECK(aprs_bearing_deg(0, 0, 0, -APRS_DEG) == 270, "west");
	// across the date line: 179.5 E to 179.5 W is 1 degree of longitude on the equator
	d = aprs_distance_m(0, 1795000000, 0, -1795000000);
	CHECK(d > 111100 && d < 111300, "date line %u", d);
	char b[4];
	aprs_base91(15427503, 4, b);
	CHECK(memcmp(b, "5L!!", 4) == 0 && aprs_base91_value("5L!!", 4) == 15427503, "base91");
}

// --- NMEA ---

static void test_nmea(void)
{
	nmea_fix_t f;
	const char *rmc = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n";
	CHECK(nmea_parse(rmc, strlen(rmc), &f) && f.kind == NMEA_RMC && f.fix, "rmc");
	CHECK(f.lat == 481173000 && f.lon == 115166666, "rmc pos %d %d", f.lat, f.lon);
	CHECK(f.speed_kn10 == 224 && f.course10 == 844 && f.date == 230394 && f.utc_hms == 123519, "rmc fields");
	const char *gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
	CHECK(nmea_parse(gga, strlen(gga), &f) && f.kind == NMEA_GGA && f.fix && f.sats == 8 && f.alt_dm == 5454,
	      "gga %d", f.alt_dm);
	const char *bad = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6B";
	CHECK(!nmea_parse(bad, strlen(bad), &f), "bad checksum");
	const char *nofix = "$GNRMC,000000,V,,,,,,,010120,,,N*4F";
	bool ok = nmea_parse(nofix, strlen(nofix), &f);
	CHECK(ok && !f.fix, "no fix (ok=%d)", ok);
	const char *gsv = "$GPGSV,1,1,00*79";
	CHECK(!nmea_parse(gsv, strlen(gsv), &f), "other sentence");
	// west / south, a long longitude
	const char *ws = "$GPRMC,010203,A,3352.1300,S,15112.5600,W,0.0,,010125,,*3B";
	ok = nmea_parse(ws, strlen(ws), &f);
	CHECK(ok && f.lat < 0 && f.lon < -1500000000, "south west ok=%d %d %d", ok, f.lat, f.lon);
}

// --- KISS ---

static void test_kiss(void)
{
	uint8_t data[] = { 0x01, KISS_FEND, 0x02, KISS_FESC, 0x03 };
	uint8_t out[32];
	size_t n = kiss_encode(0x00, data, sizeof(data), out, sizeof(out));
	static const uint8_t want[] = { 0xC0, 0x00, 0x01, 0xDB, 0xDC, 0x02, 0xDB, 0xDD, 0x03, 0xC0 };
	CHECK(n == sizeof(want) && memcmp(out, want, n) == 0, "encode %zu", n);
	kiss_dec_t k;
	kiss_dec_init(&k);
	size_t got = 0;
	kiss_dec_byte(&k, 0x55);	// junk before the first FEND
	for (size_t i = 0; i < n; i++) {
		size_t r = kiss_dec_byte(&k, out[i]);
		if (r) {
			got = r;
		}
	}
	CHECK(got == 6 && k.buf[0] == 0 && memcmp(k.buf + 1, data, 5) == 0, "decode %zu", got);
	// back to back frames sharing a FEND, and an empty frame
	static const uint8_t two[] = { 0xC0, 0x01, 0x1E, 0xC0, 0xC0, 0x02, 0x40, 0xC0 };
	int frames = 0;
	for (size_t i = 0; i < sizeof(two); i++) {
		if (kiss_dec_byte(&k, two[i])) {
			frames++;
		}
	}
	CHECK(frames == 2, "two frames %d", frames);
	CHECK(kiss_encode(0, data, sizeof(data), out, 8) == 0, "too small");
}

// --- SmartBeaconing and triggers ---

static aprs_beacon_cfg_t cfg_default(void)
{
	aprs_beacon_cfg_t c;
	memset(&c, 0, sizeof(c));
	c.auto_on = true;
	c.interval_s = 600;
	c.smart = true;
	c.fast_kmh = 90;
	c.slow_kmh = 5;
	c.fast_s = 180;
	c.slow_s = 1800;
	c.turn_deg = 28;
	c.turn_slope = 400;
	c.turn_s = 30;
	c.min_s = 30;
	c.ptt_gap_s = 300;
	return c;
}

static void test_beacon(void)
{
	aprs_beacon_cfg_t c = cfg_default();
	CHECK(aprs_smart_rate_s(&c, 0) == 1800 && aprs_smart_rate_s(&c, 5) == 1800, "slow");
	CHECK(aprs_smart_rate_s(&c, 90) == 180 && aprs_smart_rate_s(&c, 130) == 180, "fast");
	CHECK(aprs_smart_rate_s(&c, 45) == 360, "45 km/h %u", aprs_smart_rate_s(&c, 45));
	CHECK(aprs_smart_turn_deg(&c, 20) == 48, "turn at 20 %u", aprs_smart_turn_deg(&c, 20));

	aprs_beacon_t b;
	aprs_beacon_pos_t pos = { true, 400000000, -740000000, 45, 90 };
	uint32_t t = 1000;
	aprs_beacon_init(&b, &c, t);
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 300000) == APRS_WHY_NONE, "not yet at 300 s");
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 360000) == APRS_WHY_SMART, "smart at 360 s");
	t += 360000;
	aprs_beacon_sent(&b, APRS_WHY_SMART, &pos, t);
	// a 60 degree turn at 45 km/h (threshold 28 + 400 / 45 = 36), after the turn time
	pos.course = 150;
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 20000) == APRS_WHY_NONE, "turn too soon (min 30 s)");
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 31000) == APRS_WHY_TURN, "turn");
	// a small turn: nothing
	aprs_beacon_sent(&b, APRS_WHY_TURN, &pos, t + 31000);
	t += 31000;
	pos.course = 170;
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 60000) == APRS_WHY_NONE, "20 deg is no turn");
	// across north: 350 -> 30 is 40 degrees
	b.last_course = 350;
	pos.course = 30;
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 60000) == APRS_WHY_TURN, "turn across north");
	// stopped: no turn beacons, slow rate
	pos.speed_kmh = 2;
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 60000) == APRS_WHY_NONE, "stopped");
	// fixed interval when SmartBeaconing is off, and the hard minimum
	c.smart = false;
	c.interval_s = 5;	// below the minimum
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 20000) == APRS_WHY_NONE, "min interval");
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 30000) == APRS_WHY_INTERVAL, "interval after min");
	c.min_s = 0;
	c.interval_s = 1;
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 9000) == APRS_WHY_NONE, "floor 10 s");
	// auto off: nothing but manual
	c = cfg_default();
	c.auto_on = false;
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 4000000) == APRS_WHY_NONE, "auto off");
	aprs_beacon_manual(&b, t);
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 1) == APRS_WHY_MANUAL, "manual");
	aprs_beacon_sent(&b, APRS_WHY_MANUAL, &pos, t + 1);
	aprs_beacon_manual(&b, t + 2000);
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 2000) == APRS_WHY_NONE, "manual floor");
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 6000) == APRS_WHY_MANUAL, "manual after floor");
	aprs_beacon_sent(&b, APRS_WHY_MANUAL, &pos, t + 6000);
	t += 6000;
	// after PTT: dropped when the last beacon is recent, sent after the settle time otherwise
	c = cfg_default();
	c.smart = false;
	c.interval_s = 0;
	c.after_ptt = true;
	c.ptt_gap_s = 120;
	aprs_beacon_ptt_end(&b, &c, t + 60000);
	CHECK(!b.ptt_end, "ptt too soon dropped");
	aprs_beacon_ptt_end(&b, &c, t + 130000);
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 130500) == APRS_WHY_NONE, "ptt settle");
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 131600) == APRS_WHY_PTT, "ptt");
	aprs_beacon_sent(&b, APRS_WHY_PTT, &pos, t + 131600);
	t += 131600;
	// distance
	c.after_ptt = false;
	c.distance_m = 500;
	pos.lat += 40000;	// about 445 m north
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 60000) == APRS_WHY_NONE, "445 m");
	pos.lat += 10000;	// 556 m
	CHECK(aprs_beacon_due(&b, &c, &pos, t + 60000) == APRS_WHY_DISTANCE, "556 m");
	// boot: the first valid position at once, even before the minimum
	c = cfg_default();
	c.boot = true;
	aprs_beacon_init(&b, &c, 0);
	pos.valid = false;
	CHECK(aprs_beacon_due(&b, &c, &pos, 1000) == APRS_WHY_NONE, "boot without position");
	pos.valid = true;
	CHECK(aprs_beacon_due(&b, &c, &pos, 2000) == APRS_WHY_BOOT, "boot");
	aprs_beacon_sent(&b, APRS_WHY_BOOT, &pos, 2000);
	CHECK(aprs_beacon_due(&b, &c, &pos, 3000) == APRS_WHY_NONE, "boot once");
	CHECK(aprs_beacon_next_s(&b, &c, &pos, 3000) != 0xFFFF, "next planned");
}

// --- iGate and digipeater ---

static bool fake_direct(const char *call, uint8_t ssid, uint32_t secs, void *ctx)
{
	(void)secs;
	(void)ctx;
	return strcmp(call, "K1ABC") == 0 && ssid == 9;
}

static bool fake_rf(const char *call, uint8_t ssid, uint32_t secs, void *ctx)
{
	(void)secs;
	(void)ctx;
	return (strcmp(call, "K1ABC") == 0 && ssid == 9) || strcmp(call, "W2RF") == 0;
}

static ax25_frame_t parse_line(const char *line)
{
	ax25_frame_t f;
	if (!ax25_from_tnc2(line, strlen(line), &f)) {
		printf("bad line %s\n", line);
		exit(2);
	}
	return f;
}

static void test_igate(void)
{
	aprs_dedup_t d;
	memset(&d, 0, sizeof(d));
	ax25_addr_t me = addr("N0CALL-10");
	char out[AX25_TNC2_MAX + 20];
	ax25_frame_t f = parse_line("K1ABC-9>APZODR,W1XYZ-1*,WIDE2-1:!4903.50N/07201.75W-Test\r");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 1000, out, sizeof(out)) == APRS_GATE_OK, "gate");
	CHECK(strcmp(out, "K1ABC-9>APZODR,W1XYZ-1*,WIDE2-1,qAR,N0CALL-10:!4903.50N/07201.75W-Test") == 0, "line %s", out);
	// the same packet over another digipeater within 30 s: once only; after 30 s again
	f = parse_line("K1ABC-9>APZODR,W1ZZZ*,WIDE2*:!4903.50N/07201.75W-Test");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 5000, out, sizeof(out)) == APRS_GATE_DUP, "dup");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 32000, out, sizeof(out)) == APRS_GATE_OK, "after 30 s");
	f = parse_line("K1ABC-9>APZODR,RFONLY:>local");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 40000, out, sizeof(out)) == APRS_GATE_NO_PATH, "rfonly");
	f = parse_line("K1ABC-9>APZODR,NOGATE:>local");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 40000, out, sizeof(out)) == APRS_GATE_NO_PATH, "nogate");
	f = parse_line("K1ABC-9>APZODR:?APRS?");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 40000, out, sizeof(out)) == APRS_GATE_QUERY, "query");
	// third party: from the Internet (TCPIP inside) stays; from RF elsewhere is gated as the inner packet
	f = parse_line("W1GATE>APRS,WIDE1-1:}K9XYZ>APRS,TCPIP,W1GATE*::K1ABC-9  :hi{1");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 40000, out, sizeof(out)) == APRS_GATE_FROM_IS, "third party from IS");
	f = parse_line("W1GATE>APRS,WIDE1-1:}K9XYZ>APRS,W9DIGI*:>far away");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 40000, out, sizeof(out)) == APRS_GATE_OK &&
	      strcmp(out, "K9XYZ>APRS,W9DIGI*,qAR,N0CALL-10:>far away") == 0, "third party inner %s", out);
	// the information field ends at a CR in the middle
	f = parse_line("K1ABC-9>APZODR:>one\rtwo");
	CHECK(aprs_igate_rf_to_is(&d, &f, &me, 41000, out, sizeof(out)) == APRS_GATE_OK &&
	      strcmp(out, "K1ABC-9>APZODR,qAR,N0CALL-10:>one") == 0, "cr %s", out);

	// IS -> RF
	aprs_igate_env_t env = { fake_direct, fake_rf, NULL };
	char info[AX25_INFO_MAX + 1];
	size_t len;
	const char *msg = "K9XYZ>APRS,TCPIP*,qAC,T2TEXAS::K1ABC-9  :Hello{12";
	CHECK(aprs_igate_is_to_rf(&d, msg, strlen(msg), &me, &env, 50000, info, sizeof(info), &len), "is to rf");
	CHECK(strcmp(info, "}K9XYZ>APRS,TCPIP,N0CALL-10*::K1ABC-9  :Hello{12") == 0 && len == strlen(info), "%s", info);
	CHECK(!aprs_igate_is_to_rf(&d, msg, strlen(msg), &me, &env, 52000, info, sizeof(info), &len), "is dup");
	const char *far = "K9XYZ>APRS,TCPIP*,qAC,T2TEXAS::W5FAR    :Hello{13";
	CHECK(!aprs_igate_is_to_rf(&d, far, strlen(far), &me, &env, 53000, info, sizeof(info), &len), "not local");
	const char *pos = "K9XYZ>APRS,TCPIP*,qAC,T2TEXAS:!4903.50N/07201.75W-";
	CHECK(!aprs_igate_is_to_rf(&d, pos, strlen(pos), &me, &env, 53000, info, sizeof(info), &len), "not a message");
	const char *rfs = "W2RF>APRS,TCPIP*,qAC,T2TEXAS::K1ABC-9  :Hi{14";
	CHECK(!aprs_igate_is_to_rf(&d, rfs, strlen(rfs), &me, &env, 53000, info, sizeof(info), &len), "sender on RF");
	const char *cmt = "# aprsc 2.1.14";
	CHECK(!aprs_igate_is_to_rf(&d, cmt, strlen(cmt), &me, &env, 53000, info, sizeof(info), &len), "comment");
}

static void test_digi(void)
{
	aprs_dedup_t d;
	memset(&d, 0, sizeof(d));
	ax25_addr_t me = addr("N0CALL-10");
	ax25_frame_t in, out;
	char s[AX25_TNC2_MAX];
	in = parse_line("K1ABC-9>APZODR,WIDE1-1,WIDE2-1:>hi");
	CHECK(aprs_digi(&d, &in, &out, &me, APRS_DIGI_FILL_IN, 1000), "fill-in");
	ax25_to_tnc2(&out, s, sizeof(s));
	CHECK(strcmp(s, "K1ABC-9>APZODR,N0CALL-10,WIDE1*,WIDE2-1:>hi") == 0, "fill-in path %s", s);
	CHECK(!aprs_digi(&d, &in, &out, &me, APRS_DIGI_FILL_IN, 2000), "dup");
	// WIDE2-2 after a first hop: only in WIDE mode
	in = parse_line("K1ABC-9>APZODR,W1XYZ*,WIDE2-2:>two");
	CHECK(!aprs_digi(&d, &in, &out, &me, APRS_DIGI_FILL_IN, 3000), "fill-in leaves WIDE2");
	memset(&d, 0, sizeof(d));
	CHECK(aprs_digi(&d, &in, &out, &me, APRS_DIGI_WIDE, 3000), "wide");
	ax25_to_tnc2(&out, s, sizeof(s));
	CHECK(strcmp(s, "K1ABC-9>APZODR,W1XYZ,N0CALL-10*,WIDE2-1:>two") == 0, "wide path %s", s);
	// directed at us
	in = parse_line("K1ABC-9>APZODR,N0CALL-10:>direct");
	CHECK(aprs_digi(&d, &in, &out, &me, APRS_DIGI_FILL_IN, 4000) && out.digi[0].h, "direct");
	// used up, our own, WIDE3-3
	in = parse_line("K1ABC-9>APZODR,WIDE1*,WIDE2*:>done");
	CHECK(!aprs_digi(&d, &in, &out, &me, APRS_DIGI_WIDE, 5000), "used up");
	in = parse_line("N0CALL-10>APZODR,WIDE1-1:>mine");
	CHECK(!aprs_digi(&d, &in, &out, &me, APRS_DIGI_WIDE, 5000), "own");
	in = parse_line("K1ABC-9>APZODR,WIDE3-3:>far");
	CHECK(!aprs_digi(&d, &in, &out, &me, APRS_DIGI_WIDE, 5000), "WIDE3-3");
	CHECK(!aprs_digi(&d, &in, &out, &me, APRS_DIGI_OFF, 5000), "off");
}

// --- WAV files for an external decoder (black box check, tests/host/aprs_wav_check.sh) ---

static void wav_u32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void wav_u16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

static int write_wav(const char *dir)
{
	static const char *const lines[] = {
		"N0CALL-7>APZODR,WIDE1-1,WIDE2-1:=4903.50N/07201.75W-OpenDigitalRadio test 1",
		"N0CALL-7>APZODR,WIDE1-1:=/5L!!<*e7>7PCOpenDigitalRadio test 2",
		"N0CALL-7>APZODR::K1ABC-9  :Hello from the host test{42",
	};
	static int16_t pcm[100000];
	static uint8_t frame[AX25_FRAME_MAX], tones[1024];
	char path[512];
	snprintf(path, sizeof(path), "%s/aprs_test_44k1.wav", dir);
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		perror(path);
		return 1;
	}
	size_t total = 0;
	fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, f);
	wav_u32(f, 16);
	wav_u16(f, 1);
	wav_u16(f, 1);
	wav_u32(f, 44100);
	wav_u32(f, 88200);
	wav_u16(f, 2);
	wav_u16(f, 16);
	fwrite("data\0\0\0\0", 1, 8, f);
	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		size_t n = make_frame(lines[i], frame, sizeof(frame));
		hdlc_enc_t e;
		hdlc_enc_init(&e, tones, sizeof(tones));
		hdlc_enc_flags(&e, 40);
		hdlc_enc_frame(&e, frame, n);
		hdlc_enc_flags(&e, 3);
		afsk_mod_t m;
		afsk_mod_init(&m, 44100);
		size_t k = afsk_mod(&m, tones, e.nbits, 16000, pcm, sizeof(pcm) / sizeof(pcm[0]));
		for (size_t j = 0; j < k; j++) {
			wav_u16(f, (uint16_t)pcm[j]);
		}
		for (int j = 0; j < 22050; j++) {
			wav_u16(f, 0);		// half a second of silence
		}
		total += k + 22050;
	}
	fseek(f, 4, SEEK_SET);
	wav_u32(f, (uint32_t)(36 + total * 2));
	fseek(f, 40, SEEK_SET);
	wav_u32(f, (uint32_t)(total * 2));
	fclose(f);
	printf("%s: %zu frames\n", path, sizeof(lines) / sizeof(lines[0]));
	return 0;
}

// Compressed positions whose course / speed / altitude bytes are outside base-91 (APRS101 chapter 9: '!' to '{'):
// no course, speed or altitude from them
static void test_compressed_cs(void)
{
	aprs_parsed_t p;
	ax25_addr_t dst = addr("APRS");
	const char *s = "=/5L!!<*e7>\xFF\xFF\x40";
	CHECK(aprs_parse(&dst, (const uint8_t *)s, 14, &p) && p.has_pos && p.pos.speed_kn == APRS_SPEED_NONE &&
	      p.pos.course == APRS_COURSE_NONE, "cs 0xFF: %u %u", p.pos.speed_kn, p.pos.course);
	s = "=/5L!!<*e7>\xFFSS";	// GGA altitude from c = 0xFF
	CHECK(aprs_parse(&dst, (const uint8_t *)s, 14, &p) && p.has_pos && p.pos.alt_m == APRS_ALT_NONE, "alt 0xFF: %d",
	      p.pos.alt_m);
	s = "=/5L!!<*e7>7\x90[";	// s past '{'
	CHECK(aprs_parse(&dst, (const uint8_t *)s, 14, &p) && p.has_pos && p.pos.speed_kn == APRS_SPEED_NONE, "s 0x90: %u",
	      p.pos.speed_kn);
}


// What's heard over the air at the edges of the formats, parsed from exact-size heap copies (ASan sees a read past
// the end): compressed positions whose course / speed / altitude bytes are outside base-91 (APRS101 chapter 9: '!'
// to '{'), every truncation of each format, Mic-E with and without the altitude and type characters, messages at
// the field limits
static bool parse_exact(const ax25_addr_t *dst, const char *s, size_t n, aprs_parsed_t *p)
{
	uint8_t *q = fuzz_exact((const uint8_t *)s, (int)n);
	bool ok = aprs_parse(dst, q, n, p);
	// text inside the info field, whatever the type
	if (ok && p->text_len > 0 && ((const uint8_t *)p->text < q || (const uint8_t *)p->text + p->text_len > q + n)) {
		ok = false;
	}
	free(q);
	return ok;
}

static void test_parse_edges(void)
{
	aprs_parsed_t p;
	ax25_addr_t dst = addr("APRS"), mice = addr("SSRUVT");
	char s[64];

	// the largest base-91 values are taken (test_compressed_cs has the bytes past '{')
	strcpy(s, "=/5L!!<*e7>{{[");
	CHECK(parse_exact(&dst, s, 14, &p) && p.has_pos && p.pos.course == APRS_COURSE_NONE, "c '{'");

	// every truncation of each format: parsed or refused, never read past
	static const char *const formats[] = {
		"=4903.50N/07201.75W-088/036/A=003281Hello", "@092345z4903.50N/07201.75W>", "=/5L!!<*e7>7P[comment",
		";LEADER   *092345z4903.50N/07201.75W>088/036", ")AID #2!4903.50N/07201.75WA", ":N0CALL-7 :hello{12}AB",
		":N0CALL   :ack123", "`(_fn\"Oj/`\"4T}text", ">status", "}N0CALL>APRS:>third",
	};
	int bad = 0;
	for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		size_t n = strlen(formats[f]);
		for (size_t k = 0; k <= n; k++) {
			bool ok = parse_exact(formats[f][0] == '`' ? &mice : &dst, formats[f], k, &p);
			bad += (k > 0) && !ok;
		}
	}
	CHECK(bad == 0, "truncations: %d not parsed", bad);

	// Mic-E: the altitude after the type character, the old place (bytes 9-12), none; a 5-character destination
	strcpy(s, "`(_fn\"Oj/`\"4T}");
	CHECK(parse_exact(&mice, s, 14, &p) && p.pos.alt_m != APRS_ALT_NONE && p.text_len == 0, "mic-e alt %d", p.pos.alt_m);
	strcpy(s, "`(_fn\"Oj/\"4T}x");
	CHECK(parse_exact(&mice, s, 14, &p) && p.pos.alt_m != APRS_ALT_NONE && p.text_len == 1, "mic-e alt 2");
	ax25_addr_t five = addr("SSRUV");
	CHECK(parse_exact(&five, s, 14, &p) && p.type == APRS_T_UNKNOWN, "5-char dest");
	CHECK(parse_exact(NULL, s, 14, &p) && p.type == APRS_T_UNKNOWN, "no dest");
	// latitude 91 degrees in the destination: refused
	ax25_addr_t lat91 = addr("YQRUVT");
	CHECK(parse_exact(&lat91, s, 14, &p) && p.type == APRS_T_UNKNOWN, "mic-e lat 91");

	// Messages: addressee all spaces, message number at its limit and past it, an ack with no number
	strcpy(s, ":         :x");
	CHECK(parse_exact(NULL, s, strlen(s), &p) && p.type == APRS_T_MESSAGE && p.addressee[0] == '\0', "blank addressee");
	strcpy(s, ":N0CALL   :hi{123456789");
	CHECK(parse_exact(NULL, s, strlen(s), &p) && strlen(p.msgno) == APRS_MSGNO_MAX && p.text_len == 2, "msgno max");
	strcpy(s, ":N0CALL   :ack");
	CHECK(parse_exact(NULL, s, strlen(s), &p) && !p.ack && p.text_len == 3, "ack without number");
	strcpy(s, ":N0CALL   :ack ");
	CHECK(parse_exact(NULL, s, strlen(s), &p) && !p.ack, "ack space");
	// objects: one short of the position, killed; items with names of 3 and 9 characters, 10 is too long
	strcpy(s, ";LEADER   _092345z4903.50N/07201.75W>");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.type == APRS_T_OBJECT && p.killed, "killed object");
	CHECK(parse_exact(&dst, s, 17, &p) && p.type == APRS_T_UNKNOWN, "short object");
	strcpy(s, ")ABC_4903.50N/07201.75WA");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.type == APRS_T_ITEM && p.killed && strcmp(p.name, "ABC") == 0, "item 3");
	strcpy(s, ")ABCDEFGHI!4903.50N/07201.75WA");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.type == APRS_T_ITEM && strcmp(p.name, "ABCDEFGHI") == 0, "item 9");
	strcpy(s, ")ABCDEFGHIJ!4903.50N/07201.75WA");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.type == APRS_T_UNKNOWN, "item 10");
	// an uncompressed position at the extremes, ambiguity, a bad table character
	strcpy(s, "!9000.00S/18000.00E-");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.pos.lat == -900000000 && p.pos.lon == 1800000000, "extremes");
	strcpy(s, "!9001.00N/18000.00E-");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.type == APRS_T_POSITION, "lat 90 01 taken as written");
	strcpy(s, "!49  .  N/072  .  W-");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.has_pos, "ambiguity");
	strcpy(s, "!4903.50N 07201.75W-");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.type == APRS_T_UNKNOWN, "table ' '");
	// "/A=" at the very end of a comment, and with a sign and five digits
	strcpy(s, "!4903.50N/07201.75W-/A=00");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.pos.alt_m == APRS_ALT_NONE, "cut altitude");
	strcpy(s, "!4903.50N/07201.75W-/A=-00100");
	CHECK(parse_exact(&dst, s, strlen(s), &p) && p.pos.alt_m == -30, "negative altitude %d", p.pos.alt_m);
}

// Mutated frames off the air: AX.25 decode, then the APRS parse of the info field and the TNC2 text, in exact-size
// buffers; whatever is decoded stays inside its buffers (ODR_FUZZ_ITERS)
static void test_parse_fuzz(void)
{
	static const char *const lines[] = {
		"K1ABC-9>APZODR,W1XYZ-1*,WIDE2-1:!4903.50N/07201.75W-088/036/A=003281Test",
		"N0CALL>APRS:=/5L!!<*e7>7P[compressed",
		"N0CALL>SSRUVT,WIDE1-1:`(_fn\"Oj/`\"4T}mic-e",
		"N0CALL>APRS::N0CALL-7 :hello{12}AB",
		"N0CALL>APRS:;LEADER   *092345z4903.50N/07201.75W>088/036",
		"N0CALL>APRS:)AID #2!4903.50N/07201.75WA",
		"N0CALL>APRS:}W1AW>APRS,TCPIP,N0CALL*:>third party",
		"N0CALL>APRS:@092345z4903.50N/07201.75W_220/004g005t077",
	};
	uint8_t seeds[8][AX25_FRAME_MAX];
	size_t seedLen[8];
	for (int i = 0; i < 8; i++) {
		seedLen[i] = make_frame(lines[i], seeds[i], sizeof(seeds[i]));
	}
	fuzz_t f;
	fuzz_init(&f, 0x41505253u);
	long iters = fuzz_iters(50000), decoded = 0, parsed = 0, bad = 0;
	uint8_t buf[AX25_FRAME_MAX + 64];
	for (long i = 0; i < iters; i++) {
		int s = (int)fuzz_below(&f, 8);
		memcpy(buf, seeds[s], seedLen[s]);
		int n = fuzz_mutate(&f, buf, (int)seedLen[s], (int)sizeof(buf));
		uint8_t *q = fuzz_exact(buf, n);
		ax25_frame_t fr;
		if (ax25_decode(q, (size_t)n, &fr)) {
			decoded++;
			if (fr.info_len > AX25_INFO_MAX || fr.ndigi > AX25_MAX_DIGIS) {
				bad++;
			}
			char tnc2[AX25_TNC2_MAX + 1];
			size_t t = ax25_to_tnc2(&fr, tnc2, sizeof(tnc2));
			if (t >= sizeof(tnc2)) {
				bad++;
			}
			aprs_parsed_t p;
			uint8_t *info = fuzz_exact(fr.info, (int)fr.info_len);
			if (aprs_parse(&fr.dst, info, fr.info_len, &p)) {
				parsed++;
				if (p.text_len > 0 && ((const uint8_t *)p.text < info || (const uint8_t *)p.text + p.text_len > info + fr.info_len)) {
					bad++;
				}
				if (p.name[APRS_NAME_MAX] != '\0' || p.addressee[APRS_ADDRESSEE_LEN] != '\0' || p.msgno[APRS_MSGNO_MAX] != '\0') {
					bad++;
				}
			}
			free(info);
		}
		free(q);
	}
	CHECK(bad == 0 && decoded > iters / 10, "fuzz: %ld bad, %ld decoded", bad, decoded);
	printf("# aprs fuzz: %ld frames, %ld decoded, %ld parsed\n", iters, decoded, parsed);
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--sweep") == 0) {
		// frames decoded of 100 against the SNR (noise over the full band), flat and tilted, 8 kHz
		for (int snr = 4; snr <= 20; snr += 2) {
			printf("%2d dB: flat %3d tilt %3d\n", snr, afsk_run(8000, 8000, (float)snr, 0.0f, 100),
			       afsk_run(8000, 8000, (float)snr, 0.8f, 100));
		}
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "--wav") == 0) {
		return write_wav(argv[2]);
	}
	test_fcs();
	test_addresses();
	test_hdlc();
	test_afsk();
	test_position_build();
	test_position_parse();
	test_mice();
	test_messages();
	test_compressed_cs();
	test_parse_edges();
	test_parse_fuzz();
	test_geo();
	test_nmea();
	test_kiss();
	test_beacon();
	test_igate();
	test_digi();

	printf("# aprsTest %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
