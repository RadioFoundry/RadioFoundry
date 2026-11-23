#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""What a change can break: the switch-off builds a diff needs (docs/dev/ci.md "Affected builds").

    tools/affected.py builds --base REF [--head REF] [--board a,b] [--limit N]
        one "board name make-args" line per build, the same names tools/ci.sh's matrix uses (no-<SWITCH>,
        fm-minimal, a board.toml [ci] extra), most relevant first; --limit keeps the first N. "board matrix" (no make
        arguments) asks for that board's whole matrix: the fallback when the tool can't tell (below)
    tools/affected.py summary --base REF [--head REF] [--board a,b]
        the boards and switches the diff reaches, and why
    tools/affected.py boards --base REF [--head REF]
        the boards the diff reaches, one a line: the radios a HIL run of it needs (tools/hil_batch.sh)
    tools/affected.py features --base REF [--head REF]
        the HIL checklist features (tests/hil/features.toml) the diff reaches, and why: what
        pytest tests/hil --affected REF runs besides the smoke tier
    tools/affected.py --self-test

The full matrix builds each of about a hundred switches off on every board. A pull request breaks only the switches
its code sits under, so this picks them from the diff:

  boards    read through make (tools/boards.py make_vars(): board.mk, config.mk and their conditionals, as the build
            sees them), one make per board and none for a diff without firmware sources. A file reaches the boards
            whose BOARD_DIRS (with BOARD_USB_DIRS, BOARD_NAND_DIRS and the platform's PLATFORM_DIRS outside
            platform/) list its directory, the deepest listed directory deciding; boot/<stage>/ the boards with that
            stage; platform/<p>/ that no board lists every board on the platform; any other source (core/, hal/,
            config/, drivers/ no board lists, the Makefile) every board
  switches  every CONFIG_<X> named in a changed line (a guard added, moved or removed), in the #if conditions around a
            changed line (in head and, for a removed line, in base), or in a macro such a condition uses that is
            defined from a switch or under one (#if HAS_FOO, HAS_FOO defined in a CONFIG_X block); the switches of the
            declarations a changed line uses that exist only under a switch, in the headers the file reaches through
            quoted includes (transitively, each resolved as the compiler does: the file's own directory, then the
            board's -I directories in order); a file the Makefile builds only for a switch (ifeq on it) adds that
            switch. A board's own switch (a hardware revision's, not in config/features.mk) selects the board.toml
            [ci] extra that turns it off
  builds    a switch from a board's own files is built off on that board; a switch from shared code on one board per
            platform (the one with the most switches on), since the platforms differ in glue and linker scripts.
            fm-minimal on every board whose platform, board, boot stage or linker script changed, and on the
            per-platform board when DMR is among the switches. Only switches on by default and not required by the
            board (BOARD_REQUIRED) are built off, as in the matrix

The C side is a heuristic, by regular expressions over the source text (comments stripped, continued directives
joined): no compiler. The selection runs before any build and where none can run: on the HIL host for pytest
--affected (tests/hil/hil_plugin.py) and in tools/hil_batch.sh, without the boards' cross compilers or third_party.
gcc -MMD or gcc -E per board would be exact for the includes and the guards but need both, and whether a new line
uses a name only a switch declares can't be known without building it anyway. What the heuristic misses: a guard
through a macro defined in a .c file other than the changed one or outside the source tops, a conditional include,
and a name the preprocessor builds (token pasting).

When it can't tell it says so: a firmware file that maps to no board (a platform no board uses, a board or boot
directory no board.mk names) or a board make fails on prints a warning (stderr, "affected: warning:") and selects the
full matrix ("board matrix") for every board, or for the board make failed on, instead of fewer builds.

Files outside the firmware sources (docs, site, tools, tests) reach nothing. The nightly matrix stays the backstop.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boards as registry  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_TOPS = ("core", "hal", "drivers", "platform", "boards", "boot", "config")
SRC_EXT = (".c", ".h", ".S", ".s", ".ld", ".mk")
SHARED_FILES = ("Makefile",)
# tools/ci.sh's fm-minimal configuration
FM_MINIMAL = "CONFIG_DMR=n CONFIG_USB=n CONFIG_VFO=n CONFIG_DIAG=n CONFIG_SUBTONE=n CONFIG_FONT_XL=n"
CONFIG_RE = registry.CONFIG_RE
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
# what make tells about each board (with CONFIG_<s> for every switch)
BOARD_VARS = ("PLATFORM", "BOOT_STAGE", "BOARD_DIRS", "PLATFORM_DIRS", "BOARD_USB_DIRS", "BOARD_REQUIRED", "INCLUDES")
# the Makefile's own -I directories, for a file when no board's are known
DEFAULT_INCLUDES = ("hal", "core", "drivers")


def warn_stderr(msg):
    print(f"affected: warning: {msg}", file=sys.stderr)


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


