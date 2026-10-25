# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""String catalogs: site/i18n/<lang>.json, flat "key": "text" maps, and site/i18n/languages.json. The default
language's catalog must have every key the templates and scripts use; another language falls back to it key by key
(docs/dev/website.md "Translating").

Texts that come from the repository (feature help, variant reasons, radio facts) are English at the source. A
catalog can override them by key (feature.<SWITCH>.title, feature.<SWITCH>.help, variant.<id>.title,
variant.<id>.summary) without touching the source.
"""
import json
import os

from . import repo


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
			self.missing.add(key)
			text = key
		return text.format(**fmt) if fmt else text

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
		for src in (self.cat[self.default], self.cat.get(lang, {})):
			for k, v in src.items():
				if any(k.startswith(p) for p in prefixes):
					out[k] = v
		return out

	def unused_check(self):
		return sorted(self.missing)


def load():
	return Catalogs(repo.path("site", "i18n"))
