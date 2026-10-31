# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for the AnyTone update mode (rfoundry/anytone.py) and rfoundry's commands for it: the frames, the
identity, the variant checks (a variant picked by the user, an Artery radio, a vendor set for another model), a whole
install and restore against the simulated radio, NAKs and a radio that stops answering. No serial port.

    python3 -m unittest discover -s tools/rfoundry/tests -v        (tools/ci.sh runs this)
"""
import contextlib
import hashlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from rfoundry import anytone as at, boards, cli, modemboot  # noqa: E402

FAMILY = boards.by(family="anytone-update")
B = FAMILY[0] if FAMILY else None


def model_of(code):
    return B.anytone_models[code][0]


def runs_here():
    return [c for c, r in B.variant_boards.items() if r == B.id]


def no_image():
    return [c for c, r in B.variant_boards.items() if r is None and c in B.anytone_models]


def other_board():
    return [c for c, r in B.variant_boards.items() if r is not None and r != B.id and c in B.anytone_models]


def app_image(n=5000, seed=1):
    rng = random.Random(seed)
    return struct.pack("<II", 0x20010000, B.app_base | 1) + bytes(rng.getrandbits(8) for _ in range(n - 8))


def fake_block():
    """A boot block that passes the hash check, without the vendor's bytes: the check's constants patched"""
    blk = modemboot.BOOT_PREFIX + bytes(random.Random(5).getrandbits(8) for _ in range(modemboot.BOOT_LEN - 8))
    return blk, mock.patch.multiple(modemboot, BOOT_SHA256=hashlib.sha256(blk).hexdigest())


def vendor_set(d, models, data, addr=None, name="FW"):
    """A vendor firmware set in directory d: .CDD (8 bytes of header, the image), .CDI, .spi naming models"""
    addr = B.app_base if addr is None else addr
    cdd = os.path.join(d, name + ".CDD")
    with open(cdd, "wb") as f:
        f.write(b"\x00" * 8 + data)
    with open(os.path.join(d, name + ".CDI"), "wb") as f:
        f.write(b"fw".ljust(256, b"\x00") + struct.pack("<III", addr, len(data), 8) + bytes(10))
    spi = struct.pack("<IHII", 0x20, 1, len(data), 0)
    spi += models[0].encode().ljust(8) + b"V208" + (models[1] if len(models) > 1 else "").encode().ljust(16) + b"V208"
    with open(os.path.join(d, name + ".spi"), "wb") as f:
        f.write(spi.ljust(51, b" "))
    return cdd


@unittest.skipIf(B is None, "no board on the anytone-update family")
class Protocol(unittest.TestCase):
    def test_frame(self):
        f = at.frame(0x08006000, b"\x01\x02")
        self.assertEqual(len(f), at.FRAME_LEN)
        self.assertEqual((f[0], f[-1]), (1, at.ACK))
        self.assertEqual(f[1:5], bytes.fromhex("00600008"))
        self.assertEqual(f[5:37], b"\x01\x02" + bytes(30))
        self.assertEqual(struct.unpack_from("<H", f, 37)[0], (0x00 + 0x60 + 0x00 + 0x08 + 1 + 2) & 0xFFFF)
        full = at.frame(0xFFFFFFFF, b"\xff" * 32)
        self.assertEqual(struct.unpack_from("<H", full, 37)[0], (0xFF * 36) & 0xFFFF)
        with self.assertRaises(ValueError):
            at.frame(0, bytes(33))

    def test_identify(self):
        self.assertEqual(at.parse_identify(b"ID578UV\x00\x01V100\x00\x00\x06"), ("D578UV", 1, "V100"))
        self.assertEqual(at.parse_identify(b"I578UV2A\x02V101\x00\x00\x06")[0], "578UV2A")
        for bad in (b"", b"X" * 16, b"ID578UV\x00\x01V100\x00\x00\x15", b"ID578UV\x00\x01V100\x00\x06"):
            with self.assertRaises(at.UpdateError):
                at.parse_identify(bad)

    def test_variants(self):
        for code, names in B.anytone_models.items():
            for n in names:
                self.assertEqual(at.variant_of(B, n), code)
        self.assertIsNone(at.variant_of(B, "NOPE"))
        for code in runs_here():
            self.assertEqual(at.check_install(B, model_of(code)), code)
            self.assertEqual(at.check_install(B, model_of(code), code), code)
        with self.assertRaisesRegex(at.UpdateError, "not a"):
            at.check_install(B, "NOPE")

    def test_picked_variant_mismatch(self):
        codes = list(B.anytone_models)
        if len(codes) < 2:
            self.skipTest("one variant")
        with self.assertRaisesRegex(at.UpdateError, "you said"):
            at.check_radio(B, model_of(codes[0]), codes[1])

    def test_no_image_variant_refused_but_restores(self):
        for code in no_image():
            m = model_of(code)
            with self.assertRaisesRegex(at.UpdateError, "no image|has no image|RadioFoundry has no image"):
                at.check_install(B, m)
            self.assertEqual(at.check_restore(B, m, {m}), code)
            with self.assertRaisesRegex(at.UpdateError, "vendor's updater"):
                at.check_restore(B, m, {"OTHER"})

    def test_other_board_variant_refused_but_restores(self):
        for code in other_board():
            m = model_of(code)
            with self.assertRaisesRegex(at.UpdateError, "takes the %s image" % B.variant_boards[code]):
                at.check_install(B, m)
            self.assertEqual(at.check_restore(B, m, {m}), code)

    def test_stock_set(self):
        with tempfile.TemporaryDirectory() as d:
            data = app_image()
            names = B.anytone_models[runs_here()[0]]
            addr, got, models = at.read_stock(vendor_set(d, names, data))
            self.assertEqual((addr, got, models), (B.app_base, data, set(names)))
            os.remove(os.path.join(d, "FW.spi"))
            with self.assertRaisesRegex(at.UpdateError, "SPI"):
                at.read_stock(os.path.join(d, "FW.CDD"))
        with self.assertRaisesRegex(at.UpdateError, "entries"):
            at.stock_set(b"", bytes(at.CDI_ENTRY * 2), b"")

    def test_install_image(self):
        blk, patch = fake_block()
        app = app_image()
        with patch:
            img = at.install_image(B, app, blk)
            self.assertEqual(len(img), B.codec_slot[0] - B.app_base + len(blk))
            self.assertEqual(at.split_image(B, img), (app, blk))
            self.assertEqual(at.split_image(B, app), (app, None))

    def test_write_nak_retries_and_silence(self):
        fake = at.FakeRadio(model=model_of(runs_here()[0]), nak_every=5)
        r = at.Radio(fake, log=lambda m: None, timeout=0.01)
        r.hello()
        data = app_image(1000)
        self.assertEqual(r.write(B.app_base, data), at.frames_needed(len(data)))
        self.assertTrue(fake.finished)
        self.assertEqual(fake.image()[:len(data)], data)
        silent = at.FakeRadio(silent_after=3)
        r = at.Radio(silent, log=lambda m: None, timeout=0.01)
        with self.assertRaisesRegex(at.UpdateError, "no answer"):
            r.write(B.app_base, data)
        always = at.FakeRadio(nak_every=1)
        with self.assertRaisesRegex(at.UpdateError, "NAK"):
            at.Radio(always, log=lambda m: None, timeout=0.01).write(B.app_base, data)

    def test_hello_needs_update_mode(self):
        class Mute(at.FakeRadio):
            def write(self, b):
                pass
        with self.assertRaisesRegex(at.UpdateError, "update mode"):
            at.Radio(Mute(), log=lambda m: None, timeout=0.01).hello()


@unittest.skipIf(B is None, "no board on the anytone-update family")
class Commands(unittest.TestCase):
    """rfoundry flash and restore-stock against the simulated radio on the port"""

    def run_cli(self, argv, fake):
        out = io.StringIO()
        with mock.patch.object(at, "open_port", lambda port: fake), mock.patch.object(at, "TIMEOUT", 0.01), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = cli.main(argv)
        return rc, out.getvalue()

    def test_flash_install_and_refusals(self):
        blk, patch = fake_block()
        with tempfile.TemporaryDirectory() as d, patch:
            img = os.path.join(d, "rfoundry.bin")
            with open(img, "wb") as f:
                f.write(app_image())
            stock = os.path.join(d, "stock.CDD")
            with open(stock, "wb") as f:
                f.write(b"\x00" * 100 + blk + b"\x00" * 100)
            base = ["flash", img, "--board", B.id, "--yes", "--no-reporting"]
            # no boot block: refused before the radio is asked
            rc, out = self.run_cli(base, at.FakeRadio(model=model_of(runs_here()[0])))
            self.assertEqual(rc, 2, out)
            self.assertIn("--stock", out)
            fake = at.FakeRadio(model=model_of(runs_here()[0]), nak_every=11)
            rc, out = self.run_cli(base + ["--stock", stock], fake)
            self.assertEqual(rc, 0, out)
            self.assertTrue(fake.finished)
            want = at.install_image(B, app_image(), blk)
            self.assertEqual(fake.image(), want)
            for code in no_image():
                fake = at.FakeRadio(model=model_of(code))
                rc, out = self.run_cli(base + ["--stock", stock], fake)
                self.assertEqual(rc, 2, out)
                self.assertEqual((fake.mem, fake.finished), ({}, False))
                rc, out = self.run_cli(base + ["--stock", stock, "--variant", code],
                                       at.FakeRadio(model=model_of(runs_here()[0])))
                self.assertEqual(rc, 2, out)
                self.assertIn("you said", out)

    def test_restore_stock(self):
        with tempfile.TemporaryDirectory() as d:
            data = app_image(3000, seed=4)
            for code in B.anytone_models:
                names = B.anytone_models[code]
                cdd = vendor_set(d, names, data, name="FW" + code)
                fake = at.FakeRadio(model=names[0])
                rc, out = self.run_cli(["restore-stock", cdd, "--board", B.id, "--yes"], fake)
                self.assertEqual(rc, 0, out)
                self.assertEqual(fake.image()[:len(data)], data)
                # the set of another variant: refused as the vendor's updater refuses it
                for other in B.anytone_models:
                    if other != code:
                        fake = at.FakeRadio(model=model_of(other))
                        rc, out = self.run_cli(["restore-stock", cdd, "--board", B.id, "--yes"], fake)
                        self.assertEqual(rc, 2, out)
                        self.assertEqual(fake.mem, {})


if __name__ == "__main__":
    unittest.main()
