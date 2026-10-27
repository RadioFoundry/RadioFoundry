# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Everything the site shows, read from the repository: radios (the board registry, tools/boards.py), feature
switches, variants, the UI gallery, the flash layouts rfoundry uses. Nothing here is hand-copied into the site; when
a source changes, the next build shows it (docs/dev/website.md "Sources").
"""
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


class SourceError(Exception):
	"""A source the site depends on doesn't have the shape the parser expects."""


def path(*parts):
	return os.path.join(ROOT, *parts)


def read(rel):
	with open(path(rel), encoding="utf-8") as f:
		return f.read()


def git(*args, default=""):
	try:
		return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
	except (OSError, subprocess.CalledProcessError):
		return default


def make(board, target, variant=None):
	"""Output of a make query target (print-config, print-unsupported, ...) for a board and variant."""
	cmd = ["make", "-s", "--no-print-directory", "BOARD=" + board, target]
	if variant:
		cmd.insert(3, "VARIANT=" + variant)
	r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
	if r.returncode:
		raise SourceError("make %s for %s%s failed: %s" % (target, board, " " + variant if variant else "",
								   r.stderr.strip()[-400:]))
	return r.stdout


# ---------------------------------------------------------------------------------------------------------------
# Markdown tables

def md_tables(text):
	"""[(heading, header cells, rows)] for every pipe table, with the nearest ## or ### heading above it."""
	out, heading, lines = [], "", text.splitlines()
	i = 0
	while i < len(lines):
		line = lines[i]
		m = re.match(r"^(#{2,3})\s+(.*)", line)
		if m:
			heading = m.group(2).strip()
		if line.startswith("|") and i + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|\s*$", lines[i + 1]):
			head = split_row(line)
			rows = []
			i += 2
			while i < len(lines) and lines[i].startswith("|"):
				rows.append(split_row(lines[i]))
				i += 1
			out.append((heading, head, rows))
			continue
		i += 1
	return out


def split_row(line):
	# a cell may hold an escaped pipe (\|), as the survey table's sources do
	cells = re.split(r"(?<!\\)\|", line.strip().strip("|"))
	return [c.strip().replace("\\|", "|") for c in cells]


def strip_md(text):
	"""Plain text of a short Markdown cell: links to their text, no emphasis or code marks."""
	text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
	return re.sub(r"[*`]", "", text).strip()


# ---------------------------------------------------------------------------------------------------------------
# feature switches: config/features.mk, docs/feature-matrix.md

FEATURE_LINE = re.compile(r"^CONFIG_(\w+)\s*\?=\s*(.*)$")


def features():
	"""Every switch in config/features.mk, in its order: name, help text (the comment above it), default expression,
	what it needs (from the dependency checks), and the feature matrix's group, title, sizes and notes."""
	text = read("config/features.mk")
	m = re.search(r"^FEATURES\s*:=\s*((?:.*\\\n)*.*)$", text, re.M)
	if not m:
		raise SourceError("config/features.mk: no FEATURES list")
	names = m.group(1).replace("\\\n", " ").split()
	out, comment = {}, []
	for line in text.splitlines():
		if line.startswith("##"):
			comment = []
			continue
		if line.startswith("#"):
			comment.append(line[1:].strip())
			continue
		fm = FEATURE_LINE.match(line)
		if fm and fm.group(1) in names and fm.group(1) not in out:
			out[fm.group(1)] = {"name": fm.group(1), "help": " ".join(comment), "default": fm.group(2).strip()}
		comment = []
	missing = [n for n in names if n not in out]
	if missing:
		raise SourceError("config/features.mk: no default line for %s" % ", ".join(missing))
	for name, needs in dependencies(text).items():
		if name in out:
			out[name]["needs"] = needs
	matrix = feature_matrix()
	for name, f in out.items():
		f.setdefault("needs", {"all": [], "any": []})
		f.update(matrix.get(name, {}))
	return [out[n] for n in names]


