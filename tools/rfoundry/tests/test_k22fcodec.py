# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Host tests for rfoundry's K22F codec placement (k22fcodec.py): a synthetic stock image with the same shape as a real
build (a main block, a low block, a table window, a code pointer and cross-part branches), the codec packs of each board
(codec_packs/<board>/), and the numbers each board's vocoder pins. No stock file, no radio.

    python3 -m unittest discover -s tools/rfoundry/tests -v        (tools/ci.sh runs this)
"""
import os
import random
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from rfoundry import k22fcodec as G  # noqa: E402

REPO = os.path.join(HERE, "..", "..", "..")


def bl(at, target, cond_b=False):
    """Bytes of a Thumb-2 BL (or B.W) at address `at` to `target`."""
    h1, h2 = G._encode_branch(target - (at + 4), 0x9000 if cond_b else 0xD000)
    return struct.pack("<HH", h1, h2)


def synthetic(rng):
    """A flat image (from G.FLASH_BASE) and a spec for it: main 0x54000 (0x2000 bytes), low 0x4400 (0x400), table window
    0x68000 (0x1000); a code pointer in main, a BL main -> low, a B.W low -> main, a BL main -> table island, a BL
    table island -> main, and a BL inside main that must not change."""
    size = 0x7B800 - G.FLASH_BASE
    img = bytearray(rng.getrandbits(8) for _ in range(0x1000)) * (size // 0x1000 + 1)
    img = bytearray(img[:size])
    main, low, tab = (0x54000, 0x2000), (0x4400, 0x400), (0x68000, 0x1000)

    def put(addr, data):
        img[addr - G.FLASH_BASE:addr - G.FLASH_BASE + len(data)] = data
    put(0x54100, struct.pack("<I", 0x54501))		# a function pointer into main (thumb)
    put(0x54200, bl(0x54200, 0x4500))			# main -> low
    put(0x4410, bl(0x4410, 0x54300, cond_b=True))	# low -> main (B.W)
    put(0x54210, bl(0x54210, 0x68040))			# main -> island in the tables
    put(0x68040, bl(0x68040, 0x54180))			# island -> main
    put(0x54220, bl(0x54220, 0x54010))			# inside main: unchanged
    spec = {"profile": "synthetic", "stock": {}, "entries": {k: 0x100 + 4 * i for i, k in enumerate(G.ENTRY_NAMES)},
            "main": {"flash": main[0], "len": main[1]}, "low": {"flash": low[0], "len": low[1]},
            "tab": {"flash": tab[0], "len": tab[1]},
            "words": [(0x54100, 0x54501)],
            "branches": [(0x54200, 0x4500), (0x4410, 0x54300), (0x54210, 0x68040), (0x68040, 0x54180), ]}
    for p in ("main", "low", "tab"):
        spec[p]["sha256"] = G.sha256(G._window(bytes(img), spec[p]))
    return bytes(img), spec


class Placement(unittest.TestCase):
    def test_move_and_patch(self):
        img, spec = synthetic(random.Random(77))
        pk = G.pack(img, spec)
        self.assertEqual(pk.main_new, G.CODE_BASE)
        self.assertEqual(pk.low_new, G.CODE_BASE + 0x2000)
        code = pk.code
        # the pointer follows its target: main 0x54501 -> CODE_BASE + 0x501
        self.assertEqual(struct.unpack_from("<I", code, 0x100)[0], G.CODE_BASE + 0x501)
        # main -> low: the target moved to low_new + 0x100
        h1, h2 = struct.unpack_from("<HH", code, 0x200)
        self.assertEqual(G.CODE_BASE + 0x200 + 4 + G._decode_branch(h1, h2), pk.low_new + 0x100)
        # low -> main, as a B.W (the encoding kind is kept)
        h1, h2 = struct.unpack_from("<HH", code, 0x2010)
        self.assertEqual(pk.low_new + 0x10 + 4 + G._decode_branch(h1, h2), G.CODE_BASE + 0x300)
        self.assertEqual(h2 & 0xD000, 0x9000)
        # main -> island in the table window (which stays): the distance changed
        h1, h2 = struct.unpack_from("<HH", code, 0x210)
        self.assertEqual(G.CODE_BASE + 0x210 + 4 + G._decode_branch(h1, h2), 0x68040)
        # island -> main, patched in the table window's copy
        h1, h2 = struct.unpack_from("<HH", pk.tab, 0x40)
        self.assertEqual(0x68040 + 4 + G._decode_branch(h1, h2), G.CODE_BASE + 0x180)
        # a branch inside main, and everything else, unchanged
        self.assertEqual(code[0x220:0x224], img[0x54220 - G.FLASH_BASE:0x54220 - G.FLASH_BASE + 4])
        orig = bytearray(img[0x54000 - G.FLASH_BASE:0x56000 - G.FLASH_BASE])
        got = bytearray(code[:0x2000])
        for a in (0x100, 0x200, 0x210):
            orig[a:a + 4] = b"\0" * 4
            got[a:a + 4] = b"\0" * 4
        self.assertEqual(got, orig)
        self.assertEqual(pk.tab[0x100:], img[0x68100 - G.FLASH_BASE:0x69000 - G.FLASH_BASE])

    def test_descriptor_and_merge(self):
        img, spec = synthetic(random.Random(5))
        pk = G.pack(img, spec)
        d = pk.descriptor(0x4309)
        w = struct.unpack("<%dI" % (G.DESC_LEN // 4), d)
        self.assertEqual(w[:7], (G.DESC_MAGIC, 1, 0x4309, G.CODE_BASE, len(pk.code), G.crc32(pk.code), 1))
        self.assertEqual(w[7:10], (0x68000, 0x1000, G.crc32(pk.tab)))
        self.assertEqual(w[-1], G.crc32(d[:-4]))
        app = bytes(range(256)) * 20
        merged = G.merge(app, pk, 0x4309)
        self.assertEqual(merged[:len(app)], app)
        for a, data in pk.segments(0x4309):
            self.assertEqual(merged[a - G.FLASH_BASE:a - G.FLASH_BASE + len(data)], data)
        self.assertEqual(len(merged), G.DESC_ADDR + G.DESC_LEN - G.FLASH_BASE)
        with self.assertRaises(G.PackError):
            G.merge(b"\0" * (G.CODE_BASE - G.FLASH_BASE + 1), pk, 0x4309)

    def test_refuses_a_wrong_file(self):
        img, spec = synthetic(random.Random(1))
        bad = bytearray(img)
        bad[0x54500 - G.FLASH_BASE] ^= 1
        with self.assertRaises(G.PackError):
            G.pack(bytes(bad), spec)
        self.assertIsNone(G.find_spec(bytes(bad), {"synthetic": spec}))
        self.assertIs(G.find_spec(img, {"synthetic": spec}), spec)
        spec2 = dict(spec, words=[(0x54100, 0x54503)])
        with self.assertRaises(G.PackError):
            G.pack(img, spec2)
        spec3 = dict(spec, branches=spec["branches"] + [(0x54300, 0x4500)])
        with self.assertRaises(G.PackError):
            G.pack(img, spec3)

    def test_code_may_not_reach_the_tables(self):
        img, spec = synthetic(random.Random(2))
        with self.assertRaises(G.PackError):
            G.pack(img, spec, code_base=0x67800)


def pack_boards():
    """The boards with codec packs: the directories of codec_packs/"""
    return sorted(d for d in os.listdir(G.PACKS_DIR) if os.path.isdir(os.path.join(G.PACKS_DIR, d)))


class Specs(unittest.TestCase):
    def test_specs_are_sound(self):
        specs = G.load_specs()
        self.assertTrue(specs)
        for name, s in specs.items():
            tab_end = s["tab"]["flash"] + s["tab"]["len"]
            self.assertGreaterEqual(s["tab"]["flash"], G.TABLE_LO, name)
            self.assertLessEqual(tab_end, G.TABLE_HI, name)
            region = (s["main"]["len"] + (s["low"]["flash"] - G.CODE_BASE - s["main"]["len"]) % 4) + s["low"]["len"]
            self.assertEqual(s["placed"]["code_len"], region + ((G.CODE_BASE - s["main"]["flash"]) % 4 and 0), name)
            self.assertLessEqual(G.CODE_BASE + s["placed"]["code_len"], s["tab"]["flash"], name)
            self.assertEqual(len(s["branches"]), 171, name)
            self.assertTrue(all(len(set(x)) == len(x) for x in ([a for a, _ in s["branches"]], [a for a, _ in s["words"]])), name)
            self.assertEqual(s["entries"]["encode"], 0x130)

    def test_the_firmware_pins_the_same_numbers(self):
        self.assertTrue(pack_boards())
        for bid in pack_boards():
            bdir = os.path.join(REPO, "boards", bid)
            srcs = []
            for fn in sorted(os.listdir(bdir)):
                if fn.endswith(".c"):
                    with open(os.path.join(bdir, fn)) as f:
                        srcs.append(f.read())
            src = "".join(t for t in srcs if "define CODEC_CODE" in t)
            self.assertTrue(src, "%s: no source defines CODEC_CODE" % bid)
            rows = re.findall(r"\{ 0x([0-9A-F]{4})u, 0x([0-9A-F]+)u, 0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, (\d+)u, 0x([0-9A-F]{8})u \}", src)
            specs = G.load_specs(board=bid)
            self.assertEqual(len(rows), len(specs), bid)
            for (tag, code_len, code_crc, tab_addr, tab_len, tab_crc), (name, s) in zip(rows, sorted(specs.items())):
                p = s["placed"]
                self.assertEqual((int(tag, 16), int(code_len, 16), int(code_crc, 16), int(tab_crc, 16)),
                                 (p["tag"], p["code_len"], p["code_crc"], p["tab_crc"]), name)
                self.assertEqual((int(tab_addr, 16), int(tab_len)), (s["tab"]["flash"], s["tab"]["len"]), name)
            with open(os.path.join(bdir, "memory.ld")) as f:
                self.assertIn("__codec_slot = 0x%08X;" % G.CODE_BASE, f.read())
            self.assertIn("define CODEC_CODE		0x%08Xu" % G.CODE_BASE, src)


if __name__ == "__main__":
    unittest.main()
