#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The channels the site build picks (site/rfoundrysite/channels.py, site/cloudflare-build.sh): Stable and Beta from
the release list, Nightly from the `nightly` tag, with no tag, only a nightly tag, or all three; the footer's line;
and the links of the docs trees per channel (site/rfoundrysite/docs.py). Needs site/requirements.txt.

    python3 site/tests/channels_test.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rfoundrysite import build, channels, docs, repo  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"
TAG_SHA = "fedcba9876543210fedcba9876543210fedcba98"
COMMIT = {"commit": {"committer": {"date": "2026-11-21T03:12:00Z"}}}


def rel(tag, pre=False, draft=False):
	return {"tag_name": tag, "prerelease": pre, "draft": draft, "assets": []}


class Channels(unittest.TestCase):
	def test_no_tag(self):
		self.assertEqual(channels.stable_and_beta([]), (None, None))
		self.assertIsNone(channels.nightly(None))
		self.assertIsNone(channels.parse_nightly("", ""))

	def test_only_nightly(self):
		self.assertEqual(channels.stable_and_beta([rel("nightly")]), (None, None), "the nightly tag is no release")
		ref = {"object": {"type": "commit", "sha": SHA}}
		self.assertEqual(channels.nightly(ref, None, COMMIT), {"sha": SHA, "date": "2026-11-21T03:12:00Z"})
		self.assertEqual(channels.nightly(ref), {"sha": SHA, "date": ""}, "before the commit's date is asked for")

	def test_annotated_nightly_tag(self):
		ref = {"object": {"type": "tag", "sha": TAG_SHA}}
		tag = {"object": {"type": "commit", "sha": SHA}}
		self.assertEqual(channels.nightly(ref, tag, COMMIT)["sha"], SHA)
		self.assertIsNone(channels.nightly(ref, None), "an annotated tag without its object")

	def test_release_beta_and_nightly(self):
		rels = [rel("v26.11"), rel("v26.12"), rel("v27.1b1", pre=True), rel("v27.2b1", pre=True, draft=True), rel("test-x")]
		self.assertEqual(channels.stable_and_beta(rels), ("v26.12", "v27.1b1"))
		self.assertEqual(channels.stable_and_beta([rel("v26.12"), rel("v26.12b2", pre=True)]), ("v26.12", None),
				 "a beta older than the release")
		self.assertEqual(channels.stable_and_beta([rel("v26.12b2", pre=True)]), (None, "v26.12b2"))

	def test_nightly_argument(self):
		self.assertEqual(channels.parse_nightly(SHA, "2026-11-21T03:12:00Z"), {"sha": SHA, "date": "2026-11-21T03:12:00Z"})
		for bad in ("abc", SHA.upper(), SHA + "0"):
			with self.assertRaises(repo.SourceError):
				channels.parse_nightly(bad, "")

	def test_footer_line(self):
		with tempfile.TemporaryDirectory() as out:
			site = build.Site(out, "https://github.com/o/r", "main", log=lambda *a: None,
					  nightly={"sha": SHA, "date": "2026-11-21T03:12:00Z"})
			site.release = "v26.12"
			self.assertEqual(site.firmware_line("en"),
					 '<a href="https://github.com/o/r/releases/tag/v26.12">Firmware v26.12</a>'
					 ' · <a href="https://github.com/o/r/commit/%s">Nightly 2026-11-21</a>'
					 ' · <a href="https://github.com/o/r/releases">Changelog</a>' % SHA)


class DocTrees(unittest.TestCase):
	"""A Stable checkout's manual at docs/, main's other sections beside it and main's manual under nightly/."""

	def setUp(self):
		self.tmp = tempfile.TemporaryDirectory()
		root = self.tmp.name
		os.makedirs(os.path.join(root, "docs", "user"))
		os.makedirs(os.path.join(root, "docs", "ux", "screens"))
		with open(os.path.join(root, "docs", "user", "01-a.md"), "w") as f:
			f.write("# A\n\n[spec](../spec/radios.md) ![s](../ux/screens/s.png) [b](02-b.md)\n")
		with open(os.path.join(root, "docs", "ux", "screens", "s.png"), "wb") as f:
			f.write(b"png")
		self.root = root

	def tearDown(self):
		self.tmp.cleanup()

	def mk(self, ref, **kw):
		return docs.Docs("https://github.com/o/r", ref, ["en"], "en", **kw)

	def test_stable_tree(self):
		t = self.mk("v26.12", root=self.root, sections=["user"], files_prefix="stable/")
		self.assertEqual([p["out"] for p in t.pages], ["docs/user/01-a.html"])
		self.assertEqual(t.link("../spec/radios.md", "docs/user/01-a.md"), "https://github.com/o/r/blob/v26.12/docs/spec/radios.md",
				 "a section no tree builds from the release: the repository at the tag")
		self.assertEqual(t.link("../ux/screens/s.png", "docs/user/01-a.md"), "../../stable/docs/ux/screens/s.png")
		self.assertEqual(t.link("02-b.md", "docs/user/01-a.md"), "02-b.html")
		with tempfile.TemporaryDirectory() as out:
			t.copy_assets(out)
			self.assertTrue(os.path.isfile(os.path.join(out, "stable", "docs", "ux", "screens", "s.png")))

	def test_nightly_and_main_trees(self):
		others = [s for s in docs.SECTIONS if s != "user"]
		night = self.mk("main", sections=["user"], prefix="nightly/", elsewhere={s: "" for s in others})
		self.assertTrue(night.pages and all(p["out"].startswith("nightly/docs/user/") for p in night.pages))
		self.assertEqual(night.link("../spec/radios.md", "docs/user/02-installing.md"), "../../../docs/spec/radios.html")
		self.assertEqual(night.link("03-first-boot.md#x", "docs/user/02-installing.md"), "03-first-boot.html#x")
		main = self.mk("main", sections=others, elsewhere={"user": "nightly/"})
		self.assertFalse([p for p in main.pages if p["section"] == "user"])
		self.assertEqual(main.link("../user/02-installing.md", "docs/dev/website.md"), "../../nightly/docs/user/02-installing.html")


if __name__ == "__main__":
	unittest.main()
