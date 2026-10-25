# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The link preview image (og:image): every radio side by side, each showing a different screen of the UI gallery,
drawn as an SVG and rasterized to a PNG, since Facebook and most chat apps don't take SVG.

The rasterizer is resvg (resvg_py, MIT; a wheel, no system libraries). The card has no text or logo: link previews print the
page title and domain next to it, and fonts would make the image depend on the build machine.
"""
import base64
import re
import struct
import zlib

from . import device, repo

W, H = 1200, 630	# the size Facebook, LinkedIn and X ask for (1.91:1)
ZOOM = 2	# rasterized at twice that: the screens' text stays legible on high-density displays
MARGIN, GAP = 28, 24
# each radio shows the first of these its gallery has that no radio before it shows
SCREENS = ("boot", "home_dual", "home_dmr", "rx_group", "message_view", "channels", "tools")


def inner(svg):
	"""A radio drawing's viewBox and its contents, to nest in the card."""
	m = re.search(r"<svg\b[^>]*\bviewBox=\"([-\d.\s]+)\"[^>]*>(.*)</svg>", svg, re.S)
	if not m:
		raise repo.SourceError("a radio drawing has no viewBox")
	return [float(v) for v in m.group(1).split()], m.group(2)


def columns(png_bytes):
	"""The first and last pixel column, as fractions of the width, where an RGBA PNG (resvg's output) isn't
	transparent."""
	w, h = struct.unpack(">II", png_bytes[16:24])
	data, pos = b"", 8
	while pos < len(png_bytes):
		n, kind = struct.unpack(">I4s", png_bytes[pos:pos + 8])
		if kind == b"IDAT":
			data += png_bytes[pos + 8:pos + 8 + n]
		pos += 12 + n
	raw, stride = zlib.decompress(data), 4 * w
	prev, seen = bytearray(stride), bytearray(w)
	for y in range(h):
		ft, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
		for i in range(stride):	# undo the row filter
			a = line[i - 4] if i >= 4 else 0
			b, c = prev[i], prev[i - 4] if i >= 4 else 0
			if ft == 1:
				line[i] = (line[i] + a) & 255
			elif ft == 2:
				line[i] = (line[i] + b) & 255
			elif ft == 3:
				line[i] = (line[i] + (a + b) // 2) & 255
			elif ft == 4:
				pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
				line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
		for x in range(w):
			if line[4 * x + 3]:
				seen[x] = 1
		prev = line
	xs = [x for x in range(w) if seen[x]]
	return xs[0] / w, (xs[-1] + 1) / w


def with_screen(frame, geometry, png):
	"""The radio's drawing with a screenshot in its display."""
	s = geometry["screen"]
	image = ('<image href="data:image/png;base64,%s" x="%g" y="%g" width="%g" height="%g" image-rendering="optimizeSpeed" '
		 'preserveAspectRatio="none"/>' % (base64.b64encode(png).decode(), s["x"], s["y"], s["w"], s["h"]))
	return device.SCREEN_RECT.sub(image, frame, count=1)


def card(boards):
	"""The card as SVG. boards: the site's boards, each with its render() result as b["render"]."""
	used, items = set(), []
	for b in boards:
		have = set(repo.screens(b["gallery"]))
		name = next((n for n in SCREENS if n in have and n not in used), "boot")
		used.add(name)
		with open(repo.path("docs", "ux", "screens", b["gallery"], name + ".png"), "rb") as f:
			png = f.read()
		drawing = with_screen(b["render"]["frame"], b["render"]["geometry"], png)
		vb, body = inner(drawing)
		# the drawings' viewBoxes pad them unevenly: crop to what is drawn, so the gaps between radios are equal
		lo, hi = columns(rasterize(drawing, width=200))
		vb = [vb[0] + lo * vb[2], vb[1], (hi - lo) * vb[2], vb[3]]
		# handhelds as tall as the card; a mobile radio about as wide as two handhelds
		mobile = vb[2] > vb[3]
		items.append({"vb": vb, "body": body, "scale": (H * 0.5 / vb[2]) if mobile else ((H - 2 * MARGIN) / vb[3])})
	width = sum(i["vb"][2] * i["scale"] for i in items) + GAP * (len(items) - 1)
	fit = min(1, (W - 2 * MARGIN) / width)
	x = (W - width * fit) / 2
	bottom = (H + max(i["vb"][3] * i["scale"] for i in items) * fit) / 2	# the row centered, its radios on one line
	parts = []
	for i in items:
		w, h = i["vb"][2] * i["scale"] * fit, i["vb"][3] * i["scale"] * fit
		parts.append('<svg x="%g" y="%g" width="%g" height="%g" viewBox="%s">%s</svg>'
			     % (x, bottom - h, w, h, " ".join("%g" % v for v in i["vb"]), i["body"]))
		x += w + GAP * fit
	return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
		'<defs><radialGradient id="glow" cx=".5" cy=".85" r=".7">'
		'<stop offset="0" stop-color="#f2601f" stop-opacity=".45"/><stop offset="1" stop-color="#f2601f" stop-opacity="0"/>'
		'</radialGradient></defs>'
		'<rect width="%d" height="%d" fill="#121417"/><rect width="%d" height="%d" fill="url(#glow)"/>'
		'%s</svg>\n'
		% (W, H, W, H, W, H, W, H, "".join(parts)))


def rasterize(svg, **size):
	try:
		import resvg_py
	except ImportError:
		raise repo.SourceError("resvg_py is missing (site/requirements.txt)")
	return bytes(resvg_py.svg_to_bytes(svg_string=svg, skip_system_fonts=True, **size))


def png(svg):
	return rasterize(svg, zoom=ZOOM)
