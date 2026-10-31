# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The site's string catalogs (docs/dev/website.md "Translating"): the check every catalog must pass
(site/rfoundrysite/i18n.py check), plural forms, and a whole build with a second language made up for the test.
Needs site/requirements.txt (tools/ci.sh runs it with the site build's Python).

    python3 site/tests/i18n_test.py
"""
import json
import os
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rfoundrysite import build, i18n, repo  # noqa: E402

EN = repo.path("site", "i18n", "en.json")


def catalogs(tmp, extra, languages=None, raw=None):
	"""site/i18n in tmp: the real en.json, a test language "es" with the keys in extra (or the text raw)."""
	shutil.copy(EN, os.path.join(tmp, "en.json"))
	langs = languages or [{"code": "en", "name": "English", "dir": "ltr", "locale": "en-US"},
			      {"code": "es", "name": "Español", "dir": "ltr", "locale": "es-ES"}]
	with open(os.path.join(tmp, "languages.json"), "w", encoding="utf-8") as f:
		json.dump({"default": "en", "languages": langs}, f)
	with open(os.path.join(tmp, "es.json"), "w", encoding="utf-8") as f:
		f.write(raw if raw is not None else json.dumps(extra, ensure_ascii=False))
	return tmp


class Check(unittest.TestCase):
	def problems(self, extra=None, **kw):
		with tempfile.TemporaryDirectory() as tmp:
			return i18n.check(catalogs(tmp, extra or {}, **kw))

	def test_repository_catalogs_pass(self):
		self.assertEqual(i18n.check(), [])
		self.assertEqual(i18n.missing(), [])	# every language complete: a pull request leaves none

	def test_missing_keys_listed(self):
		with tempfile.TemporaryDirectory() as tmp:
			catalogs(tmp, {"nav.radios": "Radios"})
			out = i18n.missing(tmp)
		self.assertEqual(len(out), 1)
		self.assertRegex(out[0], r"^site/i18n/es\.json: \d+ key\(s\) not translated: ")
		self.assertNotIn("nav.radios,", out[0])
		with tempfile.TemporaryDirectory() as tmp:
			with open(EN, encoding="utf-8") as f:
				full = {k: v for k, v in json.load(f).items() if not k.startswith("_")}
			# a plural family needs only .other (a language may have other forms than English)
			self.assertEqual(i18n.missing(catalogs(tmp, {k: v for k, v in full.items() if not k.endswith(".one")})), [])

	def test_good_translation_passes(self):
		self.assertEqual(self.problems({"_about": "x", "nav.radios": "Radios", "screens.count.one": "una captura · {display}",
						"screens.count.other": "{n} capturas · {display}",
						"screens.count.many": "{n} de capturas · {display}"}), [])

	def test_unknown_key(self):
		p = self.problems({"nav.nothing": "x"})
		self.assertEqual(len(p), 1)
		self.assertIn("nav.nothing: not in en.json", p[0])

	def test_plural_of_a_plain_key_is_refused(self):
		p = self.problems({"nav.radios.one": "Radio"})
		self.assertIn("add the plural forms there first", p[0])

	def test_placeholders_kept(self):
		self.assertIn("placeholders", self.problems({"radio.description": "Una radio"})[0])
		self.assertIn("placeholders", self.problems({"radio.description": "{name} {extra}"})[0])
		# the count's form for one may leave the number out, the others may not
		self.assertEqual(self.problems({"screens.count.one": "una · {display}"}), [])
		self.assertIn("placeholders", self.problems({"screens.count.other": "capturas · {display}"})[0])

	def test_html_tags_kept(self):
		with tempfile.TemporaryDirectory() as tmp:
			catalogs(tmp, {})
			p = os.path.join(tmp, "en.json")
			with open(p, encoding="utf-8") as f:
				en = json.load(f)
			en["test.tagged"] = "Press <b>Start</b> at <radio address>"
			with open(p, "w", encoding="utf-8") as f:
				json.dump(en, f)
			with open(os.path.join(tmp, "es.json"), "w", encoding="utf-8") as f:
				json.dump({"test.tagged": "Pulse Iniciar en <dirección>"}, f)
			self.assertIn("HTML tags", i18n.check(tmp)[0])
			with open(os.path.join(tmp, "es.json"), "w", encoding="utf-8") as f:
				json.dump({"test.tagged": "Pulse <b>Iniciar</b> en <dirección de la radio>"}, f)
			self.assertEqual(i18n.check(tmp), [])

	def test_bad_json_and_duplicates(self):
		self.assertIn("es.json", self.problems(raw="{\"nav.radios\": ")[0])
		self.assertIn("duplicate keys: nav.radios", self.problems(raw='{"nav.radios": "a", "nav.radios": "b"}')[0])
		self.assertIn("not a string", self.problems({"nav.radios": 3})[0])

	def test_languages_file(self):
		p = self.problems(languages=[{"code": "en", "name": "English"}, {"code": "Spanish", "name": "Español"}])
		self.assertTrue(any("needs a code" in x for x in p), p)


class Plurals(unittest.TestCase):
	def setUp(self):
		self.tmp = tempfile.mkdtemp()
		catalogs(self.tmp, {"screens.count.one": "una captura · {display}", "screens.count.other": "{n} capturas · {display}"},
			 languages=[{"code": "en", "name": "English"}, {"code": "es", "name": "Español"}, {"code": "fr", "name": "Français"}])
		shutil.copy(os.path.join(self.tmp, "en.json"), os.path.join(self.tmp, "fr.json"))
		self.c = i18n.load(self.tmp)

	def tearDown(self):
		shutil.rmtree(self.tmp)

	def test_forms(self):
		self.assertEqual(self.c.t("en", "screens.count", n=1, display="d"), "1 screen · d")
		self.assertEqual(self.c.t("en", "screens.count", n=0, display="d"), "0 screens · d")
		self.assertEqual(self.c.t("es", "screens.count", n=1, display="d"), "una captura · d")
		self.assertEqual(self.c.t("es", "screens.count", n=4, display="d"), "4 capturas · d")
		self.assertEqual(i18n.plural("fr", 0), "one")
		self.assertEqual(i18n.plural("pt-BR", 1), "one")
		self.assertEqual(i18n.plural("ru", 22), "few")

	def test_scripts_get_the_language_forms_only(self):
		es = self.c.subset("es", ("screens.",))
		self.assertEqual(es["screens.count.other"], "{n} capturas · {display}")
		self.assertEqual(es["screens.count.one"], "una captura · {display}")
		self.assertEqual(self.c.subset("en", ("screens.",))["screens.count.one"], "{n} screen · {display}")


class SecondLanguage(unittest.TestCase):
	"""A whole build with a made-up Spanish catalog: pages under /es/, the switcher, the alternate links, and English
	for every key the catalog lacks."""

	@classmethod
	def setUpClass(cls):
		cls.tmp = tempfile.mkdtemp()
		cls.out = os.path.join(cls.tmp, "site")
		os.makedirs(os.path.join(cls.tmp, "i18n"))
		cats = catalogs(os.path.join(cls.tmp, "i18n"),
				{"nav.radios": "Radios (es)", "home.title": "Inicio", "flash.title": "Instalar (es)",
				 "screens.count.one": "una captura · {display}", "screens.count.other": "{n} capturas · {display}"})
		site = build.Site(cls.out, "https://github.com/o/r", "main", log=lambda *a: None, i18n_dir=cats)
		cls.warnings = site.build()

	@classmethod
	def tearDownClass(cls):
		shutil.rmtree(cls.tmp)

	def read(self, rel):
		with open(os.path.join(self.out, rel), encoding="utf-8") as f:
			return f.read()

	def test_builds_clean(self):
		# the build's other checks (a feature without a feature-matrix row) are the main build's, not this test's
		self.assertEqual([w for w in self.warnings if "feature-matrix" not in w], [])

	def test_pages_under_the_code(self):
		for rel in ("index.html", "radios/index.html", "flash/index.html", "codeplug/index.html", "tools/index.html",
			    "features/index.html", "screenshots/index.html", "docs/index.html", "404.html"):
			page = self.read("es/" + rel)
			self.assertIn('<html lang="es" dir="ltr">', page, rel)
		self.assertIn('<html lang="en" dir="ltr">', self.read("index.html"))

	def test_switcher_and_alternates(self):
		page = self.read("es/radios/index.html")
		sw = re.search(r'<details class="lang-switch">(.*?)</details>', page).group(1)
		self.assertIn('>ES</summary>', sw)	# the menu shows the page's language
		self.assertRegex(sw, r'href="\.\./\.\./radios/index\.html" lang="en" hreflang="en" data-hint="[^"]+" data-go="[^"]+" '
				 r'data-no="[^"]+">English</a>')
		self.assertRegex(sw, r'href="\.\./\.\./es/radios/index\.html" aria-current="true" lang="es" hreflang="es" '
				 r'data-hint="[^"]+"[^>]*>Español</a>')
		self.assertIn('<link rel="alternate" hreflang="en" href="https://radiofoundry.org/radios/">', page)
		self.assertIn('<link rel="alternate" hreflang="es" href="https://radiofoundry.org/es/radios/">', page)
		self.assertIn('<link rel="alternate" hreflang="x-default" href="https://radiofoundry.org/radios/">', page)
		self.assertIn('<meta property="og:locale" content="es_ES">', page)
		self.assertIn('lang-switch', self.read("radios/index.html"))

	def test_translated_and_fallback(self):
		page = self.read("es/index.html")
		self.assertIn("<title>Inicio · RadioFoundry</title>", page)
		self.assertIn(">Radios (es)</a>", page)
		self.assertIn(">Features</a>", page)	# not in the catalog: English
		# links between pages stay in the language
		self.assertIn('href="../es/features/index.html"', page)
		self.assertIn("capturas · ", self.read("es/screenshots/index.html"))

	def test_scripts_get_the_language(self):
		strings = json.loads(re.search(r'<script type="application/json" id="i18n">(.*?)</script>',
					       self.read("es/flash/index.html"), re.S).group(1).replace("<\\/", "</"))
		self.assertEqual(strings["flash.title"], "Instalar (es)")
		self.assertIn("flash.build.pack_text", strings)	# English fills in

	def test_english_docs_marked(self):
		page = self.read("es/docs/user/02-installing.html")
		self.assertIn('<p class="notice">', page)
		self.assertIn('<div lang="en">', page)


if __name__ == "__main__":
	unittest.main()
