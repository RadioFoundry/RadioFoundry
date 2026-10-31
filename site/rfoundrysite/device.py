# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Each radio as an SVG with our real boot screen in its display (docs/ux/screens/<gallery>/boot.png at the site's
scale, shots.py, embedded, so the file stands alone in an <img>).

An illustration of the radio in site/static/img/radios/src/<id>.svg is used when there is one: its
<rect id="screen" .../> marks the display and is replaced by the boot screen (docs/dev/website.md "Radio images").
Its key-<name> ids, for the radio's web control page, must name keys the radio has; where each key sits goes into the
geometry, for the web flasher, which marks the keys to hold for update mode (board.toml [flash.bootloader] hold).
Without one, a generic drawing per shape (handheld, tall handheld, mobile) tinted by board.toml [frame] stands in; its
handheld shapes tag their side PTT.
"""
import base64
import importlib.util
import os
import re
import struct

from . import repo

SCALE = 2	# panel pixels to drawing units in a generic drawing: its display twice the panel's size


def png_size(data):
	if data[:8] != b"\x89PNG\r\n\x1a\n":
		raise repo.SourceError("not a PNG")
	return struct.unpack(">II", data[16:24])


SCREEN_RECT = re.compile(r"<rect\b[^>]*\bid=\"screen\"[^>]*/>")
def svg_keys():
	spec = importlib.util.spec_from_file_location("svg_keys", repo.path("tools", "svg_keys.py"))
	mod = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(mod)
	return mod


def check_keys(src, frame, radio_id):
	"""The drawing's key tags (tools/svg_keys.py check): each names a key of the radio, once, and each key of the
	radio is tagged or listed in data-undrawn, so a redrawn radio can't drop its tags unnoticed."""
	mod = svg_keys()
	bad = mod.problems(frame, mod.key_names(radio_id))
	if bad:
		raise repo.SourceError("%s: %s" % (src, "; ".join(bad)))


def attr(tag, name):
	m = re.search(r"\b%s=\"([-\d.]+)\"" % name, tag)
	if not m:
		raise repo.SourceError("the screen rect has no %s" % name)
	return float(m.group(1))


def render(radio, label, look=None):
	"""{"frame": the drawing with its screen black, "geometry": where the screen and the keys are} (boot() puts a
	screen in it). geometry: {"viewbox": [x, y, w, h], "screen": {x, y, w, h} in viewBox units,
	"screen_pct": {left, top, width, height} as percentages of the image, "keys": {name: {left, top, width, height}}
	as percentages, for each key the drawing shows}, so any screenshot can be laid over the frame (data/radios.json,
	the radio pages) and the flasher can mark a key (radiotop.js)."""
	with open(repo.path("docs", "ux", "screens", radio["gallery"], "boot.png"), "rb") as f:
		png = f.read()
	# a look (board.toml looks): the same radio in another body, drawn in <id>-<look>.svg, with the same keys
	src = repo.path("site", "static", "img", "radios", "src", radio["id"] + ("-" + look if look else "") + ".svg")
	if look and not os.path.isfile(src):
		raise repo.SourceError("boards/%s/board.toml looks %s: no drawing %s" % (radio["id"], look, src))
	if os.path.isfile(src):
		with open(src, encoding="utf-8") as f:
			frame = f.read()
		keys = frame
		mic = src[:-4] + "-mic.svg"	# its hand microphone, on the web control page only
		if os.path.isfile(mic):
			with open(mic, encoding="utf-8") as f:
				keys += f.read()
		check_keys(src, keys, radio["id"])
	else:
		frame = generic(radio, png, label)
		src = "generic drawing of %s" % radio["id"]
	rects = SCREEN_RECT.findall(frame)
	if len(rects) != 1:
		raise repo.SourceError("%s: needs exactly one <rect id=\"screen\" .../>" % src)
	r = rects[0]
	x, y, w, h = (attr(r, k) for k in ("x", "y", "width", "height"))
	pw, ph = png_size(png)
	if abs(w / h - pw / ph) > 0.02:
		raise repo.SourceError("%s: the screen rect is %gx%g, the boot screen %dx%d: the aspect ratios differ"
				       % (src, w, h, pw, ph))
	m = re.search(r"viewBox=\"([-\d.\s]+)\"", frame)
	if not m:
		raise repo.SourceError("%s: no viewBox" % src)
	vx, vy, vw, vh = (float(v) for v in m.group(1).split())
	pct = lambda x0, y0, x1, y1: {"left": round(100 * (x0 - vx) / vw, 3), "top": round(100 * (y0 - vy) / vh, 3),
				      "width": round(100 * (x1 - x0) / vw, 3), "height": round(100 * (y1 - y0) / vh, 3)}
	frame = re.sub(r"<svg\b", '<svg role="img" aria-label="%s"' % label, frame, count=1)
	geometry = {"viewbox": [vx, vy, vw, vh], "screen": {"x": x, "y": y, "w": w, "h": h},
		    "screen_pct": pct(x, y, x + w, y + h), "aspect": round(vw / vh, 5),
		    "keys": {k: pct(*b) for k, b in sorted(svg_keys().key_boxes(frame).items())}}
	return {"frame": frame, "geometry": geometry}


