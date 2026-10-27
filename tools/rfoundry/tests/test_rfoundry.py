# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for rfoundry: sector planning, manifests, the codec merge library, key derivation, the codec decision,
and end-to-end runs against the fake bootloader. No USB, no radio, no real key or stock file.

    python3 -m unittest discover -s tools/rfoundry/tests -v        (tools/ci.sh runs this)
"""
import collections
import contextlib
import hashlib
import io
import json
import os
import random
import re
import struct
import sys
import tempfile
import unittest
import zlib
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from rfoundry import boards, bootkey, cli, codecmap, codecsave, debuglink, fakedfu, manifest as mf, plan, state, stockfw  # noqa: E402
from rfoundry import dfu as dfumod  # noqa: E402

DM1701 = boards.get("dm1701")
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


def placed():
    """The synthetic DM-1701 codec as the merge writes it: (profile, segments: code moved into sector 11, the tables
    at their stock address, the descriptor)"""
    _, profiles, code, tables = synthetic_stock()
    prof = list(profiles.values())[0]
    return prof, stockfw.place(prof, [(prof["code"][0], code), (prof["tables"][0], tables)])


class NoDefaultBoard(unittest.TestCase):
    """Nothing given names the radio: rfoundry asks on a terminal and refuses otherwise, never assumes one."""

    def test_refuses_without_a_terminal(self):
        with mock.patch("sys.stdin.isatty", return_value=False):
            with self.assertRaises(cli.UsageError) as e:
                cli.pick_board(None, "download")
        for b in boards.BOARDS:
            self.assertIn(b, str(e.exception))

    def test_asks_on_a_terminal(self):
        ids = sorted(boards.BOARDS)
        with mock.patch("sys.stdin.isatty", return_value=True), mock.patch("builtins.input", return_value="2"), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cli.pick_board(None, "download").id, boards.by()[1].id)
        self.assertEqual(cli.pick_board(ids[0], "download").id, ids[0])

    def test_one_candidate_needs_no_question(self):
        b = boards.by()[0]
        self.assertIs(cli.pick_board(None, "x", [b]), b)

    def test_the_stock_file_names_the_board(self):
        stock, profiles, _, _ = synthetic_stock()
        prof = list(profiles.values())[0]
        with mock.patch.object(stockfw, "PROFILES", profiles):
            self.assertEqual(cli.stock_board(stock, None).id, prof["board"])
            other = next(b for b in boards.BOARDS if b != prof["board"])
            with self.assertRaises(cli.UsageError):
                cli.stock_board(stock, other)


class BuildOptions(unittest.TestCase):
    """Hardware build options (board.toml [[build_options]]): the image names, the radio's board name, mismatches."""

    def setUp(self):
        self.b = next((b for b in boards.BOARDS.values() if b.build_options), None)
        if self.b is None:
            self.skipTest("no board has build options")
        self.o = self.b.build_options[0]
        self.default, self.other = self.o["values"][0], self.o["values"][1]

    def args(self, *opts):
        return type("A", (), {"option": list(opts)})()

    def test_image_id_names_only_non_default_values(self):
        self.assertEqual(self.b.image_id({}), self.b.id)
        self.assertEqual(self.b.image_id({self.o["name"]: self.default["id"]}), self.b.id)
        self.assertEqual(self.b.image_id({self.o["name"]: self.other["id"]}), self.b.id + "-" + self.other["id"])

    def test_file_names(self):
        """The Makefile's names (BUILD, TARGET) and release.py's, from rfoundry/names.py"""
        oth = {self.o["name"]: self.other["id"]}
        sfx = "-" + self.other["id"]
        self.assertEqual(self.b.build_image({}), "build/%s/rfoundry_%s.bin" % (self.b.id, self.b.id))
        self.assertEqual(self.b.build_image(oth, "amateur"), "build/%s-amateur%s/rfoundry_%s%s.bin"
                         % (self.b.id, sfx, self.b.id, sfx))
        self.assertEqual(self.b.release_id(oth, "full"), self.b.id + sfx)
        self.assertEqual(mf.image_name(self.b.release_id(oth, "amateur"), "v1"), "rfoundry-%s-amateur%s-v1.bin" % (self.b.id, sfx))
        db = next((b for b in boards.BOARDS.values() if b.first_stage), None)
        if db:
            self.assertEqual(db.build_image({}), "build/%s/rfoundry_%s_install.bin" % (db.id, db.id))

    def test_the_board_name_gives_the_value(self):
        probe = {"board": self.other["board_name"]}
        self.assertEqual(cli.resolve_options(self.args(), self.b, probe), {self.o["name"]: self.other["id"]})
        self.assertEqual(self.b.options_for_name(self.b.id), {})
        self.assertIsNone(self.b.options_for_name("no such radio"))

    def test_option_and_radio_disagree(self):
        probe = {"board": self.other["board_name"]}
        with self.assertRaises(cli.UsageError):
            cli.resolve_options(self.args("%s=%s" % (self.o["name"], self.default["id"])), self.b, probe)

    def test_unknown_value_refused(self):
        with self.assertRaises(cli.UsageError):
            cli.resolve_options(self.args("%s=nope" % self.o["name"]), self.b, None)

    def test_release_needs_every_value(self):
        with mock.patch("sys.stdin.isatty", return_value=False), self.assertRaises(cli.UsageError) as e:
            cli.resolve_options(self.args(), self.b, None, need_all=True)
        self.assertIn(self.o["name"], str(e.exception))

    def test_an_image_for_other_hardware_is_refused(self):
        want = {self.o["name"]: self.other["id"]}
        cli.check_options(self.b, {"build_options": dict(want)}, want)
        with self.assertRaises(cli.UsageError):
            cli.check_options(self.b, {}, want)		# no build_options: the default build


class StockPlacement(unittest.TestCase):
    """The registry's stock-file boards ([codec] stock): tools/boards.py leaves their codec windows to stockfw, so the
    checks it makes for a linker codec slot are made here."""

    def test_stock_boards(self):
        stock = [b for b in boards.BOARDS.values() if b.codec_stock]
        self.assertTrue(stock)
        for b in stock:
            for h in b.codec_stock:
                prof = stockfw.PROFILES[h]
                self.assertEqual(prof["board"], b.id)
                self.assertEqual(prof["codec_profile"], b.codec.id, "%s: every stock file holds the board's codec" % b.id)
                if stockfw.codec_only(prof):
                    self.assertNotEqual(prof["key_sha256"], b.key_sha256, "%s: a codec-only file" % b.id)
                else:
                    self.assertEqual(prof["key_sha256"], b.key_sha256, "%s: the DFU key is the stock file's" % b.id)
            for a, n in b.codec.windows:
                self.assertTrue(b.app_region[1] <= a and a + n <= b.flash_end,
                                "%s: codec window 0x%08X+0x%X outside the flash the linker keeps free" % (b.id, a, n))


    def test_firmware_knows_every_placement(self):
        """boards/<radio>/vocoder_<radio>.c pins each stock build where the merge puts it: its tag, the code's address,
        length and CRC, the tables' address, length and CRC, and the codec id the board reports."""
        import re
        root = os.path.join(HERE, "..", "..", "..")
        for b in [b for b in boards.BOARDS.values() if b.codec_stock]:
            voc = open(os.path.join(root, "boards", b.id, "vocoder_%s.c" % b.id)).read()
            nums = {int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)u", voc)}
            self.assertIn('"%s"' % b.codec.id, voc)
            for h in b.codec_stock:
                prof = stockfw.PROFILES[h]
                if not prof.get("placement"):
                    continue
                pl, pd = prof["placement"], prof["placed"]
                for what, v in (("tag", pl["tag"]), ("code", pl["code"]), ("code length", prof["code"][1]),
                                ("code CRC", pd["code"][0]), ("tables", pl.get("tables", prof["tables"][0])),
                                ("tables length", pl.get("tables_len", prof["tables"][1])),
                                ("tables CRC", pd["tables"][0])):
                    self.assertIn(v, nums, "%s %s: %s 0x%X is not in vocoder_%s.c" % (b.id, prof["version"], what, v,
                                                                                       b.id))


class TwoStockFiles(unittest.TestCase):
    """A board whose codec comes from either of two stock files ([codec] stock lists both): the same codec, its tables
    at different addresses. The board's windows are the union of both layouts; each merge plans against its own."""

    A = {"name": "A.bin", "board": "dm1701", "codec_profile": "dm1701-two", "key_sha256": "",
         "code": (0x080E0000, 0x1000, "0" * 64), "tables": (0x080E4000, 0x2000, "1" * 64)}
    B = {"name": "B.bin", "board": "dm1701", "codec_profile": "dm1701-two", "key_sha256": "",
         "code": (0x080E0000, 0x1000, "2" * 64), "tables": (0x080C0000, 0x2000, "3" * 64)}

    def board(self, profiles):
        e = json.load(open(os.path.join(os.path.dirname(boards.__file__), "registry.json")))["boards"]["dm1701"]
        e = dict(e, codec_stock=list(profiles))
        with mock.patch.dict(stockfw.PROFILES, profiles):
            return boards.Board(e)

    def segs(self, prof):
        return [(a, bytes([i + 1]) * n) for i, (a, n, _) in enumerate(stockfw.windows(prof))]

    def test_union_and_each_merge(self):
        b = self.board({"a" * 64: self.A, "b" * 64: self.B})
        self.assertEqual(b.codec.windows, [(0x080E0000, 0x1000), (0x080E4000, 0x2000), (0x080C0000, 0x2000)])
        self.assertEqual(b.codec.sources, ["A.bin", "B.bin"])
        self.assertEqual(b.codec_codes, [0x080E0000, 0x080E0000])
        img = app_image(5000)
        for prof in (self.A, self.B):
            segs = self.segs(prof)
            p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs, b.merge_windows(prof))
            written = b"".join(bl for st in p.writes for _, bl in st.blocks)
            for a, d in segs:
                self.assertIn(d, written)
            # against the union, one file's codec never covers the other's tables: the planner refuses
            with self.assertRaises(plan.PlanError):
                plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs)

    def test_keep_protects_both(self):
        b = self.board({"a" * 64: self.A, "b" * 64: self.B})
        p = plan.make_plan(b, [(b.app_base, app_image(5000))], plan.KEEP)
        self.assertEqual([s.index for s in p.kept], [10, 11])	# B's tables in sector 10, A's in sector 11
        only_a = self.board({"a" * 64: self.A})
        self.assertEqual([s.index for s in plan.make_plan(only_a, [(b.app_base, app_image(5000))], plan.KEEP).kept],
                         [11])

    def test_different_codecs_refused(self):
        with self.assertRaises(ValueError):
            self.board({"a" * 64: self.A, "b" * 64: dict(self.B, codec_profile="dm1701-other")})