def dependencies(text):
	"""{feature: {"all": [...], "any": [...]}} from the "$(error CONFIG_X=y needs ...)" checks in features.mk."""
	deps = {}
	for line in text.splitlines():
		for m in re.finditer(r"\$\(error CONFIG_(\$\(f\)|\w+)=y needs ([^)]*)\)", line):
			subject, need = m.group(1), m.group(2)
			if subject == "$(f)":
				fm = re.search(r"\$\(foreach f,([\w ]+),", line)
				subjects = fm.group(1).split() if fm else []
			else:
				subjects = [subject]
			req = re.findall(r"CONFIG_(\w+)=y", need)
			kind = "any" if " or " in need else "all"
			for s in subjects:
				d = deps.setdefault(s, {"all": [], "any": []})
				for r in req:
					if r not in d[kind]:
						d[kind].append(r)
	return deps


def feature_matrix():
	"""{switch: {group, title, bytes: {radio column: text}, notes}} from docs/feature-matrix.md."""
	out = {}
	for heading, head, rows in md_tables(read("docs/feature-matrix.md")):
		if "Switch" not in head:
			continue
		si, ti = head.index("Switch"), head.index("Feature")
		ni = head.index("Notes") if "Notes" in head else None
		size_cols = [(i, h[len("Bytes "):]) for i, h in enumerate(head) if h.startswith("Bytes ")]
		for r in rows:
			if len(r) != len(head):
				continue
			name = r[si].strip("`")
			out[name] = {"group": heading, "title": strip_md(r[ti]),
				     "bytes": {radio: r[i] for i, radio in size_cols if r[i] not in ("", "—")},
				     "notes": strip_md(r[ni]) if ni is not None else ""}
	return out


# ---------------------------------------------------------------------------------------------------------------
# variants: config/variants/*.mk

def variants():
	"""The full build first, then each config/variants/<id>.mk: title, summary, the switches it turns off and why."""
	out = [{"id": "full", "title": "Full", "summary": "Every feature the radio supports.",
		"off": []}]
	for p in sorted(glob.glob(path("config", "variants", "*.mk"))):
		v = {"id": os.path.basename(p)[:-3], "title": "", "summary": "", "off": []}
		reason = []
		with open(p, encoding="utf-8") as f:
			for line in f:
				line = line.rstrip("\n")
				m = re.match(r"^## (title|summary): (.*)", line)
				if m:
					key = m.group(1)
					v[key] = (v[key] + " " + m.group(2)).strip()
					continue
				if line.startswith("##") or line.startswith("# SPDX") or line.startswith("# Copyright"):
					reason = []
					continue
				if line.startswith("#"):
					reason.append(line[1:].strip())
					continue
				fm = re.match(r"^CONFIG_(\w+)\s*\?=\s*n\s*$", line)
				if fm:
					v["off"].append({"name": fm.group(1), "why": " ".join(reason)})
				elif line.strip():
					raise SourceError("%s: a variant only turns switches off (CONFIG_X ?= n): %r" % (p, line))
				reason = []
		if not v["title"]:
			raise SourceError("%s: no '## title:' line" % p)
		out.append(v)
	return out


# ---------------------------------------------------------------------------------------------------------------
# radios: the board registry (tools/boards.py json --for site) and the radio survey's rows without a board

def registry(view="site"):
	"""tools/boards.py's view for the site. boards.py needs Python 3.11 or later: tools/rfoundry_python.sh finds one
	($RFOUNDRY_PYTHON first)."""
	py = subprocess.run(["sh", path("tools", "rfoundry_python.sh")], cwd=ROOT, capture_output=True, text=True)
	if py.returncode:
		raise SourceError(py.stderr.strip())
	r = subprocess.run([py.stdout.strip(), path("tools", "boards.py"), "json", "--for", view], cwd=ROOT,
			   capture_output=True, text=True)
	if r.returncode:
		raise SourceError("tools/boards.py json --for %s failed: %s" % (view, r.stderr.strip()[-400:]))
	return json.loads(r.stdout)


