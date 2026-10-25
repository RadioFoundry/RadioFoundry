#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Generic first (AGENTS.md rule 4): a radio's id or name appears only where that radio is the subject.

A name is a board id from the registry or one of its names (board.toml model and also), matched loosely: case, an
optional hyphen and an optional letter suffix ("XR-100UV", "XR100", "xr100"). Allowed:

  boards/<id>/                 the radio's own files
  its family                   boards of one radio (board.toml part_of) share their own places: each may name the
                               others there
  its own pages                a docs path with the id in it (hardware-<id>.md, <id>-bringup.md, results/<id>/), a
                               board.toml [docs] page no other board shares, and a directory named <id> anywhere
                               (that radio's data, such as codec_maps/<id>/)
  LIST_PAGES                   the pages that list radios on purpose
  generated blocks             between the markers tools/registry_docs.py writes, and GENERATED files
  file references              a path or a file name in the text ("docs/spec/hardware-dm32.md 3.7"): the page it
                               points to is the radio's own
  ALLOW (generic_allow.txt)    a glob and the reason it may name radios; an entry nothing needs is an error

Docs (AGENTS.md, generic docs) are stricter. A generic doc (GENERIC_DOCS: docs/spec/ but hardware-*, docs/user/ but
23-radios.md, docs/dev/, docs/build-config.md, and no radio's own page) names no radio, not even in a link to the
radio's page: it says what a board provides and links feature-matrix.md. A radio's own page under those directories
links to the shared spec, not to another radio's page. ALLOW excuses a generic doc only by its exact path, one file
at a time.

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
DOC_DIRS = ("docs/spec/", "docs/user/", "docs/dev/", "docs/build-config.md")
GENERATED = ("tools/rfoundry/rfoundry/registry.json",)
SKIP_DIRS = ("third_party/", "codecs/", "build/")
TEXT_EXT = (".c", ".h", ".S", ".ld", ".py", ".js", ".mjs", ".sh", ".mk", ".md", ".toml", ".json", ".yml", ".yaml",
            ".txt", ".html", ".css", ".cfg", ".ini", "")
# every marker style tools/registry_docs.py writes (MARKERS: html, yaml, bare)
GEN_BLOCK = re.compile(r"^[ \t]*(?:<!-- |# )?generated: tools/boards\.py docs [\w-]+;[^\n]*$.*?"
                       r"^[ \t]*(?:<!-- |# )?end generated(?: -->)?$", re.M | re.S)
FILE_REF = re.compile(r"[\w.+<>*-]*(?:/[\w.+<>*-]+)+/?|[\w+<>-]+\.(?:md|c|h|S|py|json|ld|mk|bin|js|mjs|toml|sh|elf|"
                      r"txt|html|map|log|csv|zip)\b")


def name_pattern(name, strict_suffix=False, whole=False):
    """A loose pattern for one radio name: "XR-100UV" matches XR-100UV, XR100UV, XR-100, xr100; a maker in front is
    dropped (a first word without a digit: "Acme ZT-7"), and only the model's first word counts ("XR-200 Pro":
    XR-200, xr200pro). Where two radios' names differ only by such a suffix ("ZT-7" and "ZT-7S"), strict_suffix keeps
    the longer one's suffix required and whole keeps the shorter one from matching inside the longer."""
    words = name.split()
    if len(words) > 1 and not re.search(r"\d", words[0]):
        words = words[1:]
    out, number = [], False
    for part in re.findall(r"[A-Za-z]+|\d+", words[0]):
        if part.isdigit():
            out.append(("-?" if out else "") + part)
            number = True
        elif number:
            out.append(("-?%s" if strict_suffix else "(?:-?%s)?") % part)	# a suffix after the number: optional
        else:
            out.append(("-?" if out else "") + part)
    return r"(?<![A-Za-z0-9])" + "".join(out) + (r"(?![A-Za-z0-9])" if whole else r"(?![0-9])")


def patterns(radios):
    """{id: compiled pattern} from {id: [names]}. A name whose loose form also matches another radio's name keeps its
    suffix, and the other's pattern stops at a letter (name_pattern strict_suffix, whole): "ZT-7S" doesn't name the
    ZT-7 of another board, nor "ZT-7" the ZT-7S."""
    def others(bid):
        return [o.split()[-1] for other, names in radios.items() if other != bid for o in [other] + list(names)]

    def pattern(bid, n):
        loose = re.compile(name_pattern(n), re.I)
        shorter = any(loose.fullmatch(o) and not re.compile(name_pattern(o), re.I).fullmatch(n.split()[-1])
                      for o in others(bid))
        longer = any(re.compile(name_pattern(o), re.I).fullmatch(n.split()[-1]) and not loose.fullmatch(o)
                     for o in others(bid))
        return name_pattern(n, strict_suffix=shorter, whole=longer)
    return {bid: re.compile("|".join(dict.fromkeys(pattern(bid, n) for n in [bid] + list(names))), re.I)
            for bid, names in radios.items()}


def registry_radios(root=ROOT):
    """{id: names}, {id: its [docs] pages} and {id: part_of}, read from each boards/<id>/board.toml (no make run
    needed)."""
    sys.path.insert(0, os.path.join(root, "tools"))
    import boards	# noqa: E402  (Python 3.11: tomllib)
    names, docs, part_of = {}, {}, {}
    for bid in boards.board_dirs(root):
        data = boards._read_toml(os.path.join(root, "boards", bid, "board.toml"))
        # code the radio shares with others of its board family (a boards/<dir> its BOARD_DIRS builds, with no
        # board.mk): one of its own places
        docs[bid] = set()
        names[bid] = [data["model"]] + list(data.get("also", []))
        docs[bid] = set(data.get("docs", {}).values()) | {d + "/" for d in shared_dirs(root, bid)}
        if data.get("part_of"):
            part_of[bid] = data["part_of"]
    return names, docs, part_of


def shared_dirs(root, bid):
    """The boards/<dir> directories without a board.mk that boards/<bid>/board.mk lists in BOARD_DIRS (read as text)."""
    try:
        with open(os.path.join(root, "boards", bid, "board.mk"), encoding="utf-8") as f:
            mk = f.read().replace("\\\n", " ")
    except OSError:
        return []
    out = []
    for m in re.finditer(r"(?m)^\s*BOARD_DIRS\s*[:+]?=(.*)$", mk):
        for d in m.group(1).split():
            d = d.strip("/")
            if (re.fullmatch(r"boards/[\w-]+", d) and d != "boards/" + bid and os.path.isdir(os.path.join(root, d))
                    and not os.path.isfile(os.path.join(root, d, "board.mk"))):
                out.append(d)
    return out


def families(ids, part_of):
    """{id: the ids of its radio}: a board, the one it is part_of and every other part of that one."""
    root = {b: part_of.get(b, b) for b in ids}
    return {b: {o for o in ids if root[o] == root[b]} for b in ids}


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


def own(path, bid, docs, shared_docs, family=None):
    """path is the radio's own: its board directory, its pages, or its data, or those of a board of the same radio."""
    if family:
        return any(own(path, b, docs, shared_docs) for b in family.get(bid, {bid}))
    if path.startswith("boards/%s/" % bid):
        return True
    if any(p.endswith("/") and path.startswith(p) for p in docs.get(bid, ())):
        return True	# a board family directory (shared_dirs)
    if path in docs.get(bid, ()) and shared_docs.get(path, 0) == 1:
        return True
    parts = path.split("/")
    if parts[0] == "docs" and any(bid in p for p in parts):
        return True
    return bid in parts[:-1]


def doc_scope(path):
    """path is a doc under the stricter docs rule: a generic doc, or a radio's own page beside them."""
    return path.endswith(".md") and path.startswith(DOC_DIRS) and path not in LIST_PAGES


def generic_doc(path, owned):
    """A doc that is no radio's: GENERIC_DOCS less hardware-* pages and pages a radio owns (owned)."""
    return doc_scope(path) and not os.path.basename(path).startswith("hardware-") and not owned


def check(root, radios, docs, part_of=None):
    """Problems as strings: each file and line that names a radio where it isn't the subject, and unused ALLOW
    entries. part_of ({id: id}) joins the boards of one radio."""
    pats = patterns(radios)
    family = families(list(radios), part_of or {})
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
        # a generic doc is a radio's only by its path (hardware-<id>.md), never because one board.toml [docs] lists it
        generic = generic_doc(path, any(own(path, b, {}, {}) for b in radios))
        pages = {} if generic else docs
        for bid, pat in pats.items():
            if not own(path, bid, pages, shared_docs, family) and pat.search(path):
                hits.append((0, bid))
        try:
            with open(os.path.join(root, path), encoding="utf-8") as f:
                text = f.read()
        except (OSError, UnicodeDecodeError):
            continue
        text = GEN_BLOCK.sub(lambda m: "\n" * m.group(0).count("\n"), text)
        for n, line in enumerate(text.split("\n"), 1):
            # in docs a link names the radio whose page it is: a generic doc links feature-matrix.md, a radio's page
            # the shared spec
            if not doc_scope(path):
                line = FILE_REF.sub(" ", line)
            for bid, pat in pats.items():
                if pat.search(line) and not own(path, bid, pages, shared_docs, family):
                    hits.append((n, bid))
        if not hits:
            continue
        match = [g for g, _ in allow if fnmatch.fnmatchcase(path, g) and (g == path or not generic)]
        if match:
            used.update(match)
            continue
        names = sorted({b for _, b in hits})
        lines = sorted({n for n, _ in hits})
        why = ("AGENTS.md, generic docs: say what a board provides and link feature-matrix.md" if generic else
               "AGENTS.md rule 4: name by function")
        problems.append("%s:%s: names %s (%s; or %s with a reason)"
                        % (path, ",".join(str(n) for n in lines[:8]) + ("..." if len(lines) > 8 else ""),
                           ", ".join(names), why, ALLOW))
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
        put("tools/rfoundry/maps/xr100/a.json", '{"profile": "xr100-a"}\n')
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
        os.remove(os.path.join(tmp, ALLOW))
        os.remove(os.path.join(tmp, "tools/qb7_migrate.py"))
        radios["xr100b"] = ["XR-100UV B"]
        docs["xr100b"] = set()
        put("boards/xr100/radio.c", "// the QB-7's PA, unlike the XR-100UV B\n")
        put("tools/rfoundry/maps/xr100b/a.json", '{"like": "xr100"}\n')
        got = check(tmp, radios, docs, {"xr100b": "xr100"})
        ok(got == ["boards/xr100/radio.c:1: names qb7 (AGENTS.md rule 4: name by function; or %s with a reason)"
                   % ALLOW], "boards of one radio (part_of) may name each other, another radio still fails: %s" % got)
        put("boards/xr100/radio.c", "// the PA\n")
        put("docs/spec/audio.md", "The speaker amplifier (docs/spec/hardware-xr100.md 3).\n")
        got = check(tmp, radios, docs, {"xr100b": "xr100"})
        ok([p[:22] for p in got] == ["docs/spec/audio.md:1: "] and "generic docs" in got[0],
           "a generic doc may not link a radio's page: %s" % got)
        put("docs/spec/audio.md", "The speaker amplifier ([the matrix](../feature-matrix.md)).\n")
        put("docs/spec/hardware-qb7.md", "As on [the other radio](hardware-xr100.md); see [audio](audio.md).\n")
        got = check(tmp, radios, docs, {"xr100b": "xr100"})
        ok([p[:29] for p in got] == ["docs/spec/hardware-qb7.md:1: "],
           "a radio's page links the shared spec, not another radio's page: %s" % got)
        put("docs/spec/hardware-qb7.md", "See [audio](audio.md).\n")
        put("docs/spec/audio.md", "On the QB-7 the amplifier is slow.\n")
        put(ALLOW, "docs/spec/* specs cite radios\n")
        got = check(tmp, radios, docs, {"xr100b": "xr100"})
        ok(len(got) == 2 and got[0].startswith("docs/spec/audio.md:1:") and "matches no file" in got[1],
           "a glob doesn't excuse a generic doc: %s" % got)
        put(ALLOW, "docs/spec/audio.md to rewrite\n")
        got = check(tmp, radios, docs, {"xr100b": "xr100"})
        ok(got == [], "its exact path does: %s" % got)
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
    radios, docs, part_of = registry_radios()
    problems = check(ROOT, radios, docs, part_of)
    for p in problems:
        print("check_generic: %s" % p, file=sys.stderr)
    if not problems:
        print("check_generic: no radio named outside its own places")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
