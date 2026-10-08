# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""A very small template language, so the page markup lives in site/templates/ where a designer can rework it
without touching Python (docs/dev/website.md "Templates").

  {{ name }}        the value, HTML escaped (dotted names reach into dicts: {{ radio.name }})
  {{{ name }}}      the value as it is (HTML the generator built)
  {{t key}}         the translation of key in the page's language, escaped (site/i18n/<lang>.json)
  {{> partial }}    site/templates/partials/<partial>.html, rendered with the same values
  {{root}}          the relative path from this page to the site root ("", "../", ...): static files, data
  {{home}}          the same to the root of the page's language ("../", "../de/"): links to other pages

Loops and conditions stay in Python: the generator renders a partial per item and passes the joined HTML.
"""
import html
import os
import re

TOKEN = re.compile(r"\{\{\{\s*([\w.]+)\s*\}\}\}|\{\{(t|>)\s+([\w.\-]+)\s*\}\}|\{\{\s*([\w.]+)\s*\}\}")


class TemplateError(Exception):
	pass


class Templates:
	def __init__(self, directory, translate):
		self.dir = directory
		self.translate = translate	# (lang, key) -> text
		self._cache = {}

	def source(self, name):
		if name not in self._cache:
			p = os.path.join(self.dir, name + ".html")
			if not os.path.isfile(p):
				raise TemplateError("no template %s" % p)
			with open(p, encoding="utf-8") as f:
				self._cache[name] = f.read()
		return self._cache[name]

	def render(self, name, ctx, lang):
		return self._render(self.source(name), ctx, lang, name)

	def partial(self, name, ctx, lang):
		return self.render("partials/" + name, ctx, lang)

	def each(self, name, items, lang, **common):
		return "".join(self.partial(name, dict(common, **item), lang) for item in items)

	def _render(self, src, ctx, lang, where):
		def sub(m):
			raw, kind, arg, name = m.groups()
			if raw:
				return str(lookup(ctx, raw, where))
			if kind == "t":
				return html.escape(self.translate(lang, arg))
			if kind == ">":
				return self.partial(arg, ctx, lang)
			if name in ("root", "home"):
				return "{{%s}}" % name	# filled in last, when the page's depth is known (Site.page)
			return html.escape(str(lookup(ctx, name, where)))
		return TOKEN.sub(sub, src)


def lookup(ctx, dotted, where):
	v = ctx
	for part in dotted.split("."):
		if isinstance(v, dict) and part in v:
			v = v[part]
		else:
			raise TemplateError("%s: no value for {{ %s }}" % (where, dotted))
	return "" if v is None else v