class SectorMap(unittest.TestCase):
    def test_f405_layout(self):
        s = DM1701.sectors
        self.assertEqual([x.index for x in s], list(range(3, 12)))
        self.assertEqual(s[0].addr, 0x0800C000)
        self.assertEqual(s[-1].end, 0x08100000)
        for a, b in zip(s, s[1:]):
            self.assertEqual(a.end, b.addr)

    def test_codec_sectors(self):
        self.assertEqual([x.index for x in DM1701.codec_sectors()], [10, 11])
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "memory.ld")).read()
        self.assertNotIn("FLASH2", ld)			# one region: no second image any more

    def test_every_sector_key_aligned(self):
        for s in DM1701.sectors:
            self.assertEqual((s.addr - DM1701.app_base) % 1024, 0)

    def test_linker_keeps_app_out_of_codec_sectors(self):
        # the region starts at board.mk APP_BASE (__app_start) and ends below the codec sectors
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "memory.ld")).read()
        self.assertIn("ORIGIN = __app_start, LENGTH = 0x080C0000 - __app_start", ld)
        self.assertEqual(DM1701.app_region, (0x0800C000, 0x080C0000))
        first_codec = min(s.addr for s in DM1701.codec_sectors())
        self.assertEqual(first_codec, 0x080C0000)
        # the firmware looks for the codec where the merge puts it
        voc = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1701", "vocoder_dm1701.c")).read()
        prof = stockfw.PROFILES["0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]
        self.assertIn("#define CODEC_TAG\t\t0x%04Xu" % prof["placement"]["tag"], voc)
        self.assertIn("#define CODEC_CODE\t\t0x%08Xu" % prof["placement"]["code"], voc)
        self.assertIn("#define CODEC_CRC\t\t0x%08Xu" % prof["placed"]["code"][0], voc)
        self.assertIn("#define CODEC_TABLES_LEN\t0x%05Xu" % prof["placement"]["tables_len"], voc)
        self.assertIn("#define CODEC_TABLES_CRC\t0x%08Xu" % prof["placed"]["tables"][0], voc)
        self.assertIn('"%s"' % prof["codec_profile"], voc)
        desc = open(os.path.join(HERE, "..", "..", "..", "platform", "stm32f4", "codec_desc.h")).read()
        self.assertIn("#define CODEC_DESC_ADDR\t\t0x%08Xu" % stockfw.DESC_ADDR, desc)
        self.assertIn("#define CODEC_DESC_MAGIC\t0x%08Xu" % stockfw.DESC_MAGIC, desc)

    def test_dm1801pro_matches_its_linker_script(self):
        b = boards.get("dm1801pro")
        self.assertEqual((b.app_base, b.flash_end, b.family), (0x0300C000, 0x03100000, boards.get("dm32").family))
        self.assertEqual([(s.addr, s.size) for s in b.codec_sectors()], [(0x030E8000, 0x18000)])
        self.assertEqual(b.codec.windows, boards.get("dm32").codec.windows)
        self.assertEqual(b.protected, boards.get("dm32").protected)
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "dm1801pro", "memory.ld")).read()
        self.assertIn("ORIGIN = __app_start, LENGTH = __codec_slot - __app_start", ld)
        self.assertIn("__codec_slot = 0x030E8000;", ld)
        self.assertIs(boards.from_debuglink_name("DM-1801 Pro"), b)

    def test_open_firmware_cps_data(self):
        """Every board carries the MCU flash's base (area 5's origin in the open-firmware CPS) and its CPS radio
        types from board.toml [cps]; the base is where the protected bootloader range starts"""
        for b in boards.BOARDS.values():
            with self.subTest(board=b.id):
                self.assertEqual(b.rom_base, b.protected[0][0])
                self.assertLess(b.rom_base, b.app_base)
                self.assertTrue(all(isinstance(t, int) and 0 <= t < 64 for t in b.cps_radio_types))
                toml = open(os.path.join(HERE, "..", "..", "..", "boards", b.id, "board.toml")).read()
                self.assertEqual(bool(b.cps_radio_types), "[cps]" in toml)


class Planning(unittest.TestCase):
    def test_keep_small_image(self):
        img = app_image(5000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3])
        self.assertEqual([s.index for s in p.kept], [10, 11])
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
        self.assertEqual(skipped, [7, 8, 9])	# application sectors the image doesn't reach

    def test_keep_fills_whole_app_area(self):
        img = app_image(0x080C0000 - 0x0800C000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 8, 9])

    def test_second_range_refused(self):
        """The firmware is one image from the base: a second range (as releases from before the codec moved carried for
        sector 9) is refused anywhere, in both modes and on every board."""
        img = app_image(5000)
        for addr in (0x08060000, 0x080A0000, 0x080C0000, 0x080E0000, 0x0800F000, 0x08000000):
            for mode in (plan.KEEP, plan.MERGE):
                csegs = placed()[1] if mode == plan.MERGE else []
                with self.assertRaises(plan.PlanError, msg="%s %s" % (hex(addr), mode)):
                    plan.make_plan(DM1701, [(DM1701.app_base, img), (addr, b"d" * 100)], mode, csegs)
        with self.assertRaises(plan.PlanError):
            plan.make_plan(boards.get("md380"), [(boards.get("md380").app_base, img), (0x080A0000, b"d" * 100)], plan.MERGE,
                           [(a, b"\0" * n) for a, n in boards.get("md380").codec.windows])

    def test_image_into_codec_sector_refused(self):
        img = app_image(0x080C0000 - 0x0800C000 + 1)
        for mode in (plan.KEEP, plan.MERGE):
            with self.assertRaises(plan.PlanError):
                plan.make_plan(DM1701, [(DM1701.app_base, img)], mode)

    def test_merge_writes_codec_sectors(self):
        prof, segs = placed()
        (ca, mc), (ta, mt), (da, desc) = segs
        img = app_image(100000)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.MERGE, segs, DM1701.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 10, 11])
        self.assertEqual(p.kept, [])
        s11 = [st for st in p.writes if st.sector.index == 11][0]
        flat = b"".join(b for _, b in s11.blocks)
        flat += b"\xFF" * (0x20000 - len(flat))
        end = ta + len(mt) - 0x080E0000
        self.assertEqual(flat[:end], mt[0x080E0000 - ta:])			# the tables' tail
        at = ca - 0x080E0000
        self.assertEqual(flat[end:at], b"\xFF" * (at - end))
        self.assertEqual(flat[at:at + len(mc)], mc)				# the code, moved
        self.assertEqual(flat[0x1FF80:], desc)					# the descriptor
        self.assertEqual(flat[at + len(mc):0x1FF80], b"\xFF" * (0x1FF80 - at - len(mc)))

    def test_merge_needs_whole_codec(self):
        prof, segs = placed()
        with self.assertRaises(plan.PlanError):
            plan.make_plan(DM1701, [(DM1701.app_base, app_image(1000))], plan.MERGE, segs[:2],
                           DM1701.merge_windows(prof))

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
        self.assertIn("never erased or written: codec sector(s) 10, 11", text)


class RenamedFolder(unittest.TestCase):
    def test_old_settings_folder_is_copied_once(self):
        with tempfile.TemporaryDirectory() as home:
            env = {"HOME": home, "XDG_CONFIG_HOME": os.path.join(home, ".config"), "APPDATA": os.path.join(home, "appdata")}
            with mock.patch.dict(os.environ, env):
                old = state._config_dir("odrflash")
                os.makedirs(os.path.join(old, "codecs"))
                with open(os.path.join(old, "codecs", "x.bin"), "wb") as f:
                    f.write(b"codec")
                new = state.config_dir()
                self.assertNotEqual(new, old)
                with open(os.path.join(new, "codecs", "x.bin"), "rb") as f:
                    self.assertEqual(f.read(), b"codec")
                os.remove(os.path.join(new, "codecs", "x.bin"))
                state.config_dir()						# the new folder exists: nothing is copied again
                self.assertFalse(os.path.exists(os.path.join(new, "codecs", "x.bin")))
                self.assertTrue(os.path.exists(os.path.join(old, "codecs", "x.bin")))


class Manifests(unittest.TestCase):
    def test_generate_and_verify(self):
        img = app_image(220780)
        m = mf.generate(DM1701, img, "v1.2.3", "a" * 40, "FM,DMR", "gcc 13.3")
        self.assertEqual(m["sectors"], [3, 4, 5, 6])
        self.assertEqual(m["codec"]["sectors"], [10, 11])
        self.assertEqual(m["codec"]["profile"], "dm1701-v0206-csv-s11")
        self.assertFalse(m["codec"]["included"])
        self.assertEqual(m["ranges"], [{"addr": "0x0800C000", "size": 220780}])
        self.assertEqual(m["image"]["file"], "rfoundry-dm1701-v1.2.3.bin")
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
        old = json.loads(mf.dumps(m))
        old["format"] = "odr-release/1"			# written before the project was renamed
        self.assertIs(mf.verify(old, img), DM1701)
        bad = json.loads(mf.dumps(m))
        bad["format"] = "rfoundry-release/9"
        with self.assertRaises(mf.ManifestError):
            mf.verify(bad, img)
        bad = json.loads(mf.dumps(m))
        bad["codec"]["profile"] = "dm1701-v0206-csv"	# a release from before the codec moved
        with self.assertRaises(mf.ManifestError):
            mf.verify(bad, img)

    def test_generate_refuses_codec_overlap(self):
        with self.assertRaises(mf.ManifestError):
            mf.generate(DM1701, app_image(0x080C0000 - 0x0800C000 + 4), "v1", "c" * 40)

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
                              "--features", "FM", "--toolchain", "test"])
                release.main(["--board", "dm1701", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                              "--features", "FM", "--toolchain", "test"])	# again: SHA256SUMS not duplicated
            m = mf.load(os.path.join(out, "rfoundry-dm1701-v9.9.9.json"))
            self.assertIs(mf.verify(m, open(os.path.join(out, "rfoundry-dm1701-v9.9.9.bin"), "rb").read()), DM1701)
            sums = open(os.path.join(out, "SHA256SUMS")).read().splitlines()
            self.assertEqual(len(sums), 2)
            self.assertIn(hashlib.sha256(img).hexdigest() + "  rfoundry-dm1701-v9.9.9.bin", sums)
            # a merged image is refused: the codec where this layout puts it (its first word), or where merges before
            # the move did (the stock code, here a stand-in profile's)
            code = struct.pack("<I", stockfw.CODEC_SIG) + bytes(random.Random(5).randrange(256) for _ in range(252))
            fake = {"board": "dm1701", "code": (0x08074714, len(code), hashlib.sha256(code).hexdigest())}
            for at, data in ((0x080E3A74, code[:4]), (0x08074714, code)):
                merged = bytearray(b"\xFF" * (at - 0x0800C000 + len(data)))
                merged[:len(img)] = img
                merged[at - 0x0800C000:] = data
                open(bp, "wb").write(merged)
                with mock.patch.dict(stockfw.PROFILES, {"stand-in": fake}):
                    with self.assertRaises(SystemExit) as e:
                        release.check_no_codec(DM1701, bytes(merged))
                    self.assertIn("codec signature", str(e.exception))
                    with self.assertRaises(SystemExit):
                        release.main(["--board", "dm1701", "--version", "v9", "--bin", bp, "--out", out, "--git", "d" * 40,
                                      "--features", "", "--toolchain", ""])
            # the image's own code may hold the codec's first word at a stock address: that's a push, not the codec
            mine = bytearray(b"\xFF" * (0x08074714 - 0x0800C000 + 4))
            mine[:len(img)] = img
            struct.pack_into("<I", mine, 0x08074714 - 0x0800C000, stockfw.CODEC_SIG)
            release.check_no_codec(DM1701, bytes(mine))

    def test_release_build_option(self):
        """--build-option: a non-default value of a board's [[build_options]] gets its own names and manifest field;
        an unknown option or value is refused."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        b = next(b for b in boards.BOARDS.values() if b.build_options)
        opt = b.build_options[0]
        value = opt["values"][1]["id"]
        self.assertIsNotNone(b.build_option(opt["name"], value))
        self.assertIsNone(b.build_option(opt["name"], "no-such-value"))
        img = app_image(4096)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "in.bin")
            open(bp, "wb").write(img)
            out = os.path.join(d, "dist")
            common = ["--board", b.id, "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                      "--features", "FM", "--toolchain", "test"]
            with contextlib.redirect_stdout(io.StringIO()):
                release.main(common + ["--build-option", "%s=%s" % (opt["name"], value)])
            m = mf.load(os.path.join(out, "rfoundry-%s-%s-v9.9.9.json" % (b.id, value)))
            self.assertEqual(m["build_options"], {opt["name"]: value})
            for bad in ("%s=no-such-value" % opt["name"], "NO_SUCH_OPTION=%s" % value):
                with self.assertRaises(SystemExit):
                    release.main(common + ["--build-option", bad])
            with self.assertRaises(SystemExit):
                release.main(common[:1] + ["no-such-board"] + common[2:])

    def test_release_needs_the_first_stage(self):
        """The DB25-D's image goes to 0x0300C000, its first stage's sector: the application alone is refused, the
        install image (first stage, then the application) is released."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        stage = bytes([0x2C, 0x11, 0x01, 0xC0]) + bytes(0x1000 - 4)
        app = app_image(20000)
        with tempfile.TemporaryDirectory() as d:
            bp, out = os.path.join(d, "rfoundry_db25d.bin"), os.path.join(d, "dist")
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
            self.assertEqual(open(os.path.join(out, "rfoundry-db25d-v9.9.9.bin"), "rb").read(), stage + app)


