# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Everything the site shows, read from the repository: radios, feature switches, variants, the UI gallery, the
flash layouts odrflash uses. Nothing here is hand-copied into the site; when a source changes, the next build shows
it (docs/dev/website.md "Sources").
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
# radios: boards/*/radio.json and the README status table

STATUS_MARKS = {"●": "yes", "◐": "partly", "○": "no"}


def readme_status():
	"""The README's "Radios and status" table: every radio, built or planned, in its order."""
	for heading, head, rows in md_tables(read("README.md")):
		if heading == "Radios and status":
			out = []
			for r in rows:
				row = dict(zip(head, r))
				out.append({"radio": strip_md(row["Radio"]), "builds": STATUS_MARKS.get(row["Builds"], "no"),
					    "host_tests": STATUS_MARKS.get(row["Host tests"], "no"),
					    "on_radio": STATUS_MARKS.get(row["On the radio"], "no"), "support": row["Support"]})
			return out
	raise SourceError("README.md: no table under '## Radios and status'")


def radio_status(readme_row, radio):
	"""One word for a radio: supported (on the radio), testing (builds, not on a radio yet), or planned."""
	if readme_row is None:
		return "planned"
	if readme_row["on_radio"] in ("yes", "partly"):
		return "supported"
	if readme_row["builds"] == "yes":
		return "testing"
	return "planned"


def boards(feature_names, variant_ids):
	"""Each boards/<id>/radio.json, with its README status and the resolved switches per variant."""
	status_rows = readme_status()
	out = []
	for p in sorted(glob.glob(path("boards", "*", "radio.json"))):
		with open(p, encoding="utf-8") as f:
			r = json.load(f)
		r.pop("_about", None)
		bid = r["id"]
		if os.path.basename(os.path.dirname(p)) != bid:
			raise SourceError("%s: id %r doesn't match its directory" % (p, bid))
		row = next((s for s in status_rows if s["radio"].startswith(r["readme"])), None)
		if row is None:
			raise SourceError("%s: readme %r matches no row of the README radio table" % (p, r["readme"]))
		row["board"] = bid
		r["status"] = radio_status(row, r)
		r["status_detail"] = row
		gal = path("docs", "ux", "screens", r["gallery"])
		if not os.path.isdir(gal):
			raise SourceError("%s: gallery %s isn't in docs/ux/screens" % (p, r["gallery"]))
		r["unsupported"] = make(bid, "print-unsupported").split()
		r["required"] = make(bid, "print-required").split()
		r["config"] = {}
		for v in variant_ids:
			cfg = dict(l.split("=", 1) for l in make(bid, "print-config", None if v == "full" else v).split())
			unknown = set(cfg) - set(feature_names)
			if unknown:
				raise SourceError("make print-config for %s lists unknown switches %s" % (bid, sorted(unknown)))
			r["config"][v] = sorted(k for k, val in cfg.items() if val == "y")
		out.append(r)
	planned = [s for s in status_rows if "board" not in s]
	return out, planned


def screens(gallery):
	return sorted(os.path.basename(p)[:-4] for p in glob.glob(path("docs", "ux", "screens", gallery, "*.png")))


# ---------------------------------------------------------------------------------------------------------------
# flashing: tools/odrflash's board table and stock file profiles, for the web flasher

def flash_boards():
	"""odrflash's boards.py and stockfw.py as JSON, so the web flasher and odrflash can't disagree."""
	sys.path.insert(0, path("tools", "odrflash"))
	try:
		from odrflash import boards as ob, stockfw, manifest
	finally:
		sys.path.pop(0)
	out = {"format": "odr-flash-boards/1", "manifest_format": manifest.FORMAT, "boards": {}, "profiles": {},
	       "known_files": dict(stockfw.KNOWN_FILES), "magic": stockfw.MAGIC.decode(), "header_len": stockfw.HEADER_LEN,
	       "key_len": stockfw.KEY_LEN, "codec_sig": stockfw.CODEC_SIG, "single_segment": stockfw.SINGLE_SEGMENT}
	for b in ob.BOARDS.values():
		out["boards"][b.id] = {
			"id": b.id, "name": b.name, "family": b.family, "app_base": b.app_base, "flash_end": b.flash_end,
			"sectors": [{"index": s.index, "addr": s.addr, "size": s.size} for s in b.sectors],
			"codec": {"id": b.codec.id, "windows": [list(w) for w in b.codec.windows],
				  "installed": list(b.codec.installed), "sources": list(b.codec.sources)},
			"debuglink_names": list(b.debuglink_names), "releasable": b.releasable,
			"keep_codec_supported": b.keep_codec_supported, "key_sha256": b.key_sha256,
			"dfu_ids": list(b.dfu_ids) if b.dfu_ids else None, "dfu_string1": b.dfu_string1,
			"protected": [list(p) for p in b.protected]}
	for sha, p in stockfw.PROFILES.items():
		q = dict(p)
		q["segments"] = [list(s) for s in p["segments"]]
		q["windows"] = [list(w) for w in stockfw.windows(p)]
		for k in ("code", "tables", "extra"):
			q.pop(k, None)
		out["profiles"][sha] = q
	return out


# ---------------------------------------------------------------------------------------------------------------
# release images for the flasher: a directory of odr-*.json manifests and their images (tools/release.py output)

def releases(dist):
	"""[(manifest dict, image path)] for every manifest in dist whose image is next to it."""
	out = []
	if not dist:
		return out
	for p in sorted(glob.glob(os.path.join(dist, "**", "odr-*.json"), recursive=True)):
		with open(p, encoding="utf-8") as f:
			m = json.load(f)
		img = os.path.join(os.path.dirname(p), m.get("image", {}).get("file", ""))
		if m.get("format", "").startswith("odr-release/") and os.path.isfile(img):
			out.append((m, img))
	return out
