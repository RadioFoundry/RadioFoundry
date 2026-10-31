# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""String catalogs: site/i18n/<lang>.json, flat "key": "text" maps, and site/i18n/languages.json. The default
language's catalog must have every key the templates and scripts use; another language falls back to it key by key
(docs/dev/website.md "Translating").

A count gets plural forms: key.one, key.other (and key.few, key.many, ... where the language has them), picked by the
number passed as n (plural()). English needs only one and other.

Texts that come from the repository (feature help, variant reasons, radio facts) are English at the source. A
catalog can override them by key (feature.<SWITCH>.title, feature.<SWITCH>.help, variant.<id>.title,
variant.<id>.summary) without touching the source.
"""
import json
import os
import re

from . import repo

# CLDR plural categories of whole numbers, by language: what isn't listed uses English's (one for 1, else other).
# Add a language here when its rule differs; the scripts use the browser's Intl.PluralRules instead.
PLURAL_RULES = {
	"fr": lambda n: "one" if n in (0, 1) else "other",
	"pt": lambda n: "one" if n in (0, 1) else "other",
	"es": lambda n: "one" if n == 1 else "many" if n and n % 1000000 == 0 else "other",
	"ja": lambda n: "other", "ko": lambda n: "other", "zh": lambda n: "other", "vi": lambda n: "other",
	"pl": lambda n: "one" if n == 1 else "few" if n % 10 in (2, 3, 4) and n % 100 not in (12, 13, 14) else "many",
	"ru": lambda n: ("one" if n % 10 == 1 and n % 100 != 11 else "few" if n % 10 in (2, 3, 4) and n % 100 not in (12, 13, 14)
			 else "many"),
	"uk": lambda n: ("one" if n % 10 == 1 and n % 100 != 11 else "few" if n % 10 in (2, 3, 4) and n % 100 not in (12, 13, 14)
			 else "many"),
}
PLURAL_CATEGORIES = ("zero", "one", "two", "few", "many", "other")


def plural(lang, n):
	"""The plural category of the whole number n in a language."""
	return PLURAL_RULES.get(lang.split("-")[0], lambda n: "one" if n == 1 else "other")(n)


def plural_base(key):
	"""The key of a plural family (x.one -> x), or None for a plain key."""
	base, _, cat = key.rpartition(".")
	return base if base and cat in PLURAL_CATEGORIES else None


class Catalogs:
	def __init__(self, directory):
		with open(os.path.join(directory, "languages.json"), encoding="utf-8") as f:
			meta = json.load(f)
		self.default = meta["default"]
		self.languages = meta["languages"]		# [{code, name, dir}]
		self.codes = [l["code"] for l in self.languages]
		self.cat = {}
		for code in self.codes:
			with open(os.path.join(directory, code + ".json"), encoding="utf-8") as f:
				self.cat[code] = {k: v for k, v in json.load(f).items() if not k.startswith("_")}
		self.missing = set()

	def t(self, lang, key, **fmt):
		text = self.cat.get(lang, {}).get(key)
		if text is None:
			text = self.cat[self.default].get(key)
		if text is None:
			text = self.plural_text(lang, key, fmt.get("n"))
		if text is None:
			self.missing.add(key)
			text = key
		return text.format(**fmt) if fmt else text

	def plural_text(self, lang, key, n):
		"""A plural family's form for n: the language's own forms when it has the family, else the default's."""
		for code in (lang, self.default):
			cat = self.cat.get(code, {})
			if key + ".other" in cat:
				form = cat.get("%s.%s" % (key, plural(code, n))) if isinstance(n, int) else None
				return form or cat[key + ".other"]
		return None

	def has(self, lang, key):
		return key in self.cat.get(lang, {}) or key in self.cat[self.default]

	def opt(self, lang, key, fallback):
		"""A catalog override of a text that comes from the repository, else the repository text."""
		return self.cat.get(lang, {}).get(key) or self.cat[self.default].get(key) or fallback

	def prefix(self, lang):
		"""URL prefix of a language: the default lives at the root."""
		return "" if lang == self.default else lang + "/"

	def subset(self, lang, prefixes):
		"""The keys under the given prefixes, for scripts (the default language filled in)."""
		out = {}
		own = self.cat.get(lang, {})
		for src in (self.cat[self.default], own):
			for k, v in src.items():
				if not any(k.startswith(p) for p in prefixes):
					continue
				# a plural family the language has: its own forms only (another language has other categories)
				base = plural_base(k)
				if src is not own and base and base + ".other" in own:
					continue
				out[k] = v
		return out

	def unused_check(self):
		return sorted(self.missing)


def load(directory=None):
	return Catalogs(directory or repo.path("site", "i18n"))


