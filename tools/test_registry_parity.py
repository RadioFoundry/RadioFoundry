#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Temporary (issue #106): the board registry (tools/boards.py) and the two copies of its facts that remain until
their readers move to it, boards/<id>/radio.json (the web site, until it reads the loader) and the README's status
table (until the docs generator writes it), agree field by field. The few intended differences are in KNOWN.
Deleted with radio.json.

ODR_PARITY_TABLES=DIR reads radio.json and README.md from another checkout (DIR), against this tree's registry."""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLES = os.path.abspath(os.environ.get("ODR_PARITY_TABLES") or ROOT)

sys.path.insert(0, os.path.join(ROOT, "tools"))
import boards as registry  # noqa: E402

reg = registry.load()

# Intended differences: (board, field) -> (radio.json value, registry value). Both must hold exactly.
KNOWN = {
    # new: the bring-up checklist
    ("dm32", "radio.json docs.bringup"): (None, "docs/testing/dm32-bringup.md"),
    ("dm1801pro", "radio.json docs.bringup"): (None, "docs/testing/dm1801pro-bringup.md"),
    ("md380", "radio.json docs.bringup"): (None, "docs/testing/md380-bringup.md"),
}

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


# radio.json (the site's copy) and the README's status table, until the site reads the registry
RJ_KEYS = {"_about", "also", "backup", "bands", "cpu", "display", "docs", "flash", "frame", "gallery", "id", "kind",
           "maker", "model", "name", "power", "readme"}
RF_KEYS = {"backend", "board", "bootloader", "build_options", "link", "stock_download", "stock_versions", "stub_tested"}
ids("radio.json boards", [d for d in os.listdir(os.path.join(TABLES, "boards"))
                          if os.path.isfile(os.path.join(TABLES, "boards", d, "radio.json"))], reg)
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
