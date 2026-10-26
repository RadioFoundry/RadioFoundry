# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The rfoundry tool zip the site publishes for the manual install kit (site/rfoundrysite/toolzip.py): unpacked
outside the repository, its launcher runs, and the serial route finds the tools it loads by path in the unpacked
copy, not in the checkout. Only what imports without pyusb or pyserial is run, so the test needs neither.

    python3 site/tests/toolzip_test.py
"""
import hashlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from rfoundrysite import toolzip  # noqa: E402


class ToolZip(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.tmp = tempfile.TemporaryDirectory()
		cls.info = toolzip.write(cls.tmp.name)
		cls.path = os.path.join(cls.tmp.name, toolzip.NAME)
		with open(cls.path, "rb") as f:
			cls.data = f.read()
		cls.dir = os.path.join(cls.tmp.name, "unpacked")
		with zipfile.ZipFile(io.BytesIO(cls.data)) as z:
			cls.names = z.namelist()
			z.extractall(cls.dir)

	@classmethod
	def tearDownClass(cls):
		cls.tmp.cleanup()

	def run_here(self, *args):
		env = {k: v for k, v in os.environ.items() if not k.startswith("PYTHON")}
		return subprocess.run([sys.executable, *args], cwd=self.dir, env=env, capture_output=True, text=True, timeout=60)

	def test_site_json_entry(self):
		self.assertEqual(self.info["zip"], "downloads/" + toolzip.NAME)
		self.assertEqual(self.info["size"], len(self.data))
		self.assertEqual(self.info["sha256"], hashlib.sha256(self.data).hexdigest())

	def test_reproducible(self):
		self.assertEqual(toolzip.build(), self.data)

	def test_contents(self):
		for name in ("rfoundry.py", "requirements.txt", "LICENSE", "tools/rfoundry/rfoundry/cli.py",
			     "tools/rfoundry/rfoundry/registry.json", "tools/serial_loader.py", "tools/hrc7000_codec.py",
			     "stub/stubtool.py", "stub/f4stub.py"):
			self.assertIn(name, self.names)
		self.assertTrue(any(n.startswith("tools/rfoundry/rfoundry/codec_maps/") for n in self.names))
		self.assertFalse([n for n in self.names if "__pycache__" in n or n.endswith((".pyc", ".local.md"))])
		# no tests, no build output, nothing from outside the tool
		self.assertFalse([n for n in self.names if n.split("/")[0] not in ("tools", "stub") and "/" in n])

	def test_launcher_runs(self):
		r = self.run_here("rfoundry.py", "--help")
		self.assertEqual(r.returncode, 0, r.stderr)
		self.assertIn("flash", r.stdout)
		r = self.run_here("rfoundry.py", "flash", "--help")
		self.assertEqual(r.returncode, 0, r.stderr)
		self.assertIn("--dry-run", r.stdout)
		self.assertIn("--stock", r.stdout)

	def test_serial_route_tools_load_from_the_copy(self):
		code = ("import os, sys; sys.path.insert(0, os.path.join('tools', 'rfoundry'))\n"
			"from rfoundry import serialflash\n"
			"for n in ('serial_loader', 'hrc7000_codec', 'stubtool'): print(os.path.abspath(serialflash.tool(n).__file__))\n")
		r = self.run_here("-c", code)
		self.assertEqual(r.returncode, 0, r.stderr)
		paths = r.stdout.split()
		self.assertEqual(len(paths), 3)
		for p in paths:
			self.assertTrue(os.path.realpath(p).startswith(os.path.realpath(self.dir)), p)

	def test_stubtool_runs(self):
		# the DM-1801 Pro's kit reads the program flash with it; its main imports f4stub
		r = self.run_here(os.path.join("stub", "stubtool.py"), "--help")
		self.assertEqual(r.returncode, 0, r.stderr)
		self.assertIn("--progmem", r.stdout)
		r = self.run_here("-c", "import sys; sys.path.insert(0, 'stub'); import f4stub")
		self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
	unittest.main(verbosity=1)
