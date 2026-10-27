# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Protected flash regions (rfoundry/protect.py): the shared check, the carve-out, and that every board's registry
entry carries its board.toml [[flash.protect]] list.

    python3 -m unittest discover -s tools/rfoundry/tests -v        (tools/ci.sh runs this)
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from rfoundry import boards, protect  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
CAL = {"mem": "progmem", "start": 0x1FE000, "end": 0x1FF000, "what": "factory calibration (hardware-x.md 6.1)"}
NAND = {"mem": "nand", "start": 0x280000, "end": 0x2E0000, "what": "calibration copy (hardware-x.md 6.5)"}


class Check(unittest.TestCase):
    def test_outside_passes(self):
        protect.check([CAL, NAND], "progmem", 0xC000, 0x1FE000)
        protect.check([CAL, NAND], "progmem", 0x1FF000, 0x200000)
        protect.check([CAL, NAND], "data", 0x1FE000, 0x1FF000)	# another memory

    def test_overlap_names_the_region_and_reason(self):
        with self.assertRaises(protect.ProtectedError) as e:
            protect.check([CAL], "progmem", 0x1FD000, 0x1FE001)
        self.assertIn("0x1FE000-0x1FF000", str(e.exception))
        self.assertIn("factory calibration (hardware-x.md 6.1)", str(e.exception))
        self.assertEqual(e.exception.regions, [CAL])

    def test_absolute_addresses(self):
        protect.check_absolute([CAL], 0x03000000, 0x0300C000, 0x031FE000)
        with self.assertRaises(protect.ProtectedError):
            protect.check_absolute([CAL], 0x03000000, 0x031FE000, 0x031FE100)

    def test_board_or_entry_or_list(self):
        class B:
            protect = [CAL]
        for p in (B(), {"protect": [CAL]}, [CAL]):
            with self.assertRaises(protect.ProtectedError):
                protect.check(p, "progmem", 0x1FE000, 0x1FF000)
        protect.check(None, "progmem", 0, 0x200000)

    def test_whole_chip_erase(self):
        protect.check_whole([CAL], "data")
        with self.assertRaises(protect.ProtectedError):
            protect.check_whole([CAL, NAND], "nand")

    def test_unknown_memory(self):
        with self.assertRaises(ValueError):
            protect.check([CAL], "eeprom", 0, 1)


class Carve(unittest.TestCase):
    def test_pieces_around_regions(self):
        two = [CAL, dict(CAL, start=0x100000, end=0x102000)]
        self.assertEqual(protect.carve(two, "progmem", 0xC000, 0x200000),
                         [(0xC000, 0x100000), (0x102000, 0x1FE000), (0x1FF000, 0x200000)])
        self.assertEqual(protect.carve(two, "progmem", 0x1FE000, 0x1FF000), [])
        self.assertEqual(protect.carve(two, "data", 0, 0x1000), [(0, 0x1000)])

    def test_units(self):
        units = protect.carved_units([CAL], "progmem", 0x1FC000, 0x200000, 0x1000)
        self.assertEqual(units, [0x1FC000, 0x1FD000, 0x1FF000])


class RegistryCarriesTheList(unittest.TestCase):
    """Every board's registry entry has its board.toml [[flash.protect]] list, and the regions are inside memories the
    tools know."""

    def test_every_board(self):
        try:
            import tomllib
        except ImportError:
            self.skipTest("board.toml needs Python 3.11 (tomllib); tools/boards.py check covers it there")
        for bid, b in boards.BOARDS.items():
            with open(os.path.join(ROOT, "boards", bid, "board.toml"), "rb") as f:
                want = tomllib.load(f)["flash"].get("protect", [])
            key = lambda p: (p["mem"], p["start"])
            self.assertEqual(sorted(b.protect, key=key), sorted(want, key=key), bid)
            for p in b.protect:
                self.assertIn(p["mem"], protect.MEMS, bid)

    def test_known_entries(self):
        db = boards.get("db25d")
        self.assertTrue(protect.overlapping(db, "progmem", 0x1FE000, 0x1FF000))
        self.assertTrue(protect.overlapping(db, "nand", 0x2A0000, 0x2A0800))
        self.assertTrue(protect.overlapping(boards.get("dm32"), "data", 0x7C000, 0x7D000))
        self.assertTrue(protect.overlapping(boards.get("dm1701"), "secreg", 0, 0x100))


class Writers(unittest.TestCase):
    """The rfoundry DFU route: a plan that would erase a protected sector is refused, and Dfu.run checks again before
    it sends anything."""

    def setUp(self):
        import copy
        import random
        import struct
        from rfoundry import fakedfu, stockfw
        self.b = copy.copy(boards.get("dm1701"))
        sec = self.b.sector_at(0x08020000)		# sector 5
        self.b.protect = [{"mem": "progmem", "start": sec.addr - self.b.rom_base, "end": sec.end - self.b.rom_base,
                           "what": "test (x.md 1)"}]
        rng = random.Random(3)
        self.img = struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + stockfw.random_bytes(rng, 150000)
        self.key = fakedfu.synthetic_key()
        self.dev = fakedfu.FakeAnyRoad(self.b, self.key, fakedfu.stock_flash(self.b, rng, []))

    def test_plan_refused(self):
        from rfoundry import plan
        with self.assertRaises(plan.PlanError) as e:
            plan.make_plan(self.b, [(self.b.app_base, self.img)], plan.KEEP)
        self.assertIn("protected region", str(e.exception))
        p = plan.make_plan(self.b, [(self.b.app_base, self.img[:5000])], plan.KEEP)	# sector 3 only: fine
        self.assertEqual([s.index for s in p.erase], [3])

    def test_dfu_refuses_before_anything_is_sent(self):
        from rfoundry import dfu, plan
        p = plan.make_plan(boards.get("dm1701"), [(self.b.app_base, self.img)], plan.KEEP)
        p.board = self.b
        with self.assertRaises(protect.ProtectedError):
            dfu.Dfu(self.dev, 0, log=lambda *a: None, sleep=lambda s: None).run(p, self.key)
        self.assertEqual((self.dev.erased, self.dev.vendor), ([], []))


if __name__ == "__main__":
    unittest.main()
