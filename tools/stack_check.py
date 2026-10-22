#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Static worst-case stack depth of every FreeRTOS task in a firmware build (docs/dev/stack-check.md).

The firmware is compiled with GCC's -fcallgraph-info=su: next to each object (or each LTO partition, next to the .elf)
a .ci file holds the functions, their frame sizes and their direct calls. This tool merges them into one call graph,
finds the tasks in the compiled sources (xTaskCreate calls, the stack size evaluated by the compiler's preprocessor)
and follows the deepest call chain from each task's entry function:

  need = deepest chain (sum of frames) + port overhead (what the RTOS port saves on a task stack) + margin

It fails when the need exceeds the task's stack. Calls through function pointers are resolved from the sources: a
function stored into a struct field (a designated or positional initializer, an assignment) is a target of the calls
through that field of that struct type; a function stored into a variable or passed as an argument is a target of
the calls through that variable or parameter. What the sources can't show comes from tools/stack_check_edges.txt.
Unresolved calls, recursion (each cycle counted once) and library functions without a call graph are reported.

  make BOARD=dm1701 stack-check          (the Makefile passes the build's compiler, flags and port overhead)
  tools/stack_check.py --build build/dm1701 --cc arm-none-eabi-gcc --cflags "..." --overhead 208
  tools/stack_check.py --self-test
"""
import argparse
import bisect
import os
import re
import shlex
import subprocess
import sys
import tempfile

INDIRECT = "__indirect_call"
STR = r'"((?:[^"\\]|\\.)*)"'
FIELD_RE = re.compile(r'(\w+)\s*:\s*' + STR)
FRAME_RE = re.compile(r'^(\d+) bytes \(([\w,]+)\)$')
LOC_RE = re.compile(r'^(.*):(\d+):(\d+)$')
LTRANS_RE = re.compile(r'^.*\.ltrans\d+\.o:')
IDENT = r'[A-Za-z_]\w*'
TASK_RE = re.compile(r'\bxTaskCreate(?:Static)?\s*\(\s*&?\s*(' + IDENT + r')\s*,\s*("(?:[^"\\]|\\.)*"|[^,]+?)\s*,\s*'
                     r'([^,]+?)\s*,')
# The callee of a call expression: a name, then members and subscripts, maybe as (*p)
CHAIN = IDENT + r'(?:\s*(?:\.|->)\s*' + IDENT + r'|\s*\[[^\]]*\])*'
CALLEE_RE = re.compile(r'\(?\s*\*?\s*(' + CHAIN + r')\s*\)?\s*\(')
CALLBASE_RE = re.compile(r'\(?\s*(' + IDENT + r')\s*\(\s*\)\s*\)?\s*(?:\.|->)\s*(' + CHAIN + r')\s*\(')
ASSIGN_RE = re.compile(r'(?<![\w.>\]])(' + CHAIN + r')\s*=(?!=)\s*([^;{}]+?)\s*;')
DECL_FNPTR_RE = re.compile(r'\(\s*\*\s*(' + IDENT + r')\s*\)\s*\([^()]*\)\s*=\s*([^;{}]+?)\s*[;,]')
CALL_RE = re.compile(r'\b(' + IDENT + r')\s*\(')
ATOMIC_STORE_RE = re.compile(r'__atomic_store_n\s*\(\s*&?\s*\(?\s*(' + CHAIN + r')\s*\)?\s*,\s*([^,]+?)\s*,')
DEFINE_RE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(' + IDENT + r')(\([^)]*\))?[ \t]*(.*)$', re.M)
KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "do", "else", "case", "goto", "typeof", "__typeof__",
            "_Static_assert", "__attribute__", "__asm__", "asm", "alignof", "_Alignof", "offsetof", "defined"}
QUALIFIERS = {"const", "volatile", "struct", "union", "enum", "unsigned", "signed", "register", "restrict", "static",
              "extern", "inline", "__restrict", "__inline", "__volatile__"}

DEFAULT_LIB_BYTES = 64


# ── the call graph ───────────────────────────────────────────────────────────

class Func:
    __slots__ = ("title", "name", "file", "line", "frame", "qual")

    def __init__(self, title, name, file, line, frame, qual):
        self.title, self.name, self.file, self.line, self.frame, self.qual = title, name, file, line, frame, qual


def base_name(name):
    """foo.constprop.0 / foo.lto_priv.0 / foo.isra.0 / foo.part.0 -> foo"""
    return name.split(".", 1)[0]


def parse_label(label):
    """A node label: name, then file:line:col, then 'N bytes (static|dynamic|dynamic,bounded)' when defined here"""
    parts = label.replace("\\n", "\n").split("\n")
    name = base_name(parts[0])
    if name.startswith("__builtin_"):
        name = name[len("__builtin_"):]
    file = line = frame = qual = None
    for p in parts[1:]:
        m = FRAME_RE.match(p)
        if m:
            frame, qual = int(m.group(1)), m.group(2)
            continue
        m = LOC_RE.match(p)
        if m:
            file, line = m.group(1), int(m.group(2))
    return name, file, line, frame, qual


def parse_ci(text, funcs, edges):
    """Add one .ci (VCG) file's nodes and edges. funcs: title -> Func, edges: title -> [(target title, call site)]"""
    for line in text.splitlines():
        line = line.strip()
        kind = line.split(":", 1)[0]
        if kind not in ("node", "edge"):
            continue
        f = dict(FIELD_RE.findall(line))
        if kind == "node":
            title = f.get("title")
            if title is None or title == INDIRECT:
                continue
            name, file, lno, frame, qual = parse_label(f.get("label", title))
            old = funcs.get(title)
            # a declaration (no frame) never replaces a definition seen in another file
            if old is None or (old.frame is None and frame is not None):
                funcs[title] = Func(title, name, file, lno, frame, qual)
        else:
            src, dst = f.get("sourcename"), f.get("targetname")
            if src is not None and dst is not None:
                edges.setdefault(src, []).append((dst, f.get("label")))


class Graph:
    def __init__(self):
        self.funcs = {}
        self.edges = {}
        self.by_name = {}		# source name -> [titles of definitions]

    def load_dir(self, build):
        n = 0
        for root, _dirs, files in os.walk(build):
            for fn in sorted(files):
                if fn.endswith(".ci"):
                    with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as fh:
                        parse_ci(fh.read(), self.funcs, self.edges)
                    n += 1
        self.index()
        return n

    def index(self):
        self.by_name = {}
        for t, f in self.funcs.items():
            if f.frame is not None:
                self.by_name.setdefault(f.name, []).append(t)
        # LTO: a partition may name a function another partition defines with its own prefix
        # ("/tmp/x.ltrans0.o:foo.lto_priv.0" for "/tmp/x.ltrans3.o:foo.lto_priv.0"): point the edge at the definition
        stripped = {}
        for t, f in self.funcs.items():
            if f.frame is not None:
                stripped.setdefault(LTRANS_RE.sub("", t), []).append(t)
        # and a header's static inline function promoted under another name in each partition
        # ("board_has_battery.lto_priv.1" called, "board_has_battery.lto_priv.1.lto_priv.0" defined): the same name
        # from the same file, the largest frame
        for src, lst in self.edges.items():
            for i, (dst, loc) in enumerate(lst):
                if dst == INDIRECT or self.defined(dst):
                    continue
                cand = stripped.get(LTRANS_RE.sub("", dst), [])
                decl = self.funcs.get(dst)
                if len(cand) != 1 and decl is not None and decl.file is not None and "lto_priv" in dst:
                    cand = sorted((t for t in self.by_name.get(decl.name, []) if self.funcs[t].file == decl.file),
                                  key=lambda t: -self.funcs[t].frame)[:1]
                if len(cand) == 1:
                    lst[i] = (cand[0], loc)

    def defined(self, title):
        f = self.funcs.get(title)
        return f is not None and f.frame is not None

    def lookup(self, name, file=None):
        """Titles of the definitions of a function: a static one in `file` first, else every one of that name"""
        ts = self.by_name.get(base_name(name), [])
        if file is not None and len(ts) > 1:
            local = [t for t in ts if self.funcs[t].file == file]
            if local:
                return local
        return ts

    def visible(self, name, file):
        """Definitions a name in `file` can mean: the file's own, else a global function (not another file's static
        or a compiler clone)"""
        ts = self.by_name.get(name, [])
        local = [t for t in ts if self.funcs[t].file == file]
        return local or [t for t in ts if ":" not in t and "." not in t]

    def lookup_spec(self, spec):
        """A table target: 'name' or 'path/file.c:name'"""
        if ":" in spec:
            file, name = spec.rsplit(":", 1)
            return [t for t in self.by_name.get(name, []) if self.funcs[t].file and
                    (self.funcs[t].file == file or self.funcs[t].file.endswith("/" + file))]
        return self.by_name.get(spec, [])

    def display(self, title):
        f = self.funcs.get(title)
        return f.name if f else title


# ── the hand-kept table ──────────────────────────────────────────────────────

class Table:
    """tools/stack_check_edges.txt: what the call graph and the sources can't show"""

    def __init__(self):
        self.ptr = {}		# pointer field, variable or parameter name -> [target specs]
        self.call = {}		# function name -> [target specs] for every indirect call it makes
        self.file = {}		# source file -> [target specs] for its indirect calls the sources don't resolve
        self.lib = {}		# function without call graph -> bytes
        self.extern = {}	# code outside the build (@name in a call or ptr line) -> bytes
        self.task = []		# (entry function, stack expression)
        self.notask = set()
        self.recursion = set()	# functions whose recursion is known and bounded (not reported)

    def load(self, path):
        with open(path, encoding="utf-8") as fh:
            for no, line in enumerate(fh, 1):
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                words = line.split()
                kind = words[0]
                try:
                    if kind in ("ptr", "call", "file"):
                        key = words[1].rstrip(":")
                        if len(words) < 3:
                            raise ValueError(kind)
                        getattr(self, kind).setdefault(key, []).extend(words[2:])
                    elif kind in ("lib", "extern"):
                        (self.lib if kind == "lib" else self.extern)[words[1]] = int(words[2], 0)
                    elif kind == "task":
                        self.task.append((words[1], " ".join(words[2:])))
                    elif kind == "notask":
                        self.notask.add(words[1])
                    elif kind == "recursion":
                        self.recursion.update(words[1:])
                    else:
                        raise ValueError(kind)
                except (IndexError, ValueError):
                    raise SystemExit("%s:%d: can't read '%s'" % (path, no, line))


# ── C source scanning ────────────────────────────────────────────────────────

def strip_comments(src, keep_strings=False):
    """C source without comments, strings emptied unless keep_strings (line numbers and offsets of code kept: every
    removed character becomes a space)"""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            if not keep_strings:
                for k in range(i + 1, min(j, n)):
                    out[k] = " "
            i = j + 1
            continue
        else:
            i += 1
            continue
        for k in range(i, j):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


PAIRS = {"(": ")", "[": "]", "{": "}"}


def match_close(t, i):
    """Index of the bracket closing the one at t[i] (len(t) when unbalanced)"""
    depth = 0
    for j in range(i, len(t)):
        c = t[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return j
    return len(t)


def split_top(s, sep):
    """s split at sep outside brackets"""
    out, depth, start = [], 0, 0
    for i, c in enumerate(s):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == sep and depth == 0:
            out.append(s[start:i])
            start = i + 1
    out.append(s[start:])
    return out


def type_word(decl):
    """The type name of a declaration: its first word that isn't a qualifier"""
    for w in re.findall(IDENT, re.sub(r"\[[^\]]*\]", "", decl)):
        if w not in QUALIFIERS:
            return w
    return None


def parse_params(text):
    """[(name, type)] of a parameter list (type None for a function pointer)"""
    out = []
    for p in split_top(text, ","):
        p = p.strip()
        if p in ("", "void", "..."):
            continue
        m = re.search(r"\(\s*\*\s*(" + IDENT + r")", p)
        if m:
            out.append((m.group(1), None))
            continue
        words = re.findall(IDENT, re.sub(r"\[[^\]]*\]", "", p))
        if not words:
            continue
        out.append((words[-1], type_word(p) if len(words) > 1 else None))
    return out


class FileText:
    """One source file: code without comments, line offsets, top-level function bodies, macros"""

    def __init__(self, path, raw):
        self.path = path
        self.raw_lines = raw.split("\n")
        # preprocessor lines blanked: an #if inside an initializer would hide the member after it
        self.code = re.sub(r"(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", lambda m: re.sub(r"[^\n]", " ", m.group(0)),
                           strip_comments(raw))
        self.line_off = [0]
        for ln in self.raw_lines:
            self.line_off.append(self.line_off[-1] + len(ln) + 1)
        self.funcs = []		# (body start, body end, name, params)
        self.statics = set()	# names of the functions defined static
        self.blocks = []	# top-level {...} that aren't function bodies: (start, end)
        self.macros = {}	# name -> [(params or None, body)]
        self.scan()

    def offset(self, line, col):
        if line < 1 or line > len(self.raw_lines):
            return None
        return self.line_off[line - 1] + col - 1

    def scan(self):
        t = self.code
        i, n = 0, len(t)
        while i < n:
            j = t.find("{", i)
            if j < 0:
                break
            end = match_close(t, j)
            k = j - 1
            while k >= 0 and t[k].isspace():
                k -= 1
            name = None
            if k >= 0 and t[k] == ")":
                # function definition: the name before the matching parenthesis
                depth = 0
                p = k
                while p >= 0:
                    if t[p] == ")":
                        depth += 1
                    elif t[p] == "(":
                        depth -= 1
                        if depth == 0:
                            break
                    p -= 1
                m = re.search(r"(" + IDENT + r")\s*$", t[max(0, p - 200):p])
                if m and m.group(1) not in KEYWORDS:
                    name = m.group(1)
                    self.funcs.append((j, end, name, parse_params(t[p + 1:k])))
                    head = t[max(t.rfind(";", 0, p), t.rfind("}", 0, p)) + 1:p]
                    if re.search(r"\bstatic\b", head):
                        self.statics.add(name)
            if name is None:
                self.blocks.append((j, end))
            i = end + 1
        self.func_starts = [f[0] for f in self.funcs]
        # macros, from the original text (continuation lines joined)
        raw = re.sub(r"\\\n", " ", "\n".join(self.raw_lines))
        for m in DEFINE_RE.finditer(strip_comments(raw)):
            params = None
            if m.group(2) is not None:
                params = [p.strip() for p in m.group(2)[1:-1].split(",") if p.strip()]
            self.macros.setdefault(m.group(1), []).append((params, m.group(3).strip()))

    def enclosing(self, off):
        """(name, params, body start) of the function whose body holds offset off, else None"""
        i = bisect.bisect_right(self.func_starts, off) - 1
        if i >= 0 and self.funcs[i][0] <= off <= self.funcs[i][1]:
            return self.funcs[i]
        return None


class Struct:
    def __init__(self, name):
        self.name = name
        self.members = []	# [(name, type or None, is_array)]

    def member(self, name):
        for m in self.members:
            if m[0] == name:
                return m
        return None


class CModel:
    """What the sources say about function pointers: struct types, and which functions each pointer can hold"""

    def __init__(self, graph, root, files, headers):
        self.g = graph
        self.root = root
        self.files = {}		# path -> FileText (sources and headers)
        self.structs = {}	# type name -> Struct
        self.alias = {}		# typedef name -> type name
        self.globals = {}	# variable name -> type (file scope and extern declarations)
        self.file_vars = {}	# (path, name) -> type (file scope, including anonymous struct types)
        self.binds = {}		# key -> set of refs: ("fn", title) or ("key", key)
        self.anon = 0
        for p in list(files) + list(headers):
            raw = self.read(p)
            if raw is not None:
                self.files[p] = FileText(p, raw)
        self.params = {}	# function name -> parameter names
        self.params_of = {}	# parameter key name (pname) -> parameter names
        self.defined_in = {}	# function name -> files that define it
        self.global_in = {}	# function name -> files that define it without static
        self.member_calls = []	# (member key, callee name, argument index, ref): bound once every store is known
        for p, ft in self.files.items():
            self.scan_types(ft)
            for _s, _e, name, params in ft.funcs:
                self.params.setdefault(name, [pn for pn, _t in params])
                self.defined_in.setdefault(name, set()).add(p)
                if name not in ft.statics:
                    self.global_in.setdefault(name, set()).add(p)
        for p, ft in self.files.items():
            for _s, _e, name, params in ft.funcs:
                self.params_of.setdefault(self.pname(p, name), [pn for pn, _t in params])
        for p in files:
            if p in self.files:
                self.scan_values(self.files[p])
        for p in headers:
            if p in self.files:
                self.scan_values(self.files[p])
        self.bind_member_calls()

    def pname(self, path, name):
        """The name a function's parameters are keyed by: path:name for a static function whose name another file
        also defines (two files' static start() are different functions), else the bare name"""
        ft = self.files.get(path)
        if ft is not None and name in ft.statics and len(self.defined_in.get(name, ())) > 1:
            return "%s:%s" % (path, name)
        return name

    def bind_member_calls(self):
        """Arguments of calls through a struct field go to the parameters of the functions stored in that field; when
        the field's functions aren't known, to every function of the callee's name"""
        for mkey, name, i, ref in self.member_calls:
            ts = self.targets(mkey) if mkey is not None else set()
            pnames = set()
            for t in ts:
                path, _, fn = t.rpartition(":")
                if path in self.files:
                    pnames.add(self.pname(path, fn))
                else:		# a bare or LTO title: every function of that name
                    pnames.update({self.pname(p, fn) for p in self.defined_in.get(fn, ())} or {fn})
            if not pnames:
                pnames = {self.pname(p, name) for p in self.defined_in.get(name, ())}
            for pn in pnames:
                self.bind(("p", pn, i), ref)

    def read(self, path):
        try:
            with open(os.path.join(self.root, path), encoding="utf-8", errors="replace") as fh:
                return fh.read()
        except OSError:
            return None

    # types

    def resolve_type(self, name):
        seen = 0
        while name in self.alias and seen < 10:
            name = self.alias[name]
            seen += 1
        return name

    def struct(self, name):
        return self.structs.get(self.resolve_type(name)) if name else None

    def parse_members(self, body, owner):
        st = Struct(owner)
        for d in split_top(body, ";"):
            d = d.strip()
            if not d or d.startswith("#"):
                continue
            m = re.match(r"(?:struct|union)\s*(" + IDENT + r")?\s*\{", d)
            if m:
                b = d.index("{")
                e = match_close(d, b)
                self.anon += 1
                sub = m.group(1) or "%s::%d" % (owner, self.anon)
                inner = self.parse_members(d[b + 1:e], sub)
                self.structs[sub] = inner
                names = [re.findall(IDENT, re.sub(r"\[[^\]]*\]", "", x)) for x in split_top(d[e + 1:], ",")]
                names = [(w[-1], "[" in x) for w, x in zip(names, split_top(d[e + 1:], ",")) if w]
                if names:
                    for nm, arr in names:
                        st.members.append((nm, sub, arr))
                else:
                    st.members.extend(inner.members)	# anonymous: its members belong to the parent
                continue
            m = re.search(r"\(\s*\*\s*(" + IDENT + r")\s*(\[[^\]]*\])?\s*\)\s*\(", d)
            if m:
                st.members.append((m.group(1), None, bool(m.group(2))))
                continue
            d = re.sub(r":\s*\w+\s*$", "", d)
            decls = split_top(d, ",")
            tw = type_word(decls[0])
            for k, x in enumerate(decls):
                words = re.findall(IDENT, re.sub(r"\[[^\]]*\]", "", x))
                if not words or (k == 0 and len(words) < 2):
                    continue
                st.members.append((words[-1], tw, "[" in x))
        return st

    def scan_types(self, ft):
        t = ft.code
        for m in re.finditer(r"\b(typedef\s+)?(?:struct|union)\s+(" + IDENT + r")?\s*\{", t):
            b = t.index("{", m.start())
            e = match_close(t, b)
            tag = m.group(2)
            if not tag:
                self.anon += 1
                tag = "<anon %s %d>" % (ft.path, self.anon)
            self.structs[tag] = self.parse_members(t[b + 1:e], tag)
            semi = t.find(";", e)
            tail = t[e + 1:semi if semi >= 0 else len(t)]
            if "{" in tail:		# an initializer follows: cut at the end of the declaration
                tail = t[e + 1:self.decl_end(t, e + 1)]
            for d in split_top(tail, ","):
                words = re.findall(IDENT, re.sub(r"\[[^\]]*\]", "", d.split("=", 1)[0]))
                if not words:
                    continue
                if m.group(1):
                    self.alias.setdefault(words[-1], tag)
                else:
                    self.file_vars[(ft.path, words[-1])] = tag
                    if "=" in d:
                        val = d.split("=", 1)[1].strip()
                        if val.startswith("{"):
                            self.parse_init(ft, val[1:match_close(val, 0)], tag, "[" in d.split("=", 1)[0])
        # file-scope function pointers, static bool (*name)(void): they shadow other files' functions of that name
        for m in re.finditer(r"\(\s*\*\s*(" + IDENT + r")\s*\)\s*\(", t):
            if any(s <= m.start() <= e for s, e, _n, _p in ft.funcs):
                continue
            head = t[max(t.rfind(";", 0, m.start()), t.rfind("}", 0, m.start())) + 1:m.start()]
            if head.count("(") == head.count(")") and not re.search(r"\btypedef\b", head):
                self.file_vars.setdefault((ft.path, m.group(1)), "?")
        for m in re.finditer(r"\btypedef\s+(?:struct\s+|union\s+)?(" + IDENT + r")\s+\**\s*(" + IDENT + r")\s*;", t):
            self.alias.setdefault(m.group(2), m.group(1))
        for m in re.finditer(r"\bextern\s+(?:const\s+)?(?:struct\s+)?(" + IDENT + r")\s+(?:const\s+)?\**\s*"
                             r"(?:const\s+)?(" + IDENT + r")\s*[;\[]", t):
            self.globals.setdefault(m.group(2), m.group(1))

    @staticmethod
    def decl_end(t, i):
        depth = 0
        for j in range(i, len(t)):
            c = t[j]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == ";" and depth == 0:
                return j
        return len(t)

    # values: initializers, assignments, arguments

    def bind(self, key, ref):
        if key is not None and ref is not None:
            self.binds.setdefault(key, set()).add(ref)
            if key[0] == "m":		# also reachable by the field name alone, for call sites of unknown type
                self.binds.setdefault(("f", key[2]), set()).add(ref)
            elif key[0] == "f":		# stored where the type is unknown: a candidate for every type's field
                self.binds.setdefault(("u", key[1]), set()).add(ref)
            elif key[0] == "v":		# and a file's variable by its name from other files (extern)
                self.binds.setdefault(("g", key[2]), set()).add(("key", key))

    def name_ref(self, ft, off, name):
        """A function (by title; a source function the build inlined or folded by its bare name), or the pointer a
        name chain names (a key)"""
        if re.fullmatch(IDENT, name):
            key = self.chain_key(ft, off, name)
            if key[0] == "p":
                return ("key", key)
            ts = self.visible(name, ft)
            if ts:
                return ("fns", tuple(ts))
            if ft.path in self.defined_in.get(name, ()):
                return ("fns", (name,))		# this file's function, inlined or folded away in the build
            return ("key", key)
        return ("key", self.chain_key(ft, off, name))

    def value_refs(self, ft, off, val):
        """What an expression stored into a pointer may refer to. A plain name chain (maybe cast, maybe &) exactly;
        anything else (a conditional, a macro, an atomic load) by every name in it that isn't called"""
        v = val.strip()
        v = re.sub(r"^\(\s*[\w\s\*]+\)\s*(?=[&\w(])", "", v)	# a cast
        v = v.lstrip("&").strip()
        while v.startswith("(") and match_close(v, 0) == len(v) - 1:
            v = v[1:-1].strip()
        if re.fullmatch(CHAIN, v):
            return [self.name_ref(ft, off, v)]
        refs = []
        for m in re.finditer(r"(?<![\w.>])(" + CHAIN + r")(?![\w\s]*\()", v):
            if m.group(1) not in KEYWORDS and m.group(1) not in QUALIFIERS:
                refs.append(self.name_ref(ft, off, m.group(1)))
        # a call's result (a pointer computed at run time, the stock codec's entry points) can't be followed
        if any(c not in KEYWORDS and not c.startswith("__atomic_load") for c in re.findall(r"(" + IDENT + r")\s*\(", v)):
            refs.append(("opaque", None))
        return refs

    def parse_init(self, ft, inner, typ, is_array, off=0):
        st = self.struct(typ)
        items = split_top(inner, ",")
        if is_array:
            for it in items:
                it = re.sub(r"^\s*\[[^\]]*\]\s*=", "", it).strip()
                if it.startswith("{"):
                    self.parse_init(ft, it[1:match_close(it, 0)], typ, False, off)
            return
        if st is None:
            return
        idx = 0
        for it in items:
            m = re.match(r"\s*\.\s*(" + IDENT + r")\s*=\s*(.*)$", it, re.S)
            if m:
                field, val = m.group(1), m.group(2).strip()
                names = [x[0] for x in st.members]
                idx = names.index(field) + 1 if field in names else idx
            else:
                if idx >= len(st.members):
                    break
                field, val = st.members[idx][0], it.strip()
                idx += 1
            mem = st.member(field)
            if mem is None or not val:
                continue
            if val.startswith("{"):
                sub = val[1:match_close(val, 0)]
                if mem[1] is None or self.struct(mem[1]) is None:	# an array of pointers
                    for v in split_top(sub, ","):
                        for ref in self.value_refs(ft, off, v):
                            self.bind(("m", self.resolve_type(typ), field), ref)
                else:
                    self.parse_init(ft, sub, mem[1], mem[2], off)
            else:
                for ref in self.value_refs(ft, off, val):
                    self.bind(("m", self.resolve_type(typ), field), ref)

    def scan_values(self, ft):
        t = ft.code
        # initialized declarations of a known struct type, and compound literals
        for m in re.finditer(r"\b(?:struct\s+|union\s+)?(" + IDENT + r")\s+(?:const\s+|volatile\s+)*\**\s*"
                             r"(?:const\s+)?(" + IDENT + r")\s*((?:\[[^\]]*\]\s*)*)=\s*\{", t):
            if self.struct(m.group(1)) is None:
                continue
            b = m.end() - 1
            self.parse_init(ft, t[b + 1:match_close(t, b)], m.group(1), bool(m.group(3)), m.start())
            if ft.enclosing(m.start()) is None:
                self.file_vars.setdefault((ft.path, m.group(2)), m.group(1))
        for m in re.finditer(r"\(\s*(?:const\s+)?(?:struct\s+)?(" + IDENT + r")\s*\)\s*\{", t):
            if self.struct(m.group(1)) is not None:
                b = m.end() - 1
                self.parse_init(ft, t[b + 1:match_close(t, b)], m.group(1), False, m.start())
        # assignments and pointer declarations with a value; a statement that calls one of the file's function-like
        # macros is scanned again expanded (an atomic store in a macro is an assignment)
        for m in ASSIGN_RE.finditer(t):
            self.assign(ft, m.start(1), m.group(1), m.group(2))
        for m in DECL_FNPTR_RE.finditer(t):
            self.assign(ft, m.start(1), m.group(1), m.group(2))
        fmacros = [n for n, defs in ft.macros.items() if any(d[0] is not None for d in defs)]
        if fmacros:
            for m in re.finditer(r"\b(?:%s)\s*\(" % "|".join(map(re.escape, fmacros)), t):
                if ft.enclosing(m.start()) is None:
                    continue
                for e in self.expand(ft, t[m.start():self.decl_end(t, m.start())]):
                    for a in ASSIGN_RE.finditer(e):
                        self.assign(ft, m.start(), a.group(1), a.group(2))
                    for a in ATOMIC_STORE_RE.finditer(e):
                        self.assign(ft, m.start(), a.group(1), a.group(2))
        # arguments: a function or a pointer passed to a parameter
        for start, end, _name, _params in ft.funcs:
            for m in CALL_RE.finditer(t, start, end):
                callee = m.group(1)
                if callee in KEYWORDS or callee not in self.params:
                    continue
                b = m.end() - 1
                e = match_close(t, b)
                args = t[b + 1:e]
                if not re.search(IDENT, args):
                    continue
                member = re.search(r"(?:\.|->)\s*$", t[max(start, m.start() - 4):m.start()])
                mkey = None
                if member:
                    c = re.search(r"(" + CHAIN + r")\s*$", t[max(start, m.start() - 160):m.end() - 1])
                    if c:
                        k = self.chain_key(ft, m.start(), c.group(1))
                        mkey = k if k[0] == "m" else None
                for i, a in enumerate(split_top(args, ",")):
                    for ref in self.value_refs(ft, b + 1, a):
                        if member:
                            self.member_calls.append((mkey, callee, i, ref))
                        else:
                            self.bind(("p", self.pname(ft.path, callee) if ft.path in self.defined_in.get(callee, ())
                                       else callee, i), ref)

    def assign(self, ft, off, lhs, rhs):
        for ref in self.value_refs(ft, off, rhs):
            self.bind(self.chain_key(ft, off, lhs), ref)

    # keys: what a pointer expression names

    def ident_type(self, ft, off, name):
        f = ft.enclosing(off)
        if f is not None:
            for pn, pt in f[3]:
                if pn == name:
                    return pt
        t = ft.code
        lo = f[0] if f is not None else 0
        pat = re.compile(r"\b(?:struct\s+|union\s+)?(" + IDENT + r")\s+(?:const\s+|volatile\s+)*(?:\*\s*(?:const\s+)?)*" +
                         re.escape(name) + r"\s*(?:\[[^\]]*\]\s*)*[;=,)]")
        for a, b in ((lo, off), (0, off)):
            best = None
            for m in pat.finditer(t, a, b):
                if self.struct(m.group(1)) is not None:
                    best = m.group(1)
            if best:
                return best
        return self.file_vars.get((ft.path, name)) or self.globals.get(name)

    def return_type(self, name):
        """The struct type a function returns a pointer to (its definition or declaration), else None"""
        pat = re.compile(r"\b(?:struct\s+)?(" + IDENT + r")\s*\*\s*(?:const\s+)?" + re.escape(name) + r"\s*\(")
        for ft in self.files.values():
            if name in ft.code:
                for m in pat.finditer(ft.code):
                    if self.struct(m.group(1)) is not None:
                        return m.group(1)
        return None

    def chain_key(self, ft, off, chain, base_type=None):
        """("m", type, field) / ("f", field) for a member, ("p", function, index) for a parameter, ("v", file, name)
        for a variable. base_type: the type of the chain's first part when it is a call's result"""
        chain = re.sub(r"\[[^\]]*\]", "", chain)
        parts = re.split(r"\s*(?:\.|->)\s*", chain.strip())
        if base_type is not None:
            parts = ["-"] + parts
        if len(parts) == 1:
            name = parts[0]
            f = ft.enclosing(off)
            if f is not None:
                for i, (pn, _pt) in enumerate(f[3]):
                    if pn == name:
                        return ("p", self.pname(ft.path, f[2]), i)
            return ("v", ft.path, name)
        typ = base_type or self.ident_type(ft, off, parts[0])
        for p in parts[1:-1]:
            st = self.struct(typ)
            mem = st.member(p) if st else None
            typ = mem[1] if mem else None
        st = self.struct(typ)
        if st is not None and st.member(parts[-1]) is not None:
            return ("m", self.resolve_type(typ), parts[-1])
        return ("f", parts[-1])

    def targets(self, key, opaque=None):
        """Functions a pointer may hold, following stores of one pointer into another. opaque (a list): set when a
        store along the way came from something the sources can't follow"""
        out, seen, todo = set(), set(), [key]
        while todo:
            k = todo.pop()
            if k in seen:
                continue
            seen.add(k)
            for ref in self.binds.get(k, ()):
                if ref[0] == "fns":
                    out.update(ref[1])
                elif ref[0] == "opaque":
                    if opaque is not None:
                        opaque.append(k)
                elif ref[1] is not None:
                    todo.append(ref[1])
        return out

    # call sites

    def expand(self, ft, text):
        """Call site text with a leading macro expanded (the file's macros, then the headers')"""
        for _round in range(3):
            m = re.match(r"\s*(\(?\s*\*?\s*)(" + IDENT + r")", text)
            if not m:
                return [text]
            name = m.group(2)
            defs = ft.macros.get(name)
            if defs is None:
                for other in self.files.values():
                    if name in other.macros:
                        defs = other.macros[name]
                        break
            if not defs:
                return [text]
            outs = []
            for params, body in defs:
                rest = text[m.end():]
                if params is not None:
                    rest = rest.lstrip()
                    if not rest.startswith("("):
                        continue
                    e = match_close(rest, 0)
                    args = split_top(rest[1:e], ",")
                    for p, a in zip(params, args):
                        body = re.sub(r"\b%s\b" % re.escape(p), a.strip(), body)
                    rest = rest[e + 1:]
                outs.append(m.group(1) + body + rest)
            if len(outs) != 1:
                return outs or [text]
            text = outs[0]
        return [text]

    def call_site(self, loc):
        """(keys, expression text) of the indirect call at 'file:line:col'"""
        m = LOC_RE.match(loc or "")
        if not m:
            return [], "?"
        path = m.group(1)
        ft = self.files.get(path)
        if ft is None:
            raw = self.read(path)
            if raw is None:
                return [], loc
            ft = self.files[path] = FileText(path, raw)
        off = ft.offset(int(m.group(2)), int(m.group(3)))
        if off is None:
            return [], loc
        line_end = ft.code.find(";", off)
        text = ft.code[off:line_end if line_end >= 0 else len(ft.code)]
        keys = []
        shown = []
        for t in self.expand(ft, text):
            self.callees(ft, off, t, keys, shown)
        return list(dict.fromkeys(keys)), re.sub(r"\s+", " ", shown[0] if shown else text.strip()[:60])

    def visible(self, name, ft):
        """Titles a name in ft can mean as a function. A file-scope variable of that name shadows another file's
        function. LTO titles a global it made local like a static (partition:name): the sources tell them apart"""
        if (ft.path, name) in self.file_vars:
            return []
        ts = self.g.visible(name, ft.path)
        if ts:
            return ts
        files = self.global_in.get(name, ())
        return [t for t in self.g.by_name.get(name, []) if self.g.funcs[t].file in files]

    def is_function(self, name, ft):
        """A name in ft that calls a function directly: one of the file's own, or a global one"""
        if (ft.path, name) in self.file_vars:
            return False
        return ft.path in self.defined_in.get(name, ()) or bool(self.visible(name, ft))

    def callees(self, ft, off, t, keys, shown, depth=0):
        """Keys of the pointer calls in the call expression at the start of t. GCC puts a pointer call made in an
        argument (f(x, p->g())) at the outer call's location: when the outer callee is a function, its arguments are
        searched"""
        c = CALLBASE_RE.match(t)
        if c:		# through the pointer a function returns: (current())->op()
            keys.append(self.chain_key(ft, off, c.group(2), self.return_type(c.group(1)) or "?"))
            shown.append("%s()%s" % (c.group(1), re.sub(r"\s+", "", c.group(2))))
            return
        c = CALLEE_RE.match(t)
        if not c:
            return
        chain = c.group(1)
        if not (re.fullmatch(IDENT, chain) and self.is_function(chain, ft) and
                self.chain_key(ft, off, chain)[0] != "p") or depth > 3:
            keys.append(self.chain_key(ft, off, chain))
            shown.append(chain)
            return
        b = c.end() - 1
        args = t[b + 1:match_close(t, b)]
        for m in re.finditer(r"(?<![\w.>])\(?\s*\*?\s*" + CHAIN + r"\s*\)?\s*\(", args):
            name = re.match(r"\(?\s*\*?\s*(" + IDENT + r")", m.group(0)).group(1)
            if name in KEYWORDS:
                continue
            sub_keys = []
            self.callees(ft, off, args[m.start():], sub_keys, shown, depth + 1)
            keys.extend(sub_keys)


def compiled_sources(build, root):
    """Source files of the objects in a build directory (build/x/core/ui/ui.o or .d -> core/ui/ui.c), and the headers
    their dependency files list"""
    srcs, headers = set(), set()
    for r, _dirs, files in os.walk(build):
        for fn in files:
            stem, ext = os.path.splitext(fn)
            if ext not in (".o", ".d"):
                continue
            rel = os.path.relpath(os.path.join(r, stem + ".c"), build)
            if os.path.isfile(os.path.join(root, rel)):
                srcs.add(rel)
            if ext == ".d":
                with open(os.path.join(r, fn), encoding="utf-8", errors="replace") as fh:
                    for w in fh.read().replace("\\\n", " ").split():
                        if w.endswith(".h") and not os.path.isabs(w):
                            headers.add(os.path.normpath(w))
    return sorted(srcs), sorted(headers)


# ── tasks ────────────────────────────────────────────────────────────────────

def eval_c_int(expr, macros, depth=0):
    """A C integer constant expression after macro expansion (casts and U/L suffixes dropped)"""
    if depth > 20:
        raise ValueError(expr)

    def sub(m):
        name = m.group(0)
        if name in macros:
            return "(" + str(eval_c_int(macros[name], macros, depth + 1)) + ")"
        return name
    e = re.sub(r"\(\s*(?:const\s+)?(?:unsigned\s+|signed\s+)?(?:u?int\d+_t|size_t|configSTACK_DEPTH_TYPE|"
               r"UBaseType_t|BaseType_t|StackType_t|int|long|short|char)\s*\)", "", expr)
    e = re.sub(r"\b(0[xX][0-9A-Fa-f]+|\d+)[uUlL]+\b", r"\1", e)
    e = re.sub(IDENT, sub, e)
    if not re.fullmatch(r"[\s0-9xXa-fA-F()+\-*/%<>|&~]*", e):
        raise ValueError(expr)
    return int(eval(e.replace("/", "//"), {"__builtins__": {}}, {}))


def preprocessor_macros(cc, cflags, root, path):
    drop = ("-MMD", "-MP", "-MD", "-fstack-usage")
    flags = [f for f in shlex.split(cflags) if f not in drop and not f.startswith("-fcallgraph-info") and
             not f.startswith("-flto")]
    out = subprocess.run(shlex.split(cc) + flags + ["-E", "-dM", path], cwd=root, capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("stack_check: the preprocessor failed on %s:\n%s" % (path, out.stderr[-2000:]))
    macros = {}
    for line in out.stdout.splitlines():
        m = re.match(r"#define\s+(" + IDENT + r")\s*(.*)$", line)
        if m:
            macros[m.group(1)] = m.group(2).strip()
    return macros


def find_tasks(graph, root, files, table, cc, cflags, word):
    """[(entry title, task name, stack bytes, where, expression)] from the xTaskCreate calls in the compiled sources
    and the table's `task` lines"""
    tasks = []
    macro_cache = {}

    def stack_bytes(expr, path):
        if path not in macro_cache:
            macro_cache[path] = preprocessor_macros(cc, cflags, root, path) if cc else {}
        try:
            return eval_c_int(expr, macro_cache[path]) * word
        except (ValueError, SyntaxError, ZeroDivisionError):
            return None

    seen = {}
    for path in files:
        try:
            with open(os.path.join(root, path), encoding="utf-8", errors="replace") as fh:
                t = fh.read()
        except OSError:
            continue
        if "xTaskCreate" not in t:
            continue
        t = strip_comments(t, keep_strings=True)
        for m in TASK_RE.finditer(t):
            fn, name, expr = m.group(1), m.group(2), m.group(3)
            name = name[1:-1] if name.startswith('"') else fn
            titles = graph.lookup(fn, path)
            if not titles or fn in table.notask:
                continue	# a variable holding the entry (FreeRTOS' idle task): the table names it
            for title in titles:
                where = "%s:%d" % (path, t.count("\n", 0, m.start()) + 1)
                row = (title, name, stack_bytes(expr, path), where, expr)
                if title not in seen:
                    seen[title] = len(tasks)
                    tasks.append(row)
                elif tasks[seen[title]][2] is None:	# FreeRTOS' static and dynamic variants: the one that evaluates
                    tasks[seen[title]] = row
    for fn, expr in table.task:
        for title in graph.lookup_spec(fn):
            if title not in seen:
                seen[title] = len(tasks)
                f = graph.funcs[title]
                tasks.append((title, f.name, stack_bytes(expr, f.file), "table", expr))
    return tasks


# ── code without a call graph: the disassembly ───────────────────────────────

REGLIST_RE = re.compile(r"([a-z]+)(\d+)\s*-\s*[a-z]*(\d+)")


def reg_count(text):
    """Registers in a list: 'r4, r5, lr', 'r4-r7, lr', 's16-s31' (C-SKY: 'r4-r11, r15')"""
    n = 0
    for r in text.split(","):
        r = r.strip()
        m = REGLIST_RE.fullmatch(r)
        if m:
            n += int(m.group(3)) - int(m.group(2)) + 1
        elif r:
            n += 1
    return n


class Disasm:
    """Frames and direct calls of the functions in an ELF, from objdump: the C library, the compiler's runtime and
    assembly, which have no call graph. ARM Thumb (push, stmdb sp!, vpush, sub sp) and C-SKY (push, subi sp). Every
    stack adjustment in a function is added (more than one path's), a call through a register is not seen"""
    CALL_RE = re.compile(r"^(?:bl|blx|b|b\.w|bsr|jbsr|jsri|jmpi|br|jbr)\s+(?:0x)?([0-9a-f]+)\b[^<]*<([^>+]+)>$")

    @classmethod
    def from_elf(cls, objdump, elf, fixed=None):
        dis = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf], capture_output=True, text=True)
        sym = subprocess.run([objdump, "-t", elf], capture_output=True, text=True)
        if dis.returncode != 0 or sym.returncode != 0:
            raise SystemExit("stack_check: %s failed on %s" % (objdump, elf))
        return cls(dis.stdout, sym.stdout, fixed)

    def __init__(self, dis, sym, fixed=None):
        self.funcs = {}		# address -> [name, frame, set of called addresses]
        self.addr = {}		# symbol name -> address
        self.memo = {}
        self.fixed = fixed or {}	# symbol -> bytes, not followed (the table's lib lines)
        cur = None
        for line in dis.splitlines():
            m = re.match(r"^([0-9a-f]+) <([^>]+)>:$", line)
            if m:
                cur = [m.group(2), 0, set()]
                self.funcs[int(m.group(1), 16)] = cur
                continue
            if cur is None or ":\t" not in line:
                continue
            ins = re.sub(r"\s+", " ", line.split(":\t", 1)[1].split("//")[0].split(";")[0]).strip()
            ins_full = re.sub(r"\s+", " ", line.split(":\t", 1)[1]).strip()
            m = re.match(r"^(?:push(?:\.w)?|stmdb(?:\.w)? sp!,) \{([^}]*)\}", ins)
            if m:
                cur[1] += 4 * reg_count(m.group(1))
                continue
            m = re.match(r"^vpush(?:\.\d+)? \{([^}]*)\}", ins)
            if m:
                cur[1] += (8 if m.group(1).strip().startswith("d") else 4) * reg_count(m.group(1))
                continue
            m = re.match(r"^sub(?:w|\.w|s)? sp, (?:sp, )?#(\d+)", ins)
            if m:
                cur[1] += int(m.group(1))
                continue
            m = re.match(r"^push ([r0-9, \-]+)$", ins)
            if m:
                cur[1] += 4 * reg_count(m.group(1))
                continue
            m = re.match(r"^subi (?:sp|r14), (?:(?:sp|r14), )?(\d+)$", ins)
            if m:
                cur[1] += int(m.group(1))
                continue
            m = self.CALL_RE.match(ins_full)
            if m:
                cur[2].add(int(m.group(1), 16))
        for line in sym.splitlines():
            m = re.match(r"^([0-9a-f]+)\s.*\s(\S+)$", line)
            if m and " F " in line:
                self.addr.setdefault(m.group(2), int(m.group(1), 16) & ~1)

    def depth(self, name):
        """(bytes, [names]) of the deepest chain from a symbol, or None when the ELF hasn't it"""
        a = self.addr.get(name)
        if a is None or a not in self.funcs:
            return None
        return self._depth(a, ())

    def _depth(self, a, stack):
        if a in self.memo:
            return self.memo[a]
        f = self.funcs.get(a)
        if f is None or a in stack:
            return (0, [])
        if f[0] in self.fixed:
            return (self.fixed[f[0]], [f[0]])
        best = (0, [])
        for c in f[2]:
            if c != a:
                d = self._depth(c, stack + (a,))
                if d[0] > best[0]:
                    best = d
        r = (f[1] + best[0], [f[0]] + best[1])
        if not stack or a not in stack:
            self.memo[a] = r
        return r


# ── the search ───────────────────────────────────────────────────────────────

class Analysis:
    def __init__(self, graph, model, table, lib_default, disasm=None):
        self.g, self.model, self.table = graph, model, table
        self.lib_default = lib_default
        self.disasm = disasm
        self.lib_path = {}		# function without call graph -> its deepest chain in the disassembly
        self.memo = {}			# title -> (bytes, [(title, call site, how)])
        self.unresolved = {}		# (caller title, call site) -> expression
        self.approx = {}		# (caller title, call site) -> expression: resolved by the field name only
        self.never = {}			# (caller title, call site) -> expression: a field nothing stores into
        self.recursion = set()		# titles closing a cycle
        self.assumed = {}		# functions without call graph -> bytes counted
        self.cache = {}

    def frame(self, title):
        f = self.g.funcs.get(title)
        if f is not None and f.frame is not None:
            return f.frame
        if title.startswith("@"):
            return self.table.extern[title[1:]]
        name = f.name if f else title
        for n in (name, title):
            if n in self.table.lib:
                return self.table.lib[n]
        if self.disasm is not None:
            d = self.disasm.depth(LTRANS_RE.sub("", title))
            if d is None and name != title:
                d = self.disasm.depth(name)
            if d is not None:
                self.lib_path[title] = d[1]
                return d[0]
        self.assumed[name] = self.lib_default
        return self.lib_default

    def indirect_targets(self, caller, loc):
        """Functions an indirect call may reach: the table's, and what the sources store into the pointer. A pointer
        whose stores the sources show but that never holds a function of this build is never called (a struct field
        of a known type nothing stores into: an optional operation that is NULL on this radio; a callback whose setter
        nothing in this build calls). A pointer with no store in sight is unresolved"""
        key = (caller, loc)
        if key in self.cache:
            return self.cache[key]
        f = self.g.funcs.get(caller)
        cname = f.name if f else caller
        out = set()
        for spec in self.table.call.get(cname, []):
            out.update(self.table_targets(spec))
        keys, expr = self.model.call_site(loc)
        never = bool(keys)
        for k in keys:
            if k[0] == "p":
                names = self.model.params_of.get(k[1], [])
                last = names[k[2]] if k[2] < len(names) else None
            else:
                last = k[-1]
            for spec in self.table.ptr.get(last, []):
                out.update(self.table_targets(spec))
            traced = True		# what the sources store into it is known: nothing found means never called
            opaque = []
            if k[0] == "m":
                found = self.model.targets(k, opaque) | self.model.targets(("u", k[2]), opaque)
            elif k[0] == "f":
                found = self.model.targets(k)
                traced = False
                if found:
                    self.approx[key] = expr
            elif k[0] == "v":
                found = self.model.targets(k, opaque) or self.model.targets(("g", k[2]), opaque)
                traced = k in self.model.binds or ("g", k[2]) in self.model.binds
            else:
                found = self.model.targets(k, opaque)
                traced = k in self.model.binds
            never = never and traced and not opaque and not found
            out.update(found)
        if not out and not never:
            for spec in self.table.file.get(LOC_RE.sub(r"\1", loc or ""), []):
                out.update(self.table_targets(spec))
        if not out and cname not in self.table.call:
            (self.never if never else self.unresolved)[key] = expr
        self.cache[key] = out
        return out

    def table_targets(self, spec):
        if spec.startswith("@"):
            if spec[1:] not in self.table.extern:
                raise SystemExit("stack_check: %s is not an extern in the table" % spec)
            return {spec}
        return set(self.g.lookup_spec(spec))

    def calls(self, title):
        for dst, loc in self.g.edges.get(title, []):
            if dst == INDIRECT:
                for t in sorted(self.indirect_targets(title, loc)):
                    yield t, loc
            else:
                yield dst, loc

    def depth(self, root):
        """Deepest chain from root: (bytes, path [(title, call site of the next step)]); an iterative depth-first
        search, each cycle cut where it closes (and recorded)"""
        if root in self.memo:
            return self.memo[root]
        stack = [(root, iter(list(self.calls(root))), None)]	# (title, its calls, call site in the parent)
        onstack = {root}
        best = {root: (0, None, None)}		# title -> (deepest callee bytes, callee, call site)
        while stack:
            title, it, _site = stack[-1]
            pushed = False
            for dst, loc in it:
                if dst in onstack:
                    self.recursion.add(dst)
                elif dst in self.memo:
                    if self.memo[dst][0] > best[title][0]:
                        best[title] = (self.memo[dst][0], dst, loc)
                else:
                    best[dst] = (0, None, None)
                    onstack.add(dst)
                    stack.append((dst, iter(list(self.calls(dst))), loc))
                    pushed = True
                    break
            if pushed:
                continue
            stack.pop()
            onstack.discard(title)
            b, nxt, loc = best[title]
            path = [(title, loc)] + (self.memo[nxt][1] if nxt is not None else [])
            self.memo[title] = (self.frame(title) + b, path)
            if stack:
                parent = stack[-1][0]
                if self.memo[title][0] > best[parent][0]:
                    best[parent] = (self.memo[title][0], title, _site)
        return self.memo[root]

    def reachable(self, root):
        seen = {root}
        todo = [root]
        while todo:
            t = todo.pop()
            for dst, _loc in self.calls(t):
                if dst not in seen:
                    seen.add(dst)
                    todo.append(dst)
        return seen


def run(args):
    root = os.path.abspath(args.root)
    graph = Graph()
    if graph.load_dir(args.build) == 0:
        print("stack_check: no .ci files in %s (the build needs -fcallgraph-info=su)" % args.build, file=sys.stderr)
        return 2
    table = Table()
    if args.edges and os.path.exists(args.edges):
        table.load(args.edges)
    files, headers = compiled_sources(args.build, root)
    model = CModel(graph, root, files, headers)
    tasks = find_tasks(graph, root, files, table, args.cc, args.cflags or "", args.word)
    if not tasks:
        print("stack_check: no tasks found", file=sys.stderr)
        return 2
    disasm = None
    if args.objdump and args.elf:
        disasm = Disasm.from_elf(args.objdump, args.elf, table.lib)
    an = Analysis(graph, model, table, args.lib_default, disasm)
    label = args.label or os.path.basename(os.path.normpath(args.build))
    fails = 0
    print("stack check %s: port overhead %d bytes, margin %d bytes, %d tasks (bytes)" %
          (label, args.overhead, args.margin, len(tasks)))
    reach = set()
    least = None
    for title, name, limit, where, expr in tasks:
        worst, path = an.depth(title)
        reach |= an.reachable(title)
        need = worst + args.overhead + args.margin
        if limit is None:
            print("  %-8s ?    stack size '%s' at %s not evaluated" % (name, expr, where))
            fails += 1
            continue
        free = limit - need
        if least is None or free < least[0]:
            least = (free, name)
        status = "ok" if free >= 0 else "OVER"
        fails += free < 0
        print("  %-8s %-4s stack %5d  deepest %5d  free %5d  (%s, %s)" %
              (name, status, limit, worst, free, graph.display(title), where))
        if free < 0 or args.verbose:
            prev = None
            for t, loc in path[:args.path_len]:
                f = graph.funcs.get(t)
                mark = ""
                if prev is not None and prev in an.cache:	# reached through a function pointer
                    mark = "  (pointer%s)" % (", by field name" if prev in an.approx else "")
                prev = (t, loc)
                where = f.file if f and f.file else "-"
                if t in an.lib_path:
                    where = "disassembly: " + " > ".join(an.lib_path[t][:6]) + (" > ..." if len(an.lib_path[t]) > 6 else "")
                print("      %5d  %-28s %s%s" % (an.frame(t), graph.display(t), where, mark))
            if len(path) > args.path_len:
                print("      ... %d more" % (len(path) - args.path_len))
        else:
            print("      " + " > ".join(graph.display(t) for t, _l in path[:args.path_len]) +
                  (" > ..." if len(path) > args.path_len else ""))
    unres = sorted((graph.display(c), loc, e) for (c, loc), e in an.unresolved.items() if c in reach)
    if unres:
        print("  unresolved pointer calls reachable from a task (%d; tools/stack_check_edges.txt can name targets):"
              % len(unres))
        for c, loc, e in unres[:args.max_unresolved]:
            print("      %-28s %-44s %s" % (c, loc, e))
        if len(unres) > args.max_unresolved:
            print("      ... %d more" % (len(unres) - args.max_unresolved))
    if args.verbose:
        ap = sorted((graph.display(c), loc, e) for (c, loc), e in an.approx.items() if c in reach)
        if ap:
            print("  resolved by the field name alone (type unknown at the call site, %d):" % len(ap))
            for c, loc, e in ap:
                print("      %-28s %-44s %s" % (c, loc, e))
    nv = sorted((graph.display(c), loc, e) for (c, loc), e in an.never.items() if c in reach)
    if nv and args.verbose:
        print("  fields no function is stored into in this build (calls skipped, %d):" % len(nv))
        for c, loc, e in nv:
            print("      %-28s %-44s %s" % (c, loc, e))
    rec = sorted({graph.display(t) for t in an.recursion if t in reach} - table.recursion)
    if rec:
        print("  recursion (each cycle counted once): " + ", ".join(rec))
    dyn = sorted({graph.display(t) for t in reach if graph.funcs.get(t) and graph.funcs[t].qual and
                  graph.funcs[t].qual != "static"})
    if dyn:
        print("  dynamic frames (alloca or a variable length array; the fixed part counted): " + ", ".join(dyn))
    lib = sorted(n for n in an.assumed if n not in table.lib)
    if lib and args.verbose:
        print("  no call graph, %d bytes assumed each: %s" % (args.lib_default, ", ".join(lib)))
    elif lib:
        print("  %d functions without a call graph counted as %d bytes each (-v lists them)" % (len(lib), args.lib_default))
    if fails:
        print("stack check %s: %d task(s) over%s" % (label, fails, " (report only)" if args.report_only else ""))
        return 0 if args.report_only else 1
    print("stack check %s: every task fits, the least free: %s %d bytes" % (label, least[1], least[0]))
    return 0


# ── self-test ────────────────────────────────────────────────────────────────

SELF_H = """\
typedef struct { int (*go)(int); void (*stop)(void); } ops_t;
typedef struct { const ops_t *ops; int n; } dev_t;
extern const dev_t dev;
typedef void (*cb_t)(void);
"""

SELF_C = """\
#include "t.h"
#define T_STACK (64 + 16)
#define LOCK() lock_fn()
static int deep(int x) { char b[200]; (void)b; return x; }
static void leaf(void) { }
static void other(void) { }
static int shallow(int x) { return x; }
static const ops_t ops = { .go = deep, .stop = leaf };
static const ops_t ops2 = { shallow, other };
const dev_t dev = { &ops, 1 };
static cb_t lock_fn;
static void rec(int n) { if (n) rec(n - 1); }
static void run_cb(cb_t cb) { cb(); }
static void task_a(void *p) { dev.ops->go(1); rec(3); }
static void task_b(void *p) { run_cb(leaf); LOCK(); }
void start(void)
{
	lock_fn = other;
	xTaskCreate(task_a, "a", T_STACK, NULL, 1, NULL);
	xTaskCreate(task_b, "b", 32, NULL, 1, NULL);	/* "c" */
}
static int add(int a, int b) { return a + b; }
static void task_c(void *p) { add(1, dev.ops->go(2)); }
"""

SELF_CI = """\
graph: { title: "t.c"
node: { title: "t.c:deep" label: "deep\\nt.c:4:12\\n208 bytes (static)" }
node: { title: "t.c:leaf" label: "leaf\\nt.c:5:13\\n0 bytes (static)" }
node: { title: "t.c:other" label: "other\\nt.c:6:13\\n4 bytes (static)" }
node: { title: "t.c:shallow" label: "shallow\\nt.c:7:12\\n0 bytes (static)" }
node: { title: "t.c:rec" label: "rec\\nt.c:12:13\\n8 bytes (static)" }
edge: { sourcename: "t.c:rec" targetname: "t.c:rec" label: "t.c:12:33" }
node: { title: "t.c:run_cb" label: "run_cb\\nt.c:13:13\\n8 bytes (static)" }
node: { title: "__indirect_call" label: "Indirect Call Placeholder" shape : ellipse }
edge: { sourcename: "t.c:run_cb" targetname: "__indirect_call" label: "t.c:13:31" }
node: { title: "t.c:task_a" label: "task_a\\nt.c:14:13\\n8 bytes (static)" }
edge: { sourcename: "t.c:task_a" targetname: "__indirect_call" label: "t.c:14:31" }
edge: { sourcename: "t.c:task_a" targetname: "t.c:rec" label: "t.c:14:47" }
node: { title: "t.c:task_b" label: "task_b\\nt.c:15:13\\n16 bytes (static)" }
edge: { sourcename: "t.c:task_b" targetname: "t.c:run_cb" label: "t.c:15:31" }
edge: { sourcename: "t.c:task_b" targetname: "__indirect_call" label: "t.c:15:45" }
node: { title: "start" label: "start\\nt.c:16:6\\n8 bytes (dynamic,bounded)" }
node: { title: "xTaskCreate" label: "xTaskCreate\\nt.h:1:6" shape : ellipse }
edge: { sourcename: "start" targetname: "xTaskCreate" label: "t.c:19:2" }
}
"""


SELF_ARM = """\
080549f4 <sniprintf>:
 80549f4:\tpush\t{r2, r3}
 80549f6:\tpush\t{r4, lr}
 80549f8:\tsub\tsp, #136\t@ 0x88
 80549fa:\tbl\t8054b64 <_svfiprintf_r>
 80549fe:\tbne.n\t8054a10 <sniprintf+0x1c>

08054b64 <_svfiprintf_r>:
 8054b64:\tstmdb\tsp!, {r4, r5, r6, r7, r8, r9, sl, fp, ip}
 8054b68:\tvpush\t{s16-s31}
 8054b6c:\tbx\tlr
"""

SELF_SYM = """\
080549f4 g     F .text\t00000040 sniprintf
080549f4 g     F .text\t00000040 snprintf
08054b64 g     F .text\t00000100 _svfiprintf_r
"""

SELF_CSKY = """\
0304b504 <sinf>:
 304b504:\tpush      \tr4, r15
 304b506:\tsubi      \tr14, r14, 8
 304b51e:\tbsr      \t0x304ed8c\t// 304ed8c <__subsf3>
 304b522:\tbt      \t0x304b548\t// 304b548 <sinf+0x44>
"""


def self_test():
    ok = True

    def check(cond, what):
        nonlocal ok
        print("%s %s" % ("ok  " if cond else "FAIL", what))
        ok = ok and cond

    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "build"))
        for name, text in (("t.c", SELF_C), ("t.h", SELF_H)):
            with open(os.path.join(d, name), "w") as fh:
                fh.write(text)
        with open(os.path.join(d, "build", "t.ci"), "w") as fh:
            fh.write(SELF_CI)
        with open(os.path.join(d, "build", "t.d"), "w") as fh:
            fh.write("build/t.o: t.c t.h\n")
        g = Graph()
        check(g.load_dir(os.path.join(d, "build")) == 1, "one .ci file read")
        check(g.funcs["t.c:deep"].frame == 208 and g.funcs["t.c:deep"].name == "deep", "frame and name from a label")
        check(not g.defined("xTaskCreate"), "a declaration has no frame")
        check(g.funcs["start"].qual == "dynamic,bounded", "a bounded dynamic frame")
        check(base_name("find_name.constprop.0") == "find_name", "clone suffixes dropped")
        files, headers = compiled_sources(os.path.join(d, "build"), d)
        check(files == ["t.c"] and headers == ["t.h"], "sources and headers from the dependency file")
        model = CModel(g, d, files, headers)
        check(model.struct("dev_t").member("ops")[1] == "ops_t", "a struct member's type")
        check(model.targets(("m", model.resolve_type("ops_t"), "go")) == {"t.c:deep", "t.c:shallow"},
              "designated and positional initializers bind a member")
        keys, _e = model.call_site("t.c:14:31")
        check(keys == [("m", model.resolve_type("ops_t"), "go")], "a call through board-style members: %s" % keys)
        keys, _e = model.call_site("t.c:23:31")
        check(keys == [("m", model.resolve_type("ops_t"), "go")], "a pointer call in an argument, at the outer call")
        keys, _e = model.call_site("t.c:13:31")
        check(keys == [("p", "run_cb", 0)] and model.targets(keys[0]) == {"t.c:leaf"}, "a parameter bound by a call")
        keys, _e = model.call_site("t.c:15:45")
        check(model.targets(keys[0]) == {"t.c:other"}, "a macro expanded to a variable bound by an assignment")
        check(eval_c_int("( ( uint16_t ) 128U )", {}) == 128, "a cast and a suffix")
        check(eval_c_int("T_STACK * 2", {"T_STACK": "(64 + 16)"}) == 160, "a macro expanded")
        tasks = find_tasks(g, d, files, Table(), None, "", 4)
        check([(t[1], t[2]) for t in tasks] == [("a", None), ("b", 128)],
              "tasks found (without the preprocessor a macro stack stays unknown)")
        an = Analysis(g, model, Table(), DEFAULT_LIB_BYTES)
        b, path = an.depth("t.c:task_a")
        check(b == 8 + 208 and [t for t, _l in path] == ["t.c:task_a", "t.c:deep"],
              "the deepest path goes through the pointer: %d" % b)
        check("t.c:rec" in an.recursion, "recursion found")
        b, path = an.depth("t.c:task_b")
        check(b == 16 + 8 and [t for t, _l in path] == ["t.c:task_b", "t.c:run_cb"],
              "a callback parameter and a macro call: %d" % b)
        model.binds.clear()
        an2 = Analysis(g, model, Table(), DEFAULT_LIB_BYTES)
        an2.depth("t.c:task_a")
        an2.depth("t.c:task_b")
        check(any(loc == "t.c:14:31" for (_c, loc) in an2.never), "a typed field nothing stores into is never called")
        check(any(loc == "t.c:15:45" for (_c, loc) in an2.unresolved), "an unbound pointer variable is reported")
        ft = FileText("u.c", "static bool (*start)(void);\nstatic void a(void)\n{\n}\nvoid b(void)\n{\n}\n")
        check(ft.statics == {"a"}, "static definitions told from global ones: %s" % ft.statics)
        model.scan_types(ft)
        check(not model.is_function("start", ft) and model.is_function("start", FileText("w.c", "")),
              "a file's pointer variable shadows another file's function of that name")
        t = Table()
        t.ptr["go"] = ["leaf"]
        an3 = Analysis(g, model, t, DEFAULT_LIB_BYTES)
        check(an3.depth("t.c:task_a")[0] == 8 + 8, "a table entry names the target")
        # two files' static start(): a direct call and a call through a struct field bind each one's own parameters
        for name, text in (("x.c", "typedef struct { void (*start)(void (*cb)(void), int n); } aud_t;\n"
                                   "static void fa(void) {}\nstatic void start(void (*cb)(void), int n) { cb(); }\n"
                                   "const aud_t aud = { .start = start };\nvoid use(const aud_t *a) { a->start(fa, 0); }\n"),
                           ("y.c", "static void fb(void) {}\nstatic void start(void (*cb)(void), int n) { cb(); }\n"
                                   "void go(void) { start(fb, 0); }\n")):
            with open(os.path.join(d, name), "w") as fh:
                fh.write(text)
        g2 = Graph()
        for f, fn, line in (("x", "fa", 2), ("x", "start", 3), ("y", "fb", 1), ("y", "start", 2)):
            parse_ci('node: { title: "%s.c:%s" label: "%s\\n%s.c:%d:13\\n8 bytes (static)" }\n' % (f, fn, fn, f, line),
                     g2.funcs, g2.edges)
        g2.index()
        m2 = CModel(g2, d, ["x.c", "y.c"], [])
        check(m2.targets(("p", "x.c:start", 0)) == {"x.c:fa"} and m2.targets(("p", "y.c:start", 0)) == {"y.c:fb"},
              "static functions of one name keep their own parameters: %s %s" %
              (m2.targets(("p", "x.c:start", 0)), m2.targets(("p", "y.c:start", 0))))
    dis = Disasm(SELF_ARM, SELF_SYM)
    check(dis.depth("snprintf") == (8 + 8 + 136 + 4 * 9 + 4 * 16, ["sniprintf", "_svfiprintf_r"]),
          "Thumb disassembly: push, sub sp, vpush, a call, an alias: %s" % (dis.depth("snprintf"),))
    dis = Disasm(SELF_ARM, SELF_SYM, {"_svfiprintf_r": 10})
    check(dis.depth("snprintf")[0] == 8 + 8 + 136 + 10, "a lib line fixes a function inside the disassembly")
    dis = Disasm(SELF_CSKY, "")
    check(dis.depth("sinf") is None and dis.funcs[0x304b504][1] == 16 and dis.funcs[0x304b504][2] == {0x304ed8c},
          "C-SKY disassembly: push, subi, bsr")
    print("stack_check self-test: %s" % ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", help="build directory with the .ci files and objects")
    ap.add_argument("--root", default=".", help="source tree root (default: the current directory)")
    ap.add_argument("--cc", help="the compiler, to evaluate stack size macros with its preprocessor")
    ap.add_argument("--cflags", help="the build's compile flags (defines and include paths)")
    ap.add_argument("--objdump", help="objdump for the target: frames of code without a call graph (with --elf)")
    ap.add_argument("--elf", help="the linked image")
    ap.add_argument("--edges", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                     "stack_check_edges.txt"))
    ap.add_argument("--overhead", type=int, default=0, help="bytes the RTOS port saves on a task stack")
    ap.add_argument("--margin", type=int, default=128, help="bytes that must stay free (default 128)")
    ap.add_argument("--word", type=int, default=4, help="bytes per stack word (default 4)")
    ap.add_argument("--lib-default", type=int, default=DEFAULT_LIB_BYTES,
                    help="bytes for a function without call graph that the table doesn't list (default %d)" %
                    DEFAULT_LIB_BYTES)
    ap.add_argument("--label", help="name in the report (default: the build directory's)")
    ap.add_argument("--report-only", action="store_true", help="report tasks over their stack, exit 0")
    ap.add_argument("--path-len", type=int, default=40)
    ap.add_argument("--max-unresolved", type=int, default=400)
    ap.add_argument("-v", "--verbose", action="store_true", help="every task's path with frames, and more")
    ap.add_argument("--self-test", action="store_true", help="check the parser and the search on a fixture")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.build:
        ap.error("--build is required (or --self-test)")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
