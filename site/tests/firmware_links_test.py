# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The places the flasher sends people to for a radio's stock firmware (`stock_download` in boards/*/board.toml) still
exist. A page that is gone (404, 410), a host that no longer resolves or one that refuses the connection fails the
test. A site that answers a script with 401, 403, 429 or a 5xx (bot protection, a bad day) only warns, and so does a
proxy in front of the test that won't connect to the host (CI's egress filter): the link wasn't checked. The test
skips when the network is down. The network check is off for now (too many false failures from CI's network): run
it by hand with RFOUNDRY_LINK_CHECK=1.

    RFOUNDRY_LINK_CHECK=1 python3 site/tests/firmware_links_test.py
"""
import glob
import os
import re
import sys
import unittest
import urllib.error
import urllib.request

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
SOFT = {401, 403, 405, 429, 500, 502, 503, 504}


def links():
	"""{url: [board ids]} from every board's stock_download."""
	found = {}
	for path in sorted(glob.glob(os.path.join(ROOT, "boards", "*", "board.toml"))):
		with open(path, encoding="utf-8") as f:
			m = re.search(r'^stock_download\s*=\s*"([^"]+)"', f.read(), re.M)
		if m:
			found.setdefault(m.group(1), []).append(os.path.basename(os.path.dirname(path)))
	return found


def status(url, method):
	req = urllib.request.Request(url, method=method, headers={"User-Agent": "Mozilla/5.0 (RadioFoundry link check)"})
	try:
		with urllib.request.urlopen(req, timeout=20) as r:
			return r.status
	except urllib.error.HTTPError as e:
		return e.code


def unchecked(e):
	"""An error from a proxy refusing to reach the host, not from the host: says nothing about the link."""
	return "Tunnel connection failed" in str(e)


def reachable():
	try:
		status("https://github.com", "HEAD")
		return True
	except OSError:
		return False


class FirmwareLinks(unittest.TestCase):
	def test_links_exist(self):
		if os.environ.get("RFOUNDRY_LINK_CHECK") != "1":
			self.skipTest("off unless RFOUNDRY_LINK_CHECK=1")
		if not reachable():
			self.skipTest("no network")
		bad = []
		for url, boards in links().items():
			try:
				code = status(url, "HEAD")
				if code in SOFT | {404}:	# some servers refuse HEAD: ask again with GET before judging
					code = status(url, "GET")
			except OSError as e:
				if unchecked(e):
					print("warning: %s not checked: %s" % (url, e), file=sys.stderr)
					continue
				bad.append("%s (%s): %s" % (url, ", ".join(boards), e))
				continue
			if code in SOFT:
				print("warning: %s answered %d (not counted)" % (url, code), file=sys.stderr)
			elif code >= 400:
				bad.append("%s (%s): HTTP %d" % (url, ", ".join(boards), code))
		self.assertEqual(bad, [], "dead firmware links")

	def test_proxy_refusal_is_not_a_dead_link(self):
		self.assertTrue(unchecked(urllib.error.URLError(OSError("Tunnel connection failed: 403 Filtered"))))
		self.assertFalse(unchecked(urllib.error.URLError(OSError("[Errno 8] nodename nor servname provided"))))

	def test_links_are_https(self):
		self.assertTrue(links(), "no board has a stock_download")
		for url in links():
			self.assertTrue(url.startswith("https://"), url)


if __name__ == "__main__":
	unittest.main()
