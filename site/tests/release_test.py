# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The release the site's flasher installs, the beta it offers (site/rfoundrysite/build.py release_and_beta) and the
footer's firmware line. Needs site/requirements.txt (tools/ci.sh runs it with the site build's Python).

    python3 site/tests/release_test.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rfoundrysite import build, repo  # noqa: E402


class Release(unittest.TestCase):
	def test_newest_release_and_newer_beta(self):
		self.assertEqual(build.release_and_beta(["v26.11", "v26.11"], "v26.12-beta.1"), ("v26.11", "v26.12-beta.1"))

	def test_beta_older_than_release_is_dropped(self):
		self.assertEqual(build.release_and_beta(["v26.12"], "v26.12-beta.2"), ("v26.12", ""))
		self.assertEqual(build.release_and_beta(["v27.1"], "v26.12-beta.1"), ("v27.1", ""))

	def test_no_release(self):
		self.assertEqual(build.release_and_beta([], ""), ("", ""))
		self.assertEqual(build.release_and_beta([], "v26.12-beta.1"), ("", "v26.12-beta.1"))

	def test_bad_beta_refused(self):
		with self.assertRaises(repo.SourceError):
			build.release_and_beta(["v26.11"], "v1.0.0")

	def test_footer_line(self):
		with tempfile.TemporaryDirectory() as out:
			site = build.Site(out, "https://github.com/o/r", "main", log=lambda *a: None)
			site.release = ""
			self.assertEqual(site.firmware_line("en"), '<a href="https://github.com/o/r/releases">Changelog</a>')
			site.release = "v26.11"
			self.assertEqual(site.firmware_line("en"), '<a href="https://github.com/o/r/releases/tag/v26.11">Firmware v26.11</a>'
					 ' · <a href="https://github.com/o/r/releases">Changelog</a>')


if __name__ == "__main__":
	unittest.main()
