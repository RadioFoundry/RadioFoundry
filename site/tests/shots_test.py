# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The site's scaled screenshots (site/rfoundrysite/shots.py): a copy is the gallery screen k times each way, pixel for
pixel, whatever filters and bit depth the source PNG uses; k covers the largest size a page reported at DPR; and the
stylesheet still has every size the build reads from it.

    python3 site/tests/shots_test.py
"""
import glob
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rfoundrysite import repo, shots  # noqa: E402


def galleries():
	"""The UI gallery's directories: each has a boot screen."""
	return sorted(os.path.basename(os.path.dirname(p)) for p in glob.glob(repo.path("docs", "ux", "screens", "*", "boot.png")))


def paeth(a, b, c):
	"""The PNG specification's predictor, written out apart from shots.py's so a mistake there shows."""
	p = a + b - c
	best = min((abs(p - a), 0, a), (abs(p - b), 1, b), (abs(p - c), 2, c))
	return best[2]


def encode(rows, w, depth, ctype, filters, plte=b""):
	"""A PNG of rows (packed bytes per row), row i filtered with filters[i % len(filters)]."""
	bpp = 3 if ctype == 2 else 1
	out, prev = [], bytes(len(rows[0]))
	for i, r in enumerate(rows):
		ft = filters[i % len(filters)]
		left = lambda x: r[x - bpp] if x >= bpp else 0
		up_left = lambda x: prev[x - bpp] if x >= bpp else 0
		pred = lambda x: [0, left(x), prev[x], (left(x) + prev[x]) // 2, paeth(left(x), prev[x], up_left(x))][ft]
		f = bytes((r[x] - pred(x)) & 255 for x in range(len(r)))
		out.append(bytes([ft]) + f)
		prev = r
	chunks = [(b"IHDR", struct.pack(">IIBBBBB", w, len(rows), depth, ctype, 0, 0, 0))]
	chunks += [(b"PLTE", plte)] if plte else []
	chunks += [(b"IDAT", zlib.compress(b"".join(out))), (b"IEND", b"")]
	return shots.SIGNATURE + b"".join(shots._chunk(t, d) for t, d in chunks)


def decode(png):
	"""(w, h, rows of pixels as tuples of bytes) of an 8-bit PNG."""
	chunks = list(shots._chunks(png))
	w, h, depth, ctype = struct.unpack(">IIBB", chunks[0][1][:10])
	assert depth == 8
	bpp = 3 if ctype == 2 else 1
	rows = shots._unfilter(zlib.decompress(b"".join(d for t, d in chunks if t == b"IDAT")), h, w * bpp, bpp)
	return w, h, [[r[x:x + bpp] for x in range(0, w * bpp, bpp)] for r in rows]


class Upscale(unittest.TestCase):
	def check(self, png, pixels, k):
		w, h, rows = decode(shots.upscale(png, k))
		self.assertEqual((w, h), (len(pixels[0]) * k, len(pixels) * k))
		for y in range(h):
			for x in range(w):
				self.assertEqual(rows[y][x], pixels[y // k][x // k], (x, y))

	def test_rgb_every_filter(self):
		w, h = 7, 10
		pixels = [[bytes(((x * 37 + y * 11) & 255, (x * y * 5) & 255, (200 - x * 13 + y) & 255)) for x in range(w)]
			  for y in range(h)]
		for filters in ([0], [1], [2], [3], [4], [0, 1, 2, 3, 4]):
			with self.subTest(filters=filters):
				self.check(encode([b"".join(r) for r in pixels], w, 8, 2, filters), pixels, 3)

	def test_palette_depths(self):
		w, h = 11, 5
		for depth in (1, 2, 4, 8):
			n = 1 << depth
			idx = [[(x * 3 + y) % n for x in range(w)] for y in range(h)]
			per = 8 // depth
			rows = [bytes(sum(r[x + i] << (8 - depth * (i + 1)) for i in range(per) if x + i < w)
				      for x in range(0, w, per)) for r in idx]
			plte = bytes(i & 255 for i in range(3 * n))
			with self.subTest(depth=depth):
				png = encode(rows, w, depth, 3, [0, 1, 4], plte)
				self.assertIn(b"PLTE" + plte, shots.upscale(png, 4))
				self.check(png, [[bytes([v]) for v in r] for r in idx], 4)

	def test_gallery_screens(self):
		"""One screen of each gallery, as tests/host/sim/ui_png.c writes them."""
		for g in galleries():
			with self.subTest(gallery=g), open(repo.path("docs", "ux", "screens", g, "boot.png"), "rb") as f:
				png = f.read()
				w, h, rows = decode(shots.upscale(png, 1))
				self.assertEqual((w, h), shots.size(g))
				big = decode(shots.upscale(png, 3))[2]
				for y in range(0, h, 7):
					for x in range(0, w, 5):
						self.assertEqual(big[3 * y + 2][3 * x + 1], rows[y][x])

	def test_not_a_screenshot(self):
		with self.assertRaises(repo.SourceError):
			shots.upscale(b"GIF89a" + bytes(40), 2)
		gray = encode([bytes(4)] * 2, 4, 8, 0, [0])
		with self.assertRaises(repo.SourceError):
			shots.upscale(gray, 2)

	def test_chunks(self):
		"""Color space chunks come along, text goes, an unknown critical chunk fails."""
		png = encode([bytes(3)], 1, 8, 2, [0])
		end = png.index(b"IDAT") - 4
		extra = shots._chunk(b"sRGB", b"\0") + shots._chunk(b"gAMA", struct.pack(">I", 45455)) + shots._chunk(b"tEXt", b"a\0b")
		kinds = [t for t, _ in shots._chunks(shots.upscale(png[:end] + extra + png[end:], 2))]
		self.assertEqual(kinds, [b"IHDR", b"sRGB", b"gAMA", b"IDAT", b"IEND"])
		with self.assertRaises(repo.SourceError):
			shots.upscale(png[:end] + shots._chunk(b"XYZW", b"") + png[end:], 2)


class Factor(unittest.TestCase):
	def test_largest_use_at_dpr(self):
		g = galleries()[0]
		w, _ = shots.size(g)
		s = shots.Shots()
		s.use(g, w / 4)
		self.assertEqual(s.factor(g), 1)
		s.use(g, w)
		self.assertEqual(s.factor(g), shots.DPR)
		s.use(g, w * 1.01)
		self.assertEqual(s.factor(g), shots.DPR + 1)
		s.use(g, w / 2)	# a smaller use keeps the largest
		self.assertEqual(s.factor(g), shots.DPR + 1)

	def test_stylesheet_sizes(self):
		"""Every size the build reads from site.css is still there."""
		for sel, prop in ((".carousel", "--shot-h"), (".shots", "--shot-col"), (".device-frame", "max-width"),
				  (".hero-device img", "max-height"), (".radio-card img", "height"), (".rt-pic", "max-width")):
			with self.subTest(selector=sel):
				self.assertGreater(shots.css_px(sel, prop), 0)
		with self.assertRaises(repo.SourceError):
			shots.css_px(".carousel", "--no-such")


if __name__ == "__main__":
	unittest.main(verbosity=1)
