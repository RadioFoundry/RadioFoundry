# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Host tests for the codec maps (odrflash/codecmap.py): loading, the checks, moving code and tables. The vectors in
codecmap_vectors.json are synthetic (random bytes with planted address words) and shared with the web flasher's port
(site/tests/codecmap.test.mjs), so both move a codec to the same bytes.

    python3 -m unittest discover -s tools/odrflash/tests -v
"""
import copy
import json
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from odrflash import codecmap as cm  # noqa: E402

with open(os.path.join(HERE, "codecmap_vectors.json")) as f:
    V = json.load(f)
CODE = bytes.fromhex(V["code"])
NEW = int(V["new_run"], 0)


def word(b, off):
    return struct.unpack_from("<I", b, off)[0]


def broken(**change):
    raw = copy.deepcopy(V["map"])
    for path, value in change.items():
        node = raw
        keys = path.split("__")
        for k in keys[:-1]:
            node = node[int(k)] if k.isdigit() else node[k]
        node[int(keys[-1]) if keys[-1].isdigit() else keys[-1]] = value
    return raw


class TestCodecMap(unittest.TestCase):
    def setUp(self):
        self.m = cm.normalize(copy.deepcopy(V["map"]))

    def test_numbers_as_hex_strings(self):
        self.assertEqual(self.m["code"]["run"], 0x1CEB0)
        self.assertEqual(self.m["entries"]["encode"], 0x60)
        self.assertEqual(self.m["self_refs"][0]["value"], 0x1CEC0)

    def test_move_code_rewrites_only_the_self_refs(self):
        out = cm.move_code(CODE, self.m, NEW)
        self.assertEqual(out.hex(), V["moved"])
        delta = NEW - 0x1CEB0
        for r in self.m["self_refs"]:
            self.assertEqual(word(out, r["off"]), r["value"] + delta)
        listed = {r["off"] + i for r in self.m["self_refs"] for i in range(4)}
        self.assertTrue(all(out[i] == CODE[i] for i in range(len(CODE)) if i not in listed))
        self.assertEqual(cm.move_code(CODE, self.m, 0x1CEB0), CODE, "moving to the stock address changes nothing")
        self.assertEqual(cm.relocations(self.m), [0x40, 0x84, 0xFC])

    def test_move_code_refuses_another_build(self):
        other = bytearray(CODE)
        other[5] ^= 1
        with self.assertRaises(cm.MapError):
            cm.move_code(bytes(other), self.m, NEW)
        with self.assertRaises(cm.MapError):
            cm.move_code(CODE[:-4], self.m, NEW)
        # without a hash in the map, a listed word that doesn't hold its value still stops the move
        m = cm.normalize(broken(code__sha256=None))
        other = bytearray(CODE)
        struct.pack_into("<I", other, 0x40, 0)
        with self.assertRaises(cm.MapError):
            cm.move_code(bytes(other), m, NEW)

    def test_move_tables(self):
        moves = {int(k, 0): int(v, 0) for k, v in V["table_moves"].items()}
        out = cm.move_tables(CODE, self.m, moves)
        self.assertEqual(out.hex(), V["tables_moved"])
        self.assertEqual(word(out, 0x24), 0x18002000 + 0xC, "a ref into an object keeps its offset in it")
        self.assertEqual(cm.move_tables(CODE, self.m, {}), CODE)
        with self.assertRaises(cm.MapError):
            cm.move_tables(CODE, self.m, {0x18001600: 0x18003000})	# its group partner stays

    def test_checks(self):
        bad = {
            "unaligned self ref": broken(self_refs__0__off="0x41"),
            "self ref outside the code": broken(self_refs__0__off="0x100"),
            "self ref value outside the code": broken(self_refs__0__value="0x1"),
            "unknown ref kind": broken(self_refs__0__kind="pair"),
            "movw/movt on C-SKY": broken(self_refs__0__kind="movw_movt", self_refs__0__off2="0x44"),
            "entry outside the code": broken(entries__encode="0x200"),
            "table ref into no object": broken(table_refs__0__target="0x18009000"),
            "refs outside the codec": broken(outside_refs=["0x40021000"]),
            "word listed twice": broken(table_refs__0__off="0x40"),
            "other format": broken(format="odr-codec-map/0"),
            "unknown isa": broken(isa="mips"),
            "not a number": broken(code__len="lots"),
        }
        for what, raw in bad.items():
            with self.subTest(what):
                with self.assertRaises(cm.MapError):
                    cm.normalize(raw)

    def test_move_tables_refuses_pairs_and_island_refs_only_when_they_move(self):
        T = cm.normalize(copy.deepcopy(V["thumb"]["map"]))
        code = bytes.fromhex(V["thumb"]["code"])
        self.assertEqual(cm.move_tables(code, T, {}), code)
        t = copy.deepcopy(T)
        t["island_refs"] = [{"kind": "word", "at": 0x080D19F0 + 0x40, "target": 0x080D19F0 + 0x190}]
        with self.assertRaises(cm.MapError):
            cm.move_tables(code, t, {0x080D19F0 + 0x180: 0x080D19F0 + 0x1C0})

    def test_load_maps(self):
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, "test"))
            with open(os.path.join(d, "test", "test-csky.json"), "w") as f:
                json.dump(V["map"], f)
            maps = cm.load_maps(d)
            self.assertEqual(list(maps), ["test-csky"])
            self.assertEqual(cm.by_code_sha(maps, V["map"]["code"]["sha256"])[0]["profile"], "test-csky")
            os.mkdir(os.path.join(d, "other"))
            with open(os.path.join(d, "other", "x.json"), "w") as f:
                json.dump(V["map"], f)
            with self.assertRaises(cm.MapError):	# filed under the wrong board, and a second map for the profile
                cm.load_maps(d)

    def test_shipped_maps_load(self):
        for p, m in cm.load_maps().items():
            with self.subTest(p):
                self.assertEqual(m["profile"], p)


class TestThumb(unittest.TestCase):
    """Thumb-2: movw/movt self references, and the BL calls from code kept in a table window (an island) into the
    code. The encoders agree with the GNU assembler (checked by hand); the vector is shared with codecmap.js."""

    def setUp(self):
        self.t = V["thumb"]
        self.m = cm.normalize(copy.deepcopy(self.t["map"]))
        self.code = bytes.fromhex(self.t["code"])
        self.win = bytes.fromhex(self.t["window"])
        self.at = int(self.t["window_addr"], 0)
        self.new = int(self.t["new_run"], 0)
        self.delta = self.new - self.m["code"]["run"]

    def test_bl_round_trip(self):
        b = bytearray(4)
        for at, target in ((0x080D1A00, 0x080E3AF4), (0x080D1A04, 0x08074794), (0x080D1A10, 0x07F00000),
                           (0x08000000, 0x08000004), (0x08FFFFF0, 0x08000000)):
            cm.put_bl(b, 0, at, target)
            self.assertEqual(cm.bl_target(b, 0, at), target)
        with self.assertRaises(cm.MapError):
            cm.put_bl(b, 0, 0x08000000, 0x0A000000)		# out of reach
        self.assertIsNone(cm.bl_target(b"\0\0\0\0", 0, 0x08000000))

    def test_move_code(self):
        out = cm.move_code(self.code, self.m, self.new)
        self.assertEqual(out.hex(), self.t["moved"])
        self.assertEqual(word(out, 0x40), self.m["self_refs"][0]["value"] + self.delta)
        self.assertEqual(cm.pair_value(out, self.m["self_refs"][1]), self.m["self_refs"][1]["value"] + self.delta)
        self.assertEqual(word(out, 0x80), word(self.code, 0x80), "a pointer into an island that stays is kept")
        changed = [i for i in range(len(out)) if out[i] != self.code[i]]
        self.assertTrue(all(0x40 <= i < 0x44 or 0x100 <= i < 0x108 for i in changed))
        with self.assertRaises(cm.MapError):
            cm.move_code(self.code, self.m, self.new + 2)		# Align(PC, 4) literal pools: whole words only
        with self.assertRaises(cm.MapError):
            cm.relocations(self.m)				# not a move by adding to words

    def test_move_branches(self):
        out = cm.move_branches(self.at, self.win, self.m, self.new)
        self.assertEqual(out.hex(), self.t["window_moved"])
        for b in self.m["branches"]:
            self.assertEqual(cm.bl_target(out, b["at"] - self.at, b["at"]), b["target"] + self.delta)
        self.assertEqual(cm.branches_in(self.m, self.at, len(self.win)), 2)
        self.assertEqual(cm.move_branches(self.at + 0x1000, self.win, self.m, self.new), self.win, "no branch in it")
        stale = bytearray(self.win)
        stale[0x10:0x14] = b"\0\0\0\0"
        with self.assertRaises(cm.MapError):
            cm.move_branches(self.at, bytes(stale), self.m, self.new)

    def test_checks(self):
        def bad(**change):
            raw = copy.deepcopy(self.t["map"])
            for k, v in change.items():
                node = raw
                keys = k.split("__")
                for x in keys[:-1]:
                    node = node[int(x)] if x.isdigit() else node[x]
                node[int(keys[-1]) if keys[-1].isdigit() else keys[-1]] = v
            return raw
        for what, raw in {"branch outside an island": bad(branches__0__at="0x080D1B00"),
                          "branch target outside the code": bad(branches__0__target="0x08000000"),
                          "pair without off2": bad(self_refs__1__off2=None),
                          "odd pair site": bad(self_refs__1__off="0x101"),
                          "island pointer into no island": bad(island_ptrs__0__target="0x08000000")}.items():
            with self.subTest(what):
                if raw["self_refs"][1].get("off2") is None:
                    del raw["self_refs"][1]["off2"]
                with self.assertRaises(cm.MapError):
                    cm.normalize(raw)


if __name__ == "__main__":
    unittest.main()
