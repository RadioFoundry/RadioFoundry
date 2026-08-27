// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Morse timing (morse.h)
#include <string.h>
#include "radio/morse.h"

// ITU-R M.1677-1 characters: a leading 1 marker, then the elements MSB first (0 dot, 1 dash)
static const uint8_t letters[26] = {
	0x05, 0x18, 0x1A, 0x0C, 0x02, 0x12, 0x0E, 0x10, 0x04, 0x17, 0x0D, 0x14, 0x07,	// A-M
	0x06, 0x0F, 0x16, 0x1D, 0x0A, 0x08, 0x03, 0x09, 0x11, 0x0B, 0x19, 0x1B, 0x1C,	// N-Z
};
static const uint8_t digits[10] = { 0x3F, 0x2F, 0x27, 0x23, 0x21, 0x20, 0x30, 0x38, 0x3C, 0x3E };

uint16_t morse_dot_ms(uint8_t wpm)
{
	if (wpm < MORSE_WPM_MIN) {
		wpm = MORSE_WPM_MIN;
	} else if (wpm > MORSE_WPM_MAX) {
		wpm = MORSE_WPM_MAX;
	}
	return (uint16_t)(1200u / wpm);
}

uint8_t morse_code(char c)
{
	if (c >= 'a' && c <= 'z') {
		c = (char)(c - 'a' + 'A');
	}
	if (c >= 'A' && c <= 'Z') {
		return letters[c - 'A'];
	}
	if (c >= '0' && c <= '9') {
		return digits[c - '0'];
	}
	switch (c) {
	case '/': return 0x32;	// -..-.
	case '-': return 0x61;	// -....-
	case '=': return 0x31;	// -...-
	case '?': return 0x4C;	// ..--..
	case '.': return 0x55;	// .-.-.-
	case ',': return 0x73;	// --..--
	default: return 0;
	}
}

// Elements left in a code (the bits below the marker)
static bool has_elements(uint8_t code)
{
	return code > 1;
}

// The next element (true: dash), and the code without it
static bool take_element(uint8_t *code)
{
	uint8_t marker = 0x80;
	while ((*code & marker) == 0) {
		marker >>= 1;
	}
	uint8_t next = marker >> 1;
	bool dash = (*code & next) != 0;
	*code = (uint8_t)((*code & (next - 1u)) | next);	// the marker moves down past the element
	return dash;
}

// From character pos on: the next one with a code (its index, len if none) and whether a space came before it
static uint8_t next_char(const morse_t *m, uint8_t pos, bool *space)
{
	*space = false;
	while (pos < m->len) {
		if (m->text[pos] == ' ') {
			*space = true;
		} else if (morse_code(m->text[pos]) != 0) {
			return pos;
		}
		pos++;
	}
	return m->len;
}

// The next segment: key down or up for units; false when the text is over
static bool next_segment(morse_t *m, bool *key, uint8_t *units)
{
	if (m->done) {
		return false;
	}
	if (m->gap_due) {
		m->gap_due = false;
		if (has_elements(m->code)) {
			*key = false;
			*units = 1;
			return true;
		}
		bool space;
		uint8_t pos = next_char(m, (uint8_t)(m->pos + 1u), &space);
		if (pos >= m->len) {
			m->done = true;
			return false;
		}
		m->pos = pos;
		m->code = morse_code(m->text[pos]);
		*key = false;
		*units = space ? 7 : 3;
		return true;
	}
	*key = true;
	*units = take_element(&m->code) ? 3 : 1;
	m->gap_due = true;
	return true;
}

static void load(morse_t *m, const char *text, uint8_t len)
{
	memset(m, 0, sizeof(*m));
	if (len > MORSE_TEXT_MAX) {
		len = MORSE_TEXT_MAX;
	}
	memcpy(m->text, text, len);
	m->len = len;
	bool space;
	m->pos = next_char(m, 0, &space);
	if (m->pos >= m->len) {
		m->done = true;
		return;
	}
	m->code = morse_code(m->text[m->pos]);
}

uint32_t morse_units(const char *text, uint8_t len)
{
	morse_t m;
	bool key;
	uint8_t units;
	uint32_t total = 0;
	load(&m, text, len);
	while (next_segment(&m, &key, &units)) {
		total += units;
	}
	return total;
}

void morse_start(morse_t *m, const char *text, uint8_t len, uint8_t wpm, uint32_t now_ms)
{
	load(m, text, len);
	m->dot_ms = morse_dot_ms(wpm);
	m->until = now_ms;
}

bool morse_poll(morse_t *m, uint32_t now_ms)
{
	while (!m->done && (int32_t)(now_ms - m->until) >= 0) {
		bool key;
		uint8_t units;
		if (!next_segment(m, &key, &units)) {
			break;
		}
		m->key = key;
		m->units += units;
		m->until += (uint32_t)units * m->dot_ms;
	}
	if (m->done) {
		m->key = false;
	}
	return m->key;
}
