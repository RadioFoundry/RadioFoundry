# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for the original firmware version read (rfoundry/origversion.py): the answer's format, the version to
hardware option table, the exact requests the read sends (against fakedfu.FakeOriginalFirmware), what the installer
stores over the debug link after the first start, and a simulated install. No USB, no radio.

    python3 -m unittest discover -s tools/rfoundry/tests -v        (tools/ci.sh runs this)
"""
import contextlib
import io
import os
import random
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from rfoundry import boards, cli, debuglink, fakedfu, origversion, stockfw  # noqa: E402
from rfoundry import dfu as dfumod  # noqa: E402

SPEC = {"read": "dfu-info", "hwopt": "Board revision", "major": {"06": "A", "16": "B", "26": "C"}}
# a board that declares the read, whichever it is (board.toml [flash.original_version])
DECLARED = [b for _, b in sorted(boards.BOARDS.items()) if b.original_version]
GETSTATUS = (0xA1, dfumod.DFU_GETSTATUS, 0, 6)
ABORT = (0x21, dfumod.DFU_ABORT, 0, None)
DNLOAD_INFO = (0x21, dfumod.DFU_DNLOAD, 0, b"\xA2\x02")
UPLOAD = (0xA1, 2, 0, 1024)


def board():
    if not DECLARED:
        raise unittest.SkipTest("no board declares [flash.original_version]")
    return DECLARED[0]


class ParseVersion(unittest.TestCase):
    def test_answers(self):
        self.assertEqual(origversion.parse_version(b"\x56\x00\x60\x09"), "06.09")
        self.assertEqual(origversion.parse_version(b"\x56\x01\x60\x45"), "16.45")
        self.assertEqual(origversion.parse_version(b"\x56\x02\x60\x45"), "26.45")

    def test_anything_else_is_no_version(self):
        for bad in (b"", None, b"\x56\x00\x60", b"\x56\x00\x60\x09\x00", b"\x00\x21\x41", b"\x55\x00\x60\x09",
                    b"\x56\x10\x60\x09", b"\x56\x00\x61\x09", b"\x56\x0A\x60\x09", b"\x56\x00\xA0\x09",
                    b"\x56\x00\x60\x0A", b"\xF1\xF2\xF3\xF4"):
            self.assertIsNone(origversion.parse_version(bad), bad)


class ChoiceFor(unittest.TestCase):
    def test_major_version_settles_the_choice(self):
        self.assertEqual(origversion.choice_for(SPEC, "06.09"), "A")
        self.assertEqual(origversion.choice_for(SPEC, "16.45"), "B")
        self.assertEqual(origversion.choice_for(SPEC, "26.01"), "C")

    def test_unknown_major_is_not_guessed(self):
        for v in ("36.45", "07.09", "6.09", None, ""):
            self.assertIsNone(origversion.choice_for(SPEC, v))
        self.assertIsNone(origversion.choice_for(None, "06.09"))

    def test_declared_boards(self):
        """Every declared table maps two-digit majors to choices, and the read is one the installers know."""
        for b in DECLARED:
            spec = b.original_version
            self.assertIn(spec["read"], origversion.READS)
            self.assertTrue(spec["major"])
            for m, c in spec["major"].items():
                self.assertRegex(m, r"^[0-9]{2}$")
                self.assertEqual(origversion.choice_for(spec, m + ".00"), c)
            self.assertIsNotNone(b.dfu_ids, b.id)


class ReadDfuInfo(unittest.TestCase):
    def test_version_and_the_exact_requests(self):
        dev = fakedfu.FakeOriginalFirmware(board())
        self.assertEqual(origversion.read_dfu_info(dev), "06.09")
        self.assertEqual(dev.requests, [GETSTATUS, DNLOAD_INFO, GETSTATUS, GETSTATUS, ABORT, UPLOAD])

    def test_other_version(self):
        dev = fakedfu.FakeOriginalFirmware(board(), answer=b"\x56\x01\x60\x45")
        self.assertEqual(origversion.read_dfu_info(dev), "16.45")

    def test_error_state_is_cleared_first(self):
        dev = fakedfu.FakeOriginalFirmware(board(), state=dfumod.ST_ERROR)
        self.assertEqual(origversion.read_dfu_info(dev), "06.09")
        self.assertEqual(dev.requests[:2], [GETSTATUS, (0x21, dfumod.DFU_CLRSTATUS, 0, None)])

    def test_other_state_is_aborted_first(self):
        dev = fakedfu.FakeOriginalFirmware(board(), state=9)
        self.assertEqual(origversion.read_dfu_info(dev), "06.09")
        self.assertEqual(dev.requests[:2], [GETSTATUS, ABORT])

    def test_a_stall_is_no_version(self):
        """A bootloader (or anything else) that doesn't take A2 02: no version, and nothing more is sent."""
        dev = fakedfu.FakeOriginalFirmware(board(), answer=None)
        logs = []
        self.assertIsNone(origversion.read_dfu_info(dev, log=logs.append))
        self.assertEqual(dev.requests, [GETSTATUS, DNLOAD_INFO])
        self.assertIn("no version", logs[0])

    def test_unexpected_answer_is_no_version(self):
        dev = fakedfu.FakeOriginalFirmware(board(), answer=b"\x00\x21\x41")
        self.assertIsNone(origversion.read_dfu_info(dev))

    def test_unexpected_states_end_the_read(self):
        """A device whose command doesn't go busy, then idle, gets no UPLOAD."""
        dev = fakedfu.FakeOriginalFirmware(board())
        real = dev.ctrl_transfer

        def flat(rtype, req, value, intf, data, timeout=None):
            r = real(rtype, req, value, intf, data, timeout)
            if req == dfumod.DFU_GETSTATUS:
                dev.state = dfumod.ST_IDLE
                return bytes([0, 0, 0, 0, dfumod.ST_IDLE, 0])
            return r
        dev.ctrl_transfer = flat
        self.assertIsNone(origversion.read_dfu_info(dev))
        self.assertNotIn(UPLOAD, dev.requests)


