#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""What a change can break: the switch-off builds a diff needs (docs/dev/ci.md "Affected builds").

    tools/affected.py builds --base REF [--head REF] [--board a,b] [--limit N]
        one "board name make-args" line per build, the same names tools/ci.sh's matrix uses (no-<SWITCH>,
        fm-minimal), most relevant first; --limit keeps the first N
    tools/affected.py summary --base REF [--head REF] [--board a,b]
        the boards and switches the diff reaches, and why
    tools/affected.py --self-test

The full matrix builds each of about a hundred switches off on every board. A pull request breaks only the switches
its code sits under, so this picks them from the diff:

  switches  every CONFIG_<X> named in a changed line (a guard added, moved or removed), in a changed source file, or in
            a header the file includes with quotes (core/config.h, which names them all, left out); a file the Makefile
            builds only for a switch (ifeq on it) adds that switch
  boards    a file under a directory a board.mk lists (BOARD_DIRS and the other *_DIRS) reaches the boards that list
            it; boards/<b>/ and boot/<stage>/ their board; platform/<p>/ every board on the platform; any other source
            (core/, hal/, config/, drivers/ no board lists, the Makefile) every board
  builds    a switch from a board's own files is built off on that board; a switch from shared code on one board per
            platform (the one with the most switches on), since the platforms differ in glue and linker scripts.
            fm-minimal on every board whose platform, board, boot stage or linker script changed, and on the
            per-platform board when DMR is among the switches. Only switches on by default and not required by the
            board (print-required) are built off, as in the matrix

Files outside the firmware sources (docs, site, tools, tests) reach nothing. The nightly matrix stays the backstop.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_TOPS = ("core", "hal", "drivers", "platform", "boards", "boot", "config")
SRC_EXT = (".c", ".h", ".S", ".s", ".ld", ".mk")
SHARED_FILES = ("Makefile",)
# tools/ci.sh's fm-minimal configuration
FM_MINIMAL = "CONFIG_DMR=n CONFIG_USB=n CONFIG_VFO=n CONFIG_DIAG=n CONFIG_SUBTONE=n CONFIG_FONT_XL=n"
CONFIG_RE = re.compile(r"\bCONFIG_([A-Z0-9_]+)\b")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def git(root, *args):
    return subprocess.run(["git", "-C", root, *args], capture_output=True, text=True, check=True).stdout


def read(root, path):
    try:
        with open(os.path.join(root, path), errors="replace") as f:
            return f.read()
    except OSError:
        return ""


# ── the tree ──

def switches(root):
    """The switches of config/features.mk, in order."""
    text = read(root, os.path.join("config", "features.mk"))
    m = re.search(r"^FEATURES\s*:?=((?:.*\\\n)*.*)", text, re.M)
    return m.group(1).replace("\\\n", " ").split() if m else []


def board_info(root):
    """id -> {platform, dirs, boot, ldfiles} from each boards/<b>/board.mk (and config.mk) as text."""
    out = {}
    for b in sorted(os.listdir(os.path.join(root, "boards"))):
        mk = read(root, os.path.join("boards", b, "board.mk"))
        if not mk:
            continue
        text = (mk + "\n" + read(root, os.path.join("boards", b, "config.mk"))).replace("\\\n", " ")
        plat = re.search(r"^PLATFORM\s*:?=\s*(\S+)", text, re.M)
        boot = re.search(r"^BOOT_STAGE\s*:?=\s*(\S+)", text, re.M)
        dirs = {f"boards/{b}"}
        for m in re.finditer(r"^[A-Z0-9_]*DIRS\s*[:+]?=(.*)$", text, re.M):
            dirs.update(t for t in m.group(1).split() if "/" in t and "$" not in t)
        out[b] = {"platform": plat.group(1) if plat else "", "dirs": sorted(dirs),
                  "boot": boot.group(1) if boot else ""}
    return out