BOARD_STATUSES = ("supported", "testing", "in_progress", "planned")
# a board's [status] level as the site's badge: a radio still being brought up reads "in development"
LEVEL_STATUS = {"supported": "supported", "testing": "testing", "bring-up": "testing"}
def boards(feature_names, variant_ids):
	"""The registry's boards with their status badge and the resolved switches per variant, and the survey's radios
	without a board."""
	out, reg = [], registry("site")
	for r in reg["boards"]:
		bid = r["id"]
		r["status"] = LEVEL_STATUS[r["level"]]
		if r["codeplug"] is None:
			raise SourceError("boards/%s: no flash map with openfw_eeprom and openfw_end (tools/boards.py check)" % bid)
		gal = path("docs", "ux", "screens", r["gallery"])
		if not os.path.isdir(gal):
			raise SourceError("boards/%s/board.toml: gallery %s isn't in docs/ux/screens" % (bid, r["gallery"]))
		r["unsupported_reasons"] = r["unsupported"]
		r["unsupported"] = list(r["unsupported"])
		# the hardware can, the board's code doesn't yet (config.mk BOARD_NOT_WRITTEN): never shown as unavailable
		r["not_written_reasons"] = r["not_written"]
		r["not_written"] = list(r["not_written"])
		r["required"] = make(bid, "print-required").split()
		r["config"] = {}
		for v in variant_ids:
			cfg = dict(l.split("=", 1) for l in make(bid, "print-config", None if v == "full" else v).split())
			unknown = set(cfg) - set(feature_names)
			if unknown:
				raise SourceError("make print-config for %s lists unknown switches %s" % (bid, sorted(unknown)))
			r["config"][v] = sorted(k for k, val in cfg.items() if val == "y")
		out.append(r)
	# the radio survey's radios without a board (docs/spec/radios.md Port cells, read by the registry)
	return out, [dict(p, support=p["note"]) for p in reg["planned"]]


def screens(gallery):
	return sorted(os.path.basename(p)[:-4] for p in glob.glob(path("docs", "ux", "screens", gallery, "*.png")))


# ---------------------------------------------------------------------------------------------------------------
# flashing: tools/rfoundry's board table and stock file profiles, for the web flasher

def flash_boards():
	"""rfoundry's boards.py and stockfw.py as JSON, so the web flasher and rfoundry can't disagree."""
	sys.path.insert(0, path("tools", "rfoundry"))
	try:
		from rfoundry import boards as ob, codecmap, stockfw, manifest
	finally:
		sys.path.pop(0)
	out = {"format": "rfoundry-flash-boards/1", "manifest_format": manifest.FORMAT, "boards": {}, "profiles": {},
	       "known_files": dict(stockfw.KNOWN_FILES), "no_codec_files": dict(stockfw.NO_CODEC_FILES),
	       "magic": stockfw.MAGIC.decode(), "header_len": stockfw.HEADER_LEN,
	       "key_len": stockfw.KEY_LEN, "codec_sig": stockfw.CODEC_SIG, "single_segment": stockfw.SINGLE_SEGMENT,
	       "descriptor": {"addr": stockfw.DESC_ADDR, "len": stockfw.DESC_LEN, "magic": stockfw.DESC_MAGIC,
			      "version": stockfw.DESC_VERSION, "windows": stockfw.DESC_WINDOWS}}
	for b in ob.BOARDS.values():
		out["boards"][b.id] = {
			"id": b.id, "name": b.name, "family": b.family, "app_base": b.app_base, "flash_end": b.flash_end,
			"sectors": [{"index": s.index, "addr": s.addr, "size": s.size} for s in b.sectors],
			"codec": {"id": b.codec.id, "windows": [list(w) for w in b.codec.windows],
				  "installed": list(b.codec.installed), "sources": list(b.codec.sources)},
			"debuglink_names": list(b.debuglink_names), "releasable": b.releasable,
			"keep_codec_supported": b.keep_codec_supported, "key_sha256": b.key_sha256,
			"dfu_ids": list(b.dfu_ids) if b.dfu_ids else None, "dfu_string1": b.dfu_string1,
			"dfu_product": b.dfu_product,
			"protected": [list(p) for p in b.protected], "protect": b.protect,
			# for a backup over another open firmware's CPS (openfw.js): area 5's base and the radio types
			"rom_base": b.rom_base, "cps_radio_types": list(b.cps_radio_types)}
	for sha, p in stockfw.PROFILES.items():
		q = dict(p)
		q["segments"] = [list(s) for s in p["segments"]]
		q["windows"] = [list(w) for w in stockfw.windows(p)]
		for k in ("code", "tables", "extra"):
			q.pop(k, None)
		out["profiles"][sha] = q
	# the codec maps, normalized (every address a number), for codecmap.js
	out["codec_maps"] = codecmap.load_maps()
	out["c7000"] = c7000_flash()
	out["f4stub"] = f4_flash()
	return out