def boot(img, png):
	"""render()'s drawing with png in its display: the boot screen's copy at the site's scale (shots.py), which
	the browser scales down smoothly, so it carries no image-rendering."""
	s = img["geometry"]["screen"]
	image = ('<image href="data:image/png;base64,%s" x="%g" y="%g" width="%g" height="%g" preserveAspectRatio="none"/>'
		 % (base64.b64encode(png).decode(), s["x"], s["y"], s["w"], s["h"]))
	return SCREEN_RECT.sub(image, img["frame"], count=1)


def generic(radio, png, label):
	w, h = png_size(png)
	sw, sh = w * SCALE, h * SCALE
	fr = radio.get("frame", {})
	body, accent = fr.get("body", "#222"), fr.get("accent", "#c33")
	lx, ly = fr.get("lens", [28, 28])
	shape = fr.get("shape", "handheld")
	img = '<rect id="screen" x="{x}" y="{y}" width="%d" height="%d" fill="#000"/>' % (sw, sh)
	parts = []
	if shape == "mobile":
		W, H = sw + 2 * lx + 260, sh + 2 * ly + 60
		sx, sy = 150, (H - sh) // 2
		parts += [
			'<rect x="0" y="0" width="%d" height="%d" rx="26" fill="%s"/>' % (W, H, body),
			'<rect x="6" y="6" width="%d" height="%d" rx="22" fill="none" stroke="#ffffff14" stroke-width="2"/>'
			% (W - 12, H - 12),
			'<circle cx="75" cy="%d" r="42" fill="#111" stroke="#ffffff22" stroke-width="3"/>' % (H // 2),
			'<circle cx="75" cy="%d" r="30" fill="#2a2c30"/>' % (H // 2),
			'<rect x="75" y="%d" width="4" height="22" rx="2" fill="%s" transform="rotate(35 75 %d)"/>'
			% (H // 2 - 30, accent, H // 2),
		]
		for i in range(4):
			parts.append('<rect x="%d" y="%d" width="44" height="22" rx="6" fill="#33363b"/>'
				     % (sx + sw + lx + 30 + (i % 2) * 54, sy + 10 + (i // 2) * 34))
		parts.append('<circle cx="%d" cy="%d" r="18" fill="#33363b"/>' % (W - 50, H // 2 + 30))
	else:
		tall = shape == "handheld-tall"
		W = sw + 2 * lx + 40
		keypad = 0 if tall else 4
		top = 150
		H = top + sh + 2 * ly + 60 + keypad * 46 + 50
		sx, sy = (W - sw) // 2, top + ly + 40
		parts += [
			# antenna, knobs
			'<rect x="%d" y="0" width="34" height="%d" rx="14" fill="#16171a"/>' % (W // 4 - 17, top + 10),
			'<rect x="%d" y="70" width="40" height="%d" rx="8" fill="#16171a"/>' % (W - W // 3 - 20, top - 50),
			'<rect x="%d" y="78" width="40" height="6" fill="#ffffff18"/>' % (W - W // 3 - 20),
			'<rect x="%d" y="94" width="40" height="6" fill="#ffffff18"/>' % (W - W // 3 - 20),
			# body, side PTT (tagged: the flasher marks it when it is a key to hold)
			'<rect id="key-ptt" data-edge="left" x="-8" y="%d" width="14" height="110" rx="5" fill="%s"/>' % (top + 120, accent),
			'<rect x="0" y="%d" width="%d" height="%d" rx="40" fill="%s"/>' % (top, W, H - top, body),
			'<rect x="7" y="%d" width="%d" height="%d" rx="34" fill="none" stroke="#ffffff14" stroke-width="2"/>'
			% (top + 7, W - 14, H - top - 14),
		]
		for i in range(5):	# speaker grille
			parts.append('<rect x="%d" y="%d" width="%d" height="5" rx="2.5" fill="#00000055"/>'
				     % (W // 2 - 60, top + 14 + i * 9 - 4, 120))
		for r in range(keypad):
			for c in range(3):
				parts.append('<rect x="%d" y="%d" width="%d" height="34" rx="9" fill="#33363b"/>'
					     % (sx + c * (sw // 3) + 6, sy + sh + ly + 30 + r * 46, sw // 3 - 12))
		parts.append('<rect x="%d" y="%d" width="70" height="8" rx="4" fill="%s"/>' % (W // 2 - 35, H - 34, accent))
	# the display: bezel, lens, then the screen
	parts += [
		'<rect x="%d" y="%d" width="%d" height="%d" rx="14" fill="#0b0c0e"/>'
		% (sx - lx, sy - ly, sw + 2 * lx, sh + 2 * ly),
		# glare on the bezel only: nothing may cover the screen, where screenshots are laid over
		'<rect x="%d" y="%d" width="%d" height="%d" rx="14" fill="url(#glare)"/>'
		% (sx - lx, sy - ly, sw + 2 * lx, sh + 2 * ly),
		img.format(x=sx, y=sy),
	]
	defs = ('<defs><linearGradient id="glare" x1="0" y1="0" x2="1" y2="1">'
		'<stop offset="0" stop-color="#fff" stop-opacity=".10"/><stop offset=".45" stop-color="#fff" '
		'stop-opacity="0"/></linearGradient></defs>')
	pad = 10
	return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="%d %d %d %d"><title>%s</title>%s%s</svg>\n'
		% (-pad, -pad, W + 2 * pad, H + 2 * pad, label, defs, "".join(parts)))