def makefile_switch_paths(root):
    """path -> switches the Makefile builds it for (inside an ifeq / ifneq on the switch, or a -$(CONFIG_X) line)."""
    out = {}
    stack = []
    for line in read(root, "Makefile").replace("\\\n", " ").splitlines():
        s = line.strip()
        if re.match(r"if(n?eq|n?def)\b", s):
            stack.append(CONFIG_RE.findall(s))
            continue
        if s == "endif" and stack:
            stack.pop()
            continue
        sws = [x for frame in stack for x in frame] + re.findall(r"-\$\(CONFIG_([A-Z0-9_]+)\)", s)
        if not sws:
            continue
        for tok in re.findall(r"[A-Za-z0-9_./-]+", s):
            if "/" in tok and tok.split("/")[0] in SRC_TOPS:
                out.setdefault(tok.rstrip("/"), set()).update(sws)
    return out


def under(path, d):
    return path == d or path.startswith(d.rstrip("/") + "/")


def header_index(root):
    """basename -> paths of every header under the source tops (a quoted include is looked up by its last parts)."""
    idx = {}
    for top in SRC_TOPS:
        for dp, _, fns in os.walk(os.path.join(root, top)):
            for fn in fns:
                if fn.endswith(".h"):
                    idx.setdefault(fn, []).append(os.path.relpath(os.path.join(dp, fn), root))
    return idx


# ── the diff ──

def changed(root, base, head):
    """{path: {"new": changed line numbers in head, "old": removed line numbers in base, "added": [text],
    "removed": [text]}} of base...head (head None: the working tree)."""
    rng = [f"{base}...{head}"] if head else [base]
    out = {}
    cur = None
    for ln in git(root, "diff", "-U0", "--no-color", "--no-renames", "--ignore-submodules=all", *rng).splitlines():
        if ln.startswith("diff --git "):
            cur = None
        elif ln.startswith("+++ "):
            path = ln[4:]
            if path == "/dev/null":
                continue
            cur = out.setdefault(path[2:], {"new": set(), "old": set(), "added": [], "removed": []})
        elif ln.startswith("--- "):
            path = ln[4:]
            if path != "/dev/null":
                cur = out.setdefault(path[2:], {"new": set(), "old": set(), "added": [], "removed": []})
        elif ln.startswith("@@") and cur is not None:
            m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", ln)
            os_, oc, ns, nc = int(m.group(1)), int(m.group(2) or 1), int(m.group(3)), int(m.group(4) or 1)
            cur["old"].update(range(os_, os_ + oc))
            # a pure deletion still sits between two new lines: count the line after it
            cur["new"].update(range(ns, ns + nc) if nc else (ns, ns + 1))
        elif cur is not None and ln[:1] == "+":
            cur["added"].append(ln[1:])
        elif cur is not None and ln[:1] == "-":
            cur["removed"].append(ln[1:])
    for f in git(root, "diff", "--name-only", "--ignore-submodules=all", *rng).split("\n"):
        if f:
            out.setdefault(f, {"new": set(), "old": set(), "added": [], "removed": []})
    return out


def text_at(root, rev, path):
    """The file at rev (None: the working tree), or empty."""
    if rev is None:
        return read(root, path)
    r = subprocess.run(["git", "-C", root, "show", f"{rev}:{path}"], capture_output=True, text=True, errors="replace")
    return r.stdout if r.returncode == 0 else ""


COND_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
IDENT_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def guards(text):
    """Per line (1-based index), the CONFIG_ switches of the #if conditions around it."""
    stack, out = [], [set()]
    for line in text.splitlines():
        m = COND_RE.match(line)
        if m:
            kind, rest = m.groups()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(set(CONFIG_RE.findall(rest)))
            elif kind == "elif" and stack:
                stack[-1] |= set(CONFIG_RE.findall(rest))
            elif kind == "endif" and stack:
                stack.pop()
        out.append(set().union(*stack) if stack else set())
    return out


def guarded_names(text):
    """identifier -> switches, for identifiers a header has only inside CONFIG_ guards (its declarations under a
    switch)."""
    g = guards(text)
    inside, outside = {}, set()
    for i, line in enumerate(text.splitlines(), 1):
        if COND_RE.match(line):
            continue
        for ident in IDENT_RE.findall(line):
            if g[i]:
                inside.setdefault(ident, set()).update(g[i])
            else:
                outside.add(ident)
    return {k: v for k, v in inside.items() if k not in outside and not k.startswith("CONFIG_")}


def is_source(path):
    return path in SHARED_FILES or (path.split("/")[0] in SRC_TOPS and path.endswith(SRC_EXT))


