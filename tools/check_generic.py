#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Generic first (AGENTS.md rule 4): a radio's id or name appears only where that radio is the subject.

A name is a board id from the registry or one of its names (board.toml model and also), matched loosely: case, an
optional hyphen and an optional letter suffix ("DM-32UV", "DM32", "dm32"). Allowed:

  boards/<id>/                 the radio's own files
  its own pages                a docs path with the id in it (hardware-<id>.md, <id>-bringup.md, results/<id>/), a
                               board.toml [docs] page no other board shares, and a directory named <id> anywhere
                               (that radio's data, such as codec_maps/<id>/)
  LIST_PAGES                   the pages that list radios on purpose
  generated blocks             between the markers tools/registry_docs.py writes, and GENERATED files
  file references              a path or a file name in the text ("docs/spec/hardware-dm32.md 3.7"): the page it
                               points to is the radio's own
  ALLOW (generic_allow.txt)    a glob and the reason it may name radios; an entry nothing needs is an error

Everything else counts, in code, comments and docs alike, tests/ included. A file whose own path names a radio counts
too (a tool or test named after the radio that first needed it).

    tools/check_generic.py            the tree (needs the registry: Python 3.11, like tools/boards.py)
    tools/check_generic.py --self-test
boards.py check runs it.
"""
import fnmatch
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALLOW = "tools/generic_allow.txt"
LIST_PAGES = ("docs/spec/radios.md", "docs/user/23-radios.md", "docs/feature-matrix.md")
GENERATED = ("tools/odrflash/odrflash/registry.json",)
SKIP_DIRS = ("third_party/", "codecs/", "build/")
TEXT_EXT = (".c", ".h", ".S", ".ld", ".py", ".js", ".mjs", ".sh", ".mk", ".md", ".toml", ".json", ".yml", ".yaml",
            ".txt", ".html", ".css", ".cfg", ".ini", "")
# every marker style tools/registry_docs.py writes (MARKERS: html, yaml, bare)
GEN_BLOCK = re.compile(r"^[ \t]*(?:<!-- |# )?generated: tools/boards\.py docs [\w-]+;[^\n]*$.*?"
                       r"^[ \t]*(?:<!-- |# )?end generated(?: -->)?$", re.M | re.S)
FILE_REF = re.compile(r"[\w.+<>*-]*(?:/[\w.+<>*-]+)+/?|[\w+<>-]+\.(?:md|c|h|S|py|json|ld|mk|bin|js|mjs|toml|sh|elf|"
                      r"txt|html|map|log|csv|zip)\b")


def name_pattern(name):
    """A loose pattern for one radio name: "DM-32UV" matches DM-32UV, DM32UV, DM-32, dm32; a maker in front is
    dropped (a first word without a digit: "TYT MD-390"), and only the model's first word counts ("DM-1801 Pro":
    DM-1801, dm1801pro)."""
    words = name.split()
    if len(words) > 1 and not re.search(r"\d", words[0]):
        words = words[1:]
    out, number = [], False
    for part in re.findall(r"[A-Za-z]+|\d+", words[0]):
        if part.isdigit():
            out.append(("-?" if out else "") + part)
            number = True
        elif number:
            out.append("(?:-?%s)?" % part)	# a suffix after the number: optional
        else:
            out.append(("-?" if out else "") + part)
    return r"(?<![A-Za-z0-9])" + "".join(out) + r"(?![0-9])"


def patterns(radios):
    """{id: compiled pattern} from {id: [names]}."""
    return {bid: re.compile("|".join(dict.fromkeys(name_pattern(n) for n in [bid] + list(names))), re.I)
            for bid, names in radios.items()}


def registry_radios(root=ROOT):
    """{id: names} and {id: its [docs] pages}, read from each boards/<id>/board.toml (no make run needed)."""
    sys.path.insert(0, os.path.join(root, "tools"))
    import boards	# noqa: E402  (Python 3.11: tomllib)
    names, docs = {}, {}
    for bid in boards.board_dirs(root):
        data = boards._read_toml(os.path.join(root, "boards", bid, "board.toml"))
        names[bid] = [data["model"]] + list(data.get("also", []))
        docs[bid] = set(data.get("docs", {}).values())
    return names, docs


def tracked_files(root):
    try:
        out = subprocess.run(["git", "-C", root, "ls-files"], capture_output=True, text=True, check=True).stdout
        return out.split("\n")[:-1] if out else []
    except (OSError, subprocess.CalledProcessError):
        files = []
        for d, _, fs in os.walk(root):
            files += [os.path.relpath(os.path.join(d, f), root).replace(os.sep, "/") for f in fs]
        return sorted(files)


def read_allow(root):
    """[(glob, reason)] from ALLOW; a line without a reason is an error."""
    path = os.path.join(root, ALLOW)
    entries, problems = [], []
    if not os.path.isfile(path):
        return entries, problems
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            glob, _, reason = line.partition(" ")
            if not reason.strip():
                problems.append("%s:%d: %s has no reason" % (ALLOW, n, glob))
            else:
                entries.append((glob, reason.strip()))
    return entries, problems


def own(path, bid, docs, shared_docs):
    """path is the radio's own: its board directory, its pages, or its data."""
    if path.startswith("boards/%s/" % bid):
        return True
    if path in docs.get(bid, ()) and shared_docs.get(path, 0) == 1:
        return True
    parts = path.split("/")
    if parts[0] == "docs" and any(bid in p for p in parts):
        return True
    return bid in parts[:-1]


