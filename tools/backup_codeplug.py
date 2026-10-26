#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""backup_codeplug: the codeplugs in a radio backup as qdmr YAML (docs/spec/codeplug-port.md 5).

    backup_codeplug.py BACKUP.zip [-o DIR]            a backup bundle from the web installer (rfoundry-backup/1)
    backup_codeplug.py DUMP.bin --board ID [-o DIR]   a raw data flash dump (file offset = data flash address)
    backup_codeplug.py --self-test                    the pure logic, no dmrconf

Writes up to two codeplugs, each as a DfuSe file and the qdmr YAML dmrconf decodes from it:

    <name>-rfoundry.dfu / .yaml   RadioFoundry's codeplug: the open-firmware image at the board's hal_flash_map_t
                                  openfw_eeprom, decoded with dmrconf --radio openuv380
    <name>-stock.dfu / .yaml      the stock firmware's codeplug, for a board with [stock_codeplug] in its board.toml:
                                  its pages put at the addresses its dmrconf driver takes

A part that is blank or not in the backup is reported and skipped. The YAML opens in qdmr; the .dfu works with
dmrconf (decode, or write to a radio on that firmware). Nothing here touches a radio.

Needs Python 3.11+ (tomllib, for board.toml) and dmrconf (qdmr 0.15 or newer) on PATH or given with --dmrconf.
dmrconf is GPL and runs as an external program.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import boards  # noqa: E402  (next to this script)
import codeplug_port as cp  # noqa: E402

ROOT = os.path.dirname(HERE)
RFOUNDRY_DRIVER = "openuv380"    # the open-firmware layout RadioFoundry serves (open-firmware-cps.md 7)
OPENFW_SPAN = 0x120000        # the EEPROM image and the flash image, as the CPS wire addresses them (7.1)
PAGE = 0x1000
# the bundle parts that hold the data flash from address 0: the whole of it, or only its start
DATA_PARTS = ("spi-flash", "c7000-data", "c7000-data-start")


class BackupError(Exception):
    pass


def read_bundle(path):
    """(board id, data flash bytes from address 0, what the bundle says of the firmware) from a backup bundle."""
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        raise BackupError("%s: not a zip file" % path)
    with z:
        try:
            meta = json.loads(z.read("backup.json"))
        except KeyError:
            raise BackupError("%s: no backup.json, not a radio backup" % path)
        # odr-backup/1: the same bundle from before the project was renamed (backup.js LEGACY_FORMAT)
        if not str(meta.get("format", "")).startswith(("rfoundry-backup/", "odr-backup/")):
            raise BackupError("%s: backup format %r unknown" % (path, meta.get("format")))
        board = (meta.get("radio") or {}).get("board")
        parts = {p.get("what"): p for p in meta.get("parts", [])}
        for what in DATA_PARTS:
            p = parts.get(what)
            if p is None:
                continue
            if int(str(p.get("addr", "0")), 0) != 0:
                raise BackupError("%s: %s starts at %s, not 0" % (path, p["file"], p["addr"]))
            if not p.get("complete", True):
                print("note: %s is incomplete (the read stopped); what it holds is used" % p["file"])
            return board, z.read(p["file"]), meta.get("firmware_now") or {}
    raise BackupError("%s: no data flash in this backup (parts: %s)" % (path, ", ".join(sorted(parts)) or "none"))


def board_layout(board):
    """{openfw: data flash address of the open-firmware image or None, stock: the [stock_codeplug] table or None}."""
    if not board or board not in boards.board_dirs():
        raise BackupError("board %r unknown (boards/<id>/board.toml)" % board)
    # the registry: the board's own directory and its family's (BOARD_DIRS), where hal_flash_map_t may sit
    b = boards.load(only=[board])[board]
    return {"openfw": b.flash_map.get("openfw_eeprom"), "stock": b.data.get("stock_codeplug")}


def blank(b):
    return not b or b.count(0xFF) == len(b)