def file_switches(root, f, d, base, head, scope, hdrs):
    """The switches one changed file can break. precise: those named in its changed lines, the guards around them (in
    head and, for removed lines, in base), the guarded declarations of its quoted includes that the added lines use,
    and the Makefile's switch for the file. broad: every switch the file and those includes name."""
    lines = d["added"] + d["removed"]
    sws = set(CONFIG_RE.findall("\n".join(lines)))
    new, old = text_at(root, head, f), text_at(root, base, f)
    if f.endswith((".c", ".h", ".S", ".s")):
        for text, nums in ((new, d["new"]), (old, d["old"])):
            g = guards(text)
            for n in nums:
                if n < len(g):
                    sws |= g[n]
        used = set(IDENT_RE.findall("\n".join(d["added"])))
        for inc in INCLUDE_RE.findall(new):
            if os.path.basename(inc) == "config.h":
                continue
            for h in hdrs.get(os.path.basename(inc), []):
                if h.endswith(inc):
                    htext = read(root, h)
                    if scope == "broad":
                        sws |= set(CONFIG_RE.findall(htext))
                    else:
                        for ident, hs in guarded_names(htext).items():
                            if ident in used:
                                sws |= hs
    if scope == "broad" and not f.endswith("config.h"):
        sws |= set(CONFIG_RE.findall(new))
    for p, s in makefile_switch_paths(root).items():
        if under(f, p):
            sws |= s
    return sws


def analyze(root, diff, only=None, scope="precise", base=None, head=None):
    """(boards, {board: {switch: [why]}} from board-owned files, {switch: [why]} from shared code, {board: [why]} for
    fm-minimal). The board list, the Makefile and included headers are read from the checkout."""
    known = set(switches(root))
    boards = board_info(root)
    if only:
        boards = {b: v for b, v in boards.items() if b in only}
    hdrs = header_index(root)
    reach, shared, fm = {}, {}, {}
    for f, d in sorted(diff.items()):
        if not is_source(f):
            continue
        own = [b for b, v in boards.items() if any(under(f, x) for x in v["dirs"])]
        top = f.split("/")[0]
        if top == "boot":
            own += [b for b, v in boards.items() if v["boot"] and under(f, f"boot/{v['boot']}")]
        is_shared = False
        if top == "platform" and not own:
            plat = f.split("/")[1] if "/" in f else ""
            own = [b for b, v in boards.items() if v["platform"] == plat]
            for b in own:
                fm.setdefault(b, []).append(f)
        elif not own:
            is_shared = True
        if f.endswith((".ld", ".mk")) or top == "boot":
            for b in own:
                fm.setdefault(b, []).append(f)
        for s in file_switches(root, f, d, base, head, scope, hdrs) & known:
            if is_shared:
                shared.setdefault(s, []).append(f)
            else:
                for b in own:
                    reach.setdefault(b, {}).setdefault(s, []).append(f)
    return boards, reach, shared, fm


def board_config(root, b):
    """(switches on by default, switches the board can't build without)"""
    def words(goal):
        r = subprocess.run(["make", "-s", "-C", root, "BOARD=" + b, goal], capture_output=True, text=True)
        return r.stdout.split()
    on = {kv.split("=")[0] for kv in words("print-config") if kv.endswith("=y")}
    return on, set(words("print-required"))