def check(root, radios, docs):
    """Problems as strings: each file and line that names a radio where it isn't the subject, and unused ALLOW
    entries."""
    pats = patterns(radios)
    allow, problems = read_allow(root)
    used = set()
    shared_docs = {}
    for pages in docs.values():
        for p in pages:
            shared_docs[p] = shared_docs.get(p, 0) + 1
    for path in tracked_files(root):
        if path.startswith(SKIP_DIRS) or path in LIST_PAGES or path in GENERATED or path == ALLOW:
            continue
        if os.path.splitext(path)[1] not in TEXT_EXT:
            continue
        hits = []
        for bid, pat in pats.items():
            if not own(path, bid, docs, shared_docs) and pat.search(path):
                hits.append((0, bid))
        try:
            with open(os.path.join(root, path), encoding="utf-8") as f:
                text = f.read()
        except (OSError, UnicodeDecodeError):
            continue
        text = GEN_BLOCK.sub(lambda m: "\n" * m.group(0).count("\n"), text)
        for n, line in enumerate(text.split("\n"), 1):
            line = FILE_REF.sub(" ", line)
            for bid, pat in pats.items():
                if pat.search(line) and not own(path, bid, docs, shared_docs):
                    hits.append((n, bid))
        if not hits:
            continue
        match = [g for g, _ in allow if fnmatch.fnmatchcase(path, g)]
        if match:
            used.update(match)
            continue
        names = sorted({b for _, b in hits})
        lines = sorted({n for n, _ in hits})
        problems.append("%s:%s: names %s (AGENTS.md rule 4: name by function; or %s with a reason)"
                        % (path, ",".join(str(n) for n in lines[:8]) + ("..." if len(lines) > 8 else ""),
                           ", ".join(names), ALLOW))
    for g, _ in allow:
        if g not in used:
            problems.append("%s: %s matches no file that names a radio: remove it" % (ALLOW, g))
    return problems


def self_test():
    import shutil
    import tempfile
    fails = []

    def ok(cond, what):
        print("%s %s" % ("ok  " if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    p = patterns({"xr100": ["XR-100UV", "Acme ZT-7"]})["xr100"]
    ok(all(p.search(s) for s in ("XR-100UV", "xr100", "XR100", "XR-100", "the ZT-7's", "zt7")),
       "name_pattern: case, hyphen, suffix, the maker dropped")
    ok(not any(p.search(s) for s in ("XR-1000", "AXR100", "ZT-70")), "name_pattern: no longer or embedded numbers")
    tmp = tempfile.mkdtemp()
    try:
        def put(path, text):
            full = os.path.join(tmp, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as f:
                f.write(text)
        radios = {"xr100": ["XR-100UV"], "qb7": ["QB-7"]}
        docs = {"xr100": {"docs/spec/hardware-xr100.md", "docs/user/install.md"},
                "qb7": {"docs/spec/hardware-qb7.md", "docs/user/install.md"}}
        put("boards/xr100/radio.c", "// the XR-100UV's PA\n")
        put("boards/qb7/radio.c", "// as on the XR-100UV\n")
        put("docs/spec/hardware-xr100.md", "# XR-100UV\n")
        put("docs/testing/results/qb7/seed.json", '{"board": "qb7"}\n')
        put("tools/odrflash/maps/xr100/a.json", '{"profile": "xr100-a"}\n')
        put("docs/spec/radios.md", "XR-100UV, QB-7\n")
        put("core/radio.c", "// see docs/spec/hardware-xr100.md 3.1\nint x;\n")
        put("README.md", "<!-- generated: tools/boards.py docs radios; edit boards -->\n| XR-100UV |\n"
                         "<!-- end generated -->\n")
        put("bug.yml", "  options:\n    # generated: tools/boards.py docs radios; edit boards\n    - XR-100UV\n"
                       "    # end generated\n")
        put("PR.md", "<!-- Which radios:\ngenerated: tools/boards.py docs radios; edit boards\nXR-100UV, QB-7\n"
                     "end generated\n-->\n")
        ok(check(tmp, radios, docs) == ["boards/qb7/radio.c:1: names xr100 (AGENTS.md rule 4: name by function; or "
                                        "%s with a reason)" % ALLOW],
           "own files, pages, data, lists, file references and generated blocks pass; another radio's name in a "
           "board fails: %s" % check(tmp, radios, docs))
        put("boards/qb7/radio.c", "// as on the other radio\n")
        ok(check(tmp, radios, docs) == [], "a clean tree: %s" % check(tmp, radios, docs))
        put("docs/user/install.md", "Hold PTT on the QB-7\n")
        ok(any(p.startswith("docs/user/install.md:1:") for p in check(tmp, radios, docs)),
           "a [docs] page two boards share is not either one's own")
        put("docs/user/install.md", "Hold PTT\n")
        put("tools/qb7_migrate.py", "# moves a radio\n")
        ok(any(p.startswith("tools/qb7_migrate.py:0:") for p in check(tmp, radios, docs)),
           "a file named after a radio fails")
        put(ALLOW, "tools/qb7_*.py\n")
        ok(any("has no reason" in p for p in check(tmp, radios, docs)), "an allow entry without a reason fails")
        put(ALLOW, "tools/qb7_*.py the stock format it reads is this radio's only\ncore/*.c a stale entry\n")
        ok(check(tmp, radios, docs) == ["%s: core/*.c matches no file that names a radio: remove it" % ALLOW],
           "an allowed file passes; an entry nothing needs fails: %s" % check(tmp, radios, docs))
    finally:
        shutil.rmtree(tmp)
    print("check_generic self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                         "" if len(fails) == 1 else "s"))
    return 1 if fails else 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv == ["--self-test"]:
        return self_test()
    if argv:
        print(__doc__, file=sys.stderr)
        return 2
    radios, docs = registry_radios()
    problems = check(ROOT, radios, docs)
    for p in problems:
        print("check_generic: %s" % p, file=sys.stderr)
    if not problems:
        print("check_generic: no radio named outside its own places")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
