#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Refuse a permission in free-text HIL pytest flags.

    tools/hil_flags.py WHAT FLAGS     exit 1 (and a workflow error naming WHAT) when FLAGS hold --allow-rf or another
                                      --allow-* option
    tools/hil_flags.py --self-test

.github/workflows/hil.yml checks the scheduled and nightly flags (repository variables) and the extra input with it:
a HIL run's permissions come only from the workflow's checkboxes, so the rf checkbox, and the hil-rf environment's
reviewer behind it, stay the only way to make a run transmit. pytest takes no abbreviated long options (its parser
has allow_abbrev off), so the prefix covers every spelling. The words are split at any white space, as the workflow's
unquoted $FLAGS splits them, and more.
"""
import sys

CHECKBOXES = "rf, usb_switch, codeplug_write, reboot"


def permissions(flags):
    """The words of flags that ask for a permission."""
    return [w for w in flags.split() if w.startswith("--allow")]


def self_test():
    checks = [
        ("empty", "", []),
        ("plain options", "--tier smoke --ambe /x/y.ambe --max-foff 800 -k 'dmr and not fm'", []),
        ("--allow-rf", "--tier smoke --allow-rf", ["--allow-rf"]),
        ("another permission", "--allow-codeplug-write", ["--allow-codeplug-write"]),
        ("after a tab, after a newline", "-v\t--allow-reboot\n--allow-service-stop",
         ["--allow-reboot", "--allow-service-stop"]),
        ("spelled as a glob", "--allow*", ["--allow*"]),
        ("a value that only contains the word", "-k allow --note=--allow-rf", []),
    ]
    bad = 0
    for what, flags, exp in checks:
        got = permissions(flags)
        bad += got != exp
        print(("ok   " if got == exp else "FAIL ") + what + ("" if got == exp else f": got {got}, expected {exp}"))
    return 1 if bad else 0


def main(argv):
    if argv[:1] == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    found = permissions(argv[1])
    for w in found:
        print(f"::error::{argv[0]} may not hold {w}: a HIL run's permissions come only from the workflow's checkboxes "
              f"({CHECKBOXES})")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