def plan(root, diff, only=None, config=board_config, scope="precise", base=None, head=None):
    """[(board, name, make args, why)] most relevant first: the board's own switches, fm-minimal for a platform,
    linker or boot change, shared switches by how many files name them, then fm-minimal for DMR."""
    boards, reach, shared, fm = analyze(root, diff, only, scope, base, head)
    cfg = {b: config(root, b) for b in boards}
    reps = {}
    for b in sorted(boards):
        p = boards[b]["platform"]
        if p not in reps or len(cfg[b][0]) > len(cfg[reps[p]][0]):
            reps[p] = b
    rep_boards = sorted(reps.values())
    want = {}
    for b, sws in reach.items():
        for s, why in sws.items():
            want.setdefault((b, s), (0, why))
    for s, why in sorted(shared.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        for b in rep_boards:
            want.setdefault((b, s), (1 + 1 / (1 + len(why)), why))
        if s == "DMR":
            for b in rep_boards:
                fm.setdefault(b, []).append("DMR in shared code")
    out = []
    for (b, s), (rank, why) in sorted(want.items(), key=lambda kv: (kv[1][0], kv[0][1], kv[0][0])):
        on, required = cfg[b]
        if s in on and s not in required:
            out.append((rank, b, f"no-{s}", f"CONFIG_{s}=n", why))
    for b in sorted(fm):
        if "DMR" in cfg[b][1]:      # a board that can't build without DMR has no fm-minimal
            continue
        # a platform, linker script or boot stage change: right after the board's own switches; DMR alone: last
        direct = any(w != "DMR in shared code" for w in fm[b])
        out.append((0.5 if direct else 9, b, "fm-minimal", FM_MINIMAL, fm[b]))
    seen = set()
    res = []
    for _, b, name, args, why in sorted(out, key=lambda r: r[0]):
        if (b, name) not in seen:
            seen.add((b, name))
            res.append((b, name, args, why))
    return res


# ── commands ──

def resolve_base(a):
    """The commit the change starts from: base...head's merge base, so commits base gained since don't count (the old
    side of a removed line is read there too)."""
    a.base = git(ROOT, "merge-base", a.base, a.head or "HEAD").strip()


def cmd_builds(a):
    rows = plan(ROOT, changed(ROOT, a.base, a.head), a.board, scope=a.scope, base=a.base, head=a.head)
    total = len(rows)
    if a.limit:
        rows = rows[:a.limit]
    for b, name, args, _ in rows:
        print(b, name, args)
    if a.limit and total > a.limit:
        print(f"affected: {total - a.limit} more left to the nightly matrix (limit {a.limit})", file=sys.stderr)


def cmd_summary(a):
    diff = changed(ROOT, a.base, a.head)
    rows = plan(ROOT, diff, a.board, scope=a.scope, base=a.base, head=a.head)
    src = [f for f in diff if is_source(f)]
    print(f"affected ({a.scope}): {len(diff)} files changed, {len(src)} firmware sources, {len(rows)} builds")
    for b, name, _, why in rows:
        more = f" (+{len(why) - 3})" if len(why) > 3 else ""
        print(f"  {b:12} {name:28} {', '.join(why[:3])}{more}")


def self_test():
    with tempfile.TemporaryDirectory() as t:
        def w(p, s):
            os.makedirs(os.path.dirname(os.path.join(t, p)), exist_ok=True)
            with open(os.path.join(t, p), "w") as f:
                f.write(s)

        def sh(*a):
            subprocess.run(a, cwd=t, check=True, capture_output=True)
        w("config/features.mk", "FEATURES := FM DMR DMR_SMS CHANNEL_EDIT GPS\n")
        w("Makefile", "ifeq ($(CONFIG_GPS),y)\nCORE_SRC += core/gps/gps.c\nendif\n")
        w("boards/a/board.mk", "PLATFORM := p1\nBOARD_DIRS := boards/a boards/fam drivers/rf/x \\\n\tdrivers/modem/m\n")
        w("boards/b/board.mk", "PLATFORM := p1\nBOARD_DIRS := boards/b boards/fam\n")
        w("boards/c/board.mk", "PLATFORM := p2\nBOARD_DIRS := boards/c drivers/rf/x\nBOOT_STAGE := c\n")
        w("core/dmr/contact.h", "#if CONFIG_DMR\ntypedef int contact_t;\n#endif\nint shared_fn(void);\n")
        storage = ('#include "config.h"\n#include "dmr/contact.h"\nint a;\n'
                   "#if CONFIG_CHANNEL_EDIT\nint e;\n#endif\n#if CONFIG_GPS\nint g;\n#endif\nint z;\n")
        w("core/ui/storage.c", storage)
        w("core/gps/gps.c", "int g;\n")
        w("drivers/rf/x/x.c", "#if CONFIG_DMR_SMS\nint s;\n#endif\n")
        w("boards/fam/f.c", "#if CONFIG_FM\nint f;\n#endif\n")
        w("platform/p2/system.c", "int s;\n")
        w("boot/c/boot.c", "int s;\n")
        w("docs/x.md", "x\n")
        sh("git", "init", "-q")
        sh("git", "add", "-A")
        sh("git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base")
        cfg = {"a": ({"FM", "DMR", "DMR_SMS", "CHANNEL_EDIT", "GPS"}, set()),
               "b": ({"FM", "DMR", "DMR_SMS", "CHANNEL_EDIT"}, set()),
               "c": ({"FM", "DMR", "DMR_SMS", "CHANNEL_EDIT", "GPS"}, {"FM"})}
        config = lambda root, b: cfg[b]   # noqa: E731

        def names(edits, only=None, scope="precise"):
            for path, text in edits.items():
                w(path, text)
            try:
                return {(b, n) for b, n, _, _ in plan(t, changed(t, "HEAD", None), only, config, scope, "HEAD")}
            finally:
                sh("git", "checkout", "-q", "--", ".")
        fm_ac = {("a", "fm-minimal"), ("c", "fm-minimal")}
        checks = [
            # an unguarded use of a type a header declares only under CONFIG_DMR: no-DMR on one board per platform
            # (a has more switches on than b) and fm-minimal there, but not the file's other switches
            ("use of a guarded declaration", names({"core/ui/storage.c": storage.replace("int a;", "contact_t a;")}),
             {("a", "no-DMR"), ("c", "no-DMR")} | fm_ac),
            ("use of an unguarded declaration",
             names({"core/ui/storage.c": storage.replace("int a;", "int a = shared_fn();")}), set()),
            ("change inside a guard", names({"core/ui/storage.c": storage.replace("int e;", "int e2;")}),
             {("a", "no-CHANNEL_EDIT"), ("c", "no-CHANNEL_EDIT")}),
            ("guard removed", names({"core/ui/storage.c": storage.replace("#if CONFIG_GPS\nint g;\n#endif\n", "int g;\n")}),
             {("a", "no-GPS"), ("c", "no-GPS")}),
            ("unguarded change, broad scope", names({"core/ui/storage.c": storage.replace("int z;", "int z2;")},
                                                     scope="broad"),
             {("a", "no-DMR"), ("c", "no-DMR"), ("a", "no-CHANNEL_EDIT"), ("c", "no-CHANNEL_EDIT"),
              ("a", "no-GPS"), ("c", "no-GPS")} | fm_ac),
            ("unguarded change, precise scope", names({"core/ui/storage.c": storage.replace("int z;", "int z2;")}),
             set()),
            ("Makefile-built file", names({"core/gps/gps.c": "int g2;\n"}), {("a", "no-GPS"), ("c", "no-GPS")}),
            ("driver two boards list", names({"drivers/rf/x/x.c": "#if CONFIG_DMR_SMS\nint s2;\n#endif\n"}),
             {("a", "no-DMR_SMS"), ("c", "no-DMR_SMS")}),
            # c requires FM, and isn't on this family
            ("family dir", names({"boards/fam/f.c": "#if CONFIG_FM\nint f2;\n#endif\n"}), {("a", "no-FM"), ("b", "no-FM")}),
            ("platform file", names({"platform/p2/system.c": "int s2;\n"}), {("c", "fm-minimal")}),
            ("boot stage", names({"boot/c/boot.c": "int s2;\n"}), {("c", "fm-minimal")}),
            ("docs only", names({"docs/x.md": "y\n"}), set()),
            ("--board", names({"core/gps/gps.c": "int g2;\n"}, only={"b"}), set()),
        ]
        bad = 0
        for what, got, exp in checks:
            ok = got == exp
            bad += not ok
            print(("ok   " if ok else "FAIL ") + what + ("" if ok else f": got {sorted(got)}, expected {sorted(exp)}"))
    return 1 if bad else 0


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv[:1] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("builds", "summary"):
        p = sub.add_parser(name)
        p.add_argument("--base", required=True, help="the commit to compare with (main, HEAD^1)")
        p.add_argument("--head", help="the commit with the change (default: the working tree)")
        p.add_argument("--board", type=lambda s: set(filter(None, s.split(","))), help="only these boards")
        p.add_argument("--scope", choices=("precise", "broad"), default="precise",
                       help="precise (a pull request): the changed lines' switches; broad (the merge queue): every "
                            "switch a changed file names")
        if name == "builds":
            p.add_argument("--limit", type=int, default=0, help="at most N builds (0: all)")
    a = ap.parse_args(argv)
    try:
        resolve_base(a)
        {"builds": cmd_builds, "summary": cmd_summary}[a.cmd](a)
    except subprocess.CalledProcessError as e:
        print(f"affected: {' '.join(e.cmd)}: {e.stderr.strip()}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