def _tool(*parts):
	"""Import a repository script (tools/hrc7000_codec.py, stub/stubtool.py) as a module."""
	import importlib.util
	spec = importlib.util.spec_from_file_location(parts[-1][:-3], path(*parts))
	mod = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(mod)
	return mod


def c7000_flash():
	"""What the web flasher needs for the HR_C7000 radios it flashes through the stub (stub/stubtool.py): the program
	flash layout and LED pins from stubtool, the codec slot and the codec profiles from tools/hrc7000_codec.py. The stub
	images themselves go to c7000-stub.json (c7000_stub()), loaded only when a C7000 radio is flashed."""
	st, dc, fl = _tool("stub", "stubtool.py"), _tool("tools", "hrc7000_codec.py"), _tool("tools", "serial_loader.py")
	boards = {}
	for bid, lay in dc.BOARDS.items():
		# the bootloader's ID string (serial_loader.MODELS): in the first 48 KB of the program flash, it tells the radios apart
		boards[bid] = {"slot": [lay["slot"][0] - dc.PROGMEM_BASE, lay["slot"][1] - dc.PROGMEM_BASE],
			       "run": list(lay["run"]), "tables": list(lay["tables"]),
			       "bootloader_id": fl.MODELS[bid].tag if bid in fl.MODELS else None,
			       # the whole program flash (a backup reads it all; writes stay in the application area), whether
			       # the data memory is SPI NOR flash (else SPI NAND), where a NAND radio's calibration copies sit,
			       # and the board profile the universal stub gets (CONFIG). A stock console, the update mode and
			       # the read rate are in radios.json flash
			       "progmem_size": lay.get("progmem_size", dc.PROGMEM_SIZE), "data_flash": lay.get("data_flash", True),
			       # the boards whose profiles give this board its codec, preferred first (hrc7000_codec.py
			       # codec_boards: a second SoC that runs the first SoC's codec, then its own)
			       "codec_boards": list(lay.get("codec_boards", (bid,))),
			       "calibration": lay.get("calibration"),
			       # (CONFIG), with its LED pins, UART and bridge where the board has them (a second SoC)
			       "stub_board": {k: (list(v) if isinstance(v, tuple) else v) for k, v in st.C7_BOARDS[bid].items()
					      if k in ("id", "latch", "key", "nand", "leds", "uart", "bridge")} if bid in st.C7_BOARDS
			       else {k: st.GENERIC[k] for k in ("id", "latch", "key", "nand")},
			       # the board's [[flash.protect]] (progmem offsets are the stub's: from the program flash's base),
			       # which every write and restore here checks first (protect.js)
			       "protect": st.C7_BOARDS[bid]["protect"] if bid in st.C7_BOARDS else []}
	profiles = []
	for p in dc.PROFILES.values():
		# a stock file known only by its whole-file hash (e.g. a 2024-05-09 build) has no codec hashes: it can be
		# carved but not found in a program flash read
		if "code_sha256" not in p and p.get("kind") == "progmem":
			continue
		profiles.append({"name": p["name"], "board": p["board"], "tag": p["tag"],
				 "code_len": p["code"][1], "run": p["code"][2], "code_sha256": p.get("code_sha256"),
				 "tables_len": p["tables"][1], "tables_ram": p["tables"][2], "tables_sha256": p.get("tables_sha256"),
				 "stock_header": p.get("hdr", dc.STOCK_HEADER),
				 "tables_head_crc": p.get("tables_head_crc", dc.TABLES_HEAD_CRC),
				 "entries": [p["entries"][k] for k in dc.ENTRIES],
				 "stock_file": None if p.get("kind") == "progmem" else {"sha256": next(h for h, q in dc.PROFILES.items() if q is p),
										"size": p["size"], "code": p["code"][0], "tables": p["tables"][0]}})
	return {"progmem_base": dc.PROGMEM_BASE, "progmem_size": dc.PROGMEM_SIZE, "data_size": st.MEM["q128"][3],
		"app_start": st.APP_START, "app_end": st.APP_END, "sector": st.SECTOR,
		"app_base": dc.APP_BASE, "stock_header": dc.STOCK_HEADER, "stock_magic": dc.MAGIC.decode(),
		"led": {"red": st.LED_RED, "green": st.LED_GREEN},
		"slot_magic": dc.SLOT_MAGIC, "slot_version": dc.SLOT_VERSION,
		"slot_header": dc.HEADER_LEN, "slot_rel_max": dc.REL_MAX,
		"codec_sig": list(dc.CODEC_SIG), "boards": boards, "profiles": profiles,
		# the C-SKY builds' codec maps (docs/spec/codec-placement.md): which words a slot lists
		"codec_maps": {pid: m for pid, m in dc.codecmap.load_maps().items() if m["isa"] == "csky"}}


