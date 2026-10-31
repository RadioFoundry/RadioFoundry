# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The UI gallery's screenshots for the site's pages, blown up with hard pixel edges (docs/dev/website.md "Screenshots").

The gallery (docs/ux/screens/<gallery>/<name>.png) holds each screen at its panel's size. The pages draw them larger
and at sizes that are no whole multiple of it, on screens whose device pixel ratio may be fractional too. Scaled up
there with image-rendering: pixelated, some pixel rows come out twice as thick as others, and a browser that ignores
the property blurs them. So the site serves each gallery at k times its size, k from the largest size a page draws it
at, and lets the browser scale it down smoothly: every pixel keeps one width, at any size.

Each place that shows a screenshot reports the CSS width it draws it at (use()); write() then makes the copies,
static/img/screens/<gallery>/<name>.png, every gallery screen of every gallery shown.
"""
import concurrent.futures
import math
import os
import re
import struct
import zlib

from . import repo

# the device pixel ratio the copies cover without being scaled up: most laptops and phones (a higher one scales the
# copy up a little, smoothly: slightly soft, never uneven)
DPR = 2
SIGNATURE = b"\x89PNG\r\n\x1a\n"


def url(gallery, name):
	"""Where a page finds a screenshot's copy, from the site's root."""
	return "static/img/screens/%s/%s.png" % (gallery, name)


def css_px(selector, prop):
	"""The largest px value site/static/css/site.css gives prop in a rule whose selector list contains selector, so
	the copies' sizes follow the stylesheet. A rule that is gone or changed its unit fails the build."""
	with open(repo.path("site", "static", "css", "site.css"), encoding="utf-8") as f:
		css = re.sub(r"/\*.*?\*/", "", f.read(), flags=re.S)
	found = []
	for sel, body in re.findall(r"([^{}]+)\{([^{}]*)\}", css):
		if selector in (s.strip() for s in sel.split(",")):
			found += [int(v) for v in re.findall(r"(?:^|;)\s*%s\s*:\s*(\d+)px" % re.escape(prop), body)]
	if not found:
		raise repo.SourceError("site.css: no %s: <n>px in a %s rule (site/rfoundrysite/shots.py)" % (prop, selector))
	return max(found)


def _chunks(png):
	i = len(SIGNATURE)
	while i < len(png):
		n, kind = struct.unpack(">I4s", png[i:i + 8])
		yield kind, png[i + 8:i + 8 + n]
		i += 12 + n


def _paeth(a, b, c):
	p = a + b - c
	pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
	return a if pa <= pb and pa <= pc else b if pb <= pc else c


def _unfilter(raw, h, stride, bpp):
	rows, prev, i = [], bytes(stride), 0
	for _ in range(h):
		ft, row = raw[i], bytearray(raw[i + 1:i + 1 + stride])
		i += 1 + stride
		if ft == 1:
			for x in range(bpp, stride):
				row[x] = (row[x] + row[x - bpp]) & 255
		elif ft == 2:
			row = bytearray((a + b) & 255 for a, b in zip(row, prev))
		elif ft == 3:
			for x in range(stride):
				row[x] = (row[x] + ((row[x - bpp] if x >= bpp else 0) + prev[x]) // 2) & 255
		elif ft == 4:
			for x in range(stride):
				row[x] = (row[x] + _paeth(row[x - bpp] if x >= bpp else 0, prev[x], prev[x - bpp] if x >= bpp else 0)) & 255
		elif ft:
			raise repo.SourceError("PNG filter type %d" % ft)
		rows.append(bytes(row))
		prev = rows[-1]
	return rows


def upscale(png, k):
	"""png (8-bit RGB or a palette image of 1, 2, 4 or 8 bits, not interlaced: what the gallery writes) k times each
	way, nearest-neighbor. A palette image comes out as an 8-bit one with the same palette."""
	if png[:8] != SIGNATURE:
		raise repo.SourceError("not a PNG")
	chunks = list(_chunks(png))
	w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", chunks[0][1])
	if interlace or (ctype, depth) not in ((2, 8), (3, 1), (3, 2), (3, 4), (3, 8)):
		raise repo.SourceError("PNG color type %d, %d bits, interlace %d: not a gallery screenshot" % (ctype, depth, interlace))
	bpp = 3 if ctype == 2 else 1
	stride = (w * depth * bpp + 7) // 8
	rows = _unfilter(zlib.decompress(b"".join(d for t, d in chunks if t == b"IDAT")), h, stride, bpp)
	if depth < 8:	# one byte per pixel
		per, mask = 8 // depth, (1 << depth) - 1
		rows = [bytes((r[x // per] >> (8 - depth * (x % per + 1))) & mask for x in range(w)) for r in rows]
	out = []
	for r in rows:
		big = b"".join(r[x:x + bpp] * k for x in range(0, w * bpp, bpp))
		# the row's first copy unfiltered, the others filter Up: all zeros, next to nothing compressed
		out.append(b"\0" + big + (b"\2" + bytes(len(big))) * (k - 1))
	head = struct.pack(">IIBBBBB", w * k, h * k, 8, ctype, 0, 0, 0)
	keep = [(t, d) for t, d in chunks if t in (b"PLTE", b"tRNS")]
	return SIGNATURE + b"".join(_chunk(t, d) for t, d in
				    [(b"IHDR", head)] + keep + [(b"IDAT", zlib.compress(b"".join(out), 9)), (b"IEND", b"")])


def _chunk(kind, data):
	return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def size(gallery):
	"""A gallery's screen size in its own pixels (every screen of a gallery is one size)."""
	with open(repo.path("docs", "ux", "screens", gallery, "boot.png"), "rb") as f:
		head = f.read(24)
	if head[:8] != SIGNATURE:
		raise repo.SourceError("docs/ux/screens/%s/boot.png: not a PNG" % gallery)
	return struct.unpack(">II", head[16:24])


class Shots:
	def __init__(self):
		self.widths = {}	# gallery: the largest CSS width a page draws it at

	def use(self, gallery, css_w):
		"""A page draws gallery's screens css_w CSS pixels wide (at most)."""
		self.widths[gallery] = max(self.widths.get(gallery, 0), css_w)

	def factor(self, gallery):
		"""How many times its size a gallery's copies are: enough for its largest drawing at DPR."""
		return max(1, math.ceil(DPR * self.widths[gallery] / size(gallery)[0] - 1e-9))

	def scaled(self, gallery, name):
		"""One screen's copy (PNG bytes)."""
		with open(repo.path("docs", "ux", "screens", gallery, name + ".png"), "rb") as f:
			return upscale(f.read(), self.factor(gallery))

	def write(self, out):
		"""The copies of every screen of every gallery a page reported. {gallery: factor}."""
		def one(g, name):
			with open(os.path.join(out, "static", "img", "screens", g, name + ".png"), "wb") as f:
				f.write(self.scaled(g, name))
		jobs = []
		for g in sorted(self.widths):
			os.makedirs(os.path.join(out, "static", "img", "screens", g), exist_ok=True)
			jobs += [(g, name) for name in repo.screens(g)]
		# most of the time is zlib, which lets other threads run
		with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
			for job in [pool.submit(one, *j) for j in jobs]:
				job.result()
		return {g: self.factor(g) for g in sorted(self.widths)}
