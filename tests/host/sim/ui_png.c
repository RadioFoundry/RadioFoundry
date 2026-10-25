// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The simulator's frame buffer as a PNG (zlib-compressed; 1 bpp panels as a 2-color palette image, RGB565 as RGB),
// and contact sheets of many screens as one palette PNG
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "ui_sim.h"
#include "ui/gfx.h"
#include "ui/font.h"

static void be32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
	unsigned char hdr[8];
	be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (len)
		fwrite(data, 1, len, f);
	uLong crc = crc32(0, (const Bytef *)type, 4);
	if (len)
		crc = crc32(crc, data, len);
	unsigned char c[4];
	be32(c, (uint32_t)crc);
	fwrite(c, 1, 4, f);
}

// The safe area (what is seen through the lens: the board's insets left out), scaled by an integer
bool sim_save_png(const char *path, int scale)
{
	int vw = gfx_width(), vh = gfx_height();
	if (scale < 1)
		scale = 1;
	bool mono = gfx_bpp() == 1;
	int w = vw * scale, h = vh * scale;
	size_t row = mono ? (size_t)(w + 7) / 8 + 1 : (size_t)w * 3 + 1;
	unsigned char *raw = calloc((size_t)h, row);
	if (raw == NULL)
		return false;
	for (int y = 0; y < h; y++) {
		unsigned char *r = raw + (size_t)y * row;
		r[0] = 0;	// filter: none
		for (int x = 0; x < w; x++) {
			color_t c = gfx_get_pixel(x / scale, y / scale);
			if (mono) {
				if (c)
					r[1 + x / 8] |= (unsigned char)(0x80u >> (x & 7));
			} else {
				r[1 + x * 3] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
				r[2 + x * 3] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
				r[3 + x * 3] = (unsigned char)((c & 31) * 255 / 31);
			}
		}
	}
	uLongf zlen = compressBound((uLong)(row * (size_t)h));
	unsigned char *z = malloc(zlen);
	if (z == NULL || compress2(z, &zlen, raw, (uLong)(row * (size_t)h), 9) != Z_OK) {
		free(raw);
		free(z);
		return false;
	}
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		free(raw);
		free(z);
		return false;
	}
	static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	fwrite(sig, 1, 8, f);
	unsigned char ihdr[13];
	be32(ihdr, (uint32_t)w);
	be32(ihdr + 4, (uint32_t)h);
	ihdr[8] = mono ? 1 : 8;		// bit depth
	ihdr[9] = mono ? 3 : 2;		// palette / RGB
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	if (mono) {
		// paper and ink, like a reflective mono LCD
		static const unsigned char plte[6] = { 0xC8, 0xD0, 0xB8, 0x14, 0x18, 0x10 };
		chunk(f, "PLTE", plte, 6);
	}
	chunk(f, "IDAT", z, (uint32_t)zlen);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(z);
	return true;
}

// --- contact sheets ---
// Every screen of one display class in a grid, each under its name, as one palette PNG: a page to audit layouts
// at a glance (uiSim --sheets, and docs/ux/screens/sheets/ from the gallery). Cells are scaled like sim_save_png

#define SHEET_GAP	12
#define SHEET_LABEL	16	// label row above a cell: font_6x12 plus room
#define SHEET_BG	0xE6E6E6u
#define SHEET_INK	0x202020u
#define SHEET_FRAME	0x787878u

static struct {
	uint32_t *px;		// RGB888 canvas
	int w, h, cols, scale, cw, ch, n, max;
} sheet;

static void sheet_put(int x, int y, uint32_t rgb)
{
	if (x >= 0 && y >= 0 && x < sheet.w && y < sheet.h)
		sheet.px[(size_t)y * (size_t)sheet.w + (size_t)x] = rgb;
}

static void sheet_text(int x, int y, const char *s, uint32_t rgb)
{
	const font_t *f = &font_6x12;
	int stride = (f->w + 7) / 8;
	for (; *s; s++, x += f->w) {
		int top, rows;
		const uint8_t *g = font_glyph(f, (uint8_t)*s, &top, &rows);
		if (g == NULL)
			continue;
		for (int r = 0; r < rows; r++)
			for (int c = 0; c < f->w; c++)
				if (g[r * stride + c / 8] & (0x80u >> (c & 7)))
					sheet_put(x + c, y + top + r, rgb);
	}
}

void sim_sheet_begin(int count, int scale)
{
	free(sheet.px);
	memset(&sheet, 0, sizeof sheet);
	sheet.max = count;
	sheet.scale = scale < 1 ? 1 : scale;
}

