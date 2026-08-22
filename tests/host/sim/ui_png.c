// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The simulator's frame buffer as a PNG (zlib-compressed; 1 bpp panels as a 2-color palette image, RGB565 as RGB)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "ui_sim.h"
#include "ui/gfx.h"

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
