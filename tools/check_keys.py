#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Check that the codeplug settings area's keys agree everywhere they are listed (docs/spec/codeplug-settings.md 3).

    tools/check_keys.py

A key is named in four places: the spec's table (section 3), the firmware's defines (core/codeplug/codeplug_kv.h,
CODEPLUG_KEY_*), the firmware's tables of keyed settings (CODEPLUG_KEY_ROW rows under core/) and the Codeplug Editor's
table (site/static/js/codeplug/kv.js, KEYS). This checks that

- every key is unique in each place and inside 0x0001-0x7FFF (0x8000 up is for development, never committed),
- the spec's table and the firmware's defines hold the same keys,
- every scalar key (uint, int, bool, enum, id) has one firmware row and one Editor row, of the spec's type,
- no row names a key the spec doesn't have.

Exit status 0 when they agree, 1 otherwise.
"""
import glob
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SPEC = "docs/spec/codeplug-settings.md"
HEADER = "core/codeplug/codeplug_kv.h"
EDITOR = "site/static/js/codeplug/kv.js"
SCALARS = ("uint", "int", "bool", "enum", "id")


def read(path):
    return open(os.path.join(ROOT, path), encoding="utf-8").read()


def spec_keys(text):
    """{key: type} from the table of section 3: the rows that start with a key number"""
    section = re.search(r"^## 3\. .*?(?=^## )", text, re.M | re.S).group(0)
    rows = re.findall(r"^\| 0x([0-9A-Fa-f]{4}) \|[^|]*\| *([a-z ]+?) *\|", section, re.M)
    return [(int(k, 16), t) for k, t in rows]


def header_keys(text):
    """[(key, name)] from the CODEPLUG_KEY_* defines"""
    return [(int(v, 16), n) for n, v in re.findall(r"^#define (CODEPLUG_KEY_\w+)\s+0x([0-9A-Fa-f]+)u?\b", text, re.M)]


def firmware_rows(names):
    """[(key, type, file)] from every CODEPLUG_KEY_ROW under core/"""
    rows = []
    for path in sorted(glob.glob(os.path.join(ROOT, "core", "**", "*.[ch]"), recursive=True)):
        for name, typ in re.findall(r"CODEPLUG_KEY_ROW\(\s*(CODEPLUG_KEY_\w+)\s*,\s*CODEPLUG_T_(\w+)", open(path, encoding="utf-8").read()):
            rows.append((names.get(name), typ.lower(), os.path.relpath(path, ROOT), name))
    return rows


def editor_rows(text):
    """[(key, type)] from the KEYS table"""
    table = re.search(r"export const KEYS = \[(.*?)^\];", text, re.M | re.S).group(1)
    return [(int(k, 16), t) for k, t in re.findall(r"key:\s*0x([0-9A-Fa-f]+)\s*,\s*type:\s*\"(\w+)\"", table)]


def check():
    problems = []

    def unique(keys, where):
        for k in sorted(set(keys)):
            if keys.count(k) > 1:
                problems.append(f"{where}: key 0x{k:04X} is listed {keys.count(k)} times")
            if not 0x0001 <= k <= 0x7FFF:
                problems.append(f"{where}: key 0x{k:04X} is outside 0x0001-0x7FFF")

    spec = spec_keys(read(SPEC))
    header = header_keys(read(HEADER))
    names = {n: k for k, n in header}
    rows = firmware_rows(names)
    editor = editor_rows(read(EDITOR))
    types = dict(spec)
    unique([k for k, _ in spec], SPEC)
    unique([k for k, _ in header], HEADER)
    unique([k for k, _ in editor], EDITOR)
    if not spec:
        problems.append(f"{SPEC}: no key found in section 3")
    for k in sorted(set(types) - {k for k, _ in header}):
        problems.append(f"{HEADER}: no define for key 0x{k:04X} of {SPEC}")
    for k, n in header:
        if k not in types:
            problems.append(f"{SPEC}: no row for {n} (0x{k:04X})")
    for k, typ, path, name in rows:
        if k is None:
            problems.append(f"{path}: a row for {name}, which {HEADER} doesn't define")
        elif types.get(k) not in (None, typ):
            problems.append(f"{path}: {name} is {typ}, {SPEC} says {types[k]}")
    for k, typ in editor:
        if k not in types:
            problems.append(f"{EDITOR}: key 0x{k:04X} isn't in {SPEC}")
        elif types[k] != typ:
            problems.append(f"{EDITOR}: key 0x{k:04X} is {typ}, {SPEC} says {types[k]}")
    for k, typ in spec:
        if typ not in SCALARS:
            continue
        n = sum(1 for r in rows if r[0] == k)
        if n != 1:
            problems.append(f"core/: {n} CODEPLUG_KEY_ROW rows for key 0x{k:04X}, need 1")
        if k not in {e for e, _ in editor}:
            problems.append(f"{EDITOR}: no KEYS row for key 0x{k:04X}")
    return problems, len(spec)


def main():
    problems, count = check()
    for p in problems:
        print(p)
    print(f"check_keys: {count} keys, {len(problems)} problems" if problems else f"check_keys: {count} keys agree")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
