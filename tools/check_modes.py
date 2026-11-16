#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Check that the channel-mode registry agrees everywhere it is listed (docs/spec/channel-modes.md 4).

    tools/check_modes.py

The kinds and the system types are listed in three places: the spec's two tables (section 4), the firmware's
X-macro lists (core/codeplug/codeplug_modes.h, CODEPLUG_KINDS and CODEPLUG_SYSTEM_TYPES) and the Codeplug Editor's
registry (site/static/js/codeplug/modes.js, KINDS and SYSTEM_TYPES). This checks that

- every id is unique in each list and inside 1-254 (0 is a plain channel or no system, 0xFF is no tag),
- the firmware and the Editor hold the same rows: id, token and short name,
- the spec's table holds the same ids with the same short names; a spec row with no short name is a reserved id,
  which neither list may use.

Exit status 0 when they agree, 1 otherwise.
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SPEC = "docs/spec/channel-modes.md"
HEADER = "core/codeplug/codeplug_modes.h"
EDITOR = "site/static/js/codeplug/modes.js"
LISTS = (("kinds", "CODEPLUG_KINDS", "KINDS", "Kind"), ("system types", "CODEPLUG_SYSTEM_TYPES", "SYSTEM_TYPES", "System type"))


def read(path):
    return open(os.path.join(ROOT, path), encoding="utf-8").read()


def header_rows(text, macro):
    """[(id, token, short)] from the X-macro list: its continuation lines after #define MACRO(X)"""
    m = re.search(rf"^#define {macro}\(X\)((?:.*\\\n)*.*)$", text, re.M)
    if not m:
        return None
    return [(int(i), tok, short) for i, tok, short in re.findall(r"X\((\d+),\s*(\w+),\s*\"([^\"]*)\"", m.group(1))]


def editor_rows(text, name):
    """[(id, token, short)] from the export const NAME = [ ... ]; table"""
    m = re.search(rf"^export const {name} = \[(.*?)^\];", text, re.M | re.S)
    if not m:
        return None
    rows = re.findall(r"\{\s*id:\s*(\d+),\s*token:\s*\"(\w+)\",\s*short:\s*\"([^\"]*)\"", m.group(1))
    return [(int(i), tok, short) for i, tok, short in rows]


def spec_rows(text, first):
    """[(id, short)] from the section 4 table whose first column is named first; short is "" for a reserved id"""
    section = re.search(r"^## 4\. .*?(?=^## )", text, re.M | re.S)
    if not section:
        return None
    table = re.search(rf"^\| {first} \|.*\n\|[-|]+\|\n((?:\|.*\n)*)", section.group(0), re.M)
    if not table:
        return None
    rows = []
    for line in table.group(1).splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if cells and cells[0].isdigit():
            rows.append((int(cells[0]), cells[2] if len(cells) > 2 else ""))
    return rows


def check():
    problems = []
    header, editor, spec = read(HEADER), read(EDITOR), read(SPEC)
    count = 0
    for what, macro, name, first in LISTS:
        h, e, s = header_rows(header, macro), editor_rows(editor, name), spec_rows(spec, first)
        for rows, where in ((h, f"{HEADER} {macro}"), (e, f"{EDITOR} {name}"), (s, f"{SPEC} 4, the {what} table")):
            if not rows:
                problems.append(f"{where}: no rows found")
        if not (h and e and s):
            continue
        count += len(h)
        for rows, where in ((h, HEADER), (e, EDITOR), (s, SPEC)):
            ids = [r[0] for r in rows]
            for i in sorted({i for i in ids if ids.count(i) > 1}):
                problems.append(f"{where}: {what} id {i} listed twice")
            for i in ids:
                if not 1 <= i <= 254:
                    problems.append(f"{where}: {what} id {i} is outside 1-254")
        hd, ed = {r[0]: r for r in h}, {r[0]: r for r in e}
        for i in sorted(set(hd) | set(ed)):
            if i not in ed:
                problems.append(f"{EDITOR}: no {name} row for {what} id {i} ({hd[i][1]})")
            elif i not in hd:
                problems.append(f"{HEADER}: no {macro} row for {what} id {i} ({ed[i][1]})")
            elif hd[i][1:] != ed[i][1:]:
                problems.append(f"{what} id {i}: {HEADER} says {hd[i][1]} \"{hd[i][2]}\", {EDITOR} {ed[i][1]} \"{ed[i][2]}\"")
        sd = dict(s)
        for i, (_, _, short) in sorted(hd.items()):
            if i not in sd:
                problems.append(f"{SPEC}: no row for {what} id {i} ({short})")
            elif sd[i] == "":
                problems.append(f"{what} id {i} is reserved in {SPEC}, but {HEADER} uses it")
            elif sd[i] != short:
                problems.append(f"{what} id {i}: {SPEC} says \"{sd[i]}\", {HEADER} \"{short}\"")
        for i, short in s:
            if short and i not in hd:
                problems.append(f"{HEADER}: no row for {what} id {i} ({short}) of {SPEC}")
    return problems, count


def main():
    problems, count = check()
    for p in problems:
        print(p)
    print(f"check_modes: {count} rows, {len(problems)} problems" if problems else f"check_modes: {count} kinds and system types agree")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
