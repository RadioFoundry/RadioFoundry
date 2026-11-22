#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Size budgets (docs/dev/ci.md "Budgets"): what each board's builds must leave free, and the change against a base.

    size_budget.py check SIZES.tsv [--base BASE.tsv]
    size_budget.py --self-test

SIZES.tsv is tools/ci.sh's build/ci/sizes.tsv: board, config, image bytes, app region free, RAM free, least stack
free, one row per build ("?" when unknown). A board's boards/<id>/board.toml [ci.budget] sets
flash_free_min and ram_free_min for its default build and stack_free_min for its default and maximal builds. check
fails when a build is below its budget. With --base (main's sizes.tsv in PR CI) it also prints a Markdown table of
the default and maximal builds against the base, and warns (GitHub annotation) when RAM free drops by more than
RAM_WARN bytes. The last line is a one-line summary.
"""
import os
import sys
import tempfile
import tomllib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
RAM_WARN = 256
COLS = ("bytes", "flash_free", "ram_free", "stack_free")


def read(path):
    rows = {}
    with open(path, encoding="utf-8") as fh:
        head = fh.readline().rstrip("\n").split("\t")
        for line in fh:
            f = dict(zip(head, line.rstrip("\n").split("\t")))
            if "board" not in f or "config" not in f:
                continue
            rows[(f["board"], f["config"])] = {c: (int(f[c]) if f.get(c, "?").lstrip("-").isdigit() else None)
                                               for c in COLS}
    return rows


def budget(root, board):
    try:
        with open(os.path.join(root, "boards", board, "board.toml"), "rb") as fh:
            return tomllib.load(fh).get("ci", {}).get("budget", {})
    except OSError:
        return {}


def check(rows, root):
    errors = []
    for (b, cfg), v in sorted(rows.items()):
        bud = budget(root, b)
        limits = []
        if cfg == "default":
            limits += [("flash_free", bud.get("flash_free_min")), ("ram_free", bud.get("ram_free_min"))]
        if cfg in ("default", "maximal"):
            limits.append(("stack_free", bud.get("stack_free_min")))
        for col, lim in limits:
            if lim is None:
                continue
            if v[col] is None:
                errors.append("%s %s: %s unknown, budget %d" % (b, cfg, col, lim))
            elif v[col] < lim:
                errors.append("%s %s: %s %d bytes, below the budget of %d (boards/%s/board.toml [ci.budget])"
                              % (b, cfg, col, v[col], lim, b))
    return errors


def compare(rows, base):
    out, warns = [], []
    keys = [k for k in sorted(rows) if k[1] in ("default", "maximal") and k in base]
    if not keys:
        return out, warns
    out.append("| Board | Config | Image | Flash free | RAM free | Least stack free |")
    out.append("|---|---|---|---|---|---|")

    def cell(new, old):
        if new is None or old is None:
            return "?"
        d = new - old
        return "%d (%+d)" % (new, d) if d else str(new)
    for k in keys:
        n, o = rows[k], base[k]
        out.append("| %s | %s | %s |" % (k[0], k[1], " | ".join(cell(n[c], o[c]) for c in COLS)))
        if n["ram_free"] is not None and o["ram_free"] is not None and o["ram_free"] - n["ram_free"] > RAM_WARN:
            warns.append("::warning::%s %s: RAM free %d -> %d bytes" % (k[0], k[1], o["ram_free"], n["ram_free"]))
    return out, warns


def run(sizes, base_path, root):
    rows = read(sizes)
    errors = check(rows, root)
    if base_path and os.path.isfile(base_path):
        table, warns = compare(rows, read(base_path))
        for line in table + warns:
            print(line)
    for e in errors:
        print("FAIL " + e)
    boards = sorted({b for b, _ in rows})
    print("%d boards, %d builds%s" % (len(boards), len(rows),
                                      ", %d over budget" % len(errors) if errors else ", every one within its budget"))
    return 1 if errors else 0


def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "boards", "x"))
        with open(os.path.join(d, "boards", "x", "board.toml"), "w") as fh:
            fh.write("[ci.budget]\nflash_free_min = 1000\nram_free_min = 100\nstack_free_min = 8\n")
        t = os.path.join(d, "s.tsv")
        with open(t, "w") as fh:
            fh.write("board\tconfig\tbytes\tflash_free\tram_free\tstack_free\n"
                     "x\tdefault\t5000\t2000\t50\t16\nx\tmaximal\t6000\t500\t10\t4\nx\tno-A\t4000\t3000\t?\t?\n")
        rows = read(t)
        errs = check(rows, d)
        # default: RAM 50 < 100; maximal: stack 4 < 8 (its flash and RAM have no budget); no-A: none
        ok &= len(errs) == 2 and "ram_free 50" in errs[0] and "stack_free 4" in errs[1]
        base = {("x", "default"): {"bytes": 4900, "flash_free": 2100, "ram_free": 400, "stack_free": 16}}
        table, warns = compare(rows, base)
        ok &= len(table) == 3 and "5000 (+100)" in table[2] and "| 16 |" in table[2] and len(warns) == 1
    print("size_budget self-test: %s" % ("passed" if ok else "FAILED"))
    return ok


def main(argv):
    if argv[:1] == ["--self-test"]:
        return 0 if self_test() else 1
    if argv[:1] == ["check"] and len(argv) in (2, 4):
        base = argv[3] if len(argv) == 4 and argv[2] == "--base" else None
        return run(argv[1], base, ROOT)
    print(__doc__.strip().splitlines()[2], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