class HwoptAction(unittest.TestCase):
    OPTS = [{"name": "Transmit", "value": 0, "restart": False, "choices": ["Off", "On"]},
            {"name": "Board revision", "value": -1, "restart": True, "choices": ["A", "B", "C"]}]

    def opts(self, value):
        o = [dict(x) for x in self.OPTS]
        o[1]["value"] = value
        return o

    def test_cases(self):
        self.assertEqual(origversion.hwopt_action(self.opts(-1), "Board revision", "B"), ("store", 1, 1, None))
        self.assertEqual(origversion.hwopt_action(self.opts(1), "Board revision", "B"), ("same", 1, 1, "B"))
        self.assertEqual(origversion.hwopt_action(self.opts(2), "Board revision", "B"), ("mismatch", 1, 1, "C"))
        self.assertEqual(origversion.hwopt_action(self.opts(-1), "Board revision", "D")[0], "missing")
        self.assertEqual(origversion.hwopt_action(self.opts(7), "Board revision", "B")[0], "missing")
        self.assertEqual(origversion.hwopt_action(self.opts(-1), "Other", "B")[0], "missing")
        self.assertEqual(origversion.hwopt_action([], "Board revision", "B")[0], "missing")


class FakeLink:
    """debuglink.Link's HWOPT surface"""

    def __init__(self, opts, refuse=False):
        self.opts = opts
        self.refuse = refuse
        self.sets = []
        self.closed = False

    def hwopts(self):
        return [dict(o) for o in self.opts]

    def hwopt_set(self, option, choice):
        if self.refuse:
            raise debuglink.LinkError("command 0x21: status 7")
        self.sets.append((option, choice))
        self.opts[option]["value"] = choice

    def close(self):
        self.closed = True


def quiet(fn, *a, **k):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        r = fn(*a, **k)
    return r, out.getvalue() + err.getvalue()


class StoreLearned(unittest.TestCase):
    def setUp(self):
        self.board = board()
        spec = self.board.original_version
        self.major, self.choice = sorted(spec["major"].items())[0]
        self.other = next(c for c in spec["major"].values() if c != self.choice) if len(set(spec["major"].values())) > 1 \
            else None
        self.choices = sorted(set(spec["major"].values()))
        self.learned = {"version": self.major + ".09", "choice": self.choice, "dev": None}

    def link(self, value, **k):
        return FakeLink([{"name": self.board.original_version["hwopt"], "value": value, "restart": True,
                          "choices": self.choices}], **k)

    def test_stores_when_the_radio_has_none(self):
        link = self.link(-1)
        _, out = quiet(cli.store_learned, "port", self.board, self.learned, link_fn=lambda p: link)
        self.assertEqual(link.sets, [(0, self.choices.index(self.choice))])
        self.assertIn("stored", out)
        self.assertIn("switch the radio off and on", out)
        self.assertTrue(link.closed)

    def test_same_choice_is_left(self):
        link = self.link(self.choices.index(self.choice))
        _, out = quiet(cli.store_learned, "port", self.board, self.learned, link_fn=lambda p: link)
        self.assertEqual(link.sets, [])
        self.assertIn("already holds", out)

    def test_another_choice_is_reported_never_overwritten(self):
        if self.other is None:
            self.skipTest("one choice only")
        link = self.link(self.choices.index(self.other))
        _, out = quiet(cli.store_learned, "port", self.board, self.learned, link_fn=lambda p: link)
        self.assertEqual(link.sets, [])
        self.assertIn("warning", out)
        self.assertIn("Left as it is", out)

    def test_refused_store_says_how_to_set_it(self):
        link = self.link(-1, refuse=True)
        _, out = quiet(cli.store_learned, "port", self.board, self.learned, link_fn=lambda p: link)
        self.assertIn("Settings > Advanced > Hardware", out)

    def test_nothing_learned_opens_nothing(self):
        def boom(port):
            raise AssertionError("opened")
        quiet(cli.store_learned, "port", self.board, None, link_fn=boom)
        quiet(cli.store_learned, "port", self.board, dict(self.learned, choice=None), link_fn=boom)