class SingleImage(unittest.TestCase):
    """The DM-1701 image is one range, sectors 3-9 (docs/spec/codec-placement.md); a release from before the codec
    moved (two ranges, the old codec profile) is refused by rfoundry and by the old route."""

    def run_release(self, d, size=30000, name="rfoundry_dm1701"):
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        bp = os.path.join(d, name + ".bin")
        open(bp, "wb").write(app_image(size))
        out = os.path.join(d, "dist")
        with contextlib.redirect_stdout(io.StringIO()):
            release.main(["--board", "dm1701", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", "d" * 40,
                          "--features", "FM", "--toolchain", "test"])
        return out

    def old_release(self, d):
        """A release laid out as before the codec moved: application + sector 9 data, the old codec profile"""
        img, data = app_image(30000), app_image(9000, seed=4)
        m = mf.generate(DM1701, img, "v0.9.0", "e" * 40)
        m["ranges"].append({"addr": "0x080A0000", "size": len(data)})
        m["image"]["size"] = len(img) + len(data)
        m["image"]["sha256"] = hashlib.sha256(img + data).hexdigest()
        m["codec"]["profile"] = "dm1701-v0206-csv"
        bp = os.path.join(d, "rfoundry-dm1701-v0.9.0.bin")
        open(bp, "wb").write(img + data)
        open(os.path.join(d, "rfoundry-dm1701-v0.9.0.json"), "w").write(mf.dumps(m))
        return bp, m, img + data

    def test_release_is_one_range(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.run_release(d, size=0x080C0000 - 0x0800C000)		# all of sectors 3-9
            whole = open(os.path.join(out, "rfoundry-dm1701-v9.9.9.bin"), "rb").read()
            m = mf.load(os.path.join(out, "rfoundry-dm1701-v9.9.9.json"))
            self.assertEqual(m["sectors"], [3, 4, 5, 6, 7, 8, 9])
            self.assertEqual(m["ranges"], [{"addr": "0x0800C000", "size": len(whole)}])
            self.assertIs(mf.verify(m, whole), DM1701)
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit):
                self.run_release(d, size=0x080C0000 - 0x0800C000 + 4)		# into codec sector 10

    def test_old_release_refused(self):
        with tempfile.TemporaryDirectory() as d:
            _, m, whole = self.old_release(d)
            with self.assertRaises(mf.ManifestError) as e:
                mf.verify(m, whole)
            self.assertIn("2 flash ranges", str(e.exception))
            m1 = json.loads(mf.dumps(m))
            m1["ranges"] = m1["ranges"][:1]
            m1["ranges"][0]["size"] = len(whole)
            with self.assertRaises(mf.ManifestError) as e:
                mf.verify(m1, whole)
            self.assertIn("dm1701-v0206-csv", str(e.exception))

    def merge_cli(self, image, extra=()):
        """rfoundry merge with the synthetic stock file and key (the real profile and key hash are patched out)."""
        stock, profiles, _, _ = synthetic_stock()
        with tempfile.TemporaryDirectory() as d:
            sp, op = os.path.join(d, "stock.bin"), os.path.join(d, "merged.bin")
            open(sp, "wb").write(stock)
            with mock.patch.dict(stockfw.PROFILES, profiles, clear=True), \
                    mock.patch.object(dfumod, "check_key", lambda k, b: k), \
                    mock.patch.object(stockfw, "derive_key", lambda s, want=None: KEY):
                rc, out, err = run_cli(["merge", image, "--stock", sp, "-o", op] + list(extra))
            return rc, out, err, (open(op, "rb").read() if os.path.exists(op) else b"")

    def test_cli_merge_of_a_release_image(self):
        with tempfile.TemporaryDirectory() as d:
            out = self.run_release(d)
            rc, so, se, merged = self.merge_cli(os.path.join(out, "rfoundry-dm1701-v9.9.9.bin"))
        self.assertEqual(rc, 0, so + se)
        self.assertEqual(merged[:30000], app_image(30000))
        self.assertEqual(merged[30000:0x080D19F0 - 0x0800C000], b"\xFF" * (0x080D19F0 - 0x0800C000 - 30000))
        for a, data in placed()[1]:
            self.assertEqual(merged[a - 0x0800C000:][:len(data)], data)
        self.assertEqual(len(merged), 0x08100000 - 0x0800C000)
        with tempfile.TemporaryDirectory() as d:
            bp, _, _ = self.old_release(d)
            rc, so, se, _ = self.merge_cli(bp)
        self.assertNotEqual(rc, 0)


class OtherRadioStock(unittest.TestCase):
    def test_flash_refuses_another_radios_file_by_name_before_the_key(self):
        """rfoundry flash --stock with another radio's file and no --key: named as such, not "could not derive the
        bootloader key" (the key is derived before the codec is extracted)."""
        stock, profiles, _, _ = synthetic_stock()
        digest, prof = next(iter(profiles.items()))
        img = app_image(220780)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry-dm1701-v0.0.1.bin")
            open(bp, "wb").write(img)
            open(os.path.join(d, "rfoundry-dm1701-v0.0.1.json"), "w").write(
                mf.dumps(mf.generate(DM1701, img, "v0.0.1", "e" * 40)))
            sp = os.path.join(d, "other.bin")
            open(sp, "wb").write(stock)
            for codec in ("merge", "keep"):
                with mock.patch.dict(stockfw.PROFILES, {digest: dict(prof, board="md380")}), \
                     mock.patch.object(cli, "get_probe", return_value=None), \
                     mock.patch.object(state, "key_path", return_value=os.path.join(d, "no.key")):
                    rc, out, err = run_cli(["flash", bp, "--stock", sp, "--codec", codec, "--yes"])
                self.assertEqual(rc, 2, out + err)
                self.assertIn("is a firmware file for the %s, not the %s" % (boards.get("md380").name, DM1701.name),
                              err, codec)
                self.assertNotIn("derive", err, codec)


class NoCodecFile(unittest.TestCase):
    """A file known to hold no codec (stockfw.NO_CODEC_FILES) is named and refused, as the flasher does
    (site/tests/flasher.test.mjs). The real file is never in the repository: a stand-in registered under its hash."""
    REAL = "a09ab4d364d199853204ebdf068ba0886d7e7bb99689f08437aae52104474a7e"

    def stand_in(self):
        data = app_image(96 * 1024, seed=1702)
        entry = {stockfw.sha256(data): stockfw.NO_CODEC_FILES[self.REAL]}
        return data, mock.patch.dict(stockfw.NO_CODEC_FILES, entry)

    def test_entry(self):
        self.assertEqual(stockfw.NO_CODEC_FILES[self.REAL]["board"], "dm1701")
        for h in stockfw.NO_CODEC_FILES:		# never also a source or a stock file
            self.assertNotIn(h, stockfw.PROFILES)
            self.assertNotIn(h, stockfw.KNOWN_FILES)

    def test_refused_by_name(self):
        data, patch = self.stand_in()
        self.assertIsNone(stockfw.no_codec(data))
        with patch:
            msg = stockfw.no_codec(data)
            self.assertIn(stockfw.NO_CODEC_FILES[self.REAL]["name"], msg)
            with self.assertRaises(stockfw.StockError) as e:
                stockfw.find_profile(data)
            self.assertEqual(str(e.exception), msg)
            with self.assertRaises(cli.UsageError) as e:
                cli.stock_codec(data, None, "dm1701")
            self.assertEqual(str(e.exception), msg)

    def test_flash_with_it_as_the_stock_file(self):
        data, patch = self.stand_in()
        img = app_image(220780)
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry-dm1701-v0.0.1.bin")
            open(bp, "wb").write(img)
            open(os.path.join(d, "rfoundry-dm1701-v0.0.1.json"), "w").write(
                mf.dumps(mf.generate(DM1701, img, "v0.0.1", "e" * 40)))
            sp = os.path.join(d, "open.bin")
            open(sp, "wb").write(data)
            with patch, mock.patch.object(cli, "get_probe", return_value=None), \
                    mock.patch.object(state, "key_path", return_value=os.path.join(d, "no.key")):
                rc, out, err = run_cli(["flash", bp, "--stock", sp, "--codec", "merge", "--yes"])
            self.assertEqual(rc, 2, out + err)
            self.assertIn(stockfw.NO_CODEC_FILES[self.REAL]["name"], err)

    @unittest.skipUnless(os.environ.get("RFOUNDRY_NOCODEC_FILE", "").endswith(".bin"), "set RFOUNDRY_NOCODEC_FILE (the .bin)")
    def test_real_file(self):
        """The file holds neither the codec's code nor its tables (docs/spec/codec-dm1701.md "Files without the
        codec"): where it expects the code, a code window's length is blank, and it ends before the stock tables"""
        data = open(os.environ["RFOUNDRY_NOCODEC_FILE"], "rb").read()
        self.assertEqual(stockfw.sha256(data), self.REAL)
        self.assertIsNotNone(stockfw.no_codec(data))
        self.assertIsNone(stockfw.from_flash(data, base=stockfw.APP_BASE))
        code = 0x0807537C - stockfw.APP_BASE
        self.assertEqual(data[code:code + 0x1021C], b"\xFF" * 0x1021C)
        self.assertLess(stockfw.APP_BASE + len(data), 0x080D19F0)


class WaitForDfu(unittest.TestCase):
    def test_no_bootloader_raises_the_hint_not_a_format_error(self):
        with mock.patch.object(dfumod, "find", return_value=[]):
            with self.assertRaises(cli.UsageError) as cm:
                cli.wait_for_dfu(None, boards.get("md380"), 0)
        self.assertIn("no radio bootloader found (USB 0483:DF11)", str(cm.exception))