PLACEHOLDER = re.compile(r"\{\{.*?\}\}|\{\w+\}")
# the HTML a string may carry; anything else in angle brackets is text ("<radio address>")
TAG = re.compile(r"</?(?:a|abbr|b|br|code|em|i|kbd|small|span|strong)\b[^<>]*>")


def _pairs(pairs):
	keys = [k for k, _ in pairs]
	dup = sorted({k for k in keys if keys.count(k) > 1})
	if dup:
		raise ValueError("duplicate keys: %s" % ", ".join(dup))
	return dict(pairs)


def check(directory=None):
	"""What is wrong with the catalogs (docs/dev/website.md "Translating"), as messages; [] when nothing. Every file is
	valid JSON without a key twice; languages.json names the default and each language's file; a language other than
	the default has only keys the default has (a plural form also where the default has that family: key.other), each
	a string with the default's placeholders ({name}, {{...}}) and HTML tags. A plural form other than "other" may leave
	{n} out ("one file")."""
	directory = directory or repo.path("site", "i18n")
	out = []

	def read(name):
		try:
			with open(os.path.join(directory, name), encoding="utf-8") as f:
				return json.load(f, object_pairs_hook=_pairs)
		except (OSError, ValueError) as e:
			out.append("site/i18n/%s: %s" % (name, e))
			return None
	meta = read("languages.json")
	if not meta:
		return out
	codes = [l.get("code") for l in meta.get("languages", [])]
	default = meta.get("default")
	if default not in codes:
		out.append("site/i18n/languages.json: the default %r isn't among the languages" % default)
		return out
	for l in meta["languages"]:
		if not re.fullmatch(r"[a-z]{2,3}(-[A-Za-z0-9]+)*", l.get("code") or "") or not l.get("name") or l.get("dir", "ltr") not in ("ltr", "rtl"):
			out.append("site/i18n/languages.json: %s needs a code (\"es\", \"pt-BR\"), a name and dir ltr or rtl" % l)
	base = read(default + ".json")
	if base is None:
		return out
	for code in codes:
		if code == default:
			continue
		cat = read(code + ".json")
		if cat is None:
			continue
		for key, text in cat.items():
			if key.startswith("_"):
				continue
			where = "site/i18n/%s.json %s" % (code, key)
			fam = plural_base(key)
			if key in base:
				src = base[key]
			elif fam and fam + ".other" in base:
				src = base[fam + ".other"]
			else:
				out.append("%s: not in %s.json" % (where, default) + (" (add the plural forms there first)" if fam else ""))
				continue
			if not isinstance(text, str):
				out.append("%s: not a string" % where)
				continue
			want, got = set(PLACEHOLDER.findall(src)), set(PLACEHOLDER.findall(text))
			if fam and fam + ".other" in base and not key.endswith(".other") and "{n}" in want:
				got.add("{n}")	# "one file": a form for one count may leave the number out
			if want != got:
				out.append("%s: placeholders %s, the %s text has %s" % (where, " ".join(sorted(got)) or "none", default,
										" ".join(sorted(want)) or "none"))
			if sorted(TAG.findall(src)) != sorted(TAG.findall(text)):
				out.append("%s: HTML tags differ from the %s text" % (where, default))
	return out


def missing(directory=None):
	"""The keys a language other than the default lacks, as messages (one a language). The site shows the default's
	text for them, so the build passes; a pull request leaves none (docs/dev/translating.md). A plural family needs
	only its .other form."""
	directory = directory or repo.path("site", "i18n")
	try:
		with open(os.path.join(directory, "languages.json"), encoding="utf-8") as f:
			meta = json.load(f)
		with open(os.path.join(directory, meta["default"] + ".json"), encoding="utf-8") as f:
			base = json.load(f)
	except (OSError, ValueError, KeyError):
		return []	# check() reports it
	want = sorted({(plural_base(k) + ".other") if plural_base(k) and plural_base(k) + ".other" in base else k
		       for k in base if not k.startswith("_")})
	out = []
	for l in meta.get("languages", []):
		if l.get("code") == meta["default"]:
			continue
		try:
			with open(os.path.join(directory, l["code"] + ".json"), encoding="utf-8") as f:
				cat = json.load(f)
		except (OSError, ValueError, KeyError):
			continue
		lack = [k for k in want if k not in cat]
		if lack:
			out.append("site/i18n/%s.json: %d key(s) not translated: %s%s" % (l["code"], len(lack), ", ".join(lack[:10]),
											   " ..." if len(lack) > 10 else ""))
	return out


if __name__ == "__main__":
	import sys
	problems = check(sys.argv[1] if len(sys.argv) > 1 else None)
	problems += missing(sys.argv[1] if len(sys.argv) > 1 else None)
	for p in problems:
		print(p)
	print("i18n: %s" % ("%d problem(s)" % len(problems) if problems else "catalogs ok"))
	sys.exit(1 if problems else 0)
