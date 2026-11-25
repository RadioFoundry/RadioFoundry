#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Release names (docs/dev/release.md): calendar versions, so a user sees at a glance how old the firmware is.

  vYY.M          the first release of a month (v26.11)
  vYY.M.N        the next ones that month (v26.11.1, v26.11.2)
  ...bN          a pre-release of that name (v26.12b1); the final keeps the name even if it ships later

The month has no leading zero and the count starts at 1, so v26.11.3 never reads as March. The Makefile (the
RFOUNDRY_RELEASE the radio shows), tools/release_build.sh, the release workflow and the web site's build all ask this
file, so the rule lives in one place.

  tools/release_version.py check TAG       exit 0 for a release name, else 1 with the reason
  tools/release_version.py pick TAG...     print the newest release name among TAGs (other tags ignored), or nothing
  tools/release_version.py prerelease TAG  exit 0 when TAG is a pre-release (GitHub marks it so)
  tools/release_version.py next beta [YYYY-MM-DD]   the next beta to tag for a cut on that day (today): the month
                                           the release ships (after the 14th, the next month), its next bN
  tools/release_version.py next final NAME          the final of NAME's release (v26.12b2 -> v26.12)
  tools/release_version.py next point NAME          the next vYY.M.N after NAME's release (v26.12 -> v26.12.1)
  The next commands read the existing tags from git (tag -l 'v*') and never print a name that exists.
  tools/release_version.py --self-test
"""
import datetime
import os
import re
import subprocess
import sys

# the year in two digits (26 is 2026, saving room on small displays), from the first release on
NAME_RE = re.compile(r"v(2[6-9]|[3-9][0-9])\.(1[0-2]|[1-9])(?:\.([1-9][0-9]{0,2}))?(?:b([1-9][0-9]{0,2}))?")


def parse(tag):
    """The sort key of a release name (newer is larger), or None when TAG isn't one."""
    m = NAME_RE.fullmatch(tag or "")
    if m is None:
        return None
    year, month, n, beta = m.groups()
    # a beta sorts before its final: v26.12b2 < v26.12
    return (int(year), int(month), int(n or 0), (0, int(beta)) if beta else (1, 0))


def is_prerelease(tag):
    return parse(tag) is not None and "b" in tag


def newest(tags):
    names = [t for t in tags if parse(t) is not None]
    return max(names, key=parse) if names else None


def shipping_month(day):
    """(year, month) a release cut on DAY ships in: cuts are around the 20th for the next month's release."""
    y, m = day.year, day.month
    if day.day > 14:
        y, m = (y + 1, 1) if m == 12 else (y, m + 1)
    return y % 100, m


def next_name(kind, arg, tags):
    """The next tag of KIND (beta, final, point) given the existing TAGS; None when ARG doesn't make one."""
    tags = set(tags)
    if kind == "beta":
        y, m = shipping_month(arg)
        base = "v%d.%d" % (y, m)
        if base in tags:
            return None     # that month's final is out: further releases are point releases
        n = 1
        while "%sb%d" % (base, n) in tags:
            n += 1
        return "%sb%d" % (base, n)
    m = NAME_RE.fullmatch(arg or "")
    if m is None:
        return None
    base = "v%s.%s" % (m.group(1), m.group(2))
    if kind == "final":
        return None if base in tags else base
    if kind == "point":
        n = 1
        while "%s.%d" % (base, n) in tags:
            n += 1
        return "%s.%d" % (base, n)
    return None


def git_tags():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    r = subprocess.run(["git", "-C", root, "tag", "-l", "v*"], capture_output=True, text=True)
    return r.stdout.split()