def board_extras(root, b):
    """[(name, make args)] of boards/<b>/board.toml [ci] extra: the configurations only that board builds."""
    path = os.path.join(root, "boards", b, "board.toml")
    if not os.path.isfile(path):
        return []
    return [(e["name"], list(e["make"])) for e in registry._read_toml(path).get("ci", {}).get("extra", [])]


def board_facts(root, sws):
    """({id: facts}, {id: error}) for every boards/<b>/board.mk, read through make (tools/boards.py make_vars(), one
    make per board): platform, dirs (the source directories the board builds besides the platform's own), boot,
    on (the switches on by default), required (BOARD_REQUIRED), includes (its -I directories in the tree, in order) and
    extra (board.toml [ci] extra). A board make fails on goes in the second map."""
    facts, failed = {}, {}
    for b in registry.board_dirs(root):
        try:
            mk = registry.make_vars(root, b, BOARD_VARS + tuple("CONFIG_" + s for s in sws))
            extra = board_extras(root, b)
        except registry.RegistryError as e:
            failed[b] = str(e)
            continue
        # PLATFORM_DIRS under platform/ are the platform rule's (fm-minimal on its boards); what else the platform
        # builds (a GPS driver) it owns as a board does. BOARD_NAND_DIRS is in BOARD_DIRS already (the Makefile adds it)
        dirs = {f"boards/{b}"} | set(mk.get("BOARD_DIRS", "").split()) | set(mk.get("BOARD_USB_DIRS", "").split())
        dirs |= {d for d in mk.get("PLATFORM_DIRS", "").split() if d.split("/")[0] != "platform"}
        incs = [t[2:].rstrip("/") for t in mk.get("INCLUDES", "").split()
                if t.startswith("-I") and t[2:].split("/")[0] in SRC_TOPS]
        facts[b] = {"platform": mk.get("PLATFORM", ""), "boot": mk.get("BOOT_STAGE", ""),
                    "dirs": sorted(d.rstrip("/") for d in dirs if "/" in d and d.split("/")[0] in SRC_TOPS),
                    "on": {s for s in sws if mk.get("CONFIG_" + s) == "y"},
                    "required": set(mk.get("BOARD_REQUIRED", "").split()), "includes": incs, "extra": extra}
    return facts, failed


def under(path, d):
    return path == d or path.startswith(d.rstrip("/") + "/")


def owners(f, facts):
    """(boards, rule) a source file reaches. rule: "own" (the boards listing its directory, the deepest listed one
    deciding, or using its boot stage), "platform" (platform/<p>/ no board lists: every board on p), "shared" (core/,
    hal/, config/, drivers/ no board lists, the Makefile: every board) or "none" (a platform, board or boot directory
    no board uses)."""
    top = f.split("/")[0]
    best, own = 0, set()
    for b, v in facts.items():
        for d in v["dirs"]:
            if under(f, d) and len(d) >= best:
                own = ({b} | own) if len(d) == best else {b}
                best = len(d)
    if top == "boot":
        own |= {b for b, v in facts.items() if v["boot"] and under(f, f"boot/{v['boot']}")}
    if own:
        return own, "own"
    if top == "platform":
        plat = f.split("/")[1] if "/" in f else ""
        own = {b for b, v in facts.items() if v["platform"] == plat}
        return own, ("platform" if own else "none")
    if top in ("boards", "boot"):
        return set(), "none"
    return set(facts), "shared"


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
    if not head:
        # the working tree: a file not yet added is a change too, every line of it new
        for f in git(root, "ls-files", "--others", "--exclude-standard").split("\n"):
            if f and f not in out:
                lines = read(root, f).split("\n")
                out[f] = {"new": set(range(1, len(lines) + 1)), "old": set(), "added": lines, "removed": []}
    return out


def text_at(root, rev, path):
    """The file at rev (None: the working tree), or empty."""
    if rev is None:
        return read(root, path)
    r = subprocess.run(["git", "-C", root, "show", f"{rev}:{path}"], capture_output=True, text=True, errors="replace")
    return r.stdout if r.returncode == 0 else ""


# ── the C side (a heuristic: the module's docstring says what it misses, and why no compiler) ──

COND_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b(.*)$")
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?:\([^)]*\))?(.*)$")
IDENT_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
# never a declaration under a switch: C's keywords and the standard headers' types and constants
C_WORDS = set("""auto break case char const continue default do double else enum extern float for goto if inline int long
register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while _Bool
_Static_assert static_assert bool true false NULL size_t ssize_t uintptr_t intptr_t int8_t int16_t int32_t int64_t
uint8_t uint16_t uint32_t uint64_t""".split())
# strings and character constants kept (an include's name), comments replaced by their newlines
COMMENT_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.S)


def strip_comments(text):
    """The text without its comments, line for line (a block comment leaves its newlines)."""
    def sub(m):
        t = m.group(0)
        return t if t[0] in "\"'" else ("\n" * t.count("\n") or " ")
    return COMMENT_RE.sub(sub, text)


