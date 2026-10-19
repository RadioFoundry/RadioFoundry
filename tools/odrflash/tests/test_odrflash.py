# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Host tests for odrflash: sector planning, manifests, the codec merge library, key derivation, the codec decision,
and end-to-end runs against the fake bootloader. No USB, no radio, no real key or stock file.

    python3 -m unittest discover -s tools/odrflash/tests -v        (tools/ci.sh runs this)
"""
import collections
import contextlib
import hashlib
import io
import json
import os
import random
import struct
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from odrflash import boards, bootkey, cli, codecsave, debuglink, fakedfu, manifest as mf, plan, state, stockfw  # noqa: E402
from odrflash import dfu as dfumod  # noqa: E402

DM1701 = boards.DM1701
KEY = fakedfu.synthetic_key()


def app_image(n, seed=1):
    rng = random.Random(seed)
    return struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + stockfw.random_bytes(rng, n - 8)


def run_cli(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = cli.main(argv)
    return rc, out.getvalue(), err.getvalue()


_STOCK = {}


def synthetic_stock():
    if not _STOCK:
        _STOCK["v"] = stockfw.build_synthetic_stock(KEY, random.Random(7))
    return _STOCK["v"]


class SectorMap(unittest.TestCase):
    def test_f405_layout(self):
        s = DM1701.sectors
        self.assertEqual([x.index for x in s], list(range(3, 12)))
        self.assertEqual(s[0].addr, 0x0800C000)
        self.assertEqual(s[-1].end, 0x08100000)
        for a, b in zip(s, s[1:]):
            self.assertEqual(a.end, b.addr)

    def test_codec_sectors(self):
        self.assertEqual([x.index for x in DM1701.codec_sectors()], [7, 8, 10, 11])

    def test_every_sector_key_aligned(self):
        for s in DM1701.sectors:
            self.assertEqual((s.addr - DM1701.app_base) % 1024, 0)

    def test_linker_keeps_app_out_of_codec_sectors(self):
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "dm1701.ld")).read()
        self.assertIn("ORIGIN = 0x0800C000, LENGTH = 0x08060000 - 0x0800C000", ld)
        self.assertIn("ORIGIN = 0x080A0000, LENGTH = 0x080C0000 - 0x080A0000", ld)
        first_codec = min(s.addr for s in DM1701.codec_sectors())
        self.assertEqual(first_codec, 0x08060000)

    def test_sector9_is_the_data_region(self):
        """The linker's FLASH2, the board's data region and the profile's app_data are the same sector 9, which is not a
        codec sector."""
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "dm1701.ld")).read()
        self.assertIn("FLASH2  (r)   : ORIGIN = 0x080A0000, LENGTH = 0x080C0000 - 0x080A0000", ld)
        self.assertIn("} >FLASH2", ld)
        self.assertEqual(DM1701.data_regions, [(0x080A0000, 0x20000)])
        s9 = DM1701.sector_at(0x080A0000)
        self.assertEqual((s9.index, s9.addr, s9.size), (9, 0x080A0000, 0x20000))
        self.assertNotIn(9, [x.index for x in DM1701.codec_sectors()])
        mk = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "board.mk")).read()
        self.assertIn("BIN2_SECTIONS := .rodata2", mk)
        self.assertEqual(boards.DM32.data_regions, [])
        self.assertEqual(boards.MD380.data_regions, [])


    def test_dm1801pro_matches_its_linker_script(self):
        b = boards.get("dm1801pro")
        self.assertEqual((b.app_base, b.flash_end, b.family), (0x0300C000, 0x03100000, boards.DM32.family))
        self.assertEqual([(s.addr, s.size) for s in b.codec_sectors()], [(0x030E8000, 0x18000)])
        self.assertEqual(b.codec.windows, boards.DM32.codec.windows)
        self.assertEqual(b.protected, boards.DM32.protected)
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1801pro", "dm1801pro.ld")).read()
        self.assertIn("ORIGIN = 0x0300C000, LENGTH = 0x030E8000 - 0x0300C000", ld)
        self.assertIn("__codec_slot = 0x030E8000;", ld)
        self.assertIs(boards.from_debuglink_name("DM-1801 Pro"), b)


class Planning(unittest.TestCase):
    def test_keep_small_image(self):
        img = app_image(5000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3])
        self.assertEqual([s.index for s in p.kept], [7, 8, 10, 11])
        st = p.writes[0]
        self.assertEqual([n for n, _ in st.blocks], [2, 3, 4, 5, 6])
        self.assertEqual(st.blocks[-1][1], img[4096:] + b"\xFF" * (5 * 1024 - len(img)))

    def test_keep_release_sized_image(self):
        img = app_image(220780)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6])
        self.assertEqual(len(p.writes[0].blocks), 16)
        self.assertEqual(len(p.writes[1].blocks), 64)
        self.assertEqual(len(p.writes[2].blocks), 128)
        self.assertEqual(len(p.writes[3].blocks), (220780 - 0x34000 - 1) // 1024 + 1)
        self.assertEqual(b"".join(b for st in p.writes for _, b in st.blocks)[:len(img)], img)
        skipped = [st.sector.index for st in p.steps if st.action == "skip"]
        self.assertEqual(skipped, [9])

    def test_keep_fills_whole_app_area(self):
        img = app_image(0x08060000 - 0x0800C000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6])

    def sector9_plan(self, mode, n=40000, app_n=150000, segs=()):
        data = app_image(n, seed=5)
        p = plan.make_plan(DM1701, [(DM1701.app_base, app_image(app_n)), (0x080A0000, data)], mode, segs)
        return p, data

    def test_keep_with_sector9_data(self):
        p, data = self.sector9_plan(plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 9])
        self.assertEqual([s.index for s in p.kept], [7, 8, 10, 11])
        s9 = [st for st in p.writes if st.sector.index == 9][0]
        self.assertEqual([n for n, _ in s9.blocks][:2], [2, 3])			# numbered from 2 per sector
        self.assertEqual(len(s9.blocks), (40000 + 1023) // 1024)
        flat = b"".join(b for _, b in s9.blocks)
        self.assertEqual(flat[:40000], data)
        self.assertEqual(flat[40000:], b"\xFF" * (len(flat) - 40000))
        text = "\n".join(p.describe())
        self.assertIn("sector  9  0x080A0000..0x080BFFFF", text)
        self.assertIn("never erased or written: codec sector(s) 7, 8, 10, 11", text)
        self.assertIn("never erased or written: the bootloader", text)

    def test_merge_with_sector9_data(self):
        _, profiles, code, tables = synthetic_stock()
        prof = list(profiles.values())[0]
        segs = [(prof["code"][0], code), (prof["tables"][0], tables)]
        p, _ = self.sector9_plan(plan.MERGE, segs=segs)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 7, 8, 9, 10, 11])

    def test_sector9_data_outside_its_region_refused(self):
        """Firmware data goes in sector 9 only: never the codec sectors, the bootloader or the gap past sector 9."""
        img = app_image(5000)
        for addr, n in ((0x08060000, 100), (0x08090000, 100), (0x080C0000, 100), (0x080E0000, 100),
                        (0x0800F000, 100), (0x08000000, 100), (0x080BFFC0, 100), (0x08040000, 100)):
            for mode in (plan.KEEP, plan.MERGE):
                _, profiles, code, tables = synthetic_stock()
                prof = list(profiles.values())[0]
                csegs = [(prof["code"][0], code), (prof["tables"][0], tables)] if mode == plan.MERGE else []
                with self.assertRaises(plan.PlanError, msg="%s %s" % (hex(addr), mode)):
                    plan.make_plan(DM1701, [(DM1701.app_base, img), (addr, b"d" * n)], mode, csegs)
        # a board without a data region takes no second range
        with self.assertRaises(plan.PlanError):
            plan.make_plan(boards.MD380, [(boards.MD380.app_base, img), (0x080A0000, b"d" * 100)], plan.MERGE,
                           [(a, b"\0" * n) for a, n in boards.MD380.codec.windows])

    def test_image_into_codec_sector_refused(self):
        img = app_image(0x08060000 - 0x0800C000 + 1)
        for mode in (plan.KEEP, plan.MERGE):
            with self.assertRaises(plan.PlanError):
                plan.make_plan(DM1701, [(DM1701.app_base, img)], mode)

    def test_merge_writes_codec_sectors(self):
        _, profiles, code, tables = synthetic_stock()
        prof = list(profiles.values())[0]
        img = app_image(100000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.MERGE, [(prof["code"][0], code), (prof["tables"][0], tables)])
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 7, 8, 10, 11])
        self.assertEqual(p.kept, [])
        s7 = [st for st in p.writes if st.sector.index == 7][0]
        flat = b"".join(b for _, b in s7.blocks)
        off = 0x08074714 - 0x08060000
        self.assertEqual(flat[:off], b"\xFF" * off)
        self.assertEqual(flat[off:], code[:0x08080000 - 0x08074714])
        s11 = [st for st in p.writes if st.sector.index == 11][0]
        self.assertEqual(b"".join(b for _, b in s11.blocks)[:0x080E4240 - 0x080E0000],
                         tables[0x080E0000 - 0x080D19F0:])

    def test_merge_needs_whole_codec(self):
        _, profiles, code, tables = synthetic_stock()
        prof = list(profiles.values())[0]
        segs = [(prof["code"][0], code), (prof["tables"][0], tables)][:1]
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(DM1701.app_base, app_image(1000))], plan.MERGE, segs)

    def test_keep_rejects_codec_data(self):
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(DM1701.app_base, app_image(1000))], plan.KEEP, [(0x08074714, b"\0" * 4)])

    def test_bootloader_and_bounds(self):
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(0x08008000, b"\0" * 2048)], plan.KEEP)
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(0x0800C400, b"\0" * 2048)], plan.KEEP)	# not at the vector table
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [], plan.KEEP)

    def test_describe_names_every_sector(self):
        p = plan.make_plan(DM1701, [(DM1701.app_base, app_image(30000))], plan.KEEP)
        text = "\n".join(p.describe())
        for i in range(3, 12):
            self.assertIn("sector %2d" % i, text)
        self.assertIn("erase 2 sector(s): 3, 4", text)
        self.assertIn("never erased or written: codec sector(s) 7, 8, 10, 11", text)


class Manifests(unittest.TestCase):
    def test_generate_and_verify(self):
        img = app_image(220780)
        m = mf.generate(DM1701, img, "v1.2.3", "a" * 40, "FM,DMR", "gcc 13.3")
        self.assertEqual(m["sectors"], [3, 4, 5, 6])
        self.assertEqual(m["codec"]["sectors"], [7, 8, 10, 11])
        self.assertEqual(m["codec"]["profile"], "dm1701-v0206-csv")
        self.assertFalse(m["codec"]["included"])
        self.assertEqual(m["ranges"], [{"addr": "0x0800C000", "size": 220780}])
        self.assertEqual(m["image"]["file"], "odr-dm1701-v1.2.3.bin")
        m2 = json.loads(mf.dumps(m))
        self.assertIs(mf.verify(m2, img), DM1701)
        self.assertIs(mf.verify(m2, img, "dm1701"), DM1701)

    def test_verify_refusals(self):
        img = app_image(4096)
        m = mf.generate(DM1701, img, "v1", "b" * 40)
        with self.assertRaises(mf.ManifestError):
            mf.verify(m, img[:-1] + b"\0")				# hash
        with self.assertRaises(mf.ManifestError):
            mf.verify(m, img, "dm32")				# wrong radio
        bad = json.loads(mf.dumps(m))
        bad["format"] = "odr-release/9"
        with self.assertRaises(mf.ManifestError):
            mf.verify(bad, img)
        bad = json.loads(mf.dumps(m))
        bad["codec"]["profile"] = "dm1701-v0203-csv"
        with self.assertRaises(mf.ManifestError):
            mf.verify(bad, img)

    def test_manifest_with_sector9_data(self):
        img, data = app_image(150000), app_image(40000, seed=3)
        m = mf.generate(DM1701, img, "v1.2.3", "a" * 40, data=data)
        self.assertEqual(m["ranges"], [{"addr": "0x0800C000", "size": 150000}, {"addr": "0x080A0000", "size": 40000}])
        self.assertEqual(m["sectors"], [3, 4, 5, 9])
        self.assertEqual(m["image"]["size"], 190000)
        self.assertEqual(m["image"]["sha256"], hashlib.sha256(img + data).hexdigest())
        m2 = json.loads(mf.dumps(m))
        self.assertIs(mf.verify(m2, img + data, "dm1701"), DM1701)
        self.assertEqual(mf.segments(m2, img + data), [(0x0800C000, img), (0x080A0000, data)])
        with self.assertRaises(mf.ManifestError):
            mf.verify(m2, img)								# the file must hold both parts
        for addr in ("0x08060000", "0x08090000", "0x080C0000", "0x080BFFF0", "0x0800F000", "0x08000000"):
            bad = json.loads(mf.dumps(m))
            bad["ranges"][1]["addr"] = addr
            with self.assertRaises(mf.ManifestError, msg=addr):
                mf.verify(bad, img + data)
        bad = json.loads(mf.dumps(m))
        bad["ranges"][0]["addr"] = "0x08010000"
        with self.assertRaises(mf.ManifestError):
            mf.verify(bad, img + data)						# the application starts at the base
        with self.assertRaises(mf.ManifestError):
            mf.generate(DM1701, img, "v1", "a" * 40, data=b"x" * (0x20000 + 1))	# more than sector 9
        with self.assertRaises(mf.ManifestError):
            mf.generate(boards.DM32, img, "v1", "a" * 40, data=data)		# no data region on that board

    def test_generate_refuses_codec_overlap(self):
        with self.assertRaises(mf.ManifestError):
            mf.generate(DM1701, app_image(0x08060000 - 0x0800C000 + 4), "v1", "c" * 40)

    def test_release_tool(self):
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        img = app_image(12345)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "in.bin")
            open(bp, "wb").write(img)
            out = os.path.join(d, "dist")
            with contextlib.redirect_stdout(io.StringIO()):
                release.main(["--board", "dm1701", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                              "--features", "FM", "--toolchain", "test", "--no-rodata2"])
                release.main(["--board", "dm1701", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                              "--features", "FM", "--toolchain", "test", "--no-rodata2"])	# again: SHA256SUMS not duplicated
            m = mf.load(os.path.join(out, "odr-dm1701-v9.9.9.json"))
            self.assertIs(mf.verify(m, open(os.path.join(out, "odr-dm1701-v9.9.9.bin"), "rb").read()), DM1701)
            sums = open(os.path.join(out, "SHA256SUMS")).read().splitlines()
            self.assertEqual(len(sums), 2)
            self.assertIn(hashlib.sha256(img).hexdigest() + "  odr-dm1701-v9.9.9.bin", sums)
            # a merged image (codec signature at the code window) is refused
            merged = bytearray(b"\xFF" * (0x08074714 - 0x0800C000 + 4))
            merged[:len(img)] = img
            struct.pack_into("<I", merged, 0x08074714 - 0x0800C000, stockfw.CODEC_SIG)
            open(bp, "wb").write(merged)
            with self.assertRaises(SystemExit):
                release.main(["--board", "dm1701", "--version", "v9", "--bin", bp, "--out", out, "--git", "d" * 40,
                              "--features", "", "--toolchain", "", "--no-rodata2"])

    def test_release_needs_the_first_stage(self):
        """The DB25-D's image goes to 0x0300C000, its first stage's sector: the application alone is refused, the
        install image (first stage, then the application) is released."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        stage = bytes([0x2C, 0x11, 0x01, 0xC0]) + bytes(0x1000 - 4)
        app = app_image(20000)
        with tempfile.TemporaryDirectory() as d:
            bp, out = os.path.join(d, "odr_db25d.bin"), os.path.join(d, "dist")
            args = ["--board", "db25d", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                    "--features", "FM", "--toolchain", "test"]
            open(bp, "wb").write(app)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as e:
                release.main(args)
            self.assertIn("_install.bin", str(e.exception))
            open(bp, "wb").write(stage)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                release.main(args)							# the first stage alone
            open(bp, "wb").write(stage + app)
            with contextlib.redirect_stdout(io.StringIO()):
                release.main(args)
            self.assertEqual(open(os.path.join(out, "odr-db25d-v9.9.9.bin"), "rb").read(), stage + app)