def self_test():
    ok = True

    def want(cond, what):
        nonlocal ok
        if not cond:
            print("FAIL:", what)
            ok = False

    for good in ("v26.11", "v26.1", "v26.11.1", "v26.12b1", "v27.3.2b10"):
        want(parse(good) is not None, "accepts " + good)
    for bad in ("v0.1.0", "v26.011", "v26.13", "v26.0", "v26.11.0", "2026.11", "v26.11-rc.1",
                "v26.11b0", "v26.11b", "v26.11B1", "v26.12-beta.1", "v26.12-b1", "v2026", "v26.11.1.1", "v2026.11", "v25.12", "v026.1", ""):
        want(parse(bad) is None, "refuses " + repr(bad))
    want(newest(["v26.11", "v26.12b1", "v26.11.1", "v0.9.0"]) == "v26.12b1", "newest with a beta")
    want(newest(["v26.12b2", "v26.12", "v26.12b10"]) == "v26.12", "the final after its betas")
    want(newest(["v26.2", "v26.10"]) == "v26.10", "months sort as numbers")
    want(newest(["main", "v1.0.0"]) is None, "no release name")
    want(is_prerelease("v26.12b1") and not is_prerelease("v26.12"), "pre-release")
    d = datetime.date
    want(next_name("beta", d(2026, 11, 20), []) == "v26.12b1", "a cut on Nov 20 is v26.12b1")
    want(next_name("beta", d(2026, 12, 21), ["v26.12"]) == "v27.1b1", "a December cut is next year's January")
    want(next_name("beta", d(2026, 11, 26), ["v26.12b1"]) == "v26.12b2", "the second beta")
    want(next_name("beta", d(2026, 12, 3), ["v26.12b1", "v26.12b2"]) == "v26.12b3", "a beta early in the month")
    want(next_name("beta", d(2026, 12, 3), ["v26.12"]) is None, "no beta once the final is out")
    want(next_name("final", "v26.12b2", ["v26.12b1", "v26.12b2"]) == "v26.12", "the final")
    want(next_name("final", "v26.12b2", ["v26.12"]) is None, "no second final")
    want(next_name("point", "v26.12", ["v26.12", "v26.12.1"]) == "v26.12.2", "the next point release")
    want(next_name("point", "main", []) is None, "point of no release name")

    # the Makefile takes RELEASE= only when it is a release name
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

    # any board will do (print-vars builds nothing): the first in boards/
    board = sorted(d for d in os.listdir(os.path.join(root, "boards"))
                   if os.path.isfile(os.path.join(root, "boards", d, "board.mk")))[0]

    def make(*args):
        return subprocess.run(["make", "-s", "-C", root, "BOARD=" + board, "print-vars", "VARS=RFOUNDRY_RELEASE"] + list(args),
                              capture_output=True, text=True, timeout=120)
    r = make("RELEASE=v26.11.1")
    want(r.returncode == 0 and "RFOUNDRY_RELEASE=v26.11.1" in r.stdout.splitlines(), "make RELEASE=v26.11.1: " + r.stdout + r.stderr)
    r = make("RELEASE=v1.2.0")
    want(r.returncode != 0 and "not a release name" in r.stderr, "make RELEASE=v1.2.0 refused: " + r.stdout + r.stderr)
    r = make("RELEASE=")
    want(r.returncode == 0 and "RFOUNDRY_RELEASE=" in r.stdout.splitlines(), "make RELEASE= (none): " + r.stdout + r.stderr)
    # tools/release_build.sh refuses a tag that isn't one before it builds anything
    r = subprocess.run([os.path.join(root, "tools", "release_build.sh"), "--toolchain", "arm", "--version", "v1.2.0",
                        "--out", os.devnull], capture_output=True, text=True, timeout=120)
    want(r.returncode == 2 and "not a release name" in r.stderr, "release_build.sh v1.2.0 refused: " + r.stdout + r.stderr)

    print("release_version self-test: %s" % ("passed" if ok else "FAILED"))
    return ok


def main(argv):
    if argv[:1] == ["--self-test"]:
        return 0 if self_test() else 1
    if len(argv) == 2 and argv[0] == "check":
        if parse(argv[1]) is None:
            print("%r is not a release name: vYY.M, vYY.M.N, either with bN (docs/dev/release.md)" % argv[1],
                  file=sys.stderr)
            return 1
        return 0
    if argv[:1] == ["pick"]:
        n = newest(argv[1:])
        if n:
            print(n)
        return 0
    if len(argv) == 2 and argv[0] == "prerelease":
        return 0 if is_prerelease(argv[1]) else 1
    if argv[:1] == ["next"] and len(argv) in (2, 3) and argv[1] in ("beta", "final", "point"):
        if argv[1] == "beta":
            try:
                arg = datetime.date.fromisoformat(argv[2]) if len(argv) == 3 else datetime.date.today()
            except ValueError:
                print("not a date: %r (YYYY-MM-DD)" % argv[2], file=sys.stderr)
                return 2
        elif len(argv) == 3:
            arg = argv[2]
        else:
            print("next %s needs a release name" % argv[1], file=sys.stderr)
            return 2
        n = next_name(argv[1], arg, git_tags())
        if n is None:
            print("no next %s from %s: that release exists or the name is not a release name" % (argv[1], arg),
                  file=sys.stderr)
            return 1
        print(n)
        return 0
    print("usage: release_version.py check TAG | pick TAG... | prerelease TAG | next beta [DATE] | "
          "next final NAME | next point NAME | --self-test", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
