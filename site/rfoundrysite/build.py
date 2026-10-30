# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Builds the static site into one directory (docs/dev/website.md).

Layout of the output:

  index.html, radios/, features/, flash/, tools/, docs/   pages of the default language
  <lang>/...                                              the same pages in another language
  data/*.json                                             what the pages and the flasher read (also a public API)
  static/                                                 CSS, scripts, images (shared by every language)
  firmware/<board>/                                       release images and manifests, with --firmware
  downloads/rfoundry-tool.zip                             the rfoundry tool for the flasher's manual install kit
"""
import datetime
import hashlib
import html
import importlib.util
import json
import os
import posixpath
import re
import shutil

from . import device, docs as docsmod, i18n, repo, social, tmpl, toolzip


def _release_version():
	"""tools/release_version.py: the release names (vYY.M[.N][bN]) and their order."""
	spec = importlib.util.spec_from_file_location("release_version", repo.path("tools", "release_version.py"))
	mod = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(mod)
	return mod


release_version = _release_version()


def release_and_beta(versions, beta):
	"""The release the flasher installs (the newest name among the images' versions; they are all one release's, "" for
	none) and the pre-release it offers (beta when it is newer, else "")."""
	release = release_version.newest(versions) or ""
	if beta and release_version.parse(beta) is None:
		raise repo.SourceError("--beta %s is not a release name (tools/release_version.py)" % beta)
	if beta and release and release_version.parse(beta) <= release_version.parse(release):
		beta = ""
	return release, beta

# feature-matrix.md groups of developer tools that never ship in a release: not on the visitor pages
BENCH_ONLY = {"Bench"}
# the production address: link previews need absolute URLs (og:url, og:image)
SITE_URL = "https://radiofoundry.org/"
# where users help each other: the footer and every failure message link it, next to the GitHub issues
COMMUNITY_URL = "https://www.facebook.com/groups/radiofoundry/"
BOARD_ORDER = {"supported": 0, "testing": 1, "in_progress": 2, "planned": 3}
# a radio page's documentation links (board.toml [docs]), in this order
DOC_ORDER = ("hardware", "install", "codec", "bringup")


class Site:
	def __init__(self, out, repo_url, ref, firmware=None, strict=False, log=print, beta=""):
		self.out = out
		self.repo_url = repo_url.rstrip("/")
		self.ref = ref
		self.firmware = firmware
		self.beta = beta	# a pre-release newer than the release in firmware (site/cloudflare-build.sh), or ""
		self.strict = strict
		self.log = log
		self.warnings = []
		self.i18n = i18n.load()
		self.tpl = tmpl.Templates(repo.path("site", "templates"), self.i18n.t)
		self.git = repo.git("rev-parse", "--short=8", "HEAD", default="unknown")
		# where the web tools send error reports (report.js); unset: none are sent
		self.sentry_dsn = os.environ.get("RFOUNDRY_SENTRY_DSN", "")
		# the branch this site is built from, for the flasher's default build ref: Cloudflare's, else the checked-out one
		# (empty on a detached HEAD)
		self.branch = os.environ.get("CF_PAGES_BRANCH") or repo.git("branch", "--show-current", default="")
		self.git_dirty = bool(repo.git("status", "--porcelain", "--untracked-files=no"))
		self.version = repo.git("describe", "--tags", "--always", "--dirty", default=self.git)
		now = datetime.datetime.now(datetime.timezone.utc)
		self.built = now.strftime("%Y-%m-%d")
		self.built_at, self.built_iso = now.strftime("%Y-%m-%d %H:%M UTC"), now.strftime("%Y-%m-%dT%H:%MZ")
		self.git_full = repo.git("rev-parse", "HEAD", default="")
		self.asset_version = self.static_hash()
		self.tool = None	# downloads/rfoundry-tool.zip, once built (toolzip.write)

	@staticmethod
	def static_hash():
		"""A short hash of site/static: the version stamped on scripts and styles, so it changes with their content
		(an uncommitted change included) and a browser never mixes cached files of two builds."""
		h = hashlib.sha256()
		for dirpath, dirs, files in sorted(os.walk(repo.path("site", "static"))):
			dirs.sort()
			for f in sorted(files):
				p = os.path.join(dirpath, f)
				h.update(os.path.relpath(p, repo.ROOT).encode())
				with open(p, "rb") as fh:
					h.update(fh.read())
		return h.hexdigest()[:10]

	def warn(self, msg):
		self.warnings.append(msg)
		self.log("warning: " + msg)

	# -----------------------------------------------------------------------------------------------------------
	# data

	def load(self):
		self.features = repo.features()
		self.fmap = {f["name"]: f for f in self.features}
		self.variants = repo.variants()
		vids = [v["id"] for v in self.variants]
		self.boards, self.planned = repo.boards(list(self.fmap), vids)
		# board.toml order: the first is the home page's radio
		self.boards.sort(key=lambda b: (BOARD_ORDER[b["status"]], b["order"]))
		self.flash = repo.flash_boards()
		with open(repo.path("site", "data", "screens.json"), encoding="utf-8") as f:
			self.screens = json.load(f)
		for f in self.features:
			if "group" not in f:
				self.warn("feature %s has no row in docs/feature-matrix.md" % f["name"])
				f["group"], f["title"], f["bytes"], f["notes"] = "Other", f["name"], {}, ""
		for v in self.variants:
			for off in v["off"]:
				if off["name"] not in self.fmap:
					raise repo.SourceError("variant %s turns off unknown switch %s" % (v["id"], off["name"]))
		for name, entry in self.screens["screens"].items():
			if entry["group"] not in self.screens["groups"]:
				self.warn("site/data/screens.json: screen %s has group %s, not in \"groups\"" % (name, entry["group"]))
		for b in self.boards:
			names = repo.screens(b["gallery"])
			unknown = [n for n in names if self.screen(n) is None]
			if unknown:
				self.warn("screens not in site/data/screens.json: %s" % ", ".join(unknown))
			b["screens"] = [n for n in names if self.screen(n) is not None]
			if b["flash"].get("board") and b["flash"]["board"] not in self.flash["boards"]:
				raise repo.SourceError("boards/%s/board.toml: flash board %s isn't in rfoundry" % (b["id"],
															 b["flash"]["board"]))
		self.groups = []
		for f in self.features:
			if f["group"] not in self.groups:
				self.groups.append(f["group"])
		self.releases = repo.releases(self.firmware)
		self.release, self.beta = release_and_beta([m["version"] for m, _ in self.releases], self.beta)

	def feature_state(self, board, name, variant="full"):
		"""on: in this build; off: the board supports it but this build leaves it out; nw: the hardware can, the
		board's code isn't written yet; na: the hardware can't."""
		if name in board["config"][variant]:
			return "on"
		if name in board["unsupported"]:
			return "na"
		if name in board["not_written"]:
			return "nw"
		return "off"

	def json_data(self):
		radios = []
		for b in self.boards:
			r = {k: b[k] for k in ("id", "name", "maker", "model", "also", "kind", "order", "status", "bands",
					       "power", "gallery", "cpu", "display", "docs", "flash", "backup", "unsupported",
					       "unsupported_reasons", "not_written", "not_written_reasons", "required", "config", "screens", "codeplug", "volts",
					       "clock", "variants", "variant_pattern", "variant_check")}
			r["support"] = docsmod.plain(b["summary"])
			r["image"] = "static/img/radios/%s.svg" % b["id"]
			r["frame_image"] = "static/img/radios/%s-frame.svg" % b["id"]
			r["frame_geometry"] = b["frame_geometry"]
			radios.append(r)
		planned = [{"radio": p["radio"], "support": p["support"], "status": p["status"], "pr": p["pr"],
			    "hardware_needed": p["hardware_needed"]} for p in self.planned]
		features = [{k: f.get(k) for k in ("name", "title", "group", "help", "needs", "bytes", "notes")}
			    for f in self.features]
		return {
			"site.json": {"format": "rfoundry-site/1", "git": self.git, "version": self.version, "built": self.built,
				      "release": self.release, "beta": self.beta,
				      "branch": self.branch, "repo": self.repo_url, "community": COMMUNITY_URL, "languages": self.i18n.languages, "default_language": self.i18n.default,
				      "reports": {"sentry_dsn": self.sentry_dsn}, "tool": self.tool},
			"radios.json": {"radios": radios, "planned": planned},
			"features.json": {"groups": self.groups, "features": features},
			"variants.json": {"variants": self.variants},
			"flash-boards.json": self.flash,
			"c7000-stub.json": repo.c7000_stub(),
			"f4-stub.json": repo.f4_stub(),
			"screens.json": self.screens,
			"releases.json": {"releases": self.release_list()},
		}

	def firmware_line(self, lang):
		"""The footer's firmware release (what the flasher installs) and the changelog: the GitHub releases."""
		rels = self.repo_url + "/releases"
		line = '<a href="%s">%s</a>' % (html.escape(rels), html.escape(self.i18n.t(lang, "footer.changelog")))
		if self.release:
			line = '<a href="%s/tag/%s">%s</a> · %s' % (html.escape(rels), html.escape(self.release),
				html.escape(self.i18n.t(lang, "footer.firmware", version=self.release)), line)
		return line

	def commit_time(self, commit):
		"""A commit's time (Unix seconds) for manifests from before they carried git_time; None when the clone
		doesn't have it"""
		t = repo.git("show", "-s", "--format=%ct", commit, default="") if commit else ""
		return int(t) if t.isdigit() else None

	def release_list(self):
		out = []
		for m, img in self.releases:
			out.append({"board": m["board"], "variant": m.get("variant", "full"), "version": m["version"],
				    "git": m.get("git"), "git_time": m.get("git_time") or self.commit_time(m.get("git_full")),
				    "features": m.get("features", ""),
				    "image": "firmware/%s/%s" % (m["board"], m["image"]["file"]),
				    "manifest": "firmware/%s/%s" % (m["board"], os.path.basename(img)[:-4] + ".json"),
				    "size": m["image"]["size"], "sha256": m["image"]["sha256"]})
		return out

	# -----------------------------------------------------------------------------------------------------------
	# output helpers

	def write(self, rel, text):
		p = os.path.join(self.out, rel)
		os.makedirs(os.path.dirname(p), exist_ok=True)
		with open(p, "w", encoding="utf-8") as f:
			f.write(text)

	def page(self, lang, rel, title, content, page_class, description="", scripts="", nav="", footer=""):
		"""A page of the site: rel is its path inside the language's tree; footer goes above the site footer's line."""
		prefix = self.i18n.prefix(lang)
		full = prefix + rel
		depth = full.count("/")
		root = "../" * depth
		meta = next(l for l in self.i18n.languages if l["code"] == lang)
		langs = "".join(
			'<a href="%s%s%s"%s lang="%s">%s</a>' % (root, self.i18n.prefix(l["code"]), rel,
								  ' aria-current="true"' if l["code"] == lang else "",
								  l["code"], html.escape(l["name"]))
			for l in self.i18n.languages) if len(self.i18n.languages) > 1 else ""
		ctx = {"lang": lang, "dir": meta.get("dir", "ltr"), "title": title, "root": root,
		       "home": root + prefix, "content": content, "page_class": page_class,
		       "description": description or self.i18n.t(lang, "site.tagline"), "scripts": scripts, "footer": footer,
		       "languages": langs, "git": self.git, "assets": self.asset_version, "version": self.version, "built": self.built,
		       "built_at": self.built_at, "built_iso": self.built_iso, "git_full": self.git_full,
		       "git_stamp": self.git + ("-dirty" if self.git_dirty else ""), "firmware_release": self.firmware_line(lang),
		       "repo": self.repo_url, "community": COMMUNITY_URL, "nav_current": nav,
		       "url": SITE_URL + (full[:-len("index.html")] if full.endswith("index.html") else full),
		       "og_image": SITE_URL + "static/img/social.png?v=" + self.asset_version,
		       "og_width": social.W * social.ZOOM, "og_height": social.H * social.ZOOM}
		ctx["nav"] = self.nav(lang, ctx["home"], nav)
		self.write(full, self.tpl.render("base", ctx, lang).replace("{{root}}", root).replace("{{home}}", root + prefix))

	def nav(self, lang, home, current):
		items = [("radios", "radios/index.html"), ("features", "features/index.html"),
			 ("flash", "flash/index.html"), ("codeplug", "codeplug/index.html"), ("docs", "docs/index.html"),
			 ("screenshots", "screenshots/index.html"), ("tools", "tools/index.html")]
		return "".join('<a href="%s%s"%s>%s</a>' % (home, href, ' aria-current="page"' if key == current else "",
							    html.escape(self.i18n.t(lang, "nav." + key)))
			       for key, href in items)

	def t(self, lang, key, **fmt):
		return self.i18n.t(lang, key, **fmt)

	def screen(self, name):
		"""A gallery screen's screens.json entry; a color page's shot in a highlight color or a theme (<page>__<color>)
		takes the page's."""
		return self.screens["screens"].get(name.partition("__")[0])

	def has_screen(self, board, name):
		"""The board's build shows this gallery screen: its gallery has the shot and the full build has every switch
		the screen needs."""
		s = self.screen(name)
		return name in board["screens"] and s is not None and all(x in board["config"]["full"] for x in s["needs"])

	def screen_caption(self, lang, name):
		page, _, color = name.partition("__")
		cap = self.t(lang, "screen." + page)
		if color in ("light", "high_contrast"):	# a theme page in another theme (uiSim G_THEMES)
			return self.t(lang, "screen.in_theme", screen=cap, theme=self.t(lang, "theme." + color))
		return self.t(lang, "screen.in_color", screen=cap, color=self.t(lang, "color." + color)) if color else cap

	def ftitle(self, lang, f):
		return self.i18n.opt(lang, "feature.%s.title" % f["name"], f["title"])

	def fhelp(self, lang, f):
		return self.i18n.opt(lang, "feature.%s.help" % f["name"], f["help"])

	def vtitle(self, lang, v):
		return self.i18n.opt(lang, "variant.%s.title" % v["id"], v["title"])

	def vsummary(self, lang, v):
		return self.i18n.opt(lang, "variant.%s.summary" % v["id"], v["summary"])

	def badge(self, lang, status):
		return '<span class="badge badge-%s">%s</span>' % (status, html.escape(self.t(lang, "status." + status)))

	def variants_html(self, lang, b):
		"""The hardware variants sold under the model (board.toml [hardware] variants): each with whether RadioFoundry
		runs on it, and how to tell which one a radio is. Empty for a radio that lists none."""
		if not b["variants"]:
			return ""
		def state(v):
			if not v.get("runs"):
				return self.t(lang, "radio.variant.none")
			if v["runs"]["board"] != b["id"]:
				other = next((o["name"] for o in self.boards if o["id"] == v["runs"]["board"]), v["runs"]["board"])
				return self.t(lang, "radio.variant.other", board=other)
			return self.t(lang, "status." + repo.LEVEL_STATUS[v["level"]])
		rows = "".join("<li><code>%s</code> %s: <b>%s</b>%s</li>" % (
			html.escape(v["code"]), html.escape(v["name"]), html.escape(state(v)),
			" " + html.escape(v["notice"]) if v.get("notice") else "") for v in b["variants"])
		return ('<div class="radio-variants"><h3>%s</h3><ul class="small">%s</ul>'
			'<details class="small"><summary>%s</summary><p>%s</p></details></div>'
			% (html.escape(self.t(lang, "radio.variants_title")), rows, html.escape(self.t(lang, "radio.variant_how")),
			   html.escape(b["variant_check"] or "")))

	def board_note(self, lang, b):
		"""A board's line for visitors: board.toml [status] summary. A catalog may translate it (radio.note.<id>)."""
		return html.escape(self.i18n.opt(lang, "radio.note." + b["id"], docsmod.plain(b["summary"])))

	def note(self, lang, key, src):
		"""A survey radio's note for visitors: the survey's Port cell note (src), which a catalog may translate
		(planned.note.<slug>)."""
		return html.escape(self.i18n.opt(lang, key, src))

	def md_inline(self, text, src="README.md"):
		"""A Markdown table cell from the repository as HTML (links rewritten like the docs)."""
		h = self.docs.md.renderInline(text)
		return self.docs.html_links(h, "docs/" + src) if src else h

	# -----------------------------------------------------------------------------------------------------------
	# pages

	def build(self):
		self.load()
		if os.path.isdir(self.out):
			shutil.rmtree(self.out)
		os.makedirs(self.out)
		self.docs = docsmod.Docs(self.repo_url, self.ref, self.i18n.codes, self.i18n.default)
		shutil.copytree(repo.path("site", "static"), os.path.join(self.out, "static"))
		self.version_imports(os.path.join(self.out, "static", "js"))
		shutil.rmtree(os.path.join(self.out, "static", "img", "radios", "src"), ignore_errors=True)
		for b in self.boards:
			img = device.render(b, b["name"])
			self.write("static/img/radios/%s.svg" % b["id"], img["boot"])
			self.write("static/img/radios/%s-frame.svg" % b["id"], img["frame"])
			b["frame_geometry"] = img["geometry"]
			b["render"] = img
			for lk in b["looks"]:
				limg = device.render(b, lk["name"], lk["id"])
				self.write("static/img/radios/%s-%s.svg" % (b["id"], lk["id"]), limg["boot"])
		with open(os.path.join(self.out, "static", "img", "social.png"), "wb") as f:
			f.write(social.png(social.card(self.boards)))
		self.tool = toolzip.write(os.path.join(self.out, "downloads"))
		data = self.json_data()
		for name, obj in data.items():
			self.write("data/" + name, json.dumps(obj, indent=1, ensure_ascii=False) + "\n")
		self.copy_firmware()
		for lang in self.i18n.codes:
			self.docs_assets(lang)
			self.home(lang)
			self.radios_index(lang)
			for b in self.boards:
				self.radio_page(lang, b)
			self.screenshots_page(lang)
			self.features_page(lang)
			self.flash_page(lang)
			self.tools_page(lang)
			self.codeplug_page(lang)
			self.docs_pages(lang)
			self.not_found(lang)
		self.write(".nojekyll", "")
		self.check_screenshots()
		self.check_links()
		missing = self.i18n.unused_check()
		if missing:
			self.warn("keys missing from the %s catalog: %s" % (self.i18n.default, ", ".join(missing)))
		return self.warnings

	SHOTS_SECTION = re.compile(r'<section class="rshots" id="([^"]+)".*?</section>', re.S)

	def check_screenshots(self):
		"""The screenshots page has a section for every board in boards/, and each section every gallery screen
		that board's build has: what the page shows comes from the registry, the gallery and screens.json alone."""
		with open(os.path.join(self.out, "screenshots", "index.html"), encoding="utf-8") as fh:
			sections = {m.group(1): m.group(0) for m in self.SHOTS_SECTION.finditer(fh.read())}
		for b in self.boards:
			sec = sections.get(b["id"])
			if sec is None:
				self.warn("screenshots page: no section for %s" % b["id"])
				continue
			shown = set(re.findall(r'docs/ux/screens/%s/([^"/]+)\.png' % re.escape(b["gallery"]), sec))
			want = {n for n in repo.screens(b["gallery"]) if self.has_screen(b, n)}
			if shown != want:
				self.warn("screenshots page, %s: missing %s; unexpected %s" % (
					b["id"], ", ".join(sorted(want - shown)) or "none", ", ".join(sorted(shown - want)) or "none"))

	LINK = re.compile(r'(?:href|src)="([^"]*)"')

	def check_links(self):
		"""Every relative href and src in the pages names a file the build wrote (a directory: its index.html).
		The templates' own links are checked nowhere else (docs links: tools/check_doc_links.py)."""
		broken = {}
		for dirpath, _, files in os.walk(self.out):
			for f in files:
				if not f.endswith(".html"):
					continue
				page = os.path.join(dirpath, f)
				with open(page, encoding="utf-8") as fh:
					text = fh.read()
				for url in self.LINK.findall(text):
					url = html.unescape(url).split("#")[0].split("?")[0]
					if not url or re.match(r"[a-z][a-z0-9+.-]*:|//|\{\{", url):
						continue
					target = os.path.normpath(os.path.join(dirpath, url))
					if os.path.isdir(target):
						target = os.path.join(target, "index.html")
					if not os.path.exists(target):
						broken.setdefault(url, os.path.relpath(page, self.out))
		for url, page in sorted(broken.items()):
			self.warn("broken link %s (first in %s)" % (url, page))

	def version_imports(self, js_dir):
		"""Stamp the build into the scripts' relative imports (./x.js -> ./x.js?v=<git>), so a browser that cached
		one module of an earlier build doesn't mix it with the new ones (the pages stamp the entry scripts)."""
		for dirpath, _, files in os.walk(js_dir):
			for f in files:
				if f.endswith(".js"):
					p = os.path.join(dirpath, f)
					with open(p, encoding="utf-8") as fh:
						text = fh.read()
					text = re.sub(r'((?:from|import)\s+"\.{1,2}/[^"?]+\.js)"', r'\1?v=%s"' % self.asset_version, text)
					with open(p, "w", encoding="utf-8") as fh:
						fh.write(text)

	def copy_firmware(self):
		for m, img in self.releases:
			d = os.path.join(self.out, "firmware", m["board"])
			os.makedirs(d, exist_ok=True)
			shutil.copy2(img, d)
			shutil.copy2(img[:-4] + ".json", d)
			with open(img, "rb") as f:
				if hashlib.sha256(f.read()).hexdigest() != m["image"]["sha256"]:
					raise repo.SourceError("%s doesn't match its manifest" % img)
		if self.releases:
			self.log("firmware: %d image(s) from %s" % (len(self.releases), self.firmware))

	def docs_assets(self, lang):
		n = self.docs.copy_assets(os.path.join(self.out, self.i18n.prefix(lang)))
		if lang == self.i18n.default:
			self.log("docs: %d pages, %d other files" % (len(self.docs.pages), n))

	def bands(self, lang, b):
		"""The board's transmit bands, then the receive-only ones its hardware can have: air band AM and broadcast FM
		unless the board lists the switch unsupported (a part the board's code doesn't drive yet still counts)."""
		extra = [self.t(lang, "radio.bands_" + k) for sw, k in (("AM_RX", "am"), ("BCAST_FM", "bcast"))
			 if sw not in b["unsupported"]]
		return " · ".join([b["bands"]] + extra)

	def radio_card(self, lang, b):
		"""The board's card, then one per other body it ships in (board.toml looks), each with its own drawing and the
		radios in it; the board's card lists the rest."""
		dmr = "DMR" in b["config"]["full"]
		moved = {n for lk in b["looks"] for n in [lk["name"]] + lk.get("also", [])}
		cards = [(b["id"], b["name"], [n for n in b["also"] if n not in moved])]
		cards += [("%s-%s" % (b["id"], lk["id"]), lk["name"], lk.get("also", [])) for lk in b["looks"]]
		return "".join(self.tpl.partial("radio_card", {
			"id": img, "name": name, "also": ", ".join(also),
			"badge": self.badge(lang, b["status"]), "bands": self.bands(lang, b),
			"modes": self.t(lang, "radio.modes_dmr_fm" if dmr else "radio.modes_fm"),
			"href": "radios/%s.html" % b["id"]}, lang) for img, name, also in cards)	# under {{home}}

	def request_card(self, lang):
		return self.tpl.partial("request_card", {"repo": html.escape(self.repo_url)}, lang)

	def dev_rows(self, lang, limit=4):
		"""The first radios not yet supported, furthest along first: boards still in bring-up, then ports on an open pull
		request, then the planned ones (each group in the survey's order)."""
		rows = [(BOARD_ORDER[b["status"]], '<a href="radios/%s.html">%s</a>' % (b["id"], html.escape(b["name"])),
			 self.badge(lang, b["status"]), b["name"]) for b in self.boards if b["status"] != "supported"]
		for p in self.planned:
			name = html.escape(p["radio"])
			if p["pr"]:
				name = '<a href="%s/pull/%d">%s</a>' % (html.escape(self.repo_url), p["pr"], name)
			rows.append((BOARD_ORDER[p["status"]], name, self.badge(lang, p["status"]), p["radio"]))
		rows.sort(key=lambda r: r[0])	# stable: the survey's order inside a group
		out = "".join('<li><span class="dev-name" title="%s">%s</span> %s</li>' % (html.escape(full), n, bd) for _, n, bd, full in rows[:limit])
		if len(rows) > limit:	# say it isn't the whole list
			out += '<li class="dev-more"><a href="radios/index.html">%s</a></li>' % html.escape(
				self.t(lang, "home.dev_more").replace("{n}", str(len(rows) - limit)))
		return out

	def home(self, lang):
		flagship = self.boards[0]
		ctx = {"flagship_id": flagship["id"], "flagship_name": flagship["name"],
		       "cards": "".join(self.radio_card(lang, b) for b in self.boards),
		       "dev_rows": self.dev_rows(lang), "request_card": self.request_card(lang), "variants": self.variant_cards(lang),
		       "radio_count": str(len(self.boards)), "feature_count": str(len(self.features))}
		self.page(lang, "index.html", self.t(lang, "home.title"), self.tpl.render("home", ctx, lang), "home")

	def variant_cards(self, lang):
		out = []
		for v in self.variants:
			out.append(self.tpl.partial("variant_card", {
				"id": v["id"], "title": self.vtitle(lang, v), "summary": self.vsummary(lang, v),
				"off": ", ".join(self.ftitle(lang, self.fmap[o["name"]]) for o in v["off"])
				or self.t(lang, "variant.nothing_off")}, lang))
		out.append(self.tpl.partial("variant_custom_card", {"title": self.t(lang, "variant.custom_title"),
			"text": self.t(lang, "variant.custom_text")}, lang))
		return "".join(out)

	def radios_index(self, lang):
		# one list: the radios in the tree and the planned ones, in order of how far each is
		entries = [(BOARD_ORDER[b["status"]], 0, b["name"], self.tpl.partial("radio_row", {
			"name": '<a href="%s.html">%s</a>' % (b["id"], html.escape(b["name"])), "also": ", ".join(b["also"]),
			"badge": self.badge(lang, b["status"]), "cpu": b["cpu"], "display": b["display"],
			"support": self.board_note(lang, b)}, lang)) for b in self.boards]
		for n, p in enumerate(self.planned):
			st = p["status"]
			note = self.note(lang, "planned.note." + docsmod.slug(p["radio"]), p["note"])
			if p["pr"]:	# a port on an open branch: its pull request
				note = ('<a href="%s/pull/%d">#%d</a>' % (html.escape(self.repo_url), p["pr"], p["pr"])
					+ (" " + note if note else ""))
			entries.append((BOARD_ORDER[st], 1 + n, p["radio"], self.tpl.partial("radio_row", {
				"name": html.escape(p["radio"]), "also": "", "badge": self.badge(lang, st), "cpu": "—", "display": "—",
				"support": note}, lang)))
		ctx = {"cards": "".join(self.radio_card(lang, b) for b in self.boards),
		       "rows": "".join(e[3] for e in sorted(entries, key=lambda e: e[:2])),
		       "request_card": self.request_card(lang)}
		self.page(lang, "radios/index.html", self.t(lang, "radios.title"),
			  self.tpl.render("radios", ctx, lang), "radios", nav="radios")

	def radio_page(self, lang, b):
		# features: one row per switch the board can run, a column per variant
		vhead = "".join('<th scope="col">%s</th>' % html.escape(self.vtitle(lang, v)) for v in self.variants)
		groups = []
		for g in self.groups:
			rows = []
			for f in self.features:
				if f["group"] != g or self.feature_state(b, f["name"]) == "na" or f["group"] in BENCH_ONLY:
					continue
				cells = "".join(self.state_cell(lang, self.feature_state(b, f["name"], v["id"]))
						for v in self.variants)
				rows.append('<tr><th scope="row"><span class="ftitle">%s</span><code>%s</code></th>%s</tr>'
					    % (html.escape(self.ftitle(lang, f)), f["name"], cells))
			if rows:
				groups.append('<tbody><tr class="group"><th colspan="%d" scope="rowgroup">%s</th></tr>%s</tbody>'
					      % (len(self.variants) + 1, html.escape(self.group_title(lang, g)), "".join(rows)))
		na = [f for f in self.features if self.feature_state(b, f["name"]) == "na" and f["group"] not in BENCH_ONLY]
		# screenshots: six that show the look and the main features, the first featured ones this radio has, at twice
		# their pixel size (crisp: pixelated), in two rows of three
		with open(repo.path("docs", "ux", "screens", b["gallery"], "boot.png"), "rb") as fh:
			sw, sh = device.png_size(fh.read(32))
		items = []
		for n in self.screens["featured"]:
			if not self.has_screen(b, n):
				continue
			cap = self.screen_caption(lang, n)
			items.append('<figure class="shot"><button type="button" class="shot-show" title="%s" data-src="{{root}}docs/'
				     'ux/screens/%s/%s.png"><img src="{{root}}docs/ux/screens/%s/%s.png" alt="%s" width="%d" '
				     'height="%d" loading="lazy"></button><figcaption>%s</figcaption></figure>'
				     % (html.escape(self.t(lang, "radio.show_on_radio")), b["gallery"], n, b["gallery"], n,
					html.escape(cap), 2 * sw, 2 * sh, html.escape(cap)))
			if len(items) == 6:
				break
		shots = ['<div class="shots">%s</div>' % "".join(items)] if items else []
		shots.append('<p class="shots-all"><a href="{{home}}screenshots/index.html#%s">%s →</a></p>'
			     % (b["id"], html.escape(self.t(lang, "radio.screens_all"))))
		docs_links = "".join('<li><a href="{{home}}%s">%s</a></li>'
				     % (docsmod.site_path(p), html.escape(self.t(lang, "radio.doc." + k)))
				     for k, p in sorted(b["docs"].items(), key=lambda kv: DOC_ORDER.index(kv[0])))
		fl = b["flash"]
		can_flash = ((fl["backend"] == "anyroad-dfu" and bool(self.flash["boards"].get(fl["board"], {}).get("key_sha256")))
			     or (fl["backend"] == "c7000-stub" and fl["board"] in self.flash["c7000"]["boards"]))
		g = b["frame_geometry"]
		pct = g["screen_pct"]
		ctx = {"id": b["id"], "name": b["name"], "also": ", ".join(b["also"]) or "—",
		       "frame_aspect": "%g" % g["aspect"],
		       "screen_style": "left:%g%%;top:%g%%;width:%g%%;height:%g%%" % (pct["left"], pct["top"], pct["width"], pct["height"]),
		       "badge": self.badge(lang, b["status"]), "support": self.board_note(lang, b),
		       "bands": self.bands(lang, b), "power": b["power"], "kind": self.t(lang, "radio.kind." + b["kind"]),
		       "cpu": b["cpu"], "display": b["display"], "volts": self.t(lang, "radio.volts." + b["volts"]),
		       "clock": self.t(lang, "radio.clock." + b["clock"]),
		       "variants": self.variants_html(lang, b),
		       "variant_head": vhead, "feature_groups": "".join(groups),
		       "na_list": ", ".join(html.escape(self.ftitle(lang, f)) for f in na) or "—",
		       "shots": "".join(shots), "docs_links": docs_links, "gallery": b["gallery"],
		       "flash_cta": self.tpl.partial("flash_cta" if can_flash else "flash_soon",
						     {"id": b["id"], "name": b["name"]}, lang),
		       # the C7000 radios need no keys held: the page meets the chip's boot ROM when the radio is switched on
		       "bootloader": "".join("<li>%s</li>" % html.escape(s) for s in (
			       [self.t(lang, "radio.c7_step%d" % i) for i in (1, 2, 3)] if fl["backend"] == "c7000-stub"
			       else fl.get("bootloader", [])))}
		self.page(lang, "radios/%s.html" % b["id"], b["name"], self.tpl.render("radio", ctx, lang), "radio",
			  description=self.t(lang, "radio.description", name=b["name"]), nav="radios")

	def screenshots_page(self, lang):
		"""Every screen each radio's build has, a section per radio, grouped as screens.json groups them, in its order.
		A color page's shots in other highlight colors and themes follow it, hidden until the visitor asks for them.
		Without the script the first radio shows, or the one the address names (#<radio>, CSS :target)."""
		order = list(self.screens["screens"])
		picks, sections = [], []
		for b in self.boards:
			with open(repo.path("docs", "ux", "screens", b["gallery"], "boot.png"), "rb") as fh:
				sw, sh = device.png_size(fh.read(32))
			# a small display at twice its pixels, kept square; a QVGA one as it is
			scale = 2 if sw <= 160 else 1
			names = [n for n in b["screens"] if self.has_screen(b, n)]
			names.sort(key=lambda n: (order.index(n.partition("__")[0]), n.partition("__")[2]))
			groups = []
			for g in self.screens["groups"]:
				items = []
				for n in names:
					if self.screen(n)["group"] != g:
						continue
					cap = html.escape(self.screen_caption(lang, n))
					items.append('<figure class="gshot%s"><img src="{{root}}docs/ux/screens/%s/%s.png" alt="%s" width="%d" '
						     'height="%d" loading="lazy"%s><figcaption>%s</figcaption></figure>'
						     % (" gshot-variant" if "__" in n else "", b["gallery"], n, cap, scale * sw, scale * sh,
							' class="px"' if scale > 1 else "", cap))
				if items:
					groups.append('<h3>%s</h3><div class="gshots">%s</div>'
						      % (html.escape(self.t(lang, "screens.group." + g)), "".join(items)))
			base = sum(1 for n in names if "__" not in n)
			count = html.escape(self.t(lang, "screens.count", n=base, display=b["display"]))
			sections.append('<section class="rshots" id="%s" aria-labelledby="rshots-%s" style="--shot-w:%dpx">'
					'<h2 id="rshots-%s">%s</h2><p class="muted small">%s · <a href="{{home}}radios/%s.html">%s</a></p>'
					'%s</section>' % (b["id"], b["id"], scale * sw, b["id"], html.escape(b["name"]), count, b["id"],
							  html.escape(self.t(lang, "screens.radio_page", name=b["name"])), "".join(groups)))
			# site.js marks the radio shown (aria-current); without it the section's heading names the radio
			picks.append('<a href="#%s">%s</a>' % (b["id"], html.escape(b["name"])))
		ctx = {"picks": "".join(picks), "sections": "".join(sections)}
		self.page(lang, "screenshots/index.html", self.t(lang, "screens.title"),
			  self.tpl.render("screenshots", ctx, lang), "screenshots", description=self.t(lang, "screens.lead"),
			  nav="screenshots")

	def state_cell(self, lang, state):
		return '<td class="st st-%s"><span aria-hidden="true">%s</span><span class="sr">%s</span></td>' % (
			state, {"on": "●", "off": "○", "nw": "…", "na": "✕"}[state], html.escape(self.t(lang, "state." + state)))

	def group_title(self, lang, g):
		key = "group." + docsmod.slug(g)
		return self.i18n.opt(lang, key, g)

	def features_page(self, lang):
		bhead = "".join('<th scope="col"><a href="{{home}}radios/%s.html">%s</a></th>'
				% (b["id"], html.escape(b["model"])) for b in self.boards)
		groups = []
		for g in self.groups:
			if g in BENCH_ONLY:
				continue
			rows = []
			for f in self.features:
				if f["group"] != g:
					continue
				cells = "".join(self.state_cell(lang, self.feature_state(b, f["name"])) for b in self.boards)
				chips = []
				for v in self.variants[1:]:
					if not any(f["name"] in b["config"][v["id"]] for b in self.boards
						   if f["name"] in b["config"]["full"]) and any(
							f["name"] in b["config"]["full"] for b in self.boards):
						chips.append('<span class="chip chip-out">%s</span>'
							     % html.escape(self.t(lang, "features.not_in", variant=self.vtitle(lang, v))))
				needs = f["needs"]["all"] + (["(%s)" % " | ".join(f["needs"]["any"])] if f["needs"]["any"] else [])
				size = " · ".join("%s %s B" % (k, v) for k, v in f["bytes"].items())
				rows.append(self.tpl.partial("feature_row", {
					"name": f["name"], "title": self.ftitle(lang, f), "help": self.fhelp(lang, f),
					"notes": f["notes"], "needs": ", ".join(needs) or "—", "size": size or "—",
					"cells": cells, "chips": "".join(chips), "anchor": f["name"].lower()}, lang))
			groups.append('<tbody><tr class="group"><th colspan="%d" scope="rowgroup" id="%s">%s</th></tr>%s</tbody>'
				      % (len(self.boards) + 1, docsmod.slug(g), html.escape(self.group_title(lang, g)),
					 "".join(rows)))
		ctx = {"board_head": bhead, "groups": "".join(groups), "variants": self.variant_cards(lang),
		       "toc": "".join('<a href="#%s">%s</a>' % (docsmod.slug(g), html.escape(self.group_title(lang, g)))
				      for g in self.groups if g not in BENCH_ONLY)}
		self.page(lang, "features/index.html", self.t(lang, "features.title"),
			  self.tpl.render("features", ctx, lang), "features", nav="features")

	def flash_page(self, lang):
		# the beta notice's text is in the page itself (beta_notice.html), not needed by the scripts
		subset = {k: v for k, v in self.i18n.subset(lang, ("flash.", "state.", "status.", "variant.", "group.", "report.", "editor.qdmr.note.")).items()
			  if not k.startswith("flash.beta.")}
		strings = json.dumps(subset, ensure_ascii=False).replace("</", "<\\/")
		scripts = ('<script type="application/json" id="i18n">%s</script>'
			   '<script type="module" src="{{root}}static/js/flasher/app.js?v=%s"></script>' % (strings, self.asset_version))
		# the beta notice, opened once per browser session (static/js/flasher/beta.js)
		beta = self.tpl.partial("beta_notice", {"repo": self.repo_url, "community": COMMUNITY_URL, "home": "{{home}}"}, lang)	# page() fills in home
		self.page(lang, "flash/index.html", self.t(lang, "flash.title"), self.tpl.render("flash", {"beta": beta}, lang),
			  "flash", scripts=scripts, nav="flash", footer=self.tpl.partial("flash_log", {"errors": self.errors_paragraph(lang)}, lang))

	def tools_page(self, lang):
		strings = json.dumps(self.i18n.subset(lang, ("tools.", "flash.convert.")),
				     ensure_ascii=False).replace("</", "<\\/")
		scripts = ('<script type="application/json" id="i18n">%s</script>'
			   '<script type="module" src="{{root}}static/js/tools.js?v=%s"></script>' % (strings, self.asset_version))
		self.page(lang, "tools/index.html", self.t(lang, "tools.title"), self.tpl.render("tools", {}, lang),
			  "tools", scripts=scripts, nav="tools")

	def codeplug_page(self, lang):
		strings = json.dumps(self.i18n.subset(lang, ("editor.", "flash.part.", "report.")), ensure_ascii=False).replace("</", "<\\/")
		scripts = ('<script type="application/json" id="i18n">%s</script>'
			   '<script type="module" src="{{root}}static/js/editor/app.js?v=%s"></script>' % (strings, self.asset_version))
		self.page(lang, "codeplug/index.html", self.t(lang, "editor.title"), self.tpl.render("codeplug", {}, lang),
			  "codeplug", description=self.t(lang, "editor.lead"), scripts=scripts, nav="codeplug", footer=self.reports_footer(lang))

	def errors_paragraph(self, lang):
		"""The error tracker's note, folded into the flasher's one privacy note: only when the build has an address."""
		return "<p>%s</p>" % html.escape(self.t(lang, "report.privacy_text")) if self.sentry_dsn else ""

	def reports_footer(self, lang):
		"""The privacy note on automatic error reports, on the pages that send them: only when the build has an address."""
		return self.tpl.partial("error_reports", {}, lang) if self.sentry_dsn else ""

	def docs_pages(self, lang):
		prefix = self.i18n.prefix(lang)
		sections = []
		for sec in docsmod.SECTIONS:
			pages = self.docs.section_pages(sec)
			if not pages:
				continue
			links = "".join('<li><a href="{{home}}%s">%s</a></li>' % (p["out"], html.escape(p["title"]))
					for p in pages)
			sections.append(self.tpl.partial("docs_section", {
				"title": self.t(lang, "docs.section." + sec), "blurb": self.t(lang, "docs.blurb." + sec),
				"links": links}, lang))
		self.page(lang, "docs/index.html", self.t(lang, "docs.title"),
			  self.tpl.render("docs_index", {"sections": "".join(sections)}, lang), "docs", nav="docs")
		for sec in docsmod.SECTIONS:
			pages = self.docs.section_pages(sec)
			for i, p in enumerate(pages):
				if p["out"] == "docs/index.html":
					continue	# docs/README.md, if one appears, would shadow the docs home
				body, toc, fallback = self.docs.render(p, lang)
				here = posixpath.dirname(p["out"])
				side = "".join('<li><a href="%s"%s>%s</a></li>' % (
					posixpath.relpath(q["out"], here), ' aria-current="page"' if q is p else "",
					html.escape(q["title"])) for q in pages)
				toc_html = "".join('<li class="l%d"><a href="#%s">%s</a></li>' % (lvl, hid, html.escape(text))
						   for lvl, hid, text in toc)
				prev_next = ""
				if sec == "user":
					prev = pages[i - 1] if i > 0 else None
					nxt = pages[i + 1] if i + 1 < len(pages) else None
					prev_next = "".join('<a class="%s" href="%s">%s</a>' % (cls, posixpath.relpath(q["out"], here),
												 html.escape(q["title"]))
							    for cls, q in (("prev", prev), ("next", nxt)) if q)
				ctx = {"body": body, "side": side, "toc": toc_html, "section": self.t(lang, "docs.section." + sec),
				       "source": "%s/blob/%s/%s" % (self.repo_url, self.ref, p["src"]), "prev_next": prev_next,
				       "fallback": ('<p class="notice">%s</p>' % html.escape(self.t(lang, "docs.fallback")))
				       if fallback else ""}
				self.page(lang, p["out"], p["title"], self.tpl.render("doc", ctx, lang), "doc", nav="docs")

	def not_found(self, lang):
		if lang == self.i18n.default:
			self.page(lang, "404.html", self.t(lang, "notfound.title"), self.tpl.render("notfound", {}, lang), "404")