def directives(lines):
    """(first index, last index, the line) for each line, a directive continued with a backslash joined into one."""
    i = 0
    while i < len(lines):
        j = i
        if lines[i].lstrip().startswith("#"):
            while lines[j].endswith("\\") and j + 1 < len(lines):
                j += 1
        yield i, j, " ".join(x[:-1] if x.endswith("\\") else x for x in lines[i:j + 1])
        i = j + 1


def cond_switches(cond, derived):
    """The switches a #if condition tests: named, or through a macro derived from them."""
    out = set(CONFIG_RE.findall(cond))
    for ident in IDENT_RE.findall(cond):
        out |= derived.get(ident, set())
    return out


def guards(text, derived=None):
    """Per line (1-based index), the switches of the #if conditions around it. Comments are stripped first (a #if in
    a block comment doesn't count) and a condition continued with a backslash is read whole."""
    derived = derived or {}
    lines = strip_comments(text).split("\n")
    stack, out = [], [set()]
    for i, j, line in directives(lines):
        m = COND_RE.match(line)
        if m:
            kind, rest = m.groups()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(cond_switches(rest, derived))
            elif kind.startswith("elif") and stack:
                stack[-1] |= cond_switches(rest, derived)
            elif kind == "endif" and stack:
                stack.pop()
        out.extend([set().union(*stack) if stack else set()] * (j - i + 1))
    return out


def derived_macros(texts):
    """macro -> switches, for the macros whose value comes from switches: a body that names CONFIG_<X> or such a
    macro, or a #define inside a block guarded by one. texts: the C texts to read (the tree's headers and the changed
    files)."""
    derived = {}
    for _ in range(2):      # the second pass sees the defines under a guard on a derived macro
        raw = {}
        for text in texts:
            g = guards(text, derived)
            lines = strip_comments(text).split("\n")
            for i, _, line in directives(lines):
                m = DEFINE_RE.match(line)
                if not m or m.group(1).startswith("CONFIG_"):
                    continue
                e = raw.setdefault(m.group(1), [set(), set()])
                e[0] |= set(CONFIG_RE.findall(m.group(2))) | g[i + 1]
                e[1] |= set(IDENT_RE.findall(m.group(2))) - {m.group(1)}
        new = {k: set(v[0]) for k, v in raw.items()}
        grew = True
        while grew:
            grew = False
            for k, (_, refs) in raw.items():
                for r in refs:
                    if r in new and not new[r] <= new[k]:
                        new[k] |= new[r]
                        grew = True
        derived = {k: v for k, v in new.items() if v}
    return derived


def guarded_names(text, derived=None):
    """(identifier -> switches for the identifiers that appear only inside CONFIG_ guards, every identifier outside
    them) of one text: its declarations under a switch."""
    g = guards(text, derived)
    inside, outside = {}, set()
    for i, j, line in directives(strip_comments(text).split("\n")):
        if COND_RE.match(line):
            continue
        for ident in IDENT_RE.findall(line):
            if g[i + 1]:
                inside.setdefault(ident, set()).update(g[i + 1])
            else:
                outside.add(ident)
    return {k: v for k, v in inside.items()
            if k not in outside and k not in C_WORDS and not k.startswith("CONFIG_")}, outside


class CTree:
    """The tree's headers, read once, and the macros derived from switches; quoted includes resolved per -I list."""

    def __init__(self, root, extra_texts=()):
        self.root = root
        self.texts = {}
        self.names = {}
        self.extra_texts = list(extra_texts)
        self._derived = None

    def text(self, path):
        if path not in self.texts:
            self.texts[path] = read(self.root, path)
        return self.texts[path]

    def derived(self):
        if self._derived is None:
            hdrs = []
            for top in SRC_TOPS:
                for dp, _, fns in os.walk(os.path.join(self.root, top)):
                    hdrs += [os.path.relpath(os.path.join(dp, fn), self.root) for fn in fns if fn.endswith(".h")]
            self._derived = derived_macros([self.text(h) for h in sorted(hdrs)] + self.extra_texts)
        return self._derived

    def guarded(self, path):
        if path not in self.names:
            self.names[path] = guarded_names(self.text(path), self.derived())
        return self.names[path]

    def resolve(self, includer, inc, incdirs):
        """The header a quoted include names: the including file's directory first, then the -I directories in
        order, as the compiler looks; None when it isn't in the tree (a generated or third_party header)."""
        for d in (os.path.dirname(includer),) + tuple(incdirs):
            p = os.path.normpath(os.path.join(d, inc))
            if os.path.isfile(os.path.join(self.root, p)):
                return p
        return None

    def includes(self, f, text, incdirs, depth=None):
        """The headers f reaches through quoted includes, breadth first (depth 1: its own includes only).
        core/config.h, which names every switch, is left out."""
        seen, order, todo = set(), [], [(f, text, 0)]
        while todo:
            src, t, lvl = todo.pop(0)
            if depth is not None and lvl >= depth:
                continue
            for inc in INCLUDE_RE.findall(strip_comments(t)):
                if os.path.basename(inc) == "config.h":
                    continue
                h = self.resolve(src, inc, incdirs)
                if h and h not in seen:
                    seen.add(h)
                    order.append(h)
                    todo.append((h, self.text(h), lvl + 1))
        return order