class ReleaseSector9(unittest.TestCase):
    def run_release(self, d, extra, rodata=None, name="odr_dm1701"):
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        bp = os.path.join(d, name + ".bin")
        open(bp, "wb").write(app_image(30000))
        if rodata is not None:
            open(os.path.join(d, name + "_rodata2.bin"), "wb").write(rodata)
        out = os.path.join(d, "dist")
        with contextlib.redirect_stdout(io.StringIO()):
            release.main(["--board", "dm1701", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                          "--features", "FM", "--toolchain", "test"] + extra)
        return out

    def test_release_packs_both_ranges(self):
        data = app_image(9000, seed=4)
        with tempfile.TemporaryDirectory() as d:
            out = self.run_release(d, [], data)
            whole = open(os.path.join(out, "odr-dm1701-v9.9.9.bin"), "rb").read()
            self.assertEqual(whole, app_image(30000) + data)
            m = mf.load(os.path.join(out, "odr-dm1701-v9.9.9.json"))
            self.assertEqual(m["sectors"], [3, 4, 9])
            self.assertEqual([r["addr"] for r in m["ranges"]], ["0x0800C000", "0x080A0000"])
            self.assertIs(mf.verify(m, whole), DM1701)
            sums = open(os.path.join(out, "SHA256SUMS")).read()
            self.assertIn(hashlib.sha256(whole).hexdigest() + "  odr-dm1701-v9.9.9.bin", sums)

    def merge_cli(self, image, extra=()):
        """odrflash merge with the synthetic stock file and key (the real profile and key hash are patched out)."""
        stock, profiles, _, _ = synthetic_stock()
        with tempfile.TemporaryDirectory() as d:
            sp, op = os.path.join(d, "stock.bin"), os.path.join(d, "merged.bin")
            open(sp, "wb").write(stock)
            with mock.patch.dict(stockfw.PROFILES, profiles, clear=True), \
                    mock.patch.object(dfumod, "check_key", lambda k, b: k), \
                    mock.patch.object(stockfw, "derive_key", lambda s: KEY):
                rc, out, err = run_cli(["merge", image, "--stock", sp, "-o", op] + list(extra))
            return rc, out, err, (open(op, "rb").read() if os.path.exists(op) else b"")

    def test_cli_merge_of_a_release_image_keeps_sector9(self):
        """odrflash merge odr-dm1701-v.bin (two ranges, manifest next to it) puts the data at 0x080A0000, not on the
        application's end."""
        data = app_image(9000, seed=4)
        with tempfile.TemporaryDirectory() as d:
            out = self.run_release(d, [], data)
            rc, so, se, merged = self.merge_cli(os.path.join(out, "odr-dm1701-v9.9.9.bin"))
        self.assertEqual(rc, 0, so + se)
        self.assertEqual(merged[:30000], app_image(30000))
        self.assertEqual(merged[30000:0x08074714 - 0x0800C000], b"\xFF" * (0x08074714 - 0x0800C000 - 30000))
        self.assertEqual(merged[0x080A0000 - 0x0800C000:][:9000], data)

    def test_cli_merge_of_a_local_build_reads_the_sector9_file(self):
        data = app_image(9000, seed=4)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "odr_dm1701.bin")
            open(bp, "wb").write(app_image(30000))
            open(os.path.join(d, "odr_dm1701_rodata2.bin"), "wb").write(data)
            rc, so, se, merged = self.merge_cli(bp)
            self.assertEqual(rc, 0, so + se)
            self.assertEqual(merged[0x080A0000 - 0x0800C000:][:9000], data)
            other = os.path.join(d, "other.bin")
            open(other, "wb").write(app_image(7000, seed=5))
            rc, so, se, merged = self.merge_cli(bp, ["--rodata2", other])
            self.assertEqual(rc, 0, so + se)
            self.assertEqual(merged[0x080A0000 - 0x0800C000:][:7000], app_image(7000, seed=5))
            rc, so, se, _ = self.merge_cli(bp, ["--rodata2", os.path.join(d, "nope.bin")])
            self.assertNotEqual(rc, 0)							# an explicit file that isn't there
            self.assertIn("nope.bin", se)

    def test_release_needs_the_sector9_image(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit) as e:
                self.run_release(d, [])							# no sibling file
            self.assertIn("_rodata2.bin", str(e.exception))
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                self.run_release(d, [], b"")						# empty
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                self.run_release(d, ["--no-rodata2", "--bin2", "x"])
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                self.run_release(d, [], b"x" * 0x20001)					# does not fit sector 9
        with tempfile.TemporaryDirectory() as d:
            out = self.run_release(d, ["--no-rodata2"], b"ignored")			# explicit: the application alone
            m = mf.load(os.path.join(out, "odr-dm1701-v9.9.9.json"))
            self.assertEqual(m["sectors"], [3, 4])


