# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""rfoundry flash --route stub (stubroute.py): the command line it hands stubtool, per radio, and its refusals."""
import contextlib
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import home  # noqa: E402,F401 (the settings folder in a temporary folder)
from rfoundry import cli, stubroute  # noqa: E402
import random  # noqa: E402


class FakeStubtool:
    def __init__(self):
        self.calls = []

    def main(self, argv, raise_errors=False):
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

    def test_a_stub_left_running_is_attached(self):
        # an earlier run stopped with the stub running: RadioFoundry's debug link doesn't answer, the stub does
        class DumpError(Exception):
            pass

        class NoAnswer(DumpError):
            pass
        calls, real = self.fake.calls, self.fake.main

        def main(argv, raise_errors=False):
            if "--rfoundry-enter" in argv:
                calls.append((argv, None))
                raise NoAnswer("no answer from RadioFoundry's debug link")
            return real(argv)
        self.fake.main = main
        bid = stubroute.BOARDS[0]
        rc, log = run_cli(["flash", self.image(), "--board", bid, "--route", "stub", "--port", "/dev/x", "--yes"])
        self.assertEqual(rc, 0, log)
        self.assertEqual([c[0][1] for c in calls], ["--rfoundry-enter", "--attach"])
        self.assertEqual(len(calls[1][1]), 30000)
        self.assertIn("left running", log)

    def test_other_errors_are_not_retried(self):
        # RadioFoundry answered and the stub refused (or failed): attaching would only repeat it on a stub that is gone
        class DumpError(Exception):
            pass
        calls = []

        def main(argv, raise_errors=False):
            calls.append(argv)
            raise DumpError("sector(s) 8 hold the radio's codec")
        self.fake.main = main
        rc, log = run_cli(["flash", self.image(), "--board", stubroute.BOARDS[0], "--route", "stub", "--port", "/dev/x",
                           "--yes", "--no-probe"])
        self.assertEqual(rc, 1, log)
        self.assertEqual(len(calls), 1)
        self.assertIn("hold the radio's codec", log)
        self.assertNotIn("left running", log)

    def uncommitted(self, attach_ok):
        # the write stopped after the application was erased (a USB drop mid-write): one try through the stub, then
        # a plain "not installed, in DFU" with how to finish
        class DumpError(Exception):
            pass

        class UncommittedError(DumpError):
            pass
        calls, real = [], self.fake.main

        def main(argv, raise_errors=False):
            calls.append(argv[1])
            if argv[1] == "--rfoundry-enter":
                raise UncommittedError("the install stopped at step 2 of 7 (cable pulled)")
            if not attach_ok:
                raise DumpError("no stub answers")
            return real(argv)
        self.fake.main = main
        rc, log = run_cli(["flash", self.image(), "--board", stubroute.BOARDS[0], "--route", "stub", "--port", "/dev/x",
                           "--yes", "--no-probe"])
        self.assertEqual(calls, ["--rfoundry-enter", "--attach"])
        self.assertIn("step 2 of 7", log)
        return rc, log

    def test_uncommitted_finished_by_the_stub(self):
        rc, log = self.uncommitted(True)
        self.assertEqual(rc, 0, log)
        self.assertIn("finished the install", log)
        self.assertNotIn("NOT INSTALLED", log)

    def test_uncommitted_left_in_dfu_says_so(self):
        rc, log = self.uncommitted(False)
        self.assertEqual(rc, 1, log)
        self.assertIn("NOT INSTALLED", log)
        self.assertIn("bootloader (DFU)", log)
        self.assertIn("rfoundry flash again", log)

    def test_refusals(self):
        rc, log = run_cli(["flash", self.image(), "--board", stubroute.BOARDS[0], "--route", "stub", "--simulate", "blank"])
        self.assertNotEqual(rc, 0)
        self.assertIn("simulate", log)
        rc, log = run_cli(["flash", self.image(), "--board", stubroute.BOARDS[0], "--route", "stub", "--dry-run"])
        self.assertEqual(rc, 0, log)
        self.assertIn("dry run", log)
        self.assertEqual(self.fake.calls, [])


class RealStubtool(unittest.TestCase):
    """stub/stubtool.py itself, loaded as rfoundry loads it (serialflash.tool), on a port where nothing answers: the
    error it raises reaches stubroute as NoAnswer, so a stub left running is tried"""

    def test_no_answer_is_told_apart(self):
        from rfoundry import serialflash
        stub_dir = os.path.join(serialflash._ROOT, "stub")
        if stub_dir not in sys.path:
            sys.path.insert(0, stub_dir)		# as stubroute.run does
        stubtool = serialflash.tool("stubtool")
        import f4stub
        self.assertIs(f4stub.stubtool, stubtool)	# one copy: f4stub's errors are the classes stubtool catches

        class Silent:
            timeout = 0.1

            def write(self, b):
                return len(b)

            def read(self, n=1):
                return b""

            def reset_input_buffer(self):
                pass

            def flush(self):
                pass

            def close(self):
                pass

        class Clock:		# dl_request waits by the clock: a fast one, so the test doesn't
            t = 0.0

            def monotonic(self):
                self.t += 0.25
                return self.t

            def sleep(self, s):
                self.t += s

            def __getattr__(self, name):
                import time
                return getattr(time, name)
        fake_serial = type(sys)("serial")
        fake_serial.serial_for_url = lambda *a, **k: Silent()
        attached = []

        def attach(radio):
            attached.append(radio)
            raise f4stub.F4Error("the stub does not answer on this port")
        logs = []
        tmp = tempfile.mkdtemp()
        img = os.path.join(tmp, "x.bin")
        with mock.patch.dict(sys.modules, {"serial": fake_serial}), \
                mock.patch.object(stubtool, "time", Clock()), mock.patch.object(f4stub, "time", Clock()), \
                mock.patch.object(f4stub.F4Radio, "attach", attach), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc = stubroute.run(cli.boards.get(stubroute.BOARDS[0]), b"\x00" * 1024, "/dev/x", log=logs.append,
                               stubtool=stubtool)
        self.assertEqual(rc, 1)
        self.assertTrue(any("left running" in m for m in logs), logs)
        self.assertEqual(len(attached), 1, logs)
        self.assertTrue(any("no stub finished the write" in m for m in logs), logs)
        os.rmdir(tmp)


if __name__ == "__main__":
    unittest.main()