bool sim_sheet_add(const char *label)
{
	int vw = gfx_width(), vh = gfx_height(), s = sheet.scale;
	if (sheet.px == NULL) {
		// Five across for landscape panels, eight for portrait, so a sheet stays roughly page shaped
		sheet.cw = vw * s;
		sheet.ch = vh * s;
		sheet.cols = vh > vw ? 8 : 5;
		if (sheet.cols > sheet.max)
			sheet.cols = sheet.max > 0 ? sheet.max : 1;
		int rows = (sheet.max + sheet.cols - 1) / sheet.cols;
		sheet.w = SHEET_GAP + sheet.cols * (sheet.cw + SHEET_GAP);
		sheet.h = SHEET_GAP + rows * (SHEET_LABEL + sheet.ch + SHEET_GAP);
		sheet.px = malloc((size_t)sheet.w * (size_t)sheet.h * sizeof *sheet.px);
		if (sheet.px == NULL)
			return false;
		for (size_t i = 0; i < (size_t)sheet.w * (size_t)sheet.h; i++)
			sheet.px[i] = SHEET_BG;
	}
	if (sheet.n >= sheet.max || vw * s != sheet.cw || vh * s != sheet.ch)
		return false;
	int x0 = SHEET_GAP + (sheet.n % sheet.cols) * (sheet.cw + SHEET_GAP);
	int y0 = SHEET_GAP + (sheet.n / sheet.cols) * (SHEET_LABEL + sheet.ch + SHEET_GAP);
	sheet_text(x0, y0 + 1, label, SHEET_INK);
	y0 += SHEET_LABEL;
	for (int i = -1; i <= sheet.cw; i++) {
		sheet_put(x0 + i, y0 - 1, SHEET_FRAME);
		sheet_put(x0 + i, y0 + sheet.ch, SHEET_FRAME);
	}
	for (int i = -1; i <= sheet.ch; i++) {
		sheet_put(x0 - 1, y0 + i, SHEET_FRAME);
		sheet_put(x0 + sheet.cw, y0 + i, SHEET_FRAME);
	}
	bool mono = gfx_bpp() == 1;
	for (int y = 0; y < sheet.ch; y++) {
		for (int x = 0; x < sheet.cw; x++) {
			color_t c = gfx_get_pixel(x / s, y / s);
			uint32_t rgb = mono ? (c ? 0x141810u : 0xC8D0B8u) :	// the paper and ink of sim_save_png
				(uint32_t)(((c >> 11) & 31) * 255 / 31) << 16 | (uint32_t)(((c >> 5) & 63) * 255 / 63) << 8 |
				(uint32_t)((c & 31) * 255 / 31);
			sheet_put(x0 + x, y0 + y, rgb);
		}
	}
	sheet.n++;
	return true;
}

// A palette PNG at the smallest bit depth that holds the sheet's colors (RGB when there are more than 256)
bool sim_sheet_save(const char *path)
{
	if (sheet.px == NULL)
		return false;
	uint32_t pal[256];
	int np = 0, last = 0;
	bool rgb = false;
	size_t npx = (size_t)sheet.w * (size_t)sheet.h;
	unsigned char *idx = malloc(npx);
	if (idx == NULL)
		return false;
	for (size_t i = 0; i < npx && !rgb; i++) {
		uint32_t c = sheet.px[i];
		if (np == 0 || pal[last] != c) {
			int k = 0;
			while (k < np && pal[k] != c)
				k++;
			if (k == np) {
				if (np == 256) {
					rgb = true;
					break;
				}
				pal[np++] = c;
			}
			last = k;
		}
		idx[i] = (unsigned char)last;
	}
	int depth = rgb ? 8 : np <= 2 ? 1 : np <= 4 ? 2 : np <= 16 ? 4 : 8;
	size_t row = rgb ? (size_t)sheet.w * 3 + 1 : ((size_t)sheet.w * (size_t)depth + 7) / 8 + 1;
	unsigned char *raw = calloc((size_t)sheet.h, row);
	if (raw == NULL) {
		free(idx);
		return false;
	}
	for (int y = 0; y < sheet.h; y++) {
		unsigned char *r = raw + (size_t)y * row + 1;	// r[-1] = 0: filter none
		for (int x = 0; x < sheet.w; x++) {
			size_t i = (size_t)y * (size_t)sheet.w + (size_t)x;
			if (rgb) {
				r[x * 3] = (unsigned char)(sheet.px[i] >> 16);
				r[x * 3 + 1] = (unsigned char)(sheet.px[i] >> 8);
				r[x * 3 + 2] = (unsigned char)sheet.px[i];
			} else {
				int bit = x * depth;
				r[bit / 8] |= (unsigned char)(idx[i] << (8 - depth - bit % 8));
			}
		}
	}
	free(idx);
	uLongf zlen = compressBound((uLong)(row * (size_t)sheet.h));
	unsigned char *z = malloc(zlen);
	FILE *f = z != NULL ? fopen(path, "wb") : NULL;
	if (f == NULL || compress2(z, &zlen, raw, (uLong)(row * (size_t)sheet.h), 9) != Z_OK) {
		if (f != NULL)
			fclose(f);
		free(raw);
		free(z);
		return false;
	}
	static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	fwrite(sig, 1, 8, f);
	unsigned char ihdr[13];
	be32(ihdr, (uint32_t)sheet.w);
	be32(ihdr + 4, (uint32_t)sheet.h);
	ihdr[8] = (unsigned char)depth;
	ihdr[9] = rgb ? 2 : 3;
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	if (!rgb) {
		unsigned char plte[256 * 3];
		for (int k = 0; k < np; k++) {
			plte[k * 3] = (unsigned char)(pal[k] >> 16);
			plte[k * 3 + 1] = (unsigned char)(pal[k] >> 8);
			plte[k * 3 + 2] = (unsigned char)pal[k];
		}
		chunk(f, "PLTE", plte, (uint32_t)np * 3);
	}
	chunk(f, "IDAT", z, (uint32_t)zlen);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(z);
	free(sheet.px);
	sheet.px = NULL;
	return true;
}
