# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The radio pages offer the install to every backend the web flasher runs, not only the ones that existed first."""
import json
import os
import sys
import unittest

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "site"))
from rfoundrysite import repo  # noqa: E402

DIR = os.environ.get("RFOUNDRY_SITE_FIXTURES", os.path.join(ROOT, "build", "site-tests"))


class FlashCta(unittest.TestCase):
	def setUp(self):
		with open(os.path.join(DIR, "radios.json"), encoding="utf-8") as f:
			self.radios = json.load(f)["radios"]
		with open(os.path.join(DIR, "flash-boards.json"), encoding="utf-8") as f:
			self.flash = json.load(f)

	def test_every_backend_with_its_data_can_flash(self):
		backends = {r["flash"]["backend"] for r in self.radios}
		self.assertIn("anytone-update", backends)
		for backend in backends:
			self.assertTrue(any(repo.can_flash(r["flash"], self.flash) for r in self.radios if r["flash"]["backend"] == backend), backend)

	def test_a_board_without_its_data_cannot(self):
		for backend in ("anyroad-dfu", "c7000-stub", "anytone-update", "none"):
			self.assertFalse(repo.can_flash({"backend": backend, "board": "no-such"}, self.flash), backend)


if __name__ == "__main__":
	unittest.main()