class OldRouteGuard(unittest.TestCase):
    def test_stm32_dfu_refuses_a_release_image_with_two_ranges(self):
        """A release from before the codec moved holds the application and its sector 9 data back to back; written as
        one image the data would land over the application. The manifest next to it says so."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import stm32_dfu
        with tempfile.TemporaryDirectory() as d:
            bp, _, _ = SingleImage().old_release(d)
            err = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                rc = stm32_dfu.main([bp, "--model", "dm1701", "--dry-run"])
            self.assertEqual(rc, 2)
            self.assertIn("2 flash ranges", err.getvalue())
            os.makedirs(os.path.join(d, "x"))
            out1 = SingleImage().run_release(os.path.join(d, "x"))
            err = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                rc = stm32_dfu.main([os.path.join(out1, "rfoundry-dm1701-v9.9.9.bin"), "--model", "dm1701", "--dry-run"])
            self.assertEqual(rc, 0, err.getvalue())					# one range: as before


class StockLibrary(unittest.TestCase):
    def test_extract_and_merge(self):
        stock, profiles, code, tables = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual(segs, placed()[1])
        self.assertEqual([(a, len(d)) for a, d in segs], [(0x080E3A74, len(code)), (0x080D19F0, 0x12082),
                                                         (stockfw.DESC_ADDR, 0x80)])
        img = app_image(5000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(len(merged), 0x08100000 - 0x0800C000)
        self.assertEqual(merged[:len(img)], img)
        for a, d in segs:
            self.assertEqual(merged[a - 0x0800C000:a - 0x0800C000 + len(d)], d)
        self.assertEqual(merged[0x08074714 - 0x0800C000:0x080D19F0 - 0x0800C000].count(0xFF),
                         0x080D19F0 - 0x08074714)				# nothing at the stock code address

    def test_place_rewrites_and_checks(self):
        """The move rewrites the self reference and the island's call, and nothing else; a wrong map or profile stops"""
        _, profiles, code, tables = synthetic_stock()
        prof = list(profiles.values())[0]
        (ca, mc), (ta, mt), _ = placed()[1]
        delta = ca - prof["code"][0]
        word = lambda b, o: struct.unpack_from("<I", b, o)[0]
        self.assertEqual(word(mc, stockfw.SYN_SELF_REF), word(code, stockfw.SYN_SELF_REF) + delta)
        self.assertEqual(codecmap.bl_target(mt, stockfw.SYN_BL, ta + stockfw.SYN_BL),
                         codecmap.bl_target(tables, stockfw.SYN_BL, ta + stockfw.SYN_BL) + delta)
        diff = [i for i in range(len(mc)) if mc[i] != code[i]]
        self.assertTrue(diff and all(stockfw.SYN_SELF_REF <= i < stockfw.SYN_SELF_REF + 4 for i in diff))
        diff = [i for i in range(len(mt)) if mt[i] != tables[i]]
        self.assertTrue(diff and all(stockfw.SYN_BL <= i < stockfw.SYN_BL + 4 for i in diff))
        segs = [(prof["code"][0], code), (prof["tables"][0], tables)]
        for bad in (dict(prof, map=None, code=prof["code"][:2] + ("0" * 64,)),			# no map for this code
                    dict(prof, placed=dict(prof["placed"], code=(0, "0" * 64))),		# placed hash wrong
                    dict(prof, placement=dict(prof["placement"], code=prof["placement"]["code"] + 2))):	# alignment
            with self.assertRaises(stockfw.StockError):
                stockfw.place(bad, segs)

    def test_refusals(self):
        stock, profiles, _, _ = synthetic_stock()
        with self.assertRaises(stockfw.StockError):
            stockfw.extract(stock, bytes([KEY[0] ^ 1]) + KEY[1:], profiles)
        with self.assertRaises(stockfw.StockError):
            stockfw.extract(stock, KEY)				# not the real file
        # another radio's file is named as such before any decryption, never blamed on the key
        other = next(b for b in ("dm1701", "md380", "mduv380") if b != list(profiles.values())[0]["board"])
        with self.assertRaisesRegex(stockfw.StockError, "firmware file for the .*, not the " + other):
            stockfw.extract(stock, bytes(len(KEY)), profiles, board=other)
        with self.assertRaises(stockfw.StockError):
            stockfw.parse_container(b"x" * 300)
        with self.assertRaises(stockfw.StockError):
            stockfw.merge(stock, list(profiles.values())[0], [(0x08074714, b"")])

    def test_split_merged(self):
        stock, profiles, code, tables = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        img = app_image(5000)
        app, got, csegs = stockfw.split_merged(stockfw.merge(img, prof, segs), profiles)
        self.assertEqual((app, got["codec_profile"], csegs), (img, prof["codec_profile"], segs))
        self.assertIsNone(stockfw.split_merged(img, profiles))
        self.assertIsNone(stockfw.split_merged(app_image(0x80000), profiles))	# past the codec start, no codec there
        bad = bytearray(stockfw.merge(img, prof, segs))
        bad[0x080E3A74 - 0x0800C000 + 100] ^= 1
        with self.assertRaises(stockfw.StockError):
            stockfw.split_merged(bytes(bad), profiles)				# a codec, but not this profile's
        extra = bytearray(stockfw.merge(img, prof, segs))
        extra[0x080F8000 - 0x0800C000] = 0
        with self.assertRaises(stockfw.StockError):
            stockfw.split_merged(bytes(extra), profiles)			# data between the windows

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
        self.assertTrue(b.releasable)
        self.assertTrue(b.keep_codec_supported)	# the application has sectors 3-7, the codec 8-11
        self.assertEqual(b.codec.id, "md380-d2032-s11")
        # D002.032 uses sectors 8, 10 and 11, a 35.14 build 9, 10 and 11: an update keeps them all
        self.assertEqual([s.index for s in b.codec_sectors()], [8, 9, 10, 11])
        ld = open(os.path.join(HERE, "..", "..", "..", "boards", "md380", "memory.ld")).read()
        self.assertIn("ORIGIN = __app_start, LENGTH = 0x08080000 - __app_start", ld)
        self.assertEqual(b.app_region, (0x0800C000, 0x08080000))
        self.assertEqual(b.codec.windows[0][0], 0x080E0000)
        self.assertIn((stockfw.DESC_ADDR, stockfw.DESC_LEN), b.codec.windows)
        # one entry per stock file: D002.032, D35.14, S35.14 first, then every other build's code address
        self.assertEqual(b.codec_codes[:3], [0x080E0000, 0x080C45D0, 0x080C5710])
        self.assertEqual(b.codec_codes, [stockfw.placed_windows(stockfw.PROFILES[h])[0][0] for h in b.codec_stock])
        self.assertIn(0x0809A8DC, b.codec_codes)	# RT3 S14.04
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
        """A synthetic file with the D002.032 layout: the code moved to sector 11, the five table windows at their stock
        addresses, the descriptor; the merge plan writes sectors 3-7 and the codec sectors, a keep plan only 3-7."""
        b = boards.get("md380")
        stock, profiles, code, tables = stockfw.build_synthetic_stock(KEY, random.Random(380),
                                                                      stockfw.profile_for_board("md380"))
        self.assertEqual(stockfw.parse_container(stock), [(0x0800C000, 0xF2C00)])
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([a for a, _ in segs], [0x080E0000, 0x0808FC64, 0x080D0FC8, 0x080F9698, 0x080F9E74, 0x080FD490,
                                                stockfw.DESC_ADDR])
        self.assertEqual(len(segs[0][1]), len(code))
        self.assertNotEqual(segs[0][1], code)		# the self reference moved with the code
        img = app_image(0x08080000 - 0x0800C000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(merged[:len(img)], img)
        for a, d in segs:
            self.assertEqual(merged[a - 0x0800C000:a - 0x0800C000 + len(d)], d)
        with self.assertRaises(stockfw.StockError):
            stockfw.merge(img + b"\0" * (0x0808FC64 - 0x08080000 + 1), prof, segs)
        p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs, b.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 8, 10, 11])
        p = plan.make_plan(b, [(b.app_base, img)], plan.KEEP)
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7])

    def test_md380_3514_merge(self):
        """An MD-380 D35.14 file: the UV380 family's codec with three table windows. The main one stays in sector 9,
        the code goes right after it, the small ones stay in sector 11; the same application fits."""
        b = boards.get("md380")
        d3514 = "ff6c3d79b205391c9c2096f8e4367c514c1e1e4dac7d421ed69077e5b42eaf2f"
        self.assertEqual(stockfw.PROFILES[d3514]["codec_profile"], b.codec.id)
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(35), d3514)
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([a for a, _ in segs], [0x080C45D0, 0x080B7868, 0x080ECC98, 0x080F0C14, stockfw.DESC_ADDR])
        self.assertEqual(struct.unpack_from("<I", segs[-1][1], 24)[0], 3)	# table windows in the descriptor
        img = app_image(0x08080000 - 0x0800C000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(stockfw.split_merged(merged, profiles)[1], prof)
        p = plan.make_plan(b, [(b.app_base, img)], plan.MERGE, segs, b.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase], [3, 4, 5, 6, 7, 9, 10, 11])
        with self.assertRaises(plan.PlanError):		# the application can't reach the codec sectors
            plan.make_plan(b, [(b.app_base, app_image(0x08080000 - 0x0800C000 + 4))], plan.KEEP)

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
        # the board's windows: V02.06's layout, then V02.01 to V02.03's (one layout for the three), 0x40 lower, then
        # V02.06 noCSV's and V02.03 noCSV's, then the MD-9600 files' (code and tables moved)
        md = [w for h in DM1701.codec_stock if stockfw.codec_only(stockfw.PROFILES[h])
              for w in DM1701.merge_windows(stockfw.PROFILES[h])[:2]]
        self.assertEqual(len(md), 42)
        self.assertEqual(DM1701.codec.windows, [(0x080E3A74, 0x1021C), (0x080D19F0, 0x12082), (0x080FFF80, 0x80),
                                                (0x080E3A34, 0x1021C), (0x080D19B0, 0x12082),
                                                (0x080E6828, 0x10200), (0x080D4794, 0x12092),
                                                (0x080E67E8, 0x10200), (0x080D4754, 0x12092)] + md)
        self.assertEqual(DM1701.merge_windows(p), [(0x080E3A74, 0x1021C), (0x080D19F0, 0x12082), (0x080FFF80, 0x80)])
        # the real map covers the real code, and the placement keeps the move whole words
        m = stockfw.code_map(p)
        self.assertEqual(m["profile"], "dm1701-v0206-csv")
        self.assertEqual((p["placement"]["code"] - p["code"][0]) % 4, 0)
        self.assertEqual(p["placement"]["code"], (p["tables"][0] + p["placement"]["tables_len"] + 3) & ~3)
        w = m["table_windows"][0]
        self.assertEqual((w["min_start"], w["min_len"]), (p["tables"][0], p["placement"]["tables_len"]))
        self.assertEqual(codecmap.branches_in(m, p["tables"][0], p["placement"]["tables_len"]), len(m["branches"]))


def flash_readout(stock, key, profiles, placed_image=None):
    """A synthetic stub readout of the internal flash (1 MB from 0x08000000): a radio on its stock firmware (the
    application segment decrypted at its address), or with placed_image (a merged image) at 0x0800C000."""
    out = bytearray(stockfw.random_bytes(random.Random(4), 0xC000)) + b"\xFF" * (0x100000 - 0xC000)
    if placed_image is not None:
        out[0xC000:0xC000 + len(placed_image)] = placed_image
        return bytes(out)
    off = stockfw.HEADER_LEN
    for a, n in stockfw.parse_container(stock):
        if a >= stockfw.FLASH_BASE:
            out[a - stockfw.FLASH_BASE:a - stockfw.FLASH_BASE + n] = stockfw.decrypt(stock, off, n, key)
        off += n
    return bytes(out)


