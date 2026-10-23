#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The glossary: one data file (docs/glossary.json), the two pages generated from it, and the checks on it.

    tools/glossary.py --check          the data is sound and the generated pages are current (CI)
    tools/glossary.py --write          regenerate docs/user/22-glossary.md and docs/dev/glossary.md
    tools/glossary.py --unexplained    acronyms the docs use that have no entry (information only)

The site build (site/rfoundrysite/docs.py) imports this module to mark terms in the rendered pages (docs/dev/website.md).
An entry has: term, aliases, category, scope ("user" or "dev"), definition, link (a path under docs/, optional) and
preview (false lists the term on the page but never marks it in the text, for words common in plain prose).
Exit status 0 when everything is fine, 1 otherwise.
"""
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DATA = os.path.join(ROOT, "docs", "glossary.json")
PAGES = {"user": os.path.join(ROOT, "docs", "user", "22-glossary.md"),
	 "dev": os.path.join(ROOT, "docs", "dev", "glossary.md")}
SCOPES = ("user", "dev")

HEADER = {
	"user": ("Glossary", "Terms the user guide uses. In the web version, hover over, focus or tap a dotted term in the text to see "
		 "its meaning. The developer glossary ([dev/glossary.md](../dev/glossary.md)) has the protocol and "
		 "hardware terms."),
	"dev": ("Developer glossary", "Protocol, hardware and firmware terms of the specs and developer docs. Terms the "
		"user guide uses are in the [glossary](../user/22-glossary.md)."),
}


def load(path=DATA):
	with open(path, encoding="utf-8") as f:
		return json.load(f)


def anchor(term):
	"""The id of a term's row, for links from the previews."""
	return "t-" + re.sub(r"[^a-z0-9]+", "-", term.lower()).strip("-")


def names(entry):
	return [entry["term"]] + list(entry.get("aliases", []))


def loose(name):
	"""A plain word (lowercase, or capitalized for the page) matches in any case; an acronym or mixed case exactly."""
	return name == name.lower() or name == name.capitalize()


def pattern(name):
	"""Regex for one name. A plain word matches in any case and with an s; the rest match exactly.

	Not inside a longer word, a hyphenated word, a path or an identifier."""
	body = re.escape(name).replace("\\ ", r"\s+")
	flags = 0
	if loose(name):
		flags = re.I
		body += "s?"
	return re.compile(r"(?<![\w\-./])" + body + r"(?![\w\-]|\.\w)", flags)


def check(data):
	"""A list of problems with the data itself."""
	errors = []
	seen = {}
	for i, e in enumerate(data["entries"]):
		t = e.get("term", "").strip()
		if not t:
			errors.append("entry %d has no term" % i)
			continue
		if not e.get("definition", "").strip():
			errors.append("%s: empty definition" % t)
		if e.get("category") not in data["categories"]:
			errors.append("%s: category %r is not in the category list" % (t, e.get("category")))
		if e.get("scope") not in SCOPES:
			errors.append("%s: scope must be user or dev" % t)
		link = e.get("link", "")
		if link and not os.path.exists(os.path.join(ROOT, "docs", link)):
			errors.append("%s: link docs/%s does not exist" % (t, link))
		for n in names(e):
			k = n.lower() if loose(n) else n
			if k in seen:
				errors.append("%s: %r is already used by %s" % (t, n, seen[k]))
			else:
				seen[k] = t
	return errors


def render(data, scope):
	"""The Markdown of one generated page."""
	title, intro = HEADER[scope]
	out = ["<!-- SPDX-License-Identifier: MIT -->",
	       "<!-- Generated from docs/glossary.json by tools/glossary.py. Edit the data file, not this page. -->",
	       "<!-- glossary: off -->", "", "# " + title, "", intro, ""]
	for cat in data["categories"]:
		rows = sorted((e for e in data["entries"] if e["scope"] == scope and e["category"] == cat),
			      key=lambda e: e["term"].lower())
		if not rows:
			continue
		out += ["## " + cat, "", "| Term | Meaning |", "|---|---|"]
		for e in rows:
			label = e["term"]
			if e.get("aliases"):
				label += " (%s)" % ", ".join(a for a in e["aliases"] if a != a.lower())
				label = label.replace(" ()", "")
			meaning = e["definition"].replace("|", "\\|")
			if e.get("link"):
				target = os.path.join(ROOT, "docs", e["link"])
				meaning += " [More](%s)" % os.path.relpath(target, os.path.dirname(PAGES[scope]))
			out.append('| <a id="%s"></a>%s | %s |' % (anchor(e["term"]), label, meaning))
		out.append("")
	return "\n".join(out).rstrip("\n") + "\n"


def stale(data):
	"""The generated pages that differ from the data."""
	bad = []
	for scope, path in PAGES.items():
		try:
			with open(path, encoding="utf-8") as f:
				have = f.read()
		except OSError:
			have = None
		if have != render(data, scope):
			bad.append(os.path.relpath(path, ROOT))
	return bad


def markdown_files():
	for top in ("docs/user", "docs/dev", "docs/spec"):
		for dirpath, _, files in os.walk(os.path.join(ROOT, top)):
			for f in sorted(files):
				if f.endswith(".md") and f not in ("22-glossary.md", "glossary.md"):
					yield os.path.join(dirpath, f)


def unexplained(data):
	"""{acronym: files using it} for all-capital words the docs use that no entry or exclusion covers."""
	known = set()
	for e in data["entries"]:
		known.update(names(e))
	known.update(data.get("excluded", []))
	found = {}
	word = re.compile(r"(?<![\w\-./`])[A-Z][A-Z0-9_]{1,}(?![\w\-]|\.\w)")
	for path in markdown_files():
		fence = False
		with open(path, encoding="utf-8") as f:
			for line in f:
				if line.lstrip().startswith(("```", "~~~")):
					fence = not fence
					continue
				if fence:
					continue
				line = re.sub(r"`[^`]*`|\]\([^)]*\)|\*\*", "", line)
				for w in word.findall(line):
					if w not in known and not re.search(r"[\d_]", w):
						found.setdefault(w, set()).add(os.path.relpath(path, ROOT))
	return found


def main(argv):
	data = load()
	if "--write" in argv:
		for scope, path in PAGES.items():
			with open(path, "w", encoding="utf-8") as f:
				f.write(render(data, scope))
		print("glossary: wrote %s" % ", ".join(os.path.relpath(p, ROOT) for p in PAGES.values()))
		return 0
	if "--unexplained" in argv:
		for w, files in sorted(unexplained(data).items(), key=lambda kv: (-len(kv[1]), kv[0])):
			print("%-14s %d files" % (w, len(files)))
		return 0
	errors = check(data)
	errors += ["%s is not current: run tools/glossary.py --write" % p for p in stale(data)] if not errors else []
	for e in errors:
		print(e)
	print("glossary: %d entries, %d problems" % (len(data["entries"]), len(errors)))
	return 1 if errors else 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
