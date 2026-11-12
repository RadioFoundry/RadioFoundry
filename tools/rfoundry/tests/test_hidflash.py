# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The SGL update file and the USB HID bootloader client (updatefile.py, hidboot.py, rfoundry flash on a board of the
usb-hid-sgl family), with a synthetic donor file and a model loader. No radio, no vendor data."""
import io
import contextlib
import os
import random
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rfoundry import boards as OB, cli, hidboot, k22fcodec as G, updatefile as U  # noqa: E402
import test_k22fcodec as T  # noqa: E402

# a board on the route: the first of the family
BOARD = OB.by(family="usb-hid-sgl")[0].id


def make_update_file(plain, total=0x78801 + 0x400, tag=b"DV01test"):
    """A valid update file around `plain` (the flat image) with the header fields a vendor's file has."""
    rng = random.Random(5)
    seed = [rng.getrandbits(8) for _ in range(16)]
    k = U._forward(seed, U.PERIOD)
    pad, info_at, k0, k1 = 3, 0x100, 0x5A, 0xA5
    body_at = U.BODY_AT + pad
    n = total - body_at
    image = bytes(plain) + b"\xff" * (n - 1 - len(plain)) + b"\0"
    anchor = total - 32
    # the last 32 body bytes must be the raw stream: zeros there
    image = image[:-32] + b"\0" * 32
    body = bytes(U._ror3(image[j]) ^ k[(body_at + j - anchor) % U.PERIOD] for j in range(n))
    info = bytearray(U.INFO_LEN)
    struct.pack_into("<I", info, 2, 0)
    struct.pack_into("<I", info, 6, n)
    struct.pack_into("<I", info, 10, (~sum(body)) & 0xFFFFFFFF)
    info[50:66] = b"SG-XR-100".ljust(16, b"\xff")
    info[66:74] = b"XR-100".ljust(8, b"\xff")
    info[74:82] = b"V1.00.01"
    info[91], info[92] = 0x35, 0xF4
    struct.pack_into("<H", info, 93, 0x20)
    info[95:103] = tag
    head = bytearray(b"\0" * body_at)
    head[0:4] = b"SGL!"
    h = b"ENCV001" + bytes([pad, info_at & 255, info_at >> 8, k0, k1])
    head[4:16] = bytes(c ^ U.KEY[(4 + i) % 4] for i, c in enumerate(h))
    for i in range(U.INFO_LEN):
        head[info_at + i] = info[i] ^ (k0 if i % 2 == 0 else k1)
    return bytes(head) + body


class Format(unittest.TestCase):
    def test_round_trip_and_repack(self):
        plain = bytes(random.Random(1).getrandbits(8) for _ in range(30000))
        f = make_update_file(plain)
        u = U.UpdateFile(f)
        self.assertEqual(U.unpack(u)[:len(plain)], plain)
        self.assertEqual(u.tag, b"DV01test")
        # packing the donor's own image gives the donor back (up to the pad bytes it carried)
        self.assertEqual(U.pack(U.unpack(u), f), f)
        new = bytes(random.Random(2).getrandbits(8) for _ in range(50000))
        g = U.pack(new, f)
        self.assertEqual(len(g), len(f))
        v = U.UpdateFile(g)
        self.assertEqual((v.tag, v.company, v.model, v.version), (u.tag, u.company, u.model, u.version))
        self.assertEqual(U.unpack(v)[:len(new)], new)
        self.assertEqual(U.unpack(v)[len(new):len(new) + 8], b"\xff" * 8)

    def test_refusals(self):
        f = make_update_file(b"x" * 100)
        with self.assertRaises(U.UpdateFileError):
            U.UpdateFile(b"nope" + f[4:])
        bad = bytearray(f)
        bad[-40] ^= 1
        with self.assertRaises(U.UpdateFileError):
            U.UpdateFile(bytes(bad))
        with self.assertRaises(U.UpdateFileError):
            U.pack(b"\0" * (len(f)), f)


class Loader(unittest.TestCase):
    def test_flash_against_the_model(self):
        f = make_update_file(bytes(random.Random(3).getrandbits(8) for _ in range(40000)))
        u = U.UpdateFile(f)
        fake = hidboot.FakeBootloader(u)
        seen = []
        hidboot.Updater(fake, sleep=lambda x: None).flash(u, progress=lambda d, t: seen.append(d))
        self.assertEqual(fake.received(), u.body[:u.length])
        self.assertEqual(seen[-1], u.length)

    def test_refusals_stop_the_run(self):
        u = U.UpdateFile(make_update_file(b"y" * 1000))
        fake = hidboot.FakeBootloader(u, reject=u.model)
        with self.assertRaises(hidboot.BootError) as e:
            hidboot.Updater(fake, sleep=lambda x: None).flash(u)
        self.assertIn("model", str(e.exception))
        self.assertEqual(fake.mem, {})


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        img, self.spec = T.synthetic(random.Random(77))
        self.stock = os.path.join(self.tmp, "stock.sgl")
        with open(self.stock, "wb") as f:
            f.write(make_update_file(img))
        self.app = os.path.join(self.tmp, "rfoundry_%s.bin" % BOARD)
        with open(self.app, "wb") as f:
            f.write(bytes(random.Random(9).getrandbits(8) for _ in range(20000)))
        self._orig = G.load_specs
        self.spec["placed"] = {"tag": 0x1234}
        G.load_specs = lambda directory=G.PACKS_DIR, board=None: {"synthetic": self.spec}

    def tearDown(self):
        G.load_specs = self._orig

    def run_cli(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cli.main(argv)
        return rc, out.getvalue() + err.getvalue()

    def test_merge_writes_an_update_file_with_the_codec(self):
        outp = os.path.join(self.tmp, "out.sgl")
        rc, log = self.run_cli(["merge", self.app, "--board", BOARD, "--stock", self.stock, "-o", outp])
        self.assertEqual(rc, 0, log)
        flat = U.unpack(open(outp, "rb").read())
        app = open(self.app, "rb").read()
        self.assertEqual(flat[:len(app)], app)
        self.assertEqual(flat[G.DESC_ADDR - G.FLASH_BASE:G.DESC_ADDR - G.FLASH_BASE + 4], struct.pack("<I", G.DESC_MAGIC))

    def test_flash_simulated_and_dry_run(self):
        rc, log = self.run_cli(["flash", self.app, "--board", BOARD, "--stock", self.stock, "--simulate", "blank", "--yes"])
        self.assertEqual(rc, 0, log)
        self.assertIn("MERGED", log)
        rc, log = self.run_cli(["flash", self.app, "--board", BOARD, "--stock", self.stock, "--dry-run"])
        self.assertEqual(rc, 0, log)
        self.assertIn("dry run", log)

    def test_needs_stock_and_unknown_build(self):
        rc, log = self.run_cli(["flash", self.app, "--board", BOARD, "--dry-run"])
        self.assertNotEqual(rc, 0)
        self.assertIn("--stock", log)
        G.load_specs = lambda directory=G.PACKS_DIR, board=None: {}
        rc, log = self.run_cli(["flash", self.app, "--board", BOARD, "--stock", self.stock, "--dry-run"])
        self.assertNotEqual(rc, 0)
        self.assertIn("not one of the supported", log)
        rc, log = self.run_cli(["flash", self.app, "--board", BOARD, "--stock", self.stock, "--dry-run", "--codec", "keep"])
        self.assertEqual(rc, 0, log)


if __name__ == "__main__":
    unittest.main()