class FirmwareProfiles(unittest.TestCase):
    """Every placed stock profile is one the radio's firmware accepts: boards/<radio>/vocoder_<radio>.c lists its tag,
    code window and table windows with the CRC-32s the merge's descriptor carries, and nothing else."""

    WIN = re.compile(r"\{ (\w+), (\w+), (\w+) \}")

    def firmware(self, board):
        src = open(os.path.join(HERE, "..", "..", "..", "boards", board, "vocoder_%s.c" % board)).read()
        defs = dict(re.findall(r"#define (\w+)\s+(\w+)u\b", src))
        num = lambda v: int(defs.get(v, v).rstrip("u"), 0)	# a number, or a #define of one
        tables = {m.group(1): [tuple(num(x) for x in w) for w in self.WIN.findall(m.group(2))]
                  for m in re.finditer(r"codec_desc_window_t (\w+)\[\w*\] = \{(.*?)\};", src, re.S)}
        structs = {}
        for m in re.finditer(r"stm32_codec_profile_t (\w+) = \{(.*?)\n\};", src, re.S):
            f = dict(re.findall(r"\.(\w+) = (\w+)", m.group(2)))
            wins = tables[f["tables"]]
            self.assertEqual(len(wins), num(f["windows"]))
            structs[m.group(1)] = (num(f["tag"]), num(f["code"]), num(f["code_len"]), num(f["code_crc"]), tuple(wins))
        # only what the profiles[] array lists is accepted: a profile defined but not listed is not
        m = re.search(r"stm32_codec_profile_t \*const profiles\[\] = \{(.*?)\};", src, re.S)
        listed = re.findall(r"&(\w+)", m.group(1))
        self.assertEqual(len(listed), len(set(listed)), board)
        count = re.search(r"stm32_codec_profiles_t stm32_codec_profiles = \{ profiles, (\d+) \}", src)
        if count:
            self.assertEqual(int(count.group(1)), len(listed), board)
        return {structs[n] for n in listed}

    def test_every_profile_in_the_firmware(self):
        for board in ("mduv380", "md380", "dm1701"):
            want = set()
            for prof in stockfw.PROFILES.values():
                if prof["board"] != board:
                    continue
                d = stockfw.descriptor(prof)
                w = struct.unpack_from("<%dI" % (7 + 3 * stockfw.DESC_WINDOWS), d)
                want.add((w[2], w[3], w[4], w[5], tuple(tuple(w[7 + 3 * i:10 + 3 * i]) for i in range(w[6]))))
            self.assertEqual(self.firmware(board), want, board)


class Dm1701Builds(unittest.TestCase):
    """The DM-1701's stock files: V02.06 CSV, and V02.01, V02.02 and V02.03 CSV, which hold one codec 0x40 below
    V02.06's (codec-dm1701.md "Firmware files"): one map and one placement for the three. V02.06 and V02.03 noCSV hold
    it higher up, its code 0x1C bytes shorter: a map and a placement each."""

    V0206 = "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"
    V0201 = "cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713"
    V0202 = "643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7"
    V0203 = "35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948"
    V0206N = "4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9"
    V0203N = "0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a"

    def test_board(self):
        own = [self.V0206, self.V0201, self.V0202, self.V0203, self.V0206N, self.V0203N]
        self.assertEqual(DM1701.codec_stock[:6], own)
        self.assertEqual(DM1701.codec_stock[6:], list(Md9600Files.FILES))
        self.assertEqual(DM1701.codec.id, "dm1701-v0206-csv-s11")
        self.assertEqual([s.index for s in DM1701.codec_sectors()], [10, 11])
        for h in own:
            p = stockfw.PROFILES[h]
            self.assertRegex(stockfw.KNOWN_FILES[h], r"^DM-1701 V02\.0[1236] (no)?CSV$")
            self.assertEqual(stockfw.KNOWN_FILES[h], "DM-1701 " + p["version"])
            self.assertIn(p["name"], DM1701.codec.sources)
        # every known DM-1701 file is a codec source
        self.assertEqual({h for h, v in stockfw.KNOWN_FILES.items() if v.startswith("DM-1701 ")}, set(own))
        # one tag per layout, and every layout in sectors 10 and 11, the code below the descriptor
        tags = {}
        for h in DM1701.codec_stock:
            p = stockfw.PROFILES[h]
            tags.setdefault(p["placement"]["tag"], set()).add(tuple(DM1701.merge_windows(p)))
            for a, n in DM1701.merge_windows(p):
                self.assertTrue(0x080C0000 <= a and a + n <= 0x08100000, p["name"])
            self.assertLessEqual(p["placement"]["code"] + p["code"][1], stockfw.DESC_ADDR)
        self.assertEqual(sorted(tags), [0x0203, 0x0206, 0x10203, 0x10206] + sorted(Md9600Files.FILES.values()))
        self.assertTrue(all(len(v) == 1 for v in tags.values()), tags)

    def test_readout_named_by_layout(self):
        """A readout can't tell files with the same codec layout apart, so it is named after all of them"""
        for h in (self.V0201, self.V0202, self.V0203):
            self.assertEqual(stockfw.layout_name(stockfw.PROFILES[h]), "V02.01/V02.02/V02.03 CSV")
        for h, v in ((self.V0206, "V02.06 CSV"), (self.V0206N, "V02.06 noCSV"), (self.V0203N, "V02.03 noCSV")):
            self.assertEqual(stockfw.layout_name(stockfw.PROFILES[h]), v)
            self.assertEqual(stockfw.same_layout(stockfw.PROFILES[h]), [stockfw.PROFILES[h]])

    def test_one_map_for_three_files(self):
        """The three older files: one code window (by its hash), the dm1701-v0203-csv map, one placement and tag."""
        maps = codecmap.load_maps()
        profs = [stockfw.PROFILES[h] for h in (self.V0201, self.V0202, self.V0203)]
        for p in profs:
            self.assertEqual({k: p[k] for k in ("code", "tables", "placement", "placed", "segments", "size")},
                             {k: profs[0][k] for k in ("code", "tables", "placement", "placed", "segments", "size")})
            m = codecmap.by_code_sha(maps, p["code"][2])
            self.assertEqual([x["profile"] for x in m], ["dm1701-v0203-csv"], p["name"])
            m = m[0]
            self.assertEqual(m["stock"]["sha256"], self.V0203)
            self.assertEqual((m["code"]["flash"], m["code"]["len"]), p["code"][:2])
            tw = m["table_windows"][0]
            self.assertEqual((tw["flash"], tw["len"], tw["sha256"]), p["tables"])
            self.assertEqual((tw["min_start"], tw["min_len"]), (p["tables"][0], p["placement"]["tables_len"]))
            # the tables cut to the codec's data, the code right after them, word aligned, below the descriptor
            self.assertEqual(p["placement"]["code"], (p["tables"][0] + p["placement"]["tables_len"] + 3) & ~3)
            self.assertLessEqual(p["placement"]["code"] + p["code"][1], stockfw.DESC_ADDR)
            self.assertEqual(codecmap.branches_in(m, p["tables"][0], p["placement"]["tables_len"]), len(m["branches"]))
            self.assertEqual(p["placement"]["tag"], 0x0203)
        self.assertNotEqual(profs[0]["placement"]["tag"], stockfw.PROFILES[self.V0206]["placement"]["tag"])

    def test_map_is_v0206_relinked(self):
        """The V02.03 map is the V02.06 one with every address 0x40 lower: the same entries, the same sites in the code
        and in the islands, every target and value shifted, the same object lengths except where a bound depends on
        the stock code around it. A map that disagrees with the V02.06 one anywhere else is wrong. The same holds for
        the two noCSV maps."""
        maps = codecmap.load_maps()
        self.relinked(maps["dm1701-v0203-csv"], maps["dm1701-v0206-csv"], -0x40)
        self.relinked(maps["dm1701-v0203-nocsv"], maps["dm1701-v0206-nocsv"], -0x40)

    def relinked(self, a, b, d):
        self.assertEqual(a["entries"], b["entries"])
        self.assertEqual((a["code"]["flash"] - b["code"]["flash"], a["code"]["len"], a["code"]["reachable_len"]),
                         (d, b["code"]["len"], b["code"]["reachable_len"]))
        for k in ("self_refs", "island_ptrs", "table_refs", "island_refs", "branches"):
            self.assertEqual(len(a[k]), len(b[k]), k)
            for x, y in zip(a[k], b[k]):
                self.assertEqual(set(x), set(y), k)
                for f in x:
                    if f in ("kind", "off", "off2", "reachable"):
                        self.assertEqual(x[f], y[f], (k, f))
                    else:
                        self.assertEqual(x[f] - y[f], d, (k, f, x))
        self.assertEqual([(i["start"] - d, i["len"]) for i in a["islands"]],
                         [(i["start"], i["len"]) for i in b["islands"]])
        self.assertEqual([o["start"] - d for o in a["objects"]], [o["start"] for o in b["objects"]])
        self.assertEqual([[v - d for v in g] for g in a["groups"]], b["groups"])
        ta, tb = a["table_windows"][0], b["table_windows"][0]
        self.assertEqual((ta["min_start"] - d, ta["min_len"], ta["len"]), (tb["min_start"], tb["min_len"], tb["len"]))
        self.assertEqual(a["outside_refs"], [])

    def test_nocsv_map_matches_csv(self):
        """The noCSV maps against the CSV map of the same version, site for site: the same entries and counts; every
        site in the code at the same offset, 0x1C lower past the 0x1C bytes the noCSV code lacks (stock offset 0x4A00);
        every reference into the tables to the same object at the same offset in it, every island call to the same code
        offset. A map that finds another site or target anywhere is wrong."""
        maps = codecmap.load_maps()
        for n, c in (("dm1701-v0206-nocsv", "dm1701-v0206-csv"), ("dm1701-v0203-nocsv", "dm1701-v0203-csv")):
            a, b = maps[n], maps[c]
            self.assertEqual(a["entries"], b["entries"])
            self.assertEqual((a["code"]["len"], a["code"]["reachable_len"]),
                             (b["code"]["len"] - 0x1C, b["code"]["reachable_len"] - 0x1C))
            self.assertEqual([o["len"] for o in a["objects"][:43]], [o["len"] for o in b["objects"][:43]])

            def norm(m, nocsv):
                code, n_ = m["code"]["flash"], m["code"]["len"]
                def off(o):		# a code offset as the CSV code has it
                    self.assertFalse(nocsv and 0x4A00 <= o < 0x4A1C, o)
                    return o + 0x1C if nocsv and o >= 0x4A00 else o
                def at(v):		# where an address points: code offset, or (object or island, index, offset in it)
                    if code <= v < code + n_:
                        return ("code", off(v - code))
                    for kind in ("objects", "islands"):
                        for i, o in enumerate(m[kind]):
                            if o["start"] <= v < o["start"] + max(o["len"], 1):
                                return (kind, i, v - o["start"])
                    self.fail("0x%08X is in no window of %s" % (v, m["profile"]))
                out = {}
                for k in ("self_refs", "island_ptrs", "table_refs", "island_refs", "branches"):
                    out[k] = [tuple(sorted((f, (off(x[f]) if f in ("off", "off2") else
                                                at(x[f] & ~1 if f == "value" or k == "island_ptrs" else x[f])
                                                if f not in ("kind", "reachable") else x[f])) for f in x))
                              for x in m[k]]
                return out
            na, nb = norm(a, True), norm(b, False)
            for k in na:
                self.assertEqual(na[k], nb[k], (n, k))
            self.assertEqual(a["outside_refs"], [])
            # groups by object index: the same, but for objects 6 and 7, which the CSV builds keep together because the
            # second starts where the first ends; in noCSV 0x7C bytes the codec doesn't use lie between them
            # (docs/spec/codec-dm1701.md, "Codec in the V02.06 CSV file")
            def groups(m):
                index = {o["start"]: i for i, o in enumerate(m["objects"])}
                return sorted(tuple(index[s] for s in g) for g in m["groups"])
            self.assertEqual(groups(b), sorted(groups(a) + [(6, 7)]), n)
            for m, gap in ((a, 0x7C), (b, 0)):
                o6, o7 = m["objects"][6], m["objects"][7]
                self.assertEqual((o6["len"], o7["len"]), (96, 3072), m["profile"])
                self.assertEqual(o7["start"] - (o6["start"] + o6["len"]), gap, m["profile"])

    def test_synthetic_merge(self):
        """A V02.01 file merges, splits and reads back from a readout like V02.06; its descriptor carries tag 0x0203."""
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(201), self.V0201)
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([(a, len(d)) for a, d in segs], [(0x080E3A34, 0x1021C), (0x080D19B0, 0x12082),
                                                         (stockfw.DESC_ADDR, stockfw.DESC_LEN)])
        self.assertEqual(struct.unpack_from("<4I", segs[-1][1], 8), (0x0203, 0x080E3A34, 0x1021C,
                                                                      prof["placed"]["code"][0]))
        img = app_image(0x080C0000 - 0x0800C000)
        merged = stockfw.merge(img, prof, segs)
        self.assertEqual(stockfw.split_merged(merged, profiles)[1], prof)
        self.assertEqual(stockfw.from_flash(flash_readout(stock, KEY, profiles), profiles=profiles), (prof, segs))
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], plan.MERGE, segs, DM1701.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase if s.index >= 10], [10, 11])

    @unittest.skipUnless(os.path.isdir(os.environ.get("RFOUNDRY_DM1701_STOCK_DIR", "")),
                         "set RFOUNDRY_DM1701_STOCK_DIR (a folder with the DM-1701 stock files)")
    def test_real_files(self):
        """Every supported DM-1701 stock file in the folder: the key derived from it, every window decrypted, the codec
        placed by its map and checked against the profile's hashes (extract() raises on any mismatch)."""
        found = set()
        for root, _, files in os.walk(os.environ["RFOUNDRY_DM1701_STOCK_DIR"]):
            for n in files:
                data = open(os.path.join(root, n), "rb").read()
                h = stockfw.sha256(data)
                if h not in DM1701.codec_stock:
                    continue
                key = stockfw.derive_key(data)
                prof, segs = stockfw.extract(data, key, board="dm1701")
                self.assertEqual([a for a, _ in segs], [a for a, _ in DM1701.merge_windows(prof)])
                found.add(h)
        self.assertEqual(found, set(DM1701.codec_stock))