class WaitForDfu(unittest.TestCase):
    def test_no_bootloader_raises_the_hint_not_a_format_error(self):
        with mock.patch.object(dfumod, "find", return_value=[]):
            with self.assertRaises(cli.UsageError) as cm:
                cli.wait_for_dfu(None, boards.get("md380"), 0)
        self.assertIn("no radio bootloader found (USB 0483:DF11)", str(cm.exception))


class OldRouteGuard(unittest.TestCase):
    def test_stm32_dfu_warns_about_a_sector9_image(self):
        """tools/stm32_dfu.py writes one contiguous image: next to a build that has a sector 9 image it warns."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import stm32_dfu
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "odr_dm1701.bin")
            open(bp, "wb").write(app_image(5000))
            for sibling, expect in ((False, False), (True, True)):
                if sibling:
                    open(os.path.join(d, "odr_dm1701_rodata2.bin"), "wb").write(b"x")
                err = io.StringIO()
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                    rc = stm32_dfu.main([bp, "--model", "dm1701", "--dry-run"])
                self.assertEqual(rc, 0)
                self.assertEqual("odr_dm1701_rodata2.bin exists" in err.getvalue(), expect)

    def test_stm32_dfu_refuses_a_release_image_with_two_ranges(self):
        """A release .bin holds the application and its sector 9 data back to back; written as one image the data
        would land over sectors 3-6. The manifest next to it says so."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import stm32_dfu
        with tempfile.TemporaryDirectory() as d:
            out = ReleaseSector9().run_release(d, [], app_image(9000, seed=4))
            bp = os.path.join(out, "odr-dm1701-v9.9.9.bin")
            err = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                rc = stm32_dfu.main([bp, "--model", "dm1701", "--dry-run"])
            self.assertEqual(rc, 2)
            self.assertIn("2 flash ranges", err.getvalue())
            os.makedirs(os.path.join(d, "x"))
            out1 = ReleaseSector9().run_release(os.path.join(d, "x"), ["--no-rodata2"])
            err = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                rc = stm32_dfu.main([os.path.join(out1, "odr-dm1701-v9.9.9.bin"), "--model", "dm1701", "--dry-run"])
            self.assertEqual(rc, 0, err.getvalue())					# one range: as before


