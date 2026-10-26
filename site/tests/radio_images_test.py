# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Every radio's picture (site/rfoundrysite/device.py), which the web flasher shows beside its update mode steps
(radiotop.js, data/radios.json frame_image and frame_geometry): each board in the registry renders one, from its own
illustration or the generic drawing for its shape, with one screen where the flasher lays what the screen shows. A new
board without what it needs fails here, not on the flasher page.

    python3 site/tests/radio_images_test.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rfoundrysite import device, repo  # noqa: E402


class RadioImages(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		feats, variants = repo.features(), repo.variants()
		cls.boards, _ = repo.boards([f["name"] for f in feats], [v["id"] for v in variants])

	def test_every_board_has_a_picture(self):
		self.assertTrue(self.boards)
		for b in self.boards:
			with self.subTest(board=b["id"]):
				img = device.render(b, b["name"])
				self.assertEqual(len(device.SCREEN_RECT.findall(img["frame"])), 1)
				pct = img["geometry"]["screen_pct"]
				for k in ("left", "top", "width", "height"):
					self.assertTrue(0 <= pct[k] <= 100, (k, pct[k]))
				self.assertGreater(pct["width"], 0)
				self.assertGreater(pct["height"], 0)


if __name__ == "__main__":
	unittest.main(verbosity=1)