class Md9600Files(unittest.TestCase):
    """The TYT MD-9600 files as codec sources for the DM-1701 (codec-dm1701.md "MD-9600 files"): an open-firmware
    install leaves their codec on the radio. They give the codec only, encrypted with the MD-9600's key; the merge
    moves the codec's table window with the code into sector 10, since its stock address is the application's."""

    FILES = {	# file SHA-256: descriptor tag
        "de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532": 0x96001645,	# MD-9600 V16.45 CSV
        "7f4d760288f0c1875c7c5dc83dcdf3f355f5188becde2b505eedf609fdbe665f": 0x96011645,	# MD-9600 P16.45 CSV-GPS
        "d2ef7a7e9c484dc1a8777488816f4da47181899ff8b7299a0ead7d141d55e10c": 0x96021645,	# MD-9600 D16.45 REC
        "3d71b4358267fe5fdae8980f340b6a158e7b229c9d32c8ab894c4985964e8aff": 0x96031645,	# MD-9600 S16.45 GPS-REC
        "79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09": 0x96000609,	# MD-9600 V06.09 CSV
        "9454eb221ae1f206fca5a15ec1b540bf06b3db4991f466b1626a3a3073ca91d5": 0x96010609,	# MD-9600 P06.09 CSV-GPS
        "7b1b1b33e661766a5b0c0b3ddab9c31c96e3855b09f4e280b07f4990b563b68f": 0x96020609,	# MD-9600 D06.09 REC
        "4610ed115e80efa04be554a94915ec2e053ae00f6b64dfa4815451966840dba7": 0x96030609,	# MD-9600 S06.09 GPS-REC
        "194526d60f796cc0f7339d14c1efeedc267731d108fd66a96876b459408de0ba": 0x96000624,	# MD-9600 V06.24 CSV
        "1eafdb774fd71cb7bc3334d06617ed42264ee25df1c73f61744f6b21742bbd20": 0x96010624,	# MD-9600 P06.24 CSV-GPS
        "ec44929e855442ad248f1a5c171a467922d6cb5742fc35f759db269c645e22ab": 0x96030624,	# MD-9600 S06.24 GPS-REC
        "6643261e62b9778a5df43b1e5c3428d6d308bf54b34478513df3e08f6f4dbb63": 0x96020624,	# MD-9600 D06.24 REC
        "98302fbc26fa0db93bd7ecbd29a410ac9fbb42cbd7d6ce15d54ce085fefff329": 0x96001624,	# MD-9600 V16.24 CSV
        "e86b9e9775758548dd2d01334cfacc96c640fbf843aa6c5fa009b4bc7598e98d": 0x96011624,	# MD-9600 P16.24 CSV-GPS
        "4e1c5c081d5bdf4bef8f205668fd7381391187d8b07150df7e99275b030b811b": 0x96031624,	# MD-9600 S16.24 GPS-REC
        "e21c72e30716fb43e4d15798012aec15e964b9ec8c1a534ede5d0b0fe73f3acb": 0x96021624,	# MD-9600 D16.24 REC
        "02e366687f122ed752581344db8316666d21782d9353334518b69237a81e74dc": 0x96012645,	# MD-9600 P26.45 CSV-GPS
        "6df34152e9786530d61032ab71f1962cf194aca0b99cf0a54e3802fd2cd4ac72": 0x96032645,	# MD-9600 S26.45 GPS-REC
        "baee36b0fd852dfc1fb14952242a1231dbfa4fabc37cba57c61f1b8d3e0d830d": 0x96022645,	# MD-9600 D26.45 REC
        "2522008737d7845e43ab47a307d38498ee783c788a377fac855dff94dea53af0": 0x96022657,	# MD-9600 D26.57 REC
        "d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f": 0x96002645,	# MD-9600 V26.45 CSV
    }

    def test_profiles(self):
        maps = codecmap.load_maps()
        for h, tag in self.FILES.items():
            p = stockfw.PROFILES[h]
            with self.subTest(p["name"]):
                self.assertEqual((p["board"], p["file_radio"], p["codec_profile"]), ("dm1701", "MD-9600", DM1701.codec.id))
                self.assertEqual(p["key_sha256"], stockfw.MD9600_KEY_SHA256)
                self.assertNotEqual(p["key_sha256"], DM1701.key_sha256)
                self.assertIn("not the bootloader key", stockfw.codec_only(p))
                self.assertEqual(stockfw.KNOWN_FILES[h], p["version"])
                self.assertTrue(p["version"].startswith("MD-9600 "))
                m = codecmap.by_code_sha(maps, p["code"][2])
                self.assertEqual(len(m), 1)
                m = m[0]
                self.assertEqual((m["board"], m["stock"]["sha256"]), ("md9600", h))
                self.assertEqual((m["code"]["flash"], m["code"]["len"]), p["code"][:2])
                # the profile's table window is the map's cut, which holds everything the codec refers to
                tw = m["table_windows"][0]
                self.assertEqual((tw["min_start"], tw["min_len"], tw["min_sha256"]), p["tables"])
                self.assertEqual(codecmap.table_window(m), p["tables"][:2])
                # placed in sectors 10 and 11, the tables from 0x080D0000 at their stock page offset, the code in the
                # page after them at its own, below the descriptor
                pl = p["placement"]
                self.assertEqual(pl["tag"], tag)
                self.assertNotIn("tables_len", pl)
                self.assertEqual(pl["tables"], 0x080D0000 + (p["tables"][0] & 0xFFF))
                self.assertEqual(pl["code"] & 0xFFF, p["code"][0] & 0xFFF)
                self.assertLessEqual(pl["tables"] + p["tables"][1], pl["code"] - 0x1000 + 0x1000)
                self.assertLess(pl["code"] - (pl["tables"] + p["tables"][1]), 0x2000)
                self.assertLessEqual(pl["code"] + p["code"][1], stockfw.DESC_ADDR)
                for a, n in DM1701.merge_windows(p):
                    self.assertTrue(0x080C0000 <= a and a + n <= 0x08100000)
                self.assertEqual(stockfw.placed_windows(p)[1][:2], (pl["tables"], p["tables"][1]))

    def synthetic(self):
        """A synthetic codec-only profile and its file: the shared Thumb vector's code and table window (codecmap_vectors
        "window") in a container encrypted with a key other than the radio's"""
        with open(os.path.join(HERE, "codecmap_vectors.json")) as f:
            w = json.load(f)["window"]
        m = codecmap.normalize(w["map"])
        code, tables = bytes.fromhex(w["code"]), bytes.fromhex(w["tables"])
        lo, n = codecmap.table_window(m)
        other = bytes(random.Random(9600).randrange(256) for _ in range(1024))
        app = bytearray(0x80000)
        app[m["code"]["run"] - 0x08060000:m["code"]["run"] - 0x08060000 + len(code)] = code
        app[lo - 0x08060000:lo - 0x08060000 + n] = tables
        hdr = bytearray(stockfw.HEADER_LEN)
        hdr[:len(stockfw.MAGIC)] = stockfw.MAGIC
        struct.pack_into("<III", hdr, 0x7C, 1, 0x08060000, len(app))
        stock = bytes(hdr) + bytes(b ^ other[i % 1024] for i, b in enumerate(app)) + bytes(0x100)
        CR, TR = 0x080E2C00, 0x080D0230
        mc, mt = codecmap.move_window(code, tables, m, CR, TR)
        prof = {"name": "synthetic-md9600.bin", "version": "MD-9600 synthetic", "board": "dm1701", "file_radio": "MD-9600",
                "key_sha256": hashlib.sha256(other).hexdigest(), "codec_profile": DM1701.codec.id, "size": len(stock),
                "segments": [(0x08060000, len(app))], "code": (m["code"]["run"], len(code), m["code"]["sha256"]),
                "tables": (lo, n, hashlib.sha256(tables).hexdigest()), "map": m,
                "placement": {"code": CR, "tables": TR, "tag": 0x96FF0000},
                "placed": {"code": (zlib.crc32(mc), hashlib.sha256(mc).hexdigest()),
                           "tables": (zlib.crc32(mt), hashlib.sha256(mt).hexdigest())}}
        return prof, stock, code, tables, mc, mt

    def test_extract_with_the_files_own_key(self):
        prof, stock, _, _, mc, mt = self.synthetic()
        profiles = {hashlib.sha256(stock).hexdigest(): prof}
        got, segs = stockfw.extract(stock, KEY, profiles, board="dm1701")	# the radio's key is not the file's
        self.assertIs(got, prof)
        self.assertEqual(segs[:2], [(0x080E2C00, mc), (0x080D0230, mt)])
        self.assertEqual(segs[2], (stockfw.DESC_ADDR, stockfw.descriptor(prof)))
        d = struct.unpack_from("<10I", segs[2][1])
        self.assertEqual(d[2:10], (0x96FF0000, 0x080E2C00, len(mc), zlib.crc32(mc), 1, 0x080D0230, len(mt), zlib.crc32(mt)))

    def test_found_wherever_another_firmware_put_it(self):
        """stockfw.from_moved: the codec in a flash read, its code anywhere, its tables at their stock address or
        moved, the islands' calls into the code moved or not; placed as a merge from the file places it"""
        prof, stock, code, tables, mc, mt = self.synthetic()
        profiles = {hashlib.sha256(stock).hexdigest(): prof}
        want = stockfw.place(prof, [(prof["code"][0], code), (prof["tables"][0], tables)])
        m, lo = prof["map"], prof["tables"][0]
        for cr, tr, verbatim in ((0x0807537C, lo, False), (0x0807537C, lo, True), (0x0807537C, lo - 0x91C, False),
                                 (prof["code"][0], lo, False), (0x08060004, 0x08098010, False)):
            with self.subTest(code=hex(cr), tables=hex(tr), verbatim=verbatim):
                c2, t2 = codecmap.move_window(code, tables, m, cr, tr)
                flash = bytearray(b"\xff" * 0x100000)
                flash[cr - 0x08000000:cr - 0x08000000 + len(c2)] = c2
                flash[tr - 0x08000000:tr - 0x08000000 + len(t2)] = tables if verbatim else t2
                got = stockfw.from_moved(bytes(flash), profiles=profiles)
                self.assertIsNotNone(got)
                self.assertEqual((got[0], got[1], got[2]), (prof, want, cr))
                flash[cr - 0x08000000 + 0x123] ^= 0x10
                self.assertIsNone(stockfw.from_moved(bytes(flash), profiles=profiles), "a changed byte")
        self.assertIsNone(stockfw.from_moved(b"\xff" * 0x100000, profiles=profiles))

    def test_no_key_from_it(self):
        """The CLI never derives the DM-1701 key from an MD-9600 file, and says why"""
        prof = stockfw.PROFILES[next(iter(self.FILES))]
        self.assertIn("MD-9600", stockfw.codec_only(prof))
        hint = cli.stock_hint(DM1701)
        self.assertIn("for the codec only, one of 21 MD-9600 firmware files", hint)
        self.assertNotIn(prof["name"], hint)
        self.assertNotIn("MD-9600", cli.stock_hint(DM1701, key=True), "the key's hint names only files that give it")