class StockLibrary(unittest.TestCase):
    def test_extract_and_merge(self):
        stock, profiles, code, tables = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual(segs, [(0x08074714, code), (0x080D19F0, tables)])
        img = app_image(5000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(len(merged), 0x080E4240 - 0x0800C000)
        self.assertEqual(merged[:len(img)], img)
        self.assertEqual(merged[0x08074714 - 0x0800C000:0x08084930 - 0x0800C000], code)
        self.assertEqual(merged[0x080D19F0 - 0x0800C000:], tables)

    def test_refusals(self):
        stock, profiles, _, _ = synthetic_stock()
        with self.assertRaises(stockfw.StockError):
            stockfw.extract(stock, bytes([KEY[0] ^ 1]) + KEY[1:], profiles)
        with self.assertRaises(stockfw.StockError):
            stockfw.extract(stock, KEY)				# not the real file
        with self.assertRaises(stockfw.StockError):
            stockfw.parse_container(b"x" * 300)
        with self.assertRaises(stockfw.StockError):
            stockfw.merge(stock, list(profiles.values())[0], [(0x08074714, b"")])

    def test_split_merged(self):
        stock, profiles, code, tables = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        img = app_image(5000)
        app, got, csegs, data = stockfw.split_merged(stockfw.merge(img, prof, segs), profiles)
        self.assertEqual((app, got["codec_profile"], csegs, data), (img, prof["codec_profile"], segs, []))
        self.assertIsNone(stockfw.split_merged(img, profiles))
        self.assertIsNone(stockfw.split_merged(app_image(0x80000), profiles))	# past the codec start, no codec there
        bad = bytearray(stockfw.merge(img, prof, segs))
        bad[0x08074714 - 0x0800C000 + 100] ^= 1
        with self.assertRaises(stockfw.StockError):
            stockfw.split_merged(bytes(bad), profiles)				# a codec, but not this profile's
        extra = bytearray(stockfw.merge(img, prof, segs))
        extra[0x08090000 - 0x0800C000] = 0
        with self.assertRaises(stockfw.StockError):
            stockfw.split_merged(bytes(extra), profiles)			# data between the windows, outside sector 9

    def test_merge_with_sector9_data(self):
        stock, profiles, code, tables = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        img = app_image(5000)
        rodata = app_image(3000, seed=9)
        merged = stockfw.merge(img, prof, segs, [(0x080A0000, rodata)])
        self.assertEqual(merged[0x080A0000 - 0x0800C000:0x080A0000 - 0x0800C000 + 3000], rodata)
        self.assertEqual(merged[0x08074714 - 0x0800C000:0x08084930 - 0x0800C000], code)
        app, _, csegs, data = stockfw.split_merged(merged, profiles)
        self.assertEqual((app, csegs, data), (img, segs, [(0x080A0000, rodata)]))
        # trailing 0xFF of the data is dropped (an erased sector holds the same), an empty sector 9 gives no data
        _, _, _, data = stockfw.split_merged(stockfw.merge(img, prof, segs, [(0x080A0000, rodata + b"\xFF" * 10)]), profiles)
        self.assertEqual(data, [(0x080A0000, rodata)])
        for bad in (0x08090000, 0x080BFFFF, 0x080C0000, 0x0800C000):	# sector 8, past sector 9, sector 10, the application
            with self.assertRaises(stockfw.StockError, msg=hex(bad)):
                stockfw.merge(img, prof, segs, [(bad, b"xx")])
        with self.assertRaises(stockfw.StockError):				# a profile without a data region takes none
            stockfw.merge(img, dict(prof, app_data=[]), segs, [(0x080A0000, rodata)])

    def test_derive_key(self):
        stock, _, _, _ = synthetic_stock()
        k = stockfw.derive_key(stock, hashlib.sha256(KEY).hexdigest())
        self.assertEqual(k, KEY)
        with self.assertRaises(stockfw.StockError):
            stockfw.derive_key(stock, "0" * 64)

    def test_derive_key_corrects_wrong_columns(self):
        """Columns where the most common plain byte isn't zero are fixed by the hash-checked search."""
        stock, _, _, _ = synthetic_stock()
        s = bytearray(stock)
        segs = stockfw.parse_container(stock)
        total = sum(n for _, n in segs)
        rng = random.Random(5)
        # columns 77 and 300: just over half of their plain zero bytes become 0x5A, so 0x5A narrowly wins the vote
        for col in (77, 300):
            for i in range(col, total, 1024):
                if s[stockfw.HEADER_LEN + i] == KEY[col] and rng.random() < 0.53:
                    s[stockfw.HEADER_LEN + i] = 0x5A ^ KEY[col]
        cols = collections.Counter(s[stockfw.HEADER_LEN + 77:stockfw.HEADER_LEN + total:1024])
        self.assertEqual(cols.most_common(1)[0][0], 0x5A ^ KEY[77])	# the plain vote alone is wrong
        k = stockfw.derive_key(bytes(s), hashlib.sha256(KEY).hexdigest())
        self.assertEqual(k, KEY)

    def test_single_segment_container(self):
        """The MD-380 files: count word 0xFFFFFFFF, one segment in the first table entry."""
        hdr = bytearray(stockfw.HEADER_LEN)
        hdr[:len(stockfw.MAGIC)] = stockfw.MAGIC
        struct.pack_into("<I", hdr, 0x7C, 0xFFFFFFFF)
        struct.pack_into("<II", hdr, 0x80, 0x0800C000, 0x400)
        data = bytes(hdr) + bytes(0x400) + bytes(0x100)
        self.assertEqual(stockfw.parse_container(data), [(0x0800C000, 0x400)])
        with self.assertRaises(stockfw.StockError):
            stockfw.parse_container(data[:0x300])		# the segment runs past the end

    def test_md380_board(self):
        b = boards.get("md380")
        self.assertFalse(b.releasable)
        self.assertFalse(b.keep_codec_supported)	# the code window shares sector 6 with the application
        self.assertEqual(b.codec.id, "md380-d2032")
        self.assertEqual([s.index for s in b.codec_sectors()], [6, 7, 8, 10, 11])
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "md380", "md380.ld")).read()
        self.assertIn("ORIGIN = 0x0800C000, LENGTH = 0x08050C44 - 0x0800C000", ld)
        self.assertEqual(b.codec.windows[0][0], 0x08050C44)
        self.assertIs(boards.from_debuglink_name("MD-380 VHF"), b)

    def test_md380_profile(self):
        p = stockfw.PROFILES["e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43"]
        self.assertEqual((p["board"], p["key_sha256"]), ("md380", stockfw.MD380_KEY_SHA256))
        self.assertEqual(p["segments"], [(0x0800C000, 0xF2C00)])
        self.assertEqual([(a, n) for a, n, _ in stockfw.windows(p)],
                         [(0x08050C44, 0x10884), (0x0808FC64, 0xAFF8), (0x080D0FC8, 0x24), (0x080F9698, 0x18),
                          (0x080F9E74, 0x14), (0x080FD490, 0x20)])
        self.assertEqual(stockfw.profile_for_board("md380"),
                         "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43")
        self.assertEqual(stockfw.identify(b"x"), None)

    def test_md380_extract_merge_and_plan(self):
        """A synthetic file with the D002.032 layout: six windows out, merge, and a merge plan that shares sector 6."""
        b = boards.get("md380")
        stock, profiles, code, tables = stockfw.build_synthetic_stock(KEY, random.Random(380),
                                                                      stockfw.profile_for_board("md380"))
        self.assertEqual(stockfw.parse_container(stock), [(0x0800C000, 0xF2C00)])
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual(len(segs), 6)
        self.assertEqual(segs[0], (0x08050C44, code))
        img = app_image(0x08050C44 - 0x0800C000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(merged[0x08050C44 - 0x0800C000:0x08050C44 - 0x0800C000 + len(code)], code)
        with self.assertRaises(stockfw.StockError):
            stockfw.merge(img + b"\0", prof, segs)
        p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 8, 10, 11])
        s6 = [st for st in p.writes if st.sector.index == 6][0]
        self.assertEqual(s6.content, "app+codec")
        flat = b"".join(d for _, d in s6.blocks)
        self.assertEqual(flat[:0x08050C44 - 0x08040000], img[0x08040000 - 0x0800C000:])
        self.assertEqual(flat[0x08050C44 - 0x08040000:], code[:0x08060000 - 0x08050C44])
        with self.assertRaises(plan.PlanError):		# keep would erase the codec's part of sector 6
            plan.make_plan(b, [(b.app_base, app_image(0x08040000 - 0x0800C000 + 4))], plan.KEEP)
        with self.assertRaises(plan.PlanError):		# the application can't overlap the code window
            plan.make_plan(b, [(b.app_base, app_image(0x08050C44 - 0x0800C000 + 4))], plan.MERGE, segs)

    def test_derive_key_refines_a_bad_vote(self):
        """Plain text like Thumb code (a Markov chain over a vocabulary of halfwords) where 0x00 is common but not
        dominant, as in the MD-380 image: the vote gets about a third of the columns wrong, the pair model fixes them."""
        rng = random.Random(1)
        vocab = [rng.randrange(1, 65536) for _ in range(48)] + [0]
        weights = [1.0 / (i + 1) for i in range(48)] + [1.0]
        n = 0xF2C00
        nxt = {w: rng.choices(vocab, weights, k=3) for w in vocab}
        pool = rng.choices(vocab, weights, k=n // 2)
        hw = []
        cur = 0
        for i in range(n // 2):
            cur = nxt[cur][i % 3] if rng.random() < 0.6 else pool[i]
            hw.append(cur)
        plain = struct.pack("<%dH" % (n // 2), *hw)
        hdr = bytearray(stockfw.HEADER_LEN)
        hdr[:len(stockfw.MAGIC)] = stockfw.MAGIC
        struct.pack_into("<I", hdr, 0x7C, 0xFFFFFFFF)
        struct.pack_into("<II", hdr, 0x80, 0x0800C000, n)
        stream = (KEY * (n // 1024 + 1))[:n]
        stock = bytes(hdr) + bytes(a ^ b for a, b in zip(plain, stream)) + bytes(0x100)
        payload = stock[stockfw.HEADER_LEN:stockfw.HEADER_LEN + n]
        vote = bytes(collections.Counter(payload[j::1024]).most_common(1)[0][0] for j in range(1024))
        self.assertGreater(sum(a != b for a, b in zip(vote, KEY)), 200)	# the plain vote is far off
        self.assertEqual(stockfw.derive_key(stock, hashlib.sha256(KEY).hexdigest()), KEY)

    def test_real_profile_table(self):
        p = stockfw.PROFILES["0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]
        self.assertEqual(p["codec_profile"], DM1701.codec.id)
        self.assertEqual(DM1701.codec.windows, [(0x08074714, 0x1021C), (0x080D19F0, 0x12850)])


class CodecDecision(unittest.TestCase):
    man = {"codec": {"profile": "dm1701-v0206-csv"}}

    def d(self, **kw):
        args = dict(requested="auto", have_stock=False, probe=None, stock_installed=None, ask=None)
        args.update(kw)
        return cli.decide_codec(DM1701, self.man, args["requested"], args["have_stock"], args["probe"],
                                args["stock_installed"], args["ask"])

    def probe(self, codec, board="DM-1701"):
        return {"board": board, "codec": codec, "git": "x"}

    def test_probe_with_codec_keeps_even_with_stock(self):
        self.assertEqual(self.d(probe=self.probe("dm1701-v0206-csv")).mode, plan.KEEP)
        self.assertEqual(self.d(probe=self.probe("dm1701-v0206-csv"), have_stock=True).mode, plan.KEEP)

    def test_probe_without_codec(self):
        self.assertEqual(self.d(probe=self.probe("absent"), have_stock=True).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe("absent"))
        d = self.d(probe=self.probe("absent"), requested="keep")
        self.assertEqual(d.mode, plan.KEEP)
        self.assertFalse(d.expect_codec)
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe(None))				# older firmware: unknown, no stock

    def test_probe_other_board_refused(self):
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe("dm1701-v0206-csv", board="DM-32UV"))

    def test_stock_installed(self):
        self.assertEqual(self.d(stock_installed="V02.06 CSV").mode, plan.KEEP)
        self.assertEqual(self.d(stock_installed="v02.06-csv").mode, plan.KEEP)
        with self.assertRaises(cli.UsageError):
            self.d(stock_installed="V02.03 CSV")
        self.assertEqual(self.d(stock_installed="V02.03 CSV", have_stock=True).mode, plan.MERGE)

    def test_unknown(self):
        with self.assertRaises(cli.UsageError):
            self.d()
        self.assertEqual(self.d(have_stock=True).mode, plan.MERGE)
        self.assertEqual(self.d(requested="keep").mode, plan.KEEP)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.d(ask=lambda q: True).mode, plan.KEEP)
            with self.assertRaises(cli.UsageError):
                self.d(ask=lambda q: False)

    def test_merge_requested(self):
        self.assertEqual(self.d(requested="merge", have_stock=True, probe=self.probe("dm1701-v0206-csv")).mode,
                         plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(requested="merge")

    def test_dm32_always_merges(self):
        man = {"codec": {"profile": "dm32-049"}}
        self.assertEqual(cli.decide_codec(boards.DM32, man, "auto", True, None, None, None).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            cli.decide_codec(boards.DM32, man, "auto", False, None, None, None)

    def test_dm1801pro_always_merges(self):
        man = {"codec": {"profile": "dm1801pro-049"}}
        self.assertEqual(cli.decide_codec(boards.DM1801PRO, man, "auto", True, None, None, None).mode, plan.MERGE)


class StateFile(unittest.TestCase):
    def test_probe_memory(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(state.recent_probe(d))
            state.remember_probe({"board": "DM-1701", "codec": "x"}, d, now=1000.0)
            self.assertEqual(state.recent_probe(d, now=1000.0 + 60)["codec"], "x")
            self.assertIsNone(state.recent_probe(d, now=1000.0 + state.PROBE_MAX_AGE_S + 1))
            state.forget_probe(d)
            self.assertIsNone(state.recent_probe(d, now=1001.0))


class FakeBootloaderEndToEnd(unittest.TestCase):
    def flash_with(self, mode, initial_codec=True):
        rng = random.Random(11)
        codec = [(a, stockfw.random_bytes(rng, n)) for a, n in DM1701.codec.windows]
        flash = fakedfu.stock_flash(DM1701, rng, codec if initial_codec else [])
        dev = fakedfu.FakeAnyRoad(DM1701, KEY, flash)
        before = bytes(dev.flash)
        img = app_image(150000)
        segs = []
        if mode == plan.MERGE:
            _, profiles, code, tables = synthetic_stock()
            prof = list(profiles.values())[0]
            segs = [(prof["code"][0], code), (prof["tables"][0], tables)]
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], mode, segs)
        dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, KEY)
        return dev, before, img, p, segs

    def test_keep_leaves_codec_and_bootloader(self):
        dev, before, img, p, _ = self.flash_with(plan.KEEP)
        self.assertEqual(dev.vendor, [0x01, 0x31])
        self.assertEqual(dev.erased, [0x0800C000, 0x08010000, 0x08020000])
        self.assertTrue(dev.exited)
        self.assertEqual(dev.violations, [])
        self.assertEqual(dev.read(0x0800C000, len(img)), img)
        for s in DM1701.codec_sectors() + [DM1701.sectors[6]]:	# 7, 8, 10, 11 and the free sector 9
            self.assertEqual(dev.read(s.addr, s.size), before[s.addr - 0x08000000:s.end - 0x08000000], s)
        self.assertEqual(dev.read(0x08000000, 0xC000), before[:0xC000])

    def test_merge_writes_codec(self):
        dev, before, img, p, segs = self.flash_with(plan.MERGE, initial_codec=False)
        self.assertEqual(dev.erased, [0x0800C000, 0x08010000, 0x08020000, 0x08060000, 0x08080000, 0x080C0000,
                                      0x080E0000])
        self.assertEqual(dev.violations, [])
        for a, data in segs:
            self.assertEqual(dev.read(a, len(data)), data)
        self.assertEqual(dev.read(0x08060000, 0x08074714 - 0x08060000), b"\xFF" * (0x08074714 - 0x08060000))
        self.assertEqual(dev.read(0x08000000, 0xC000), before[:0xC000])

    def test_sector9_written_with_the_application(self):
        """The application and sector 9 data in one run: sectors 3-5 and 9 erased and written, 7, 8, 10, 11 and the
        bootloader untouched, and the fake bootloader (which flags any write outside an erased sector) saw no violation."""
        rng = random.Random(21)
        codec = [(a, stockfw.random_bytes(rng, n)) for a, n in DM1701.codec.windows]
        dev = fakedfu.FakeAnyRoad(DM1701, KEY, fakedfu.stock_flash(DM1701, rng, codec))
        before = bytes(dev.flash)
        img, data = app_image(150000), app_image(40000, seed=8)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img), (0x080A0000, data)], plan.KEEP)
        dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, KEY)
        self.assertEqual(dev.erased, [0x0800C000, 0x08010000, 0x08020000, 0x080A0000])
        self.assertEqual(dev.violations, [])
        self.assertEqual(dev.read(0x0800C000, len(img)), img)
        self.assertEqual(dev.read(0x080A0000, len(data)), data)
        self.assertEqual(dev.read(0x080A0000 + len(data), 0x20000 - len(data)), b"\xFF" * (0x20000 - len(data)))
        for s in DM1701.codec_sectors():
            self.assertEqual(dev.read(s.addr, s.size), before[s.addr - 0x08000000:s.end - 0x08000000], s)
        self.assertEqual(dev.read(0x08000000, 0xC000), before[:0xC000])

    def test_cli_dev_build_with_sector9_image(self):
        """odrflash flash odr_dm1701.bin --board dm1701 picks up odr_dm1701_rodata2.bin next to it."""
        img, data = app_image(150000), app_image(40000, seed=8)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "odr_dm1701.bin")
            open(bp, "wb").write(img)
            open(os.path.join(d, "odr_dm1701_rodata2.bin"), "wb").write(data)
            rc, out, err = run_cli(["flash", bp, "--board", "dm1701", "--simulate", "odr-codec", "--dry-run"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("40000 bytes for sector 9", out)
            self.assertIn("erase 4 sector(s): 3, 4, 5, 9", out)
            self.assertIn("never erased or written: codec sector(s) 7, 8, 10, 11", out)
            rc, out, err = run_cli(["flash", bp, "--board", "dm1701", "--simulate", "odr-codec", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("sim      PASS", out)
            os.remove(os.path.join(d, "odr_dm1701_rodata2.bin"))
            rc, out, err = run_cli(["flash", bp, "--board", "dm1701", "--simulate", "odr-codec", "--dry-run"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("no odr_dm1701_rodata2.bin next to the image", err)	# warned, not silent
            self.assertIn("erase 3 sector(s): 3, 4, 5", out)

    def test_cli_merged_image_with_sector9_data(self):
        """A merged image (codec at its addresses, data in sector 9) splits back into application, codec and data."""
        stock, profiles, code, tables = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        img, data = app_image(150000), app_image(40000, seed=8)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "odr_dm1701_codec.bin")
            open(bp, "wb").write(stockfw.merge(img, prof, segs, [(0x080A0000, data)]))
            with mock.patch.dict(stockfw.PROFILES, profiles, clear=True):
                rc, out, err = run_cli(["flash", bp, "--simulate", "blank", "--dry-run"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("sector 9 data 40000 bytes", out)
            self.assertIn("erase 8 sector(s): 3, 4, 5, 7, 8, 9, 10, 11", out)

    def test_keyless_write_detected(self):
        """Blocks are XORed with the key: the fake decrypts with its own key, so the wrong key gives other bytes."""
        rng = random.Random(2)
        dev = fakedfu.FakeAnyRoad(DM1701, KEY, fakedfu.stock_flash(DM1701, rng, []))
        p = plan.make_plan(DM1701, [(DM1701.app_base, app_image(3000))], plan.KEEP)
        other = bytes(b ^ 1 for b in KEY)
        dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, other)
        self.assertNotEqual(dev.read(0x0800C000, 3000), app_image(3000))

    def test_cli_simulations(self):
        img = app_image(220780)
        m = mf.generate(DM1701, img, "v0.0.1", "e" * 40)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "odr-dm1701-v0.0.1.bin")
            open(bp, "wb").write(img)
            open(os.path.join(d, "odr-dm1701-v0.0.1.json"), "w").write(mf.dumps(m))
            rc, out, err = run_cli(["flash", bp, "--simulate", "odr-codec", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("KEEP", out)
            self.assertIn("sim      PASS", out)
            self.assertIn("codec sector 11 unchanged", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "stock", "--stock-installed", "V02.06 CSV", "--yes"])
            self.assertEqual(rc, 0, out + err)
            rc, out, err = run_cli(["flash", bp, "--simulate", "odr-nocodec", "--stock", "synthetic", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "odr-nocodec", "--yes"])
            self.assertEqual(rc, 2)
            self.assertIn("no matching codec", err)
            rc, out, err = run_cli(["flash", bp, "--simulate", "odr-codec", "--dry-run"])
            self.assertEqual(rc, 0)
            self.assertIn("dry run: nothing was erased or written", out)
            self.assertIn("erase 4 sector(s): 3, 4, 5, 6", out)
            # a manifest for another radio is refused
            m2 = dict(m, board="dm32")
            open(os.path.join(d, "odr-dm1701-v0.0.1.json"), "w").write(mf.dumps(m2))
            rc, out, err = run_cli(["flash", bp, "--simulate", "odr-codec", "--yes"])
            self.assertEqual(rc, 2)
            # an official firmware file as the image is refused
            sp = os.path.join(d, "stock.bin")
            open(sp, "wb").write(stockfw.MAGIC + b"\0" * 300)
            rc, out, err = run_cli(["flash", sp, "--board", "dm1701", "--simulate", "odr-codec", "--yes"])
            self.assertEqual(rc, 2)
            self.assertIn("official", err)

    def test_md380_merge_simulation(self):
        """A local MD-380 build up to the code window: merged from a synthetic D002.032 file, sector 6 shared."""
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "odr_md380.bin")
            open(bp, "wb").write(app_image(0x08050C44 - 0x0800C000 - 100))
            rc, out, err = run_cli(["flash", bp, "--board", "md380", "--simulate", "odr-nocodec", "--stock", "synthetic",
                                    "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("ERASE + WRITE app+codec", out)
            self.assertIn("sim      PASS", out)

    def test_merged_image_simulation(self):
        """An image with the codec merged in: no manifest, no --board, no --stock; the codec comes from the image."""
        stock, profiles, _, _ = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(stockfw.PROFILES, profiles):
            bp = os.path.join(d, "odr_dm1701_codec.bin")
            open(bp, "wb").write(stockfw.merge(app_image(40000), prof, segs))
            rc, out, err = run_cli(["flash", bp, "--simulate", "blank", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec merged in", out)
            self.assertIn("MERGE: the image carries the codec", out)
            self.assertIn("ERASE + WRITE codec", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "odr-codec", "--codec", "keep", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec mode: keep", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", bp, "--board", "md380", "--simulate", "blank", "--yes"])
            self.assertEqual(rc, 2)						# the codec says DM-1701
            rc, out, err = run_cli(["flash", bp, "--simulate", "blank", "--stock", "synthetic", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("only gives the bootloader key", out)		# the codec still comes from the image
            self.assertIn("MERGE: the image carries the codec", out)

    def test_dev_build_without_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "odr_dm1701.bin")
            open(bp, "wb").write(app_image(40000))
            rc, out, err = run_cli(["flash", bp, "--simulate", "odr-codec", "--yes"])
            self.assertEqual(rc, 2)						# no manifest, no --board
            rc, out, err = run_cli(["flash", bp, "--board", "dm1701", "--simulate", "odr-codec", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("no manifest", err)


class BootKeyFromRadio(unittest.TestCase):
    """The key read from the bootloader over READ_MEM, found only by its hash."""

    class Radio:
        def __init__(self, flash, base=0x08000000, refuse=False):
            self.flash, self.base, self.refuse, self.reads = flash, base, refuse, 0

        def request(self, cmd, payload=b""):
            if self.refuse:
                raise debuglink.LinkError("command 0x%02X: status 1" % cmd)
            assert cmd == debuglink.CMD_READ_MEM
            addr, n = struct.unpack("<IH", payload)
            assert 1 <= n <= 1000
            self.reads += 1
            return struct.pack("<I", addr) + self.flash[addr - self.base:addr - self.base + n]

        read_mem = debuglink.Link.read_mem

    def setUp(self):
        r = random.Random(5)
        self.key = bytes(r.getrandbits(8) for _ in range(stockfw.KEY_LEN))
        self.board = boards.get("dm1701")
        self.board_key = collections.namedtuple("B", "key_sha256 protected")(
            hashlib.sha256(self.key).hexdigest(), self.board.protected)
        self.boot = bytearray(r.getrandbits(8) for _ in range(0xC000))

    def test_found_anywhere_by_hash(self):
        for off in (0x2340, 0x44B9):
            boot = bytearray(self.boot)
            boot[off:off + len(self.key)] = self.key
            radio = self.Radio(bytes(boot))
            self.assertEqual(bootkey.read_key(radio, self.board_key), self.key)
            self.assertEqual(radio.reads, 50)	# 48 KB in 1000-byte pieces

    def test_nothing_without_a_match_or_read_mem(self):
        self.assertIsNone(bootkey.read_key(self.Radio(bytes(self.boot)), self.board_key))
        self.assertIsNone(bootkey.read_key(self.Radio(b"", refuse=True), self.board_key))
        self.assertIsNone(bootkey.read_key(self.Radio(bytes(self.boot)), boards.get("dm32")))


class CodecFromRadio(unittest.TestCase):
    """The codec read back over READ_MEM where an update erases it (the MD-380), checked window by window, saved and
    loaded again; and the decision that uses it."""

    def setUp(self):
        r = random.Random(11)
        self.board = boards.get("md380")
        self.flash = bytearray(r.getrandbits(8) for _ in range(0x100000))
        wins = []
        for a, n in [(0x08050C44, 0x400), (0x0808FC64, 0x200), (0x080D0FC8, 0x24)]:
            o = a - 0x08000000
            if not wins:
                self.flash[o:o + 4] = struct.pack("<I", stockfw.CODEC_SIG)
            wins.append((a, n, hashlib.sha256(self.flash[o:o + n]).hexdigest()))
        prof = {"board": "md380", "codec_profile": self.board.codec.id, "code": wins[0], "tables": wins[1],
                "extra": wins[2:]}
        self.profiles = mock.patch.dict(stockfw.PROFILES, {"f" * 64: prof}, clear=True)
        self.profiles.start()
        self.addCleanup(self.profiles.stop)
        self.tmp = tempfile.mkdtemp()

    def radio(self, flash=None, refuse=False):
        return BootKeyFromRadio.Radio(bytes(flash if flash is not None else self.flash), refuse=refuse)

    def test_read_save_load(self):
        prof, segs = codecsave.read_codec(self.radio(), self.board)
        self.assertEqual([a for a, _ in segs], [0x08050C44, 0x0808FC64, 0x080D0FC8])
        codecsave.save(self.board, segs, self.tmp)
        self.assertEqual(codecsave.load(self.board, self.tmp), (prof, segs))

    def test_a_changed_byte_is_refused(self):
        bad = bytearray(self.flash)
        bad[0x0808FC64 - 0x08000000 + 7] ^= 1
        self.assertIsNone(codecsave.read_codec(self.radio(bad), self.board))
        self.assertIsNone(codecsave.read_codec(self.radio(refuse=True), self.board))
        _, segs = codecsave.read_codec(self.radio(), self.board)
        p = codecsave.save(self.board, segs, self.tmp)
        with open(p, "r+b") as f:
            f.seek(5)
            f.write(b"\x00")
        self.assertIsNone(codecsave.load(self.board, self.tmp))
        self.assertIsNone(codecsave.load(self.board, tempfile.mkdtemp()))

    def test_decision_uses_the_copy(self):
        man = {"codec": {"profile": self.board.codec.id}}
        d = cli.decide_codec(self.board, man, "auto", False, None, None, None, have_saved=True)
        self.assertEqual((d.mode, d.source), (plan.MERGE, "saved"))
        self.assertEqual(cli.decide_codec(self.board, man, "auto", True, None, None, None).source, "stock")
        with self.assertRaises(cli.UsageError):
            cli.decide_codec(self.board, man, "auto", False, None, None, None)


class FlashLog(unittest.TestCase):
    """The start and finish messages of a real flash (simulations send nothing), with urlopen replaced."""

    def setUp(self):
        self.sent = []
        real = cli.urllib.request.urlopen

        def fake(req, timeout=None, context=None):
            self.sent.append((req.get_method(), req.full_url, json.loads(req.data)))
            return io.BytesIO(b"")
        cli.urllib.request.urlopen = fake
        self.addCleanup(setattr, cli.urllib.request, "urlopen", real)
        os.environ.pop("REPORTING", None)

    def args(self, off=False):
        return cli.build_parser().parse_args(["flash", "x.bin"] + (["--no-reporting"] if off else []))

    def test_start_and_finish(self):
        man = {"version": "v0.1.0", "variant": "full", "features": "FM,DMR"}
        started = cli.flash_started(self.args(), DM1701, man)
        cli.flash_finished(started, dfumod.DfuError("no answer from /Users/someone/odr/x.bin"))
        (m1, u1, b1), (m2, u2, b2) = self.sent
        self.assertEqual((m1, b1["task"], b1["source"], b1["radio"], b1["firmware"], b1["variant"], b1["features"]),
                         ("POST", "install", "own", "dm1701", "v0.1.0", "full", "FM,DMR"))
        self.assertEqual((m2, u2), ("PATCH", u1 + "/" + b1["id"]))
        self.assertEqual(b2["result"], "fail")
        self.assertNotIn("someone", b2["reason"])

    def test_opt_out(self):
        self.assertIsNone(cli.flash_started(self.args(off=True), DM1701, {}))
        os.environ["REPORTING"] = "0"
        self.addCleanup(os.environ.pop, "REPORTING", None)
        self.assertIsNone(cli.flash_started(self.args(), DM1701, {}))
        cli.flash_finished(None)
        self.assertEqual(self.sent, [])


if __name__ == "__main__":
    unittest.main()
