# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""rfoundry flash --route stub (stubroute.py): the command line it hands stubtool, per radio, and its refusals."""
import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rfoundry import cli, stubroute  # noqa: E402
import random  # noqa: E402


class FakeStubtool:
    def __init__(self):
        self.calls = []

    def main(self, argv):
        path = argv[argv.index("--write-app") + 1]
        with open(path, "rb") as f:
            self.calls.append((argv, f.read()))
        return 0


def run_cli(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = cli.main(argv)
    return rc, out.getvalue() + err.getvalue()


class Route(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.fake = FakeStubtool()
        self._tool = stubroute.serialflash.tool
        stubroute.serialflash.tool = lambda name: self.fake

    def tearDown(self):
        stubroute.serialflash.tool = self._tool

    def image(self, n=30000):
        p = os.path.join(self.tmp, "odr.bin")
        with open(p, "wb") as f:
            f.write(bytes(random.Random(4).getrandbits(8) for _ in range(n)))
        return p

    def test_f4_radios_get_the_application_through_the_stub(self):
        self.assertTrue(stubroute.BOARDS)
        for bid in stubroute.BOARDS:
            self.fake.calls.clear()
            rc, log = run_cli(["flash", self.image(), "--board", bid, "--route", "stub", "--port", "/dev/x", "--yes"])
            self.assertEqual(rc, 0, log)
            argv, data = self.fake.calls[0]
            self.assertEqual(argv[:5], ["/dev/x", "--rfoundry-enter", "--board", bid, "--write-app"])
            self.assertEqual(len(data), 30000)
            self.assertIn("reset", argv)

    def test_refusals(self):
        rc, log = run_cli(["flash", self.image(), "--board", stubroute.BOARDS[0], "--route", "stub", "--simulate", "blank"])
        self.assertNotEqual(rc, 0)
        self.assertIn("simulate", log)
        rc, log = run_cli(["flash", self.image(), "--board", stubroute.BOARDS[0], "--route", "stub", "--dry-run"])
        self.assertEqual(rc, 0, log)
        self.assertIn("dry run", log)
        self.assertEqual(self.fake.calls, [])


if __name__ == "__main__":
    unittest.main()
