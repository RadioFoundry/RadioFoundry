# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""docs/ rendered as site pages, the same Markdown GitHub and the PDF manual show.

The output mirrors the docs tree (docs/user/02-installing.md -> docs/user/02-installing.html, a directory's README.md
-> its index.html) and every non-Markdown file under docs/ is copied beside it, so relative links and images keep
working with only .md -> .html rewritten. A link that leaves docs/ goes to the file on the repository web site.
Heading ids are GitHub's (tools/check_doc_links.py), so anchors that pass the link check work here too.

Translations (later): docs/i18n/<lang>/<path under docs> replaces a page for that language; pages without one
fall back to English with a notice.
"""
import html
import importlib.util
import os
import posixpath
import re
import shutil

from . import repo

try:
	from markdown_it import MarkdownIt
	from markdown_it.token import Token
except ImportError:		# reported by build.py with the install hint
	MarkdownIt = Token = None

# Section order and the directory each comes from; titles are translated (docs.section.<id>)
SECTIONS = ["user", "dev", "spec", "testing", "ux", "general"]
SCHEME = re.compile(r"^[a-z][a-z0-9+.-]*:", re.I)
HTML_REF = re.compile(r"(<(?:img|a)\b[^>]*?\b(?:src|href)\s*=\s*\")([^\"]+)(\")", re.I)


def _load_glossary():
	"""tools/glossary.py, which owns the data file and the matching rules."""
	spec = importlib.util.spec_from_file_location("rfoundry_glossary", repo.path("tools", "glossary.py"))
	mod = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(mod)
	return mod


OFF = re.compile(r"<!--\s*glossary:\s*off\b([^>]*?)-->", re.I)


def slug(text):
	"""GitHub's heading id, as tools/check_doc_links.py computes it."""
	text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
	text = re.sub(r"<[^>]+>", "", text)
	text = text.replace("`", "").lower()
	text = re.sub(r"[^\w\- ]", "", text)
	return text.replace(" ", "-")


def site_path(rel):
	"""Where a docs/ Markdown file lands in the site: docs/x/README.md -> docs/x/index.html."""
	head, base = posixpath.split(rel)
	base = "index.html" if base == "README.md" else base[:-3] + ".html"
	return posixpath.join(head, base)