def openfw_dfu(data, base):
    """RadioFoundry's codeplug as a DfuSe file dmrconf's open-firmware driver decodes, or None when it is blank. One
    element over the whole span: the decoder reads what it needs and the bytes it doesn't are kept."""
    img = data[base:base + OPENFW_SPAN]
    if len(img) < OPENFW_SPAN:
        raise BackupError("the backup ends at 0x%X, before the codeplug image (0x%X..0x%X)" %
                          (len(data), base, base + OPENFW_SPAN))
    if blank(img):
        return None
    return cp.build_dfuse([("Codeplug EEPROM", []), ("Codeplug FLASH", [(0, img)])])


def block_id_pages(data, start, end):
    """{logical block: page} of a codeplug whose 4 KB pages each name their logical block in their last byte. 0 marks a
    free page and 0xFF an erased one; a block written twice is an error (the layer was caught mid-move)."""
    out = {}
    for a in range(start, min(end, len(data)) - PAGE + 1, PAGE):
        page = data[a:a + PAGE]
        block = page[-1]
        if block in (0, 0xFF):
            continue
        if block in out:
            raise BackupError("stock codeplug: block 0x%02X in two pages (one at 0x%06X)" % (block, a))
        out[block] = page
    return out


STOCK_LAYOUTS = {
    # the address dmrconf takes for a page is its logical block times the page size
    "block-id": lambda data, span: [(b * PAGE, p) for b, p in sorted(block_id_pages(data, *span).items())],
}


def stock_dfu(data, layout):
    if layout["pages"] not in STOCK_LAYOUTS:
        raise BackupError("stock codeplug pages %r unknown" % layout["pages"])
    start, end = layout["span"]
    if len(data) < end:
        raise BackupError("the backup ends at 0x%X, before the stock codeplug's end (0x%X)" % (len(data), end))
    elems = STOCK_LAYOUTS[layout["pages"]](data, (start, end))
    if not elems:
        return None
    return cp.build_dfuse([("Stock codeplug", elems)])


