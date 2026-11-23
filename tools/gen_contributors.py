#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Write CONTRIBUTORS: everyone with a commit in the git history, by name (docs/dev/release.md, the checklist).

    tools/gen_contributors.py             rewrite CONTRIBUTORS from the history; prints "changed" when it did
    tools/gen_contributors.py --print     print the names, write nothing
    tools/gen_contributors.py --self-test

A name is a commit's author or one of its Co-authored-by lines, through .mailmap (which joins one person's names and
addresses). A name that a .mailmap name extends by one word (a call sign added later) is that person too, whatever
address the commit carried, so .mailmap need not list every address. Left out: bots (a name ending "[bot]") and coding
assistants (NOT_PEOPLE_DOMAINS). No address is written. The order is by name, without regard to case: it says nothing
about who did more.

Run by a maintainer at a release, never by CI. A first pull request's author is by definition not in the file yet,
so a check that the file is current would fail the pull requests of exactly the people it is there to credit.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "CONTRIBUTORS")
NOT_PEOPLE_DOMAINS = ("anthropic.com",)  # a coding assistant's Co-authored-by address
HEADER = """\
# Everyone who has contributed to RadioFoundry, by name. Together they are "The RadioFoundry Authors" of the
# copyright notice (LICENSE); each holds the copyright in their own contributions.
#
# Generated from the git history by tools/gen_contributors.py at each release. Don't edit it: to change how your
# name appears, add a line to .mailmap.
"""


def git(*a, stdin=None):
    return subprocess.run(["git", "-C", ROOT] + list(a), input=stdin, capture_output=True, text=True, check=True).stdout


def split(ident):
    """("Name", "address") of "Name <address>"; an identity without an address has an empty one"""
    m = re.match(r"\s*(.*?)\s*<([^>]*)>\s*$", ident)
    return (m.group(1), m.group(2)) if m else (ident.strip(), "")


def names(idents, canonical=()):
    """The contributors' names from "Name <address>" identities: people only, each once, sorted without case.
    canonical: .mailmap's names; a name one of them extends by a word becomes that one"""
    longer = {c.rpartition(" ")[0].casefold(): c for c in canonical if " " in c}
    seen = {}
    for name, addr in map(split, idents):
        if not name or name.endswith("[bot]") or addr.lower().rpartition("@")[2] in NOT_PEOPLE_DOMAINS:
            continue
        name = longer.get(name.casefold(), name)
        seen.setdefault(name.casefold(), name)
    return [seen[k] for k in sorted(seen)]


def mailmap_names():
    try:
        with open(os.path.join(ROOT, ".mailmap"), encoding="utf-8") as f:
            return [split(line)[0] for line in f if line.strip() and not line.startswith("#")]
    except OSError:
        return []


def history():
    """Every author and co-author identity in the history, through .mailmap"""
    if git("rev-parse", "--is-shallow-repository").strip() == "true":
        sys.exit("gen_contributors: a shallow clone has only part of the history (git fetch --unshallow)")
    log = git("log", "--format=%an <%ae>%n%(trailers:key=Co-authored-by,valueonly,unfold)")
    idents = sorted({line.strip() for line in log.splitlines() if "<" in line})
    return git("check-mailmap", "--stdin", stdin="\n".join(idents) + "\n").splitlines()


def self_test():
    got = names(["Zed <z@example.org>", "ann <a@example.org>", "Ann <a2@example.org>", "Bo", " <x@example.org>",
                 "dependabot[bot] <d@example.org>", "Helper <noreply@anthropic.com>"])
    assert got == ["ann", "Bo", "Zed"], got
    got = names(["Ann Lee <a@example.org>", "Ann Lee K1ABC <b@example.org>", "Ann <c@example.org>"], ["Ann Lee K1ABC"])
    assert got == ["Ann", "Ann Lee K1ABC"], got
    assert split("A B <a@b>") == ("A B", "a@b") and split("A B") == ("A B", "")
    print("gen_contributors: self-test ok")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    text = HEADER + "".join(n + "\n" for n in names(history(), mailmap_names()))
    if "--print" in sys.argv[1:]:
        sys.stdout.write(text)
        return 0
    try:
        with open(OUT, encoding="utf-8") as f:
            if f.read() == text:
                return 0
    except OSError:
        pass
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