class Docs:
	def __init__(self, repo_url, ref, langs, default_lang):
		if MarkdownIt is None:
			raise repo.SourceError("markdown-it-py is missing")
		self.md = MarkdownIt("commonmark", {"html": True, "linkify": False}).enable(["table", "strikethrough"])
		self.repo_url = repo_url.rstrip("/")
		self.ref = ref
		self.langs = langs
		self.default_lang = default_lang
		self.pages = self._collect()
		self.glossary = _load_glossary()
		self.terms = self._terms()

	def _terms(self):
		"""[(compiled pattern, entry)], longest names first so "Tier III" wins over "Tier"."""
		out = []
		for e in self.glossary.load()["entries"]:
			if e.get("preview", True):
				out += [(self.glossary.pattern(n), n, e) for n in self.glossary.names(e)]
		out.sort(key=lambda t: -len(t[1]))
		return out

	def _collect(self):
		pages = []
		docs = repo.path("docs")
		for dirpath, dirs, files in os.walk(docs):
			dirs[:] = sorted(d for d in dirs if d != "i18n")
			for f in sorted(files):
				if not f.endswith(".md"):
					continue
				rel = posixpath.relpath(os.path.join(dirpath, f), repo.ROOT).replace(os.sep, "/")
				parts = rel.split("/")
				section = parts[1] if len(parts) > 2 else "general"
				if section not in SECTIONS:
					section = "general"
				with open(os.path.join(dirpath, f), encoding="utf-8") as fh:
					text = fh.read()
				m = re.search(r"^#\s+(.+)$", text, re.M)
				pages.append({"src": rel, "out": site_path(rel), "section": section,
					      "title": m.group(1).strip() if m else f[:-3], "index": f == "README.md"})
		# the user manual reads in chapter order (its files are numbered); every other section alphabetically
		pages.sort(key=lambda p: (SECTIONS.index(p["section"]), not p["index"], p["src"]))
		return pages

	def section_pages(self, section):
		return [p for p in self.pages if p["section"] == section]

	def source_for(self, page, lang):
		"""(Markdown text, is a fallback to English) of a page in a language."""
		if lang != self.default_lang:
			tr = repo.path("docs", "i18n", lang, page["src"][len("docs/"):])
			if os.path.isfile(tr):
				with open(tr, encoding="utf-8") as f:
					return f.read(), False
		with open(repo.path(page["src"]), encoding="utf-8") as f:
			return f.read(), lang != self.default_lang

	def render(self, page, lang):
		"""(body HTML, [(level, id, text)] of the h2/h3 headings, fallback flag)."""
		text, fallback = self.source_for(page, lang)
		tokens = self.md.parse(text)
		seen, toc = {}, []
		for i, tok in enumerate(tokens):
			if tok.type == "heading_open":
				inline = tokens[i + 1]
				base = slug(inline.content)
				k = seen.get(base, 0)
				seen[base] = k + 1
				hid = base if k == 0 else "%s-%d" % (base, k)
				tok.attrSet("id", hid)
				if tok.tag in ("h2", "h3"):
					toc.append((int(tok.tag[1]), hid, plain(inline.content)))
			if tok.type == "inline" and tok.children:
				for c in tok.children:
					if c.type == "link_open":
						c.attrSet("href", self.link(c.attrGet("href"), page["src"]))
					elif c.type == "image":
						c.attrSet("src", self.link(c.attrGet("src"), page["src"]))
					elif c.type == "html_inline":
						c.content = self.html_links(c.content, page["src"])
			if tok.type == "html_block":
				tok.content = self.html_links(tok.content, page["src"])
		self.mark_terms(tokens, page, text)
		body = self.md.renderer.render(tokens, self.md.options, {})
		body = body.replace("<table>", '<div class="table-wrap"><table>').replace("</table>", "</table></div>")
		return body, toc, fallback

	def mark_terms(self, tokens, page, text):
		"""Wrap the first use of each glossary term in every section in <abbr> (docs/dev/website.md, Glossary).

		Plain text only: not code, headings, link text or a table's first column. A section starts at each h1 or h2. A page opts out of
		everything with <!-- glossary: off --> or out of some terms with <!-- glossary: off TG, SFR -->."""
		skip = set()
		for m in OFF.finditer(text):
			names = [n.strip() for n in m.group(1).split(",") if n.strip()]
			if not names:
				return
			skip.update(names)
		scope = "user" if page["section"] == "user" else None
		seen = set()
		depth = 0
		in_heading = False
		col = 0
		for tok in tokens:
			if tok.type == "tr_open":
				col = 0
			elif tok.type in ("td_open", "th_open"):
				col += 1
			if tok.type == "heading_open":
				in_heading = True
				if tok.tag in ("h1", "h2"):
					seen = set()
			elif tok.type == "heading_close":
				in_heading = False
			# the first column of a table is usually the term the row defines
			if tok.type != "inline" or not tok.children or in_heading or col == 1:
				continue
			out = []
			for c in tok.children:
				if c.type == "link_open":
					depth += 1
				elif c.type == "link_close":
					depth -= 1
				if c.type != "text" or depth:
					out.append(c)
				else:
					out += self.split_terms(c, scope, seen, skip, page)
			tok.children = out

	def split_terms(self, tok, scope, seen, skip, page):
		"""The text token as text and <abbr> tokens."""
		text, cands = tok.content, {}
		for pat, name, e in self.terms:
			if e["term"] in seen or skip.intersection(self.glossary.names(e)) or (scope and e["scope"] != scope):
				continue
			m = pat.search(text)
			if m and (e["term"] not in cands or (m.start(), -(m.end() - m.start())) < cands[e["term"]][:2]):
				cands[e["term"]] = (m.start(), -(m.end() - m.start()), m.end(), e)
		out, pos = [], 0
		for a, _, b, e in sorted(cands.values(), key=lambda c: c[:2]):
			if a < pos:
				continue		# overlapped by an earlier match in this text
			seen.add(e["term"])
			if a > pos:
				out.append(_text(text[pos:a]))
			out.append(self.abbr(e, text[a:b], page))
			pos = b
		if not out:
			return [tok]
		if pos < len(text):
			out.append(_text(text[pos:]))
		return out

	def abbr(self, e, shown, page):
		g = self.glossary
		page_name = "user/22-glossary.md" if e["scope"] == "user" else "dev/glossary.md"
		more = e["link"] or "%s#%s" % (page_name, g.anchor(e["term"]))
		more = self.link(posixpath.relpath("docs/" + more, posixpath.dirname(page["src"])), page["src"])
		t = Token("html_inline", "", 0)
		t.content = '<abbr class="gl" tabindex="0" title="%s" data-more="%s">%s</abbr>' % (
			html.escape(e["definition"], quote=True), html.escape(more, quote=True), html.escape(shown))
		return t

	def html_links(self, text, src):
		return HTML_REF.sub(lambda m: m.group(1) + self.link(m.group(2), src) + m.group(3), text)

	def link(self, href, src):
		"""A link in docs file src, rewritten for the site."""
		if not href or href.startswith("#") or SCHEME.match(href) or href.startswith("//"):
			return href
		target, _, anchor = href.partition("#")
		anchor = "#" + anchor if anchor else ""
		resolved = posixpath.normpath(posixpath.join(posixpath.dirname(src), target))
		here = posixpath.dirname(site_path(src))
		if resolved.startswith("docs/") and not resolved.startswith("docs/i18n/"):
			full = repo.path(resolved)
			if resolved.endswith(".md"):
				return posixpath.relpath(site_path(resolved), here) + anchor
			if os.path.isdir(full):
				if os.path.isfile(os.path.join(full, "README.md")):
					return posixpath.relpath(posixpath.join(resolved, "index.html"), here) + anchor
				return self.repo_link(resolved, True) + anchor
			return posixpath.relpath(resolved, here) + anchor
		if resolved.startswith(".."):
			return href
		return self.repo_link(resolved, os.path.isdir(repo.path(resolved))) + anchor

	def repo_link(self, rel, is_dir):
		return "%s/%s/%s/%s" % (self.repo_url, "tree" if is_dir else "blob", self.ref, rel)

	def copy_assets(self, out_dir):
		"""Every non-Markdown file under docs/ (screenshots, the MIB) beside the rendered pages."""
		n = 0
		docs = repo.path("docs")
		for dirpath, dirs, files in os.walk(docs):
			dirs[:] = [d for d in dirs if d != "i18n"]
			for f in files:
				if f.endswith(".md"):
					continue
				src = os.path.join(dirpath, f)
				dst = os.path.join(out_dir, os.path.relpath(src, repo.ROOT))
				os.makedirs(os.path.dirname(dst), exist_ok=True)
				shutil.copy2(src, dst)
				n += 1
		return n


def _text(content):
	t = Token("text", "", 0)
	t.content = content
	return t


def plain(md_inline):
	text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", md_inline)
	text = re.sub(r"<[^>]+>", "", text)
	return html.unescape(text.replace("`", "").replace("**", ""))
