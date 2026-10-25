# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The glossary markup of the docs pages (site/rfoundrysite/docs.py): which words get an <abbr>, and which never do.

    python3 site/tests/glossary_test.py      (needs markdown-it-py)
"""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rfoundrysite import docs  # noqa: E402


class Marking(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.docs = docs.Docs("https://example.invalid/r", "main", ["en"], "en")

	def render(self, markdown, section="user"):
		"""The page body for Markdown text, as a page in the given docs section."""
		page = {"src": "docs/%s/x.md" % section, "section": section}
		self.docs.source_for = lambda p, lang: (markdown, False)
		return self.docs.render(page, "en")[0]

	def marked(self, html):
		return re.findall(r"<abbr[^>]*>([^<]*)</abbr>", html)

	def test_whole_words_only(self):
		html = self.render("A TGX and the ATG are not talkgroup ids. TG 9 is.\n")
		self.assertEqual(self.marked(html), ["talkgroup"])	# TG stays: same entry, and the first use won

	def test_case_sensitive_acronyms(self):
		self.assertEqual(self.marked(self.render("The tg and the Tg are not it.\n")), [])
		self.assertEqual(self.marked(self.render("Plural TGs work.\n")), ["TGs"])

	def test_plain_words_any_case(self):
		self.assertEqual(self.marked(self.render("Open the Codeplug, then the codeplugs.\n")), ["Codeplug"])

	def test_first_use_per_section(self):
		html = self.render("## A\n\nUse CTCSS here and CTCSS again.\n\n## B\n\nCTCSS once more.\n")
		self.assertEqual(self.marked(html), ["CTCSS", "CTCSS"])

	def test_skips_code_headings_and_links(self):
		html = self.render("## About CTCSS\n\n`CTCSS` and [DCS](a.md) and\n\n```\nAPRS\n```\n")
		self.assertEqual(self.marked(html), [])

	def test_table_first_column_is_a_definition(self):
		html = self.render("| Term | Meaning |\n|---|---|\n| CTCSS | A tone for DCS |\n")
		self.assertEqual(self.marked(html), ["DCS"])

	def test_page_opt_out(self):
		self.assertEqual(self.marked(self.render("<!-- glossary: off -->\n\nUse CTCSS.\n")), [])
		html = self.render("<!-- glossary: off CTCSS -->\n\nUse CTCSS and DCS.\n")
		self.assertEqual(self.marked(html), ["DCS"])

	def test_opt_out_by_alias(self):
		html = self.render("<!-- glossary: off TS -->\n\nTS 1 is a timeslot.\n")
		self.assertEqual(self.marked(html), [])

	def test_title_carries_the_definition(self):
		html = self.render("Use CTCSS.\n")
		self.assertIn('title="Continuous tone coded squelch system', html)
		self.assertIn('data-more="22-glossary.html#t-ctcss"', html)

	def test_user_pages_skip_developer_terms(self):
		self.assertEqual(self.marked(self.render("The CACH carries it.\n", "user")), [])
		self.assertEqual(self.marked(self.render("The CACH carries it.\n", "spec")), ["CACH"])


if __name__ == "__main__":
	unittest.main()
