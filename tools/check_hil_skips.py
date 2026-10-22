#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Check that every HIL skip names its kind (tests/hil/README.md, "Skip kinds").

    tools/check_hil_skips.py              tests/hil and every boards/<b>/hil
    tools/check_hil_skips.py FILE...      only these files

A HIL test skips through odr_hil's skip(kind, reason), skip_mark(kind, reason) or skip_if(condition, kind, reason),
never pytest.skip, pytest.mark.skip(if) or pytest.importorskip: a bring-up run tells an expected skip from a gap by
the kind. The kind must be a string literal from odr_hil.SKIP_KINDS. Only the helpers in odr_hil.py and their tests
(test_skip_kinds.py) break these rules, on lines marked "# hil-skip:". Exit status 0 when every skip has a kind, 1
otherwise.
"""
import ast
import glob
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HELPER = os.path.join(ROOT, "tests", "hil", "odr_hil.py")
EXEMPT = (HELPER, os.path.join(ROOT, "tests", "hil", "test_skip_kinds.py"))   # where the pragma counts
PRAGMA = "# hil-skip:"
BARE = ("pytest.skip", "pytest.mark.skip", "pytest.mark.skipif", "pytest.importorskip")
KIND_ARG = {"skip": 0, "skip_mark": 0, "skip_if": 1}       # where the kind is in each helper's arguments


def skip_kinds(helper=HELPER):
    """The kinds odr_hil.py defines: the keys of its SKIP_KINDS dict (read, not imported: no pyserial needed)."""
    for node in ast.parse(open(helper, encoding="utf-8").read(), helper).body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == "SKIP_KINDS" for t in node.targets):
            return {k.value for k in node.value.keys}
    raise SystemExit(f"check_hil_skips: no SKIP_KINDS in {helper}")


def dotted(node):
    """"a.b.c" for an attribute chain on a name, else None."""
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        return ".".join([node.id] + parts[::-1])
    return None


def check_source(src, path, kinds, exempt=False):
    """The problems in one file's source, as "path:line: text" strings. exempt: the pragma counts (EXEMPT)."""
    lines = src.splitlines()
    problems = []

    def bad(node, text):
        if exempt and PRAGMA in lines[node.lineno - 1]:
            return
        problems.append(f"{os.path.relpath(path, ROOT)}:{node.lineno}: {text}")

    for node in ast.walk(ast.parse(src, path)):
        if isinstance(node, ast.Attribute):
            name = dotted(node)
            if name in BARE:       # pytest.skip.Exception too: its inner pytest.skip
                bad(node, f"{name}: use odr_hil's skip / skip_mark / skip_if with a kind")
        elif isinstance(node, ast.ImportFrom) and node.module in ("pytest", "_pytest.outcomes"):
            for a in node.names:
                if a.name in ("skip", "importorskip"):
                    bad(node, f"from {node.module} import {a.name}: use odr_hil's skip with a kind")
        elif isinstance(node, ast.Call):
            func = node.func.id if isinstance(node.func, ast.Name) else \
                node.func.attr if isinstance(node.func, ast.Attribute) and dotted(node.func.value) == "odr_hil" \
                else None
            if func not in KIND_ARG:
                continue
            i = KIND_ARG[func]
            arg = node.args[i] if len(node.args) > i else None
            if not (isinstance(arg, ast.Constant) and isinstance(arg.value, str)):
                bad(node, f"{func}(): the kind must be a string literal, one of {', '.join(sorted(kinds))}")
            elif arg.value not in kinds:
                bad(node, f"{func}(): unknown kind {arg.value!r} (one of {', '.join(sorted(kinds))})")
            elif len(node.args) <= i + 1:
                bad(node, f"{func}(): no reason")
    return problems


def hil_files():
    files = glob.glob(os.path.join(ROOT, "tests", "hil", "**", "*.py"), recursive=True)
    files += glob.glob(os.path.join(ROOT, "boards", "*", "hil", "**", "*.py"), recursive=True)
    return sorted(files)


def main(argv):
    kinds = skip_kinds()
    files = [os.path.abspath(f) for f in argv] if argv else hil_files()
    problems = []
    for f in files:
        problems += check_source(open(f, encoding="utf-8").read(), f, kinds,
                                 exempt=any(os.path.samefile(f, e) for e in EXEMPT if os.path.exists(e)))
    for p in problems:
        print(p)
    print(f"check_hil_skips: {len(files)} files, {len(problems)} skips without a kind")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