def c7000_stub():
	"""The universal stub stubtool embeds (one image for every HR_C7000 radio and every way in), checked against its
	SHA-256 by stubtool itself, as base64 for the page, with its power-on load address and the RAM it runs in."""
	import base64
	st = _tool("stub", "stubtool.py")
	image = st.stub_univ_image()
	run, end = st.univ_layout(image)
	return {"format": "rfoundry-c7000-stub/2",
		"univ": {"load": st.LOAD_ADDR, "run": run, "end": end, "sha256": st.STUB_UNIV_SHA256,
			 "data": base64.b64encode(image).decode()}}


def f4_flash():
	"""What the web flasher's loader route needs per STM32F4 radio (stub/f4stub.py BOARDS), on a radio that already
	runs RadioFoundry: the flash its firmware owns, and the board record the stub takes in CONFIG (config_record(), as hex), so
	the page and f4stub.py send the same bytes. v2_wiring: the radio has the wiring a protocol 2 stub had built in
	(f4stub.V2_WIRING), so an older stub left running can still serve it. The stub image goes to f4-stub.json (f4_stub()),
	loaded only when it is used."""
	f4 = _tool("stub", "f4stub.py")
	v2 = f4.V2_WIRING
	return {"app_base": f4.APP_BASE, "protocol": f4.VERSION, "record_len": f4.REC_LEN,
		"boards": {bid: {"owned": [list(r) for r in b["owned"]], "codec": list(b["codec"]) if b["codec"] else None,
				 "name": b["name"], "stub_id": b["id"], "record": f4.config_record(bid).hex(),
				 "v2_wiring": f4.config_record(bid)[1:] == v2, "protect": b["protect"]}
			   for bid, b in f4.BOARDS.items()}}


def f4_stub():
	"""The STM32F4 stub f4stub.py embeds, checked against its SHA-256 by f4stub itself, as base64 for the page."""
	import base64
	f4 = _tool("stub", "f4stub.py")
	return {"format": "rfoundry-f4-stub/1", "sha256": f4.F4STUB_SHA256, "run": f4.RUN_ADDR, "protocol": f4.VERSION,
		"data": base64.b64encode(f4.stub_image()).decode()}


# ---------------------------------------------------------------------------------------------------------------
# release images for the flasher: a directory of rfoundry-*.json manifests and their images (tools/release.py output)

def releases(dist):
	"""[(manifest dict, image path)] for every manifest in dist whose image is next to it."""
	out = []
	if not dist:
		return out
	for p in sorted(glob.glob(os.path.join(dist, "**", "rfoundry-*.json"), recursive=True)):
		with open(p, encoding="utf-8") as f:
			m = json.load(f)
		img = os.path.join(os.path.dirname(p), m.get("image", {}).get("file", ""))
		if m.get("format", "").startswith("rfoundry-release/") and os.path.isfile(img):
			out.append((m, img))
	return out
