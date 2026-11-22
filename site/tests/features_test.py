# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The feature list the flasher's chooser is built from (site/rfoundrysite/repo.py): what each switch needs.

    python3 site/tests/features_test.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rfoundrysite import repo  # noqa: E402


class Dependencies(unittest.TestCase):
	def test_or_after_a_parenthesis_is_an_any(self):
		# "needs CONFIG_APRS_AFSK=y (with FM) or CONFIG_APRS_DMR=y (with DMR)" is one of two, not both
		deps = repo.dependencies('$(error CONFIG_APRS=y needs CONFIG_APRS_AFSK=y (with FM) or CONFIG_APRS_DMR=y (with DMR))')
		self.assertEqual(deps["APRS"], {"all": [], "any": ["APRS_AFSK", "APRS_DMR"]})

	def test_no_group_needs_itself_in_a_cycle(self):
		# the chooser draws a feature under the one it needs; a cycle left a whole branch out of the list (#723)
		feats = {f["name"]: f for f in repo.features()}
		for name, f in feats.items():
			seen, todo = set(), list(f["needs"]["all"])
			while todo:
				n = todo.pop()
				if n == name:
					self.fail("%s needs itself through %s" % (name, sorted(seen)))
				if n not in seen and n in feats:
					seen.add(n)
					todo += feats[n]["needs"]["all"]


if __name__ == "__main__":
	unittest.main()
