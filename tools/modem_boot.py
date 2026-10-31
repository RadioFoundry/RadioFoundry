#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Merge the DMR baseband's boot block from a radio's own stock firmware into a RadioFoundry image, on your machine
(the radios whose board.toml says [codec] tool = "modem_boot").

The SCT3258 baseband loads a 2048-byte boot block from the MCU at every power-up (docs/spec/hardware-d578uv.md 4.3).
It is the radio maker's, not ours: it is never stored in this repository or in a release. This tool finds it in your
stock file by its SHA-256 (rfoundry/modemboot.py) and writes the image with the block in the board's modem boot slot
(memory.ld __codec_slot) to a file you choose (by default next to the built image, in build/, which git ignores).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "rfoundry"))
from rfoundry.modemboot import BOOT_LEN, ModemBootError, find, merge  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def identify(data, board=None):
    """The block when the file carries it, else None (codec_auto.py: any file with the block will do)"""
    try:
        return find(data)
    except ModemBootError:
        return None


def board_layout(board):
    """(app_base, slot, slot_end) from the board's board.mk and memory.ld"""
    def sym(path, name):
        m = re.search(r"^\s*%s\s*:?=\s*(0x[0-9A-Fa-f]+)" % re.escape(name), open(path).read(), re.M)
        if not m:
            raise ModemBootError("%s has no %s" % (path, name))
        return int(m.group(1), 16)
    d = os.path.join(ROOT, "boards", board)
    return (sym(os.path.join(d, "board.mk"), "APP_BASE"), sym(os.path.join(d, "memory.ld"), "__codec_slot"),
            sym(os.path.join(d, "memory.ld"), "__codec_slot_end"))


def self_test():
    blk = bytes.fromhex("448a0920f83015a6") + bytes(BOOT_LEN - 8)
    try:
        find(b"\x00" * 64 + blk)
    except ModemBootError:
        pass
    else:
        raise AssertionError("a block with the wrong hash was accepted")
    out = merge(b"\x01\x02", 0x08006000, 0x08006010, 0x08006810, b"\xaa" * 16)
    assert out == b"\x01\x02" + b"\xff" * 14 + b"\xaa" * 16, out
    try:
        merge(b"\x00" * 0x20, 0x08006000, 0x08006010, 0x08006810, b"")
    except ModemBootError:
        pass
    else:
        raise AssertionError("an image over the slot was accepted")
    print("modem_boot self-test passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--stock", help="the radio's stock firmware (.CDD) or a read of its program flash")
    ap.add_argument("--board", help="the board whose image this is (boards/<board>)")
    ap.add_argument("--image", help="built RadioFoundry image (default: build/<board>/rfoundry_<board>.bin)")
    ap.add_argument("-o", "--output", help="merged image to write (default: <image>_modem.bin next to the image)")
    ap.add_argument("--self-test", action="store_true", help="check the tool's logic on synthetic data")
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    if not a.stock or not a.board:
        ap.error("--stock and --board are required")
    image = a.image or os.path.join(ROOT, "build", a.board, "rfoundry_%s.bin" % a.board)
    out = a.output or os.path.splitext(image)[0] + "_modem.bin"
    try:
        app_base, slot, slot_end = board_layout(a.board)
        blk = find(open(a.stock, "rb").read())
        data = merge(open(image, "rb").read(), app_base, slot, slot_end, blk)
    except (ModemBootError, OSError) as e:
        print("modem_boot: %s" % e, file=sys.stderr)
        return 1
    with open(out, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes, boot block at 0x%08X)" % (out, len(data), slot))
    return 0


if __name__ == "__main__":
    sys.exit(main())
