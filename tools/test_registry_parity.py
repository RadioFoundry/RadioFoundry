#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Temporary (issue #106): the board registry (tools/boards.py) holds every fact the tools' own board tables, radio.json
and the README's status table hold. Strict: every field is compared, and the few intended differences are listed in
KNOWN by board and field, so any other change to an old table fails. Deleted with those tables once the tools read the
registry.

ODR_PARITY_TABLES=DIR reads the old tables, radio.json and README.md from another checkout (DIR), against this tree's
registry: after a rebase, run it against the new base to see what changed in the old tables meanwhile."""
import importlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLES = os.path.abspath(os.environ.get("ODR_PARITY_TABLES") or ROOT)

sys.path.insert(0, os.path.join(ROOT, "tools"))
import boards as registry  # noqa: E402

reg = registry.load()
view = registry.odrflash_view(reg)["boards"]

# the old tables, imported from TABLES (odrflash after the registry: it imports stockfw from there too)
sys.path[:0] = [os.path.join(TABLES, p) for p in ("tools", "stub", os.path.join("tools", "odrflash"))]
for name in ("boards", "odrflash", "odrflash.boards", "odrflash.stockfw"):
    sys.modules.pop(name, None)
ofb = importlib.import_module("odrflash.boards")
f4stub = importlib.import_module("f4stub")
stubtool = importlib.import_module("stubtool")
dm32_flash = importlib.import_module("dm32_flash")
hrc7000_codec = importlib.import_module("hrc7000_codec")
stm32_dfu = importlib.import_module("stm32_dfu")
codec_auto = importlib.import_module("codec_auto")

# Intended differences between an old table and the registry: (board, field) -> (old value, registry value). Both must
# hold exactly; anything else that differs fails.
KNOWN = {
    # the stub's alias nothing sends; VERSION board= reports one of debuglink_names
    ("dm32", "stubtool names"): (("DM-32UV", "dm32", "dm32uv"), ("DM-32UV", "DM-32", "dm32")),
    ("dm1801pro", "stubtool names"): (("DM-1801 Pro", "dm1801pro"), ("DM-1801 Pro", "DM-1801Pro", "dm1801pro")),
    ("db25d", "stubtool names"): (("DB25-D", "db25d"), ("DB25-D", "DB25-D PRO", "db25d")),
    # the stub's messages name the board by its model
    ("db25d", "stubtool name"): ("DB25-D", "DB25-D PRO"),
    # the DM-1801 Pro's linker script has the codec slot (hrc7000_codec merges it); the loader's note was stale
    ("dm1801pro", "loader codec_slot"): (False, True),
    # new: the bring-up checklist
    ("dm32", "radio.json docs.bringup"): (None, "docs/testing/dm32-bringup.md"),
    ("dm1801pro", "radio.json docs.bringup"): (None, "docs/testing/dm1801pro-bringup.md"),
    ("md380", "radio.json docs.bringup"): (None, "docs/testing/md380-bringup.md"),
}
STOCK_ONLY = {"mduv380", "md9600"}	# stm32_dfu.KEYS models with no board (planned radios)

fails = []
used = set()


def eq(bid, what, old, new):
    k = (bid, what)
    if k in KNOWN:
        used.add(k)
        if (old, new) != KNOWN[k]:
            fails.append("%s %s: table %r, registry %r; KNOWN says %r" % (bid, what, old, new, KNOWN[k]))
    elif old != new:
        fails.append("%s %s: table %r, registry %r" % (bid, what, old, new))


def ids(what, old, new):
    if sorted(old) != sorted(new):
        fails.append("%s: table %s, registry %s" % (what, sorted(old), sorted(new)))


ids("boards (odrflash)", ofb.BOARDS, reg)
for bid, old in ofb.BOARDS.items():
    b, v = reg[bid], view[bid]
    dfu = v["dfu"] or {}
    for what, o, n in (
            ("name", old.name, b.data["name"]), ("family", old.family, v["family"]),
            ("app_base", old.app_base, v["app_base"]), ("flash_end", old.flash_end, v["flash_end"]),
            ("sectors", [(s.index, s.addr, s.size) for s in old.sectors], [tuple(s) for s in v["sectors"]]),
            ("codec id", old.codec.id, v["codec"]["profile"]),
            ("codec windows", [tuple(w) for w in old.codec.windows], [tuple(w) for w in v["codec"]["windows"]]),
            ("codec installed", old.codec.installed, v["codec"]["installed"]),
            ("codec sources", old.codec.sources, v["codec"]["sources"]),
            ("debuglink_names", old.debuglink_names, v["debuglink_names"]),
            ("releasable", old.releasable, v["release"] == "stable"),
            ("keep_codec", old.keep_codec_supported, v["keep_codec"]),
            ("key_sha256", old.key_sha256, dfu.get("key_sha256")),
            ("dfu ids", old.dfu_ids, (dfu["vid"], dfu["pid"]) if dfu else None),
            ("dfu string1", old.dfu_string1, dfu.get("string1")), ("dfu product", old.dfu_product, dfu.get("product")),
            ("protected", [tuple(p) for p in old.protected], [tuple(p) for p in v["protected"]]),
            ("first_stage", old.first_stage, b.first_stage)):
        eq(bid, what, o, n)

ids("f4stub boards", f4stub.BOARDS, [b for b in reg if reg[b].platform.id == "stm32f4"])
for bid, old in f4stub.BOARDS.items():
    v = view[bid]
    ids("%s f4stub keys" % bid, old, ["name", "names", "owned", "codec"])
    eq(bid, "f4stub name", old["name"], v["name"])
    eq(bid, "f4stub names", old["names"], tuple(v["debuglink_names"]))
    eq(bid, "f4stub owned", old["owned"], [tuple(v["app_region"])])
    eq(bid, "f4stub codec", old["codec"], (v["app_region"][1], v["flash_end"]))

ids("stubtool C7_BOARDS", stubtool.C7_BOARDS, [b for b in reg if view[b]["stub"]])
eq("generic", "stubtool GENERIC", stubtool.GENERIC, {"id": 0, "name": "HR_C7000 radio", "names": (), "latch": 255,
                                                    "key": 255, "nand": False, "progmem": 0x100000})
for bid, old in stubtool.C7_BOARDS.items():
    s, v = view[bid]["stub"], view[bid]
    ids("%s stubtool keys" % bid, old, ["id", "name", "names", "latch", "key", "nand", "progmem"])
    for k in ("id", "latch", "key", "nand", "progmem"):
        eq(bid, "stub " + k, old[k], s[k])
    eq(bid, "stubtool name", old["name"], v["model"])
    eq(bid, "stubtool names", old["names"], tuple(v["debuglink_names"]))

ids("serial loader models", dm32_flash.MODELS, [b for b in reg if view[b]["family"] == "bfuv32-serial"])
for bid, m in dm32_flash.MODELS.items():
    v = view[bid]
    eq(bid, "loader name", m.name, v["model"])
    eq(bid, "loader id", m.tag, v["bootloader"].get("id"))
    eq(bid, "loader keys", m.keys, v["bootloader"].get("keys"))
    eq(bid, "loader codec_slot", m.codec_slot, v["codec"]["slot"] is not None)
    eq(bid, "loader app base", dm32_flash.APP_BASE, v["app_base"])
    eq(bid, "loader flash end", dm32_flash.FLASH_END, v["flash_end"])
    eq(bid, "loader slot base", dm32_flash.SLOT_BASE, v["codec"]["slot"][0])
eq("dm32", "loader default", dm32_flash.MODEL_DM32.tag, dm32_flash.FILE_MAGIC.decode())

ids("hrc7000_codec boards", hrc7000_codec.BOARDS, [b for b in reg if reg[b].platform.id == "hrc7000"])
for bid, old in hrc7000_codec.BOARDS.items():
    c, s = view[bid]["codec"], view[bid]["stub"]
    if set(old) - {"slot", "run", "tables", "progmem_size", "data_flash", "stock_console"}:
        fails.append("%s hrc7000_codec: new keys %s" % (bid, sorted(old)))
    eq(bid, "slot", old["slot"], tuple(c["slot"]))
    eq(bid, "run", old["run"], tuple(c["run"]))
    eq(bid, "tables", old["tables"], tuple(c["tables"]))
    eq(bid, "progmem_size", old.get("progmem_size", 0x100000), s["progmem"])
    eq(bid, "data_flash", old.get("data_flash", True), not s["nand"])
    eq(bid, "stock_console", old.get("stock_console", False), s["stock_console"])

dfu_boards = [b for b in reg if view[b]["dfu"]]
ids("stm32_dfu KEYS", stm32_dfu.KEYS, dfu_boards + sorted(STOCK_ONLY))
ids("stm32_dfu CODEC_WINDOWS", stm32_dfu.CODEC_WINDOWS, dfu_boards)
ids("stm32_dfu CODEC_SIG_ADDR", stm32_dfu.CODEC_SIG_ADDR, dfu_boards)
for bid in dfu_boards:
    v = view[bid]
    sha, prefix = stm32_dfu.KEYS[bid]
    eq(bid, "stm32_dfu key", sha, v["dfu"]["key_sha256"])
    eq(bid, "stm32_dfu key prefix", prefix, bytes.fromhex(v["dfu"]["key_prefix"]))
    eq(bid, "stm32_dfu codec windows", stm32_dfu.CODEC_WINDOWS[bid],
       sorted((a, a + n) for a, n in v["codec"]["windows"][:2]))
    eq(bid, "stm32_dfu codec signature address", stm32_dfu.CODEC_SIG_ADDR[bid], v["codec"]["code"])
    eq(bid, "stm32_dfu regions", stm32_dfu.REGIONS, [(a, n) for _, a, n in v["sectors"]])
    eq(bid, "stm32_dfu alias", stm32_dfu.ALIASES.get(bid), bid)

eq("all", "codec_auto CODEC_TOOLS", codec_auto.CODEC_TOOLS, {b: reg[b].data["codec"]["tool"] for b in reg})

# radio.json (the site's copy) and the README's status table, until the site reads the registry
RJ_KEYS = {"_about", "also", "backup", "bands", "cpu", "display", "docs", "flash", "frame", "gallery", "id", "kind",
           "maker", "model", "name", "power", "readme"}
RF_KEYS = {"backend", "board", "bootloader", "build_options", "link", "stock_download", "stock_versions", "stub_tested"}
rows = {}
with open(os.path.join(TABLES, "README.md"), encoding="utf-8") as f:
    for line in f:
        m = re.match(r"^\| ([^|]+) \| [^|]+ \| [^|]+ \| [^|]+ \| (.*) \|$", line.rstrip("\n"))
        if m:
            rows[m.group(1).strip()] = m.group(2).strip()
for bid, b in reg.items():
    with open(os.path.join(TABLES, "boards", bid, "radio.json"), encoding="utf-8") as f:
        rj = json.load(f)
    d = b.data
    if set(rj) - RJ_KEYS or set(rj["flash"]) - RF_KEYS:
        fails.append("%s radio.json: new keys %s" % (bid, sorted((set(rj) - RJ_KEYS) | (set(rj["flash"]) - RF_KEYS))))
    for k in ("id", "name", "maker", "model", "also", "kind"):
        eq(bid, "radio.json " + k, rj[k], d[k])
    for k in ("bands", "power", "cpu", "display", "gallery"):
        eq(bid, "radio.json " + k, rj[k], d["hardware"][k])
    for k in sorted(set(rj["docs"]) | set(d["docs"])):
        eq(bid, "radio.json docs." + k, rj["docs"].get(k), d["docs"].get(k))
    fl, rf = d["flash"], rj["flash"]
    eq(bid, "radio.json flash.board", rf["board"], bid)
    for k in ("backend", "link", "stock_versions"):
        eq(bid, "radio.json flash." + k, rf[k], fl[k])
    eq(bid, "radio.json stock_download", rf.get("stock_download"), fl.get("stock_download"))
    eq(bid, "radio.json bootloader", rf.get("bootloader", []), fl["bootloader"]["steps"])
    eq(bid, "radio.json stub_tested", rf.get("stub_tested", False), fl.get("stub", {}).get("tested", False))
    eq(bid, "radio.json build_options", rf.get("build_options", []), d.get("build_options", []))
    eq(bid, "radio.json backup", rj["backup"], fl["backup"])
    eq(bid, "radio.json frame", rj["frame"], d["frame"])
    row = [v for k, v in rows.items() if k.startswith(rj["readme"])]
    eq(bid, "README status", row[0] if len(row) == 1 else row, d["status"]["summary"])

for k in sorted(set(KNOWN) - used):
    fails.append("%s %s: listed in KNOWN but no longer compared" % k)
for f in fails:
    print("parity: " + f, file=sys.stderr)
print("registry parity %s (%d boards, %d mismatch%s; tables from %s)"
      % ("PASSED" if not fails else "FAILED", len(reg), len(fails), "" if len(fails) == 1 else "es",
         "this tree" if TABLES == ROOT else TABLES))
sys.exit(1 if fails else 0)
