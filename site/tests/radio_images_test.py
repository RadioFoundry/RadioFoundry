# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Every radio's picture (site/rfoundrysite/device.py), which the web flasher shows beside its update mode steps
(radiotop.js, data/radios.json frame_image and frame_geometry): each board in the registry renders one, from its own
illustration or the generic drawing for its shape, with one screen where the flasher lays what the screen shows, and
each key held to enter update mode (board.toml [flash.bootloader] hold) on it, or named undrawn, with its label for the
flasher's caption. A new board without what it needs fails here, not on the flasher page.

    python3 site/tests/radio_images_test.py
"""
import json
import os
import re
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

	def test_keys_to_hold_are_on_the_picture(self):
		keys = device.svg_keys()
		with open(repo.path("site", "i18n", "en.json"), encoding="utf-8") as f:
			en = json.load(f)
		for b in self.boards:
			hold = b["flash"]["bootloader_hold"]
			names = keys.key_names(b["id"])
			src = repo.path("site", "static", "img", "radios", "src", b["id"] + ".svg")
			for look in [None] + [lk["id"] for lk in b["looks"]]:
				with self.subTest(board=b["id"], look=look):
					img = device.render(b, b["name"], look)
					drawn = img["geometry"]["keys"]
					for k, box in drawn.items():
						self.assertIn(k, names)
						self.assertGreater(box["width"], 0)
						self.assertGreater(box["height"], 0)
						self.assertTrue(0 <= box["left"] and box["left"] + box["width"] <= 100, (k, box))
						self.assertTrue(0 <= box["top"] and box["top"] + box["height"] <= 100, (k, box))
					m = re.search(r"<svg\b[^>]*\bdata-undrawn=\"([^\"]*)\"", img["frame"])
					undrawn = m.group(1).split() if m else []
					for k in hold:
						self.assertIn(k, names, "hold names no key of the radio")
						self.assertIn("flash.key." + k, en, "the flasher's caption names the key: site/i18n/en.json")
						# the generic drawing (no illustration) shows only a PTT: the caption names the rest
						if os.path.isfile(src):
							self.assertTrue(k in drawn or k in undrawn, "%s: tag key-%s or list it in data-undrawn" % (b["id"], k))


if __name__ == "__main__":
	unittest.main(verbosity=1)