def is_source(path):
    return path in SHARED_FILES or (path.split("/")[0] in SRC_TOPS and path.endswith(SRC_EXT))


def file_switches(root, f, d, base, head, scope, ctree, incsets, mk):
    """(switches, {include list: switches}) one changed file can break; the second part depends on the -I list the
    file's includes resolve through (a board's). precise: those named in its changed lines and in the conditions of
    the changed directives, the guards around the changed lines (in head and, for removed lines, in base), the
    guarded declarations of the headers it reaches that the added lines use, and the Makefile's switch for the file.
    broad: every switch the file and its own includes name."""
    lines = d["added"] + d["removed"]
    sws = set(CONFIG_RE.findall("\n".join(lines)))
    per = {}
    new, old = text_at(root, head, f), text_at(root, base, f)
    if f.endswith((".c", ".h", ".S", ".s")):
        derived = ctree.derived()
        for ln in lines:
            if COND_RE.match(ln):
                sws |= cond_switches(ln, derived)
        for text, nums in ((new, d["new"]), (old, d["old"])):
            g = guards(text, derived)
            for n in nums:
                if n < len(g):
                    sws |= g[n]
        used = set(IDENT_RE.findall(strip_comments("\n".join(d["added"]))))
        # the file's own unguarded names outside the changed lines: a local declaration hides a header's
        own = set()
        g = guards(new, derived)
        for n, line in enumerate(strip_comments(new).split("\n"), 1):
            if n not in d["new"] and not (g[n] if n < len(g) else set()):
                own |= set(IDENT_RE.findall(line))
        for incs in incsets:
            got = set()
            if scope == "broad":
                for h in ctree.includes(f, new, incs, depth=1):
                    got |= set(CONFIG_RE.findall(ctree.text(h)))
            else:
                inside, outside = {}, set(own)
                for h in ctree.includes(f, new, incs):
                    gi, go = ctree.guarded(h)
                    outside |= go
                    for k, v in gi.items():
                        inside.setdefault(k, set()).update(v)
                for ident in used - outside:
                    got |= inside.get(ident, set())
            per[incs] = got
    if scope == "broad" and not f.endswith("config.h"):
        sws |= set(CONFIG_RE.findall(new))
    for p, s in mk.items():
        if under(f, p):
            sws |= s
    return sws, per


def analyze(root, diff, only=None, scope="precise", base=None, head=None, warn=warn_stderr, want_switches=True):
    """What the diff's firmware sources reach, as a namespace: facts and failed (board_facts()), boards (the facts of
    the selected ones), reach {board: {switch: [why]}} from board-owned files, shared {switch: [why]} from shared code,
    fm {board: [why]} for fm-minimal, extra {board: {name: (make args, [why])}} for a board's own switch, reached (the
    boards the files reach), unmapped (the files that map to no board) and matrix {board: [why]}: the boards that get
    their full matrix because the tool can't tell (each one warned about). No make for a diff without sources."""
    a = SimpleNamespace(facts={}, failed={}, boards={}, reach={}, shared={}, fm={}, extra={}, reached=set(),
                        unmapped=[], matrix={})
    src = [f for f in sorted(diff) if is_source(f)]
    if not src:
        return a
    sws_all = switches(root)
    known = set(sws_all)
    a.facts, a.failed = board_facts(root, sws_all)
    sel = (lambda b: not only or b in only)
    a.boards = {b: v for b, v in a.facts.items() if sel(b)}
    for b, err in sorted(a.failed.items()):
        if sel(b):
            warn(f"{b}: make failed ({err}): its full matrix")
            a.matrix.setdefault(b, []).append("make failed")
    mk = registry.makefile_switch_paths(root)
    c_files = [f for f in src if f.endswith((".c", ".h", ".S", ".s"))]
    ctree = CTree(root, [text_at(root, head, f) for f in c_files] + [text_at(root, base, f) for f in c_files])
    every_inc = {tuple(v["includes"]) for v in a.facts.values()} or {DEFAULT_INCLUDES}
    for f in src:
        d = diff[f]
        own, rule = owners(f, a.facts)
        if rule == "none":
            a.unmapped.append(f)
            warn(f"{f} maps to no board: every board's full matrix")
            for b in list(a.facts) + list(a.failed):
                if sel(b):
                    a.matrix.setdefault(b, []).append(f)
        a.reached |= own
        top = f.split("/")[0]
        if rule == "platform" or (rule == "own" and (f.endswith((".ld", ".mk")) or top == "boot")):
            for b in own:
                if sel(b):
                    a.fm.setdefault(b, []).append(f)
        if not want_switches:
            continue
        incsets = {tuple(a.facts[b]["includes"]) for b in own if b in a.facts} or every_inc
        common, per = file_switches(root, f, d, base, head, scope, ctree, sorted(incsets), mk)
        if rule in ("shared", "none"):
            for s in (common | set().union(*per.values())) & known:
                a.shared.setdefault(s, []).append(f)
            continue
        for b in sorted(own):
            if not sel(b):
                continue
            mine = common | per.get(tuple(a.facts[b]["includes"]), set())
            for s in mine & known:
                a.reach.setdefault(b, {}).setdefault(s, []).append(f)
            # a board's own switch (not in config/features.mk): the [ci] extra that turns it off
            for name, args in a.facts[b]["extra"]:
                if any(f"CONFIG_{s}=n" in args for s in mine - known):
                    a.extra.setdefault(b, {}).setdefault(name, (args, []))[1].append(f)
    return a