class DebugLinkHwopt(unittest.TestCase):
    def test_list_and_set_frames(self):
        link = object.__new__(debuglink.Link)
        sent = []

        def request(cmd, payload=b""):
            sent.append((cmd, payload))
            return (b"hwopt.count=1\nhwopt.0.value=-1\nhwopt.0.restart=1\nhwopt.0.name=Board revision\n"
                    b"hwopt.0.choices=A|B|C\n") if payload == b"\x00" else b""
        link.request = request
        self.assertEqual(link.hwopts(), [{"name": "Board revision", "value": -1, "restart": True,
                                          "choices": ["A", "B", "C"]}])
        link.hwopt_set(0, 2)
        self.assertEqual(sent, [(0x21, b"\x00"), (0x21, b"\x01\x00\x02")])


class Learn(unittest.TestCase):
    def args(self, **k):
        return types.SimpleNamespace(**{"no_version_read": False, "wait": 0, **k})

    def test_reads_and_maps(self):
        b = board()
        dev = fakedfu.FakeOriginalFirmware(b)
        r, out = quiet(cli.learn_original_version, self.args(), fakedfu.FakeUsb([dev]), b)
        self.assertEqual(r["version"], "06.09")
        self.assertEqual(r["choice"], origversion.choice_for(b.original_version, "06.09"))
        self.assertIs(r["dev"], dev)
        self.assertIn("original firmware 06.09", out)

    def test_unknown_version_is_said_and_not_guessed(self):
        b = board()
        dev = fakedfu.FakeOriginalFirmware(b, answer=b"\x56\x09\x90\x01")
        r, out = quiet(cli.learn_original_version, self.args(), fakedfu.FakeUsb([dev]), b)
        self.assertEqual((r["version"], r["choice"]), ("99.01", None))
        self.assertIn("settles it itself", out)

    def test_bootloader_already_is_quiet(self):
        b = board()
        dev = fakedfu.FakeOriginalFirmware(b, answer=None)
        r, out = quiet(cli.learn_original_version, self.args(), fakedfu.FakeUsb([dev]), b)
        self.assertIsNone(r)
        self.assertNotIn("warning", out)

    def test_skipped(self):
        b = board()
        dev = fakedfu.FakeOriginalFirmware(b)
        r, _ = quiet(cli.learn_original_version, self.args(no_version_read=True), fakedfu.FakeUsb([dev]), b)
        self.assertIsNone(r)
        self.assertEqual(dev.requests, [])
        r, _ = quiet(cli.learn_original_version, self.args(), fakedfu.FakeUsb([]), b)	# none connected, not asked
        self.assertIsNone(r)

    def test_board_without_the_declaration_sends_nothing(self):
        other = next((x for _, x in sorted(boards.BOARDS.items()) if not x.original_version and x.dfu_ids), None)
        if other is None:
            self.skipTest("every DFU board declares it")
        dev = fakedfu.FakeOriginalFirmware(other)
        r, _ = quiet(cli.learn_original_version, self.args(), fakedfu.FakeUsb([dev]), other)
        self.assertIsNone(r)
        self.assertEqual(dev.requests, [])

    def test_wait_for_dfu_passes_over_the_running_firmware(self):
        """The device that answered the version is the original firmware, not the bootloader: the flash waits for the
        radio to come back as another device."""
        b = board()
        running = fakedfu.FakeOriginalFirmware(b, address=7)
        loader = fakedfu.FakeOriginalFirmware(b, address=8)
        seq = [[running], [running], [], [loader]]
        with mock.patch.object(dfumod, "find", side_effect=lambda usb, bd: seq.pop(0) if len(seq) > 1 else seq[0]), \
                mock.patch.object(cli.time, "sleep"):
            dev, out = quiet(cli.wait_for_dfu, None, b, 5, gone=running)
        self.assertIs(dev, loader)
        self.assertIn("bootloader now", out)


class SimulatedInstall(unittest.TestCase):
    def test_stock_simulation_reads_the_version(self):
        b = board()
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry_%s.bin" % b.id)
            with open(bp, "wb") as f:
                f.write(struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + stockfw.random_bytes(random.Random(1), 0x8000))
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = cli.main(["flash", bp, "--board", b.id, "--simulate", "stock", "--stock", "synthetic", "--yes"])
            out = out.getvalue()
            self.assertEqual(rc, 0, out + err.getvalue())
            self.assertIn("version  original firmware 06.09", out)
            self.assertIn("rfoundry stores", out)
            self.assertIn("sim      PASS", out)


if __name__ == "__main__":
    unittest.main()