class MdUv380(unittest.TestCase):
    """The MD-UV380 family: TYT and Retevis files from 18.11 to 219.74, one codec, each build's tables at its own
    address."""

    B = boards.get("mduv380")
    V1926 = "75e4d470b451cb5d191a647c0836c71801433b826cc4a7fe2c48d750036bd98e"
    V21960 = "447ed4e09482c45661cd6e4c83d1e9f777463993ab7dd48b0e0597bb14f519ee"	# RT3S, the code before the tables

    def test_board(self):
        b = self.B
        self.assertEqual(b.codec.id, "mduv380-1926-s11")
        self.assertEqual(len(b.codec_stock), 30)
        self.assertEqual(b.codec_stock[0], self.V1926)		# the first: what the simulations use
        self.assertEqual([s.index for s in b.codec_sectors()], [10, 11])
        self.assertEqual(b.app_region, (0x0800C000, 0x080C0000))
        self.assertEqual(b.key_prefix, bytes.fromhex("00AA8989"))
        self.assertEqual(b.key_sha256, stockfw.MDUV380_KEY_SHA256)
        self.assertEqual(b.image_id({"POWER": "10w"}), "mduv380-10w")
        self.assertEqual(b.image_id({}), "mduv380")
        self.assertEqual(b.options_for_name("MD-UV380 10W"), {"POWER": "10w"})
        self.assertIs(boards.from_debuglink_name("MD-UV380"), b)
        self.assertTrue(any("PTT" in s for s in b.bootloader_steps))
        self.assertIn("PTT", cli.bootloader_steps(b))
        hint = cli.stock_hint(b)
        for h in b.codec_stock:
            self.assertIn(stockfw.PROFILES[h]["name"], hint)
            self.assertRegex(stockfw.KNOWN_FILES[h], r"^(MD-UV380|RT3S) ")

    def test_placements(self):
        """Each file's tables at their stock address cut to the codec's data, the code right after them, word aligned,
        or at the start of sector 10 when it doesn't fit there; everything inside sectors 10 and 11 below the
        descriptor; each its own tag; its map found by the code's hash."""
        maps = codecmap.load_maps()
        tags = set()
        for h in self.B.codec_stock:
            prof = stockfw.PROFILES[h]
            pl = prof["placement"]
            tab = prof["tables"][0]
            after = (tab + pl["tables_len"] + 3) & ~3
            if after + prof["code"][1] <= stockfw.DESC_ADDR:
                self.assertEqual(pl["code"], after, prof["name"])
            else:
                self.assertEqual(pl["code"], 0x080C0000, prof["name"])
                self.assertLessEqual(pl["code"] + prof["code"][1], tab, prof["name"])
            self.assertLessEqual(pl["code"] + prof["code"][1], stockfw.DESC_ADDR, prof["name"])
            self.assertGreaterEqual(tab, 0x080C0000)
            tags.add(pl["tag"])
            m = codecmap.by_code_sha(maps, prof["code"][2])
            self.assertEqual(len(m), 1, prof["name"])
            self.assertEqual(m[0]["stock"]["sha256"], h)
            self.assertEqual((m[0]["code"]["flash"], m[0]["code"]["len"]), prof["code"][:2])
            tw = m[0]["table_windows"][0]
            self.assertEqual((tw["flash"], tw["len"], tw["sha256"]), prof["tables"])
            self.assertEqual(tw["min_len"], pl["tables_len"])
            wins = stockfw.layout(prof)
            self.assertEqual(wins[-1][:2], (stockfw.DESC_ADDR, stockfw.DESC_LEN))
        self.assertEqual(len(tags), 30)

    def synthetic(self):
        return stockfw.build_synthetic_stock(KEY, random.Random(19), self.V1926)

    def test_synthetic_merge(self):
        stock, profiles, code, _ = self.synthetic()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([a for a, _ in segs], [0x080EB300, 0x080D8C94, stockfw.DESC_ADDR])
        self.assertEqual(len(segs[1][1]), 0x1266A)
        merged = stockfw.merge(app_image(0x080C0000 - 0x0800C000), prof, segs)
        self.assertEqual(stockfw.split_merged(merged, profiles)[1], prof)

    def test_code_before_the_tables(self):
        """A build whose code doesn't fit after its tables (RT3S 219.60): the code at the start of sector 10, below
        the tables; merged, split and read back from a readout like any other."""
        stock, profiles, _, _ = stockfw.build_synthetic_stock(KEY, random.Random(20), self.V21960)
        prof, segs = stockfw.extract(stock, KEY, profiles)
        self.assertEqual([a for a, _ in segs], [0x080C0000, 0x080E322C, stockfw.DESC_ADDR])
        self.assertLess(0x080C0000 + len(segs[0][1]), 0x080E322C)
        desc = segs[-1][1]
        self.assertEqual(struct.unpack_from("<4I", desc, 8), (0x80561960, 0x080C0000, len(segs[0][1]),
                                                              prof["placed"]["code"][0]))
        merged = stockfw.merge(app_image(0x080C0000 - 0x0800C000), prof, segs)
        self.assertEqual(stockfw.split_merged(merged, profiles)[1], prof)
        self.assertEqual(stockfw.from_flash(flash_readout(stock, KEY, profiles), profiles=profiles), (prof, segs))
        b = self.B
        p = plan.make_plan(b, [(b.app_base, app_image(5000))], plan.MERGE, segs, b.merge_windows(prof))
        self.assertEqual([s.index for s in p.erase if s.index >= 10], [10, 11])
        with self.assertRaises(stockfw.StockError):	# an image reaching into sector 10 meets the code
            stockfw.merge(app_image(0x080C0000 - 0x0800C000 + 4), prof, segs)

    def test_codec_from_a_flash_readout(self):
        """A stub readout of a radio still on its stock firmware gives the codec as the stock file's merge does; so does
        one of a radio running RadioFoundry with the codec placed. Anything else gives nothing."""
        stock, profiles, _, _ = self.synthetic()
        want = stockfw.extract(stock, KEY, profiles)
        flash = flash_readout(stock, KEY, profiles)
        self.assertEqual(stockfw.from_flash(flash, profiles=profiles), want)
        merged = stockfw.merge(app_image(5000), *want)
        self.assertEqual(stockfw.from_flash(flash_readout(None, None, None, merged), profiles=profiles), want)
        bad = bytearray(flash)
        bad[0x0807B830 - stockfw.FLASH_BASE + 0x400] ^= 1
        self.assertIsNone(stockfw.from_flash(bytes(bad), profiles=profiles))
        self.assertIsNone(stockfw.from_flash(flash[:0x80000], profiles=profiles))
        self.assertIsNone(stockfw.from_flash(flash))			# not the real build
        # the DM-1701 the same way
        dstock, dprofiles, _, _ = synthetic_stock()
        self.assertEqual(stockfw.from_flash(flash_readout(dstock, KEY, dprofiles), profiles=dprofiles),
                         stockfw.extract(dstock, KEY, dprofiles))

    def test_cli_merge_and_flash_from_a_readout(self):
        stock, profiles, _, _ = self.synthetic()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        img = app_image(60000)
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(stockfw.PROFILES, profiles):
            fp, ip, op = (os.path.join(d, n) for n in ("flash.bin", "rfoundry_mduv380.bin", "out.bin"))
            open(fp, "wb").write(flash_readout(stock, KEY, profiles))
            open(ip, "wb").write(img)
            rc, out, err = run_cli(["merge", ip, "--stock", fp, "-o", op])
            self.assertEqual(rc, 0, out + err)
            self.assertEqual(open(op, "rb").read(), stockfw.merge(img, prof, segs))
            rc, out, err = run_cli(["key", "--stock", fp])
            self.assertEqual(rc, 2)
            self.assertIn("never from a flash readout", err)
            rc, out, err = run_cli(["flash", ip, "--board", "mduv380", "--simulate", "rfoundry-nocodec", "--stock", fp,
                                    "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("a flash readout of a radio with V19.26 CSV", out)
            self.assertIn("ERASE + WRITE codec", out)
            self.assertIn("sim      PASS", out)
            # the 10 W release image, its build option in the manifest, onto a radio in its bootloader
            m = dict(mf.generate(self.B, img, "v0.0.1", "e" * 40), build_options={"POWER": "10w"})
            rp = os.path.join(d, "rfoundry-mduv380-10w-v0.0.1.bin")
            open(rp, "wb").write(img)
            open(rp[:-4] + ".json", "w").write(mf.dumps(m))
            rc, out, err = run_cli(["flash", rp, "--simulate", "blank", "--option", "POWER=10w", "--stock", fp, "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("ERASE + WRITE codec", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", rp, "--simulate", "blank", "--option", "POWER=5w", "--stock", fp, "--yes"])
            self.assertEqual(rc, 2, out + err)
            open(fp, "wb").write(b"\xFF" * 0x100000)
            rc, out, err = run_cli(["merge", ip, "--stock", fp, "-o", op, "--board", "mduv380"])
            self.assertEqual(rc, 2)
            self.assertIn("flash readout", err)

    def test_simulated_flash_from_a_stock_file(self):
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry_mduv380.bin")
            open(bp, "wb").write(app_image(0x080C0000 - 0x0800C000 - 100))
            rc, out, err = run_cli(["flash", bp, "--board", "mduv380", "--simulate", "rfoundry-nocodec", "--stock",
                                    "synthetic", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("sim      PASS", out)


class CodecDecision(unittest.TestCase):
    man = {"codec": {"profile": "dm1701-v0206-csv-s11"}}
    NEW, OLD = "dm1701-v0206-csv-s11", "dm1701-v0206-csv"	# the codec where this layout keeps it, where older RadioFoundry did

    def d(self, **kw):
        args = dict(requested="auto", have_stock=False, probe=None, stock_installed=None, ask=None, runs_stock=False)
        args.update(kw)
        return cli.decide_codec(DM1701, self.man, args["requested"], args["have_stock"], args["probe"],
                                args["stock_installed"], args["ask"], runs_stock=args["runs_stock"])

    def probe(self, codec, board="DM-1701"):
        return {"board": board, "codec": codec, "git": "x"}

    def test_probe_with_codec_keeps_even_with_stock(self):
        self.assertEqual(self.d(probe=self.probe(self.NEW)).mode, plan.KEEP)
        self.assertEqual(self.d(probe=self.probe(self.NEW), have_stock=True).mode, plan.KEEP)

    def test_probe_without_codec(self):
        self.assertEqual(self.d(probe=self.probe("absent"), have_stock=True).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe("absent"))
        d = self.d(probe=self.probe("absent"), requested="keep")
        self.assertEqual(d.mode, plan.KEEP)
        self.assertFalse(d.expect_codec)
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe(None))				# older firmware: unknown, no stock

    def test_codec_in_the_old_place_is_written_again(self):
        """RadioFoundry from before the codec moved runs its codec from sectors 7 and 8, which this application overwrites"""
        self.assertEqual(self.d(probe=self.probe(self.OLD), have_stock=True).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe(self.OLD))

    def test_probe_other_board_refused(self):
        with self.assertRaises(cli.UsageError):
            self.d(probe=self.probe(self.NEW, board="DM-32UV"))

    def test_stock_installed(self):
        """No stock firmware holds the codec where RadioFoundry keeps it: the stock file is always needed"""
        self.assertEqual(DM1701.codec.installed, [])
        for v in ("V02.06 CSV", "v02.06-csv", "V02.03 CSV"):
            with self.assertRaises(cli.UsageError):
                self.d(stock_installed=v)
            self.assertEqual(self.d(stock_installed=v, have_stock=True).mode, plan.MERGE)

    def test_keep_on_stock_expects_no_codec(self):
        """Keeping the codec of a radio on stock firmware keeps none here: the result must not be reported as a failure"""
        for kw in (dict(runs_stock=True), dict(stock_installed="V02.06 CSV"), dict(runs_stock=True, have_stock=True)):
            d = self.d(requested="keep", **kw)
            self.assertEqual(d.mode, plan.KEEP, kw)
            self.assertFalse(d.expect_codec, kw)
            self.assertIn("DMR voice", d.risk)
        self.assertTrue(self.d(requested="keep").expect_codec)		# not known to run stock: unverified
        self.assertEqual(self.d(runs_stock=True, have_stock=True).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(runs_stock=True)

    def test_unknown(self):
        with self.assertRaises(cli.UsageError):
            self.d()
        self.assertEqual(self.d(have_stock=True).mode, plan.MERGE)
        self.assertEqual(self.d(requested="keep").mode, plan.KEEP)
        asked = []
        with self.assertRaises(cli.UsageError):
            self.d(ask=lambda q: asked.append(q) or True)		# nothing a user could say makes it safe
        self.assertEqual(asked, [])

    def test_merge_requested(self):
        self.assertEqual(self.d(requested="merge", have_stock=True, probe=self.probe(self.NEW)).mode,
                         plan.MERGE)
        with self.assertRaises(cli.UsageError):
            self.d(requested="merge")

    def test_dm32_always_merges(self):
        man = {"codec": {"profile": "dm32-049"}}
        self.assertEqual(cli.decide_codec(boards.get("dm32"), man, "auto", True, None, None, None).mode, plan.MERGE)
        with self.assertRaises(cli.UsageError):
            cli.decide_codec(boards.get("dm32"), man, "auto", False, None, None, None)

    def test_dm1801pro_always_merges(self):
        man = {"codec": {"profile": "dm1801pro-049"}}
        self.assertEqual(cli.decide_codec(boards.get("dm1801pro"), man, "auto", True, None, None, None).mode, plan.MERGE)


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
        segs, wins = [], None
        if mode == plan.MERGE:
            prof, segs = placed()
            wins = DM1701.merge_windows(prof)
        p = plan.make_plan(DM1701, [(DM1701.app_base, img)], mode, segs, wins)
        dfumod.Dfu(dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, KEY)
        return dev, before, img, p, segs

    def test_keep_leaves_codec_and_bootloader(self):
        dev, before, img, p, _ = self.flash_with(plan.KEEP)
        self.assertEqual(dev.vendor, [0x01, 0x31])
        self.assertEqual(dev.erased, [0x0800C000, 0x08010000, 0x08020000])
        self.assertTrue(dev.exited)
        self.assertEqual(dev.violations, [])
        self.assertEqual(dev.read(0x0800C000, len(img)), img)
        for s in DM1701.codec_sectors():				# 10 and 11
            self.assertEqual(dev.read(s.addr, s.size), before[s.addr - 0x08000000:s.end - 0x08000000], s)
        self.assertEqual(dev.read(0x08000000, 0xC000), before[:0xC000])

    def test_merge_writes_codec(self):
        dev, before, img, p, segs = self.flash_with(plan.MERGE, initial_codec=False)
        self.assertEqual(dev.erased, [0x0800C000, 0x08010000, 0x08020000, 0x080C0000, 0x080E0000])
        self.assertEqual(dev.violations, [])
        for a, data in segs:
            self.assertEqual(dev.read(a, len(data)), data)
        self.assertEqual(dev.read(0x080C0000, 0x080D19F0 - 0x080C0000), b"\xFF" * (0x080D19F0 - 0x080C0000))
        self.assertEqual(dev.read(0x08000000, 0xC000), before[:0xC000])

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
            bp = os.path.join(d, "rfoundry-dm1701-v0.0.1.bin")
            open(bp, "wb").write(img)
            open(os.path.join(d, "rfoundry-dm1701-v0.0.1.json"), "w").write(mf.dumps(m))
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("KEEP", out)
            self.assertIn("sim      PASS", out)
            self.assertIn("codec sector 11 unchanged", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "stock", "--stock-installed", "V02.06 CSV", "--yes"])
            self.assertEqual(rc, 2, out + err)			# stock keeps the codec elsewhere: --stock is needed
            self.assertIn("--stock FILE", err)
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-nocodec", "--stock", "synthetic", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-nocodec", "--yes"])
            self.assertEqual(rc, 2)
            self.assertIn("no matching codec", err)
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--dry-run"])
            self.assertEqual(rc, 0)
            self.assertIn("dry run: nothing was erased or written", out)
            self.assertIn("erase 4 sector(s): 3, 4, 5, 6", out)
            # a manifest for another radio is refused
            m2 = dict(m, board="dm32")
            open(os.path.join(d, "rfoundry-dm1701-v0.0.1.json"), "w").write(mf.dumps(m2))
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 2)
            # an official firmware file as the image is refused
            sp = os.path.join(d, "stock.bin")
            open(sp, "wb").write(stockfw.MAGIC + b"\0" * 300)
            rc, out, err = run_cli(["flash", sp, "--board", "dm1701", "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 2)
            self.assertIn("official", err)

    def test_md380_merge_simulation(self):
        """A local MD-380 build filling sectors 3-7: merged from a synthetic D002.032 file."""
        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "rfoundry_md380.bin")
            open(bp, "wb").write(app_image(0x08080000 - 0x0800C000 - 100))
            rc, out, err = run_cli(["flash", bp, "--board", "md380", "--simulate", "rfoundry-nocodec", "--stock", "synthetic",
                                    "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("MERGE", out)
            self.assertIn("sector 11  0x080E0000..0x080FFFFF   128 KB  ERASE + WRITE codec", out)
            # a 35.14 build's tables would be in sector 9: a D002.032 merge leaves it alone
            self.assertIn("sector  9  0x080A0000..0x080BFFFF   128 KB  not touched\n", out)
            self.assertIn("sim      PASS", out)

    def test_merged_image_simulation(self):
        """An image with the codec merged in: no manifest, no --board, no --stock; the codec comes from the image."""
        stock, profiles, _, _ = synthetic_stock()
        prof, segs = stockfw.extract(stock, KEY, profiles)
        with tempfile.TemporaryDirectory() as d, mock.patch.dict(stockfw.PROFILES, profiles):
            bp = os.path.join(d, "rfoundry_dm1701_codec.bin")
            open(bp, "wb").write(stockfw.merge(app_image(40000), prof, segs))
            rc, out, err = run_cli(["flash", bp, "--simulate", "blank", "--yes"])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("codec merged in", out)
            self.assertIn("MERGE: the image carries the codec", out)
            self.assertIn("ERASE + WRITE codec", out)
            self.assertIn("sim      PASS", out)
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--codec", "keep", "--yes"])
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
            bp = os.path.join(d, "rfoundry_dm1701.bin")
            open(bp, "wb").write(app_image(40000))
            rc, out, err = run_cli(["flash", bp, "--simulate", "rfoundry-codec", "--yes"])
            self.assertEqual(rc, 2)						# no manifest, no --board
            rc, out, err = run_cli(["flash", bp, "--board", "dm1701", "--simulate", "rfoundry-codec", "--yes"])
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
    """The codec read back over READ_MEM (the MD-380's stock windows), checked window by window, saved and loaded again;
    and the decision that uses it."""

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


class CodecCopyEverywhere(unittest.TestCase):
    """On a radio that keeps its codec (the DM-1701), a copy is read once, from where the merge placed it or where stock
    keeps it, and written instead of asking for the stock file whenever the codec can't be kept or checked."""

    def setUp(self):
        self.stock, profiles, code, tables = stockfw.build_synthetic_stock(KEY, random.Random(7))
        self.profiles = mock.patch.dict(stockfw.PROFILES, profiles, clear=True)
        self.profiles.start()
        self.addCleanup(self.profiles.stop)
        self.board = boards.get("dm1701")
        self.prof = next(iter(profiles.values()))
        self.placed = stockfw.extract(self.stock, KEY)[1]
        self.stock_windows = (code, tables)

    def flash(self, segs):
        f = bytearray(b"\xff" * 0x100000)
        for a, d in segs:
            f[a - 0x08000000:a - 0x08000000 + len(d)] = d
        return BootKeyFromRadio.Radio(bytes(f))

    def test_read_from_either_layout_and_written_as_the_merge_writes_it(self):
        prof, parts = codecsave.read_codec(self.flash(self.placed), self.board)
        self.assertEqual([(a, len(d)) for a, d in parts], [(a, n) for a, n, _ in stockfw.placed_windows(prof)])
        self.assertEqual(codecsave.segments(prof, parts), self.placed)
        old = list(zip([w[0] for w in stockfw.windows(self.prof)], self.stock_windows))
        prof, parts = codecsave.read_codec(self.flash(old), self.board)
        self.assertEqual(parts[0][0], self.prof["code"][0])
        self.assertEqual(codecsave.segments(prof, parts), self.placed)
        tmp = tempfile.mkdtemp()
        codecsave.save(self.board, parts, tmp)
        self.assertEqual(codecsave.load(self.board, tmp), (prof, parts))
        bad = [(a, bytes([d[0] ^ 1]) + d[1:]) for a, d in self.placed]
        self.assertIsNone(codecsave.read_codec(self.flash(bad), self.board))

    def test_decision(self):
        man = {"codec": {"profile": self.board.codec.id}}
        rfoundry = lambda c: {"board": "DM-1701", "codec": c}	# noqa: E731
        dc = lambda *a, **k: cli.decide_codec(self.board, man, *a, **k)	# noqa: E731
        self.assertEqual(dc("auto", False, rfoundry(self.board.codec.id), None, None, have_saved=True).mode, plan.KEEP)
        self.assertEqual(dc("auto", False, None, None, None, have_saved=True).source, "saved")
        self.assertEqual(dc("auto", False, None, "V02.06 CSV", None, have_saved=True).source, "saved")
        self.assertEqual(dc("auto", False, rfoundry("absent"), None, None, have_saved=True).source, "saved")
        self.assertEqual(dc("keep", False, None, None, None, have_saved=True).mode, plan.KEEP)
        with self.assertRaises(cli.UsageError):
            dc("auto", False, rfoundry("absent"), None, None)

    def test_read_once_on_a_board_that_keeps_it(self):
        args = collections.namedtuple("A", "no_probe port")(False, None)
        got = (self.prof, [])
        probe = {"board": "DM-1701", "codec": self.board.codec.id}
        with mock.patch.object(codecsave, "load", return_value=None), \
                mock.patch.object(codecsave, "from_radio", return_value=got) as rd, \
                mock.patch.object(codecsave, "save", return_value="x"):
            self.assertEqual(cli.get_saved_codec(args, self.board, probe), got)
            self.assertEqual(rd.call_count, 1)
        with mock.patch.object(codecsave, "load", return_value=got), \
                mock.patch.object(codecsave, "from_radio") as rd:
            self.assertEqual(cli.get_saved_codec(args, self.board, probe), got)
            self.assertEqual(rd.call_count, 0)	# a copy already: not read again
            self.assertEqual(cli.get_saved_codec(args, self.board, None), got)


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
        cli.flash_finished(started, dfumod.DfuError("no answer from /Users/someone/rfoundry/x.bin"))
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