def plan(root, diff, only=None, scope="precise", base=None, head=None, warn=warn_stderr):
    """[(board, name, make args, why)] most relevant first: the boards whose full matrix the tool falls back to
    ("matrix"), the board's own switches and extras, fm-minimal for a platform, linker or boot change, shared switches
    by how many files name them, then fm-minimal for DMR."""
    a = analyze(root, diff, only, scope, base, head, warn)
    cfg = a.boards
    reps = {}
    for b in sorted(cfg):
        p = cfg[b]["platform"]
        if p not in reps or len(cfg[b]["on"]) > len(cfg[reps[p]]["on"]):
            reps[p] = b
    rep_boards = sorted(reps.values())
    want = {}
    for b, sws in a.reach.items():
        for s, why in sws.items():
            want.setdefault((b, s), (0, why))
    for s, why in sorted(a.shared.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        for b in rep_boards:
            want.setdefault((b, s), (1 + 1 / (1 + len(why)), why))
        if s == "DMR":
            for b in rep_boards:
                a.fm.setdefault(b, []).append("DMR in shared code")
    out = []
    for (b, s), (rank, why) in sorted(want.items(), key=lambda kv: (kv[1][0], kv[0][1], kv[0][0])):
        if s in cfg[b]["on"] and s not in cfg[b]["required"]:
            out.append((rank, b, f"no-{s}", f"CONFIG_{s}=n", why))
    for b, ex in a.extra.items():
        for name, (args, why) in sorted(ex.items()):
            out.append((0, b, name, " ".join(args), why))
    for b in sorted(a.fm):
        if "DMR" in cfg[b]["required"]:      # a board that can't build without DMR has no fm-minimal
            continue
        # a platform, linker script or boot stage change: right after the board's own switches; DMR alone: last
        direct = any(w != "DMR in shared code" for w in a.fm[b])
        out.append((0.5 if direct else 9, b, "fm-minimal", FM_MINIMAL, a.fm[b]))
    res = [(b, "matrix", "", why) for b, why in sorted(a.matrix.items())]
    seen = set(a.matrix)
    for _, b, name, args, why in sorted(out, key=lambda r: r[0]):
        if b not in a.matrix and (b, name) not in seen:
            seen.add((b, name))
            res.append((b, name, args, why))
    return res


def reached_boards(root, diff, warn=warn_stderr):
    """The boards a diff's firmware sources reach (analyze()'s rule), plus every board when a shared HIL test changed
    and the board when its own (boards/<b>/hil) did: the radios a HIL run of the change needs. Every board when a
    file maps to no board or make fails on one."""
    a = analyze(root, diff, warn=warn, want_switches=False)
    every = set(registry.board_dirs(root))
    out = set(a.reached)
    if a.unmapped or a.failed:
        out |= every
    for f in diff:
        parts = f.split("/")
        if f.startswith("tests/hil/"):
            out |= every
        elif parts[0] == "boards" and len(parts) > 3 and parts[2] == "hil":
            out |= {parts[1]} & every
    return sorted(out)


# ── HIL features ──

FEATURES_TOML = os.path.join("tests", "hil", "features.toml")


def load_features(root):
    """id -> entry of the HIL feature checklist (tests/hil/features.toml)."""
    path = os.path.join(root, FEATURES_TOML)
    return registry._read_toml(path).get("feature", {}) if os.path.isfile(path) else {}


def affected_features(root, base, head=None, diff=None, warn=warn_stderr):
    """{feature id: [why]} of the checklist features a diff reaches (tests/hil/README.md, "Impact selection"): the
    feature's switch among the switches analyze() finds (named in a changed line, a guard around one, the Makefile's
    switch for the file), or a changed file under its source paths. A feature without `source` takes the default
    tools/bringup.py hashes (tools/boards.py default_source(): what the Makefile builds for its switch, else the files
    that name CONFIG_<switch>), so a change that makes a feature's record need a re-run also selects its tests."""
    if diff is None:
        diff = changed(root, base, head)
    a = analyze(root, diff, base=base, head=head, warn=warn)
    sws = {s: list(w) for s, w in a.shared.items()}
    for per in a.reach.values():
        for s, w in per.items():
            sws.setdefault(s, []).extend(w)
    out = {}
    for fid, e in sorted(load_features(root).items()):
        why = []
        sw = e.get("switch")
        if sw in sws:
            why.append(f"CONFIG_{sw}: " + ", ".join(sorted(set(sws[sw]))[:3]))
        paths = list(e.get("source", [])) or registry.default_source(root, sw)
        why += [f for f in sorted(diff) if any(under(f, p.rstrip("/")) for p in paths) and f not in why]
        if why:
            out[fid] = why
    return out


# ── commands ──

def resolve_base(a):
    """The commit the change starts from: base...head's merge base, so commits base gained since don't count (the old
    side of a removed line is read there too)."""
    a.base = git(ROOT, "merge-base", a.base, a.head or "HEAD").strip()


def cmd_builds(a):
    rows = plan(ROOT, changed(ROOT, a.base, a.head), a.board, scope=a.scope, base=a.base, head=a.head)
    # a fallback to a board's full matrix is never cut by the limit
    full = [r for r in rows if r[1] == "matrix"]
    rows = [r for r in rows if r[1] != "matrix"]
    total = len(rows)
    if a.limit:
        rows = rows[:a.limit]
    for b, name, args, _ in full + rows:
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


def cmd_boards(a):
    for b in reached_boards(ROOT, changed(ROOT, a.base, a.head)):
        print(b)


def cmd_features(a):
    for fid, why in affected_features(ROOT, a.base, a.head).items():
        more = f" (+{len(why) - 3})" if len(why) > 3 else ""
        print(f"{fid:24} {', '.join(why[:3])}{more}")


SELF_TEST_MAKEFILE = """include config/features.mk
include boards/$(BOARD)/board.mk
-include boards/$(BOARD)/config.mk
$(foreach f,$(FEATURES),$(eval CONFIG_$(f) ?= y))
INCLUDES := -Icore -Iboards/$(BOARD) -Ibuild/gen
ifeq ($(CONFIG_GPS),y)
CORE_SRC += core/gps/gps.c
endif
print-vars:
\t@:$(foreach v,$(VARS),$(info $(v)=$($(v))))
"""


def self_test():
    with tempfile.TemporaryDirectory() as t:
        def w(p, s):
            os.makedirs(os.path.dirname(os.path.join(t, p)), exist_ok=True)
            with open(os.path.join(t, p), "w") as f:
                f.write(s)

        def sh(*a):
            subprocess.run(a, cwd=t, check=True, capture_output=True)
        # the Makefile's own reading of board.mk and config.mk, the way make_vars() asks for it
        w("config/features.mk", "FEATURES := FM DMR DMR_SMS CHANNEL_EDIT GPS\n")
        w("Makefile", SELF_TEST_MAKEFILE)
        w("boards/a/board.mk", "PLATFORM := p1\nBOARD_DIRS := boards/a boards/fam drivers/rf/x \\\n\tdrivers/modem/m\n")
        # a driver only a board switch builds, as boards/md9600/config.mk adds its synthesizers, and the [ci] extra
        # that turns the switch off
        w("boards/a/config.mk", "CONFIG_A_V2 ?= y\nBOARD_DIRS += $(if $(filter y,$(CONFIG_A_V2)),drivers/rf/v2)\n")
        w("boards/a/board.toml", '[ci]\nextra = [{ name = "no-v2", make = ["CONFIG_A_V2=n"] }]\n')
        w("boards/b/board.mk", "PLATFORM := p1\nBOARD_DIRS := boards/b boards/fam\n")
        w("boards/b/config.mk", "CONFIG_GPS ?= n\n")
        w("boards/c/board.mk", "PLATFORM := p2\nBOARD_DIRS := boards/c drivers/rf/x\nBOOT_STAGE := c\n"
                               "BOARD_REQUIRED := FM\n")
        w("core/dmr/contact.h", "#if CONFIG_DMR\ntypedef int contact_t;\n#endif\nint shared_fn(void);\n")
        # a macro derived from a switch
        w("core/pos.h", "#if CONFIG_GPS\n#define HAS_POS 1\n#endif\n")
        storage = ('#include "config.h"\n#include "dmr/contact.h"\n#include "pos.h"\nint a;\n'
                   "#if CONFIG_CHANNEL_EDIT\nint e;\n#endif\n#if CONFIG_GPS\nint g;\n#endif\nint z;\n"
                   "#ifdef HAS_POS\nint p;\n#endif\n")
        w("core/ui/storage.c", storage)
        # a header that includes the guarded one: the type reaches other.c through a second header
        w("core/ui/wrap.h", '#include "dmr/contact.h"\nint wrap(void);\n')
        w("core/ui/other.c", '#include "ui/wrap.h"\nint o;\n')
        # a #if inside a block comment, a condition continued on the next line
        w("core/ui/cm.c", "/*\n#if CONFIG_GPS\n*/\nint k;\n/* #endif */\n")
        w("core/ui/cont.c", "#if CONFIG_FM && \\\n    CONFIG_GPS\nint q;\n#endif\n")
        # shared plumbing that names four switches: still the default source of a feature it names
        w("core/ui/menus.c", "#if CONFIG_FM\nint m1;\n#endif\n#if CONFIG_DMR\nint m2;\n#endif\n"
                             "#if CONFIG_DMR_SMS\nint m3;\n#endif\n#if CONFIG_CHANNEL_EDIT\nint m4;\n#endif\nint m5;\n")
        w("core/gps/gps.c", "int g;\n")
        w("drivers/rf/x/x.c", "#if CONFIG_DMR_SMS\nint s;\n#endif\n")
        w("drivers/rf/v2/v.c", "#if CONFIG_A_V2\nint v;\n#endif\n")
        w("boards/fam/f.c", '#include "bcfg.h"\n#if CONFIG_FM\nint f;\n#endif\n')
        # two headers of one name: each board's -I directory decides which one f.c gets
        w("boards/a/bcfg.h", "#if CONFIG_DMR\ntypedef int bcfg_t;\n#endif\n")
        w("boards/b/bcfg.h", "typedef int bcfg_t;\n")
        w("platform/p2/system.c", "int s;\n")
        w("boot/c/boot.c", "int s;\n")
        w("docs/x.md", "x\n")
        w(FEATURES_TOML, '[feature.gps]\nswitch = "GPS"\nspec = "docs/x.md"\n'
                         '[feature.ui]\nspec = "docs/x.md"\nsource = ["core/ui"]\n'
                         '[feature.sms]\nswitch = "DMR_SMS"\nspec = "docs/x.md"\n'
                         '[feature.edit]\nswitch = "CHANNEL_EDIT"\nspec = "docs/x.md"\n')
        sh("git", "init", "-q")
        sh("git", "add", "-A")
        sh("git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base")
        w(".git/info/exclude", "boards/e/\n")      # the broken board below: there, but not part of the change
        warned = []

        def edit(edits, extra=()):
            """Write edits (a new file stays untracked: the working tree's diff counts it) and extra (outside the git
            tree's view: ignored), and return what undoes them."""
            for path, text in list(edits.items()) + list(extra):
                w(path, text)
            registry._SOURCE_TEXTS.pop(t, None)

            def undo():
                sh("git", "checkout", "-q", "--", ".")
                sh("git", "clean", "-qfdx")
            return undo

        def names(edits, only=None, scope="precise", extra=()):
            undo = edit(edits, extra)
            try:
                return {(b, n) for b, n, _, _ in plan(t, changed(t, "HEAD", None), only, scope, "HEAD",
                                                      warn=warned.append)}
            finally:
                undo()

        def feats(edits):
            undo = edit(edits)
            try:
                return set(affected_features(t, "HEAD", None, warn=warned.append))
            finally:
                undo()

        def makes(edits):
            """How many times make runs for the diff."""
            calls = []
            real = registry.make_vars
            registry.make_vars = lambda *a, **k: calls.append(a[1]) or real(*a, **k)
            try:
                names(edits)
            finally:
                registry.make_vars = real
            return len(calls)
        fm_ac = {("a", "fm-minimal"), ("c", "fm-minimal")}
        full = {("a", "matrix"), ("b", "matrix"), ("c", "matrix")}
        broken = [("boards/e/board.mk", "PLATFORM := p2\nBOARD_DIRS := boards/e\n"),
                  ("boards/e/config.mk", "$(error boards/e is broken)\n")]
        checks = [
            # an unguarded use of a type a header declares only under CONFIG_DMR: no-DMR on one board per platform
            # (a has more switches on than b) and fm-minimal there, but not the file's other switches
            ("use of a guarded declaration", names({"core/ui/storage.c": storage.replace("int a;", "contact_t a;")}),
             {("a", "no-DMR"), ("c", "no-DMR")} | fm_ac),
            ("use of an unguarded declaration",
             names({"core/ui/storage.c": storage.replace("int a;", "int a = shared_fn();")}), set()),
            ("use of a guarded declaration through a second header",
             names({"core/ui/other.c": '#include "ui/wrap.h"\ncontact_t o;\n'}),
             {("a", "no-DMR"), ("c", "no-DMR")} | fm_ac),
            ("change inside a guard", names({"core/ui/storage.c": storage.replace("int e;", "int e2;")}),
             {("a", "no-CHANNEL_EDIT"), ("c", "no-CHANNEL_EDIT")}),
            ("change inside a guard on a macro derived from a switch",
             names({"core/ui/storage.c": storage.replace("int p;", "int p2;")}), {("a", "no-GPS"), ("c", "no-GPS")}),
            ("a #if in a block comment guards nothing", names({"core/ui/cm.c": "/*\n#if CONFIG_GPS\n*/\nint k2;\n"
                                                                               "/* #endif */\n"}), set()),
            ("a condition continued on the next line",
             names({"core/ui/cont.c": "#if CONFIG_FM && \\\n    CONFIG_GPS\nint q2;\n#endif\n"}),
             {("a", "no-FM"), ("a", "no-GPS"), ("c", "no-GPS")}),
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
            # BOARD_DIRS += $(if ...): the driver is a's, and a's own switch selects a's [ci] extra
            ("driver a $(if ...) in BOARD_DIRS adds", names({"drivers/rf/v2/v.c": "#if CONFIG_A_V2\nint v2;\n#endif\n"}),
             {("a", "no-v2")}),
            # c requires FM, and isn't on this family
            ("family dir", names({"boards/fam/f.c": '#include "bcfg.h"\n#if CONFIG_FM\nint f2;\n#endif\n'}),
             {("a", "no-FM"), ("b", "no-FM")}),
            ("a header name resolved by each board's -I order",
             names({"boards/fam/f.c": '#include "bcfg.h"\nbcfg_t f0;\n#if CONFIG_FM\nint f;\n#endif\n'}),
             {("a", "no-DMR")}),
            ("platform file", names({"platform/p2/system.c": "int s2;\n"}), {("c", "fm-minimal")}),
            ("boot stage", names({"boot/c/boot.c": "int s2;\n"}), {("c", "fm-minimal")}),
            ("docs only", names({"docs/x.md": "y\n"}), set()),
            ("--board", names({"core/gps/gps.c": "int g2;\n"}, only={"b"}), set()),
            # the fallbacks: a source no board uses, a board make fails on
            ("a file that maps to no board: every board's matrix", names({"platform/p9/x.c": "int x;\n"}), full),
            ("a board make fails on: its matrix",
             names({"core/gps/gps.c": "int g2;\n"}, extra=broken), {("a", "no-GPS"), ("c", "no-GPS"), ("e", "matrix")}),
            ("warned about both", [w.split(":")[0] for w in warned[-2:]], ["platform/p9/x.c maps to no board", "e"]),
            ("make once per board, none for docs", (makes({"core/gps/gps.c": "int g2;\n"}), makes({"docs/x.md": "y\n"})),
             (3, 0)),
            # HIL features: the Makefile's file for GPS; a source path, and a file naming CHANNEL_EDIT (no Makefile
            # path) though not GPS (it has one); a guard's switch; a file naming four switches selects the features
            # it is the default source of, as tools/bringup.py counts it; docs reach none
            ("features: Makefile-built file", feats({"core/gps/gps.c": "int g2;\n"}), {"gps"}),
            ("features: source path and named switch", feats({"core/ui/storage.c": storage.replace("int z;", "int z2;")}),
             {"ui", "edit"}),
            ("features: guard", feats({"drivers/rf/x/x.c": "#if CONFIG_DMR_SMS\nint s2;\n#endif\n"}), {"sms"}),
            ("features: a file naming four switches", feats({"core/ui/menus.c": "#if CONFIG_FM\nint m1;\n#endif\n"
                                                                                 "#if CONFIG_DMR\nint m2;\n#endif\n"
                                                                                 "#if CONFIG_DMR_SMS\nint m3;\n#endif\n"
                                                                                 "#if CONFIG_CHANNEL_EDIT\nint m4;\n"
                                                                                 "#endif\nint m6;\n"}),
             {"ui", "edit", "sms"}),
            ("features: docs only", feats({"docs/x.md": "y\n"}), set()),
            ("boards: a family dir, a platform, shared code, two boards' driver, a $(if ...) driver, docs",
             [reached_boards(t, {p: {}}, warn=warned.append)
              for p in ("boards/fam/f.c", "platform/p2/system.c", "core/gps/gps.c", "drivers/rf/x/x.c",
                        "drivers/rf/v2/v.c", "docs/x.md")],
             [["a", "b"], ["c"], ["a", "b", "c"], ["a", "c"], ["a"], []]),
            ("boards: a file that maps to no board", reached_boards(t, {"boards/zz/z.c": {}}, warn=warned.append),
             ["a", "b", "c"]),
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
    for name in ("builds", "summary", "boards", "features"):
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
        {"builds": cmd_builds, "summary": cmd_summary, "boards": cmd_boards, "features": cmd_features}[a.cmd](a)
    except subprocess.CalledProcessError as e:
        print(f"affected: {' '.join(e.cmd)}: {e.stderr.strip()}", file=sys.stderr)
        return 2
    except registry.RegistryError as e:
        print(f"affected: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