def decode(dmrconf, driver, dfu_path, yaml_path):
    if os.path.exists(yaml_path):
        os.remove(yaml_path)
    r = subprocess.run([dmrconf, "decode", "--radio", driver, dfu_path, yaml_path], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(yaml_path):
        msg = (r.stdout + r.stderr).strip().splitlines()
        raise BackupError("dmrconf decode --radio %s failed (exit %d): %s" % (driver, r.returncode,
                                                                               msg[-1] if msg else "no output"))


SECTIONS = ("radioIDs", "channels", "zones", "contacts", "groupLists", "scanLists")


def counts(yaml_text):
    """{section: entries} of a qdmr YAML file, from its top-level list sections."""
    out, cur = {}, None
    for line in yaml_text.splitlines():
        if line and not line[0].isspace():
            cur = line[:-1] if line.endswith(":") else None
            continue
        if cur and line.startswith("  - "):
            out[cur] = out.get(cur, 0) + 1
    return out


def extract(data, board, outdir, stem, dmrconf):
    """Write the codeplugs found; returns [(label, yaml path, counts)]. Raises BackupError when none is found."""
    lay = board_layout(board)
    jobs = []
    if lay["openfw"] is not None:
        jobs.append(("RadioFoundry", "rfoundry", RFOUNDRY_DRIVER, lambda: openfw_dfu(data, lay["openfw"])))
    if lay["stock"]:
        jobs.append(("stock", "stock", lay["stock"]["dmrconf"], lambda: stock_dfu(data, lay["stock"])))
    if not jobs:
        raise BackupError("board %s: no codeplug layout (no hal_flash_map_t, no [stock_codeplug])" % board)
    done = []
    for label, suffix, driver, build in jobs:
        dfu = build()
        if dfu is None:
            print("%s codeplug: blank in this backup, skipped" % label)
            continue
        dfu_path = os.path.join(outdir, "%s-%s.dfu" % (stem, suffix))
        yaml_path = os.path.join(outdir, "%s-%s.yaml" % (stem, suffix))
        with open(dfu_path, "wb") as f:
            f.write(dfu)
        decode(dmrconf, driver, dfu_path, yaml_path)
        with open(yaml_path, encoding="utf-8") as f:
            n = counts(f.read())
        done.append((label, yaml_path, n))
        print("%s codeplug: %s" % (label, yaml_path))
        print("   " + ", ".join("%d %s" % (n.get(s, 0), s) for s in SECTIONS))
    if not done:
        raise BackupError("no codeplug in this backup")
    return done


# ---------------------------------------------------------------------------------------------------------------

def self_test():
    import tempfile
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    def expect_error(fn, what):
        try:
            fn()
            fails.append("no error: " + what)
        except BackupError:
            pass

    # block-id pages: free (0) and erased (0xFF) pages skipped, the rest at block * page size
    data = bytearray(b"\xff" * 0x10000)
    for addr, block in ((0x1000, 7), (0x2000, 0), (0x5000, 2)):
        data[addr:addr + PAGE] = bytes([addr >> 12]) * (PAGE - 1) + bytes([block])
    got = STOCK_LAYOUTS["block-id"](bytes(data), (0x1000, 0x10000))
    check([a for a, _ in got] == [0x2000, 0x7000], "block-id addresses %r" % [hex(a) for a, _ in got])
    check(got[1][1][0] == 0x01 and got[0][1][0] == 0x05, "block-id pages moved with their blocks")
    dup = bytearray(data)
    dup[0x6000:0x7000] = dup[0x1000:0x2000]
    expect_error(lambda: block_id_pages(bytes(dup), 0x1000, 0x10000), "a block in two pages")
    lay = {"dmrconf": "x", "span": [0x1000, 0x10000], "pages": "block-id"}
    targets = cp.parse_dfuse(stock_dfu(bytes(data), lay))
    check([a for a, _ in targets[0][1]] == [0x2000, 0x7000], "stock DfuSe elements")
    check(stock_dfu(b"\xff" * 0x10000, lay) is None, "blank stock codeplug")
    expect_error(lambda: stock_dfu(bytes(data[:0x8000]), lay), "a backup shorter than the stock span")
    expect_error(lambda: stock_dfu(bytes(data), dict(lay, pages="other")), "an unknown page layout")

    # the open-firmware image: one element from the base, blank skipped, short refused
    flash = bytearray(b"\xff" * (0x10000 + OPENFW_SPAN))
    flash[0x10080:0x10084] = b"ABCD"
    t = cp.parse_dfuse(openfw_dfu(bytes(flash), 0x10000))
    check(len(t) == 2 and t[1][1][0][0] == 0 and t[1][1][0][1][0x80:0x84] == b"ABCD", "open-firmware DfuSe")
    check(openfw_dfu(b"\xff" * OPENFW_SPAN, 0) is None, "blank open-firmware image")
    expect_error(lambda: openfw_dfu(bytes(flash), 0x20000), "a backup shorter than the image")

    # qdmr YAML section counts
    n = counts("version: 0.15.1\nsettings:\n  boot:\n    message1: X\nchannels:\n  - fm:\n      id: ch1\n"
               "  - dmr:\n      id: ch2\nzones:\n  - id: z1\n    A: [ch1]\ncontacts: []\n")
    check(n == {"channels": 2, "zones": 1}, "yaml counts %r" % n)

    # bundles: the data flash part, a wrong address, none at all
    with tempfile.TemporaryDirectory() as tmp:
        def bundle(name, meta, files):
            p = os.path.join(tmp, name)
            with zipfile.ZipFile(p, "w") as z:
                if meta is not None:
                    z.writestr("backup.json", json.dumps(meta))
                for fn, b in files.items():
                    z.writestr(fn, b)
            return p
        meta = {"format": "rfoundry-backup/1", "radio": {"board": "b1"}, "firmware_now": {"kind": "stock"},
                "parts": [{"file": "program-memory.bin", "what": "c7000-progmem", "addr": "0x03000000"},
                          {"file": "original-channels.bin", "what": "c7000-data-start", "addr": "0x00000000"}]}
        b, d, fw = read_bundle(bundle("a.zip", meta, {"original-channels.bin": b"\x01\x02",
                                                      "program-memory.bin": b"x"}))
        check(b == "b1" and d == b"\x01\x02" and fw.get("kind") == "stock", "bundle read")
        meta2 = dict(meta, parts=[{"file": "spi-flash.bin", "what": "spi-flash", "addr": "0x1000"}])
        expect_error(lambda: read_bundle(bundle("b.zip", meta2, {"spi-flash.bin": b"x"})), "data part not at 0")
        meta3 = dict(meta, parts=meta["parts"][:1])
        expect_error(lambda: read_bundle(bundle("c.zip", meta3, {})), "no data flash part")
        expect_error(lambda: read_bundle(bundle("d.zip", None, {"x": b"x"})), "no backup.json")
        b, _, _ = read_bundle(bundle("e.zip", dict(meta, format="odr-backup/1"), {"original-channels.bin": b"\x01"}))
        check(b == "b1", "a bundle from before the rename (odr-backup/1)")
        expect_error(lambda: read_bundle(bundle("f.zip", dict(meta, format="other/1"), {})), "an unknown format")
    expect_error(lambda: board_layout("no-such-board"), "unknown board")

    # every board's open-firmware image is found, also where hal_flash_map_t sits in a family directory
    for bid in boards.board_dirs():
        check(board_layout(bid)["openfw"] is not None, "%s: no openfw_eeprom found" % bid)

    # every [stock_codeplug] in boards/ names a layout this tool knows
    for bid in boards.board_dirs():
        st = boards._read_toml(os.path.join(ROOT, "boards", bid, "board.toml")).get("stock_codeplug")
        if st:
            check(st["pages"] in STOCK_LAYOUTS, "%s: [stock_codeplug] pages %r" % (bid, st["pages"]))

    for f in fails:
        print("FAIL:", f)
    print("self-test: %s" % ("FAILED (%d)" % len(fails) if fails else "ok"))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="The codeplugs in a radio backup as qdmr YAML.")
    ap.add_argument("backup", nargs="?", help="a backup bundle (.zip) or a raw data flash dump (with --board)")
    ap.add_argument("--board", help="the board id of a raw dump (boards/<id>); overrides the bundle's")
    ap.add_argument("-o", "--outdir", help="where the files go (default: next to the backup)")
    ap.add_argument("--dmrconf", help="path of dmrconf (default: from PATH)")
    ap.add_argument("--self-test", action="store_true", help="test the logic offline (no dmrconf)")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.backup:
        ap.error("give a backup file")
    try:
        if zipfile.is_zipfile(a.backup):
            board, data, fw = read_bundle(a.backup)
            board = a.board or board
            if fw.get("kind"):
                print("backup of %s, taken on %s firmware%s" % (board, fw["kind"],
                                                                " " + fw["git"] if fw.get("git") else ""))
        else:
            if not a.board:
                raise BackupError("a raw dump needs --board")
            board = a.board
            with open(a.backup, "rb") as f:
                data = f.read()
        dmrconf = a.dmrconf or shutil.which("dmrconf")
        if not dmrconf or not os.path.isfile(dmrconf):
            raise BackupError("dmrconf not found: install qdmr's command line tool or give --dmrconf")
        outdir = os.path.abspath(a.outdir or os.path.dirname(os.path.abspath(a.backup)))
        os.makedirs(outdir, exist_ok=True)
        stem = os.path.splitext(os.path.basename(a.backup))[0]
        extract(data, board, outdir, stem, dmrconf)
    except (BackupError, boards.RegistryError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
