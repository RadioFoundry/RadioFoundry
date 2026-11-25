#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Build once: the host tests' compiler front end (tests/host/Makefile sets CC to it).

    hostcc.py CC [compiler arguments]

A rule's `cc FLAGS -o test a.c b.c -lm` becomes one compile per source into out/obj/, then the link, and tests share
an object wherever that gives the same object as compiling it for them:

- Same flags but the -D / -U switches (include paths, warnings, sanitizers, optimization: compared exactly), and
- the -D / -U switches that differ between the two change no line the compiler gets from the source and the headers
  it included (the compiler's -MMD list): no such line names one of them or a macro defined from one (core/config.h's
  defaults: CONFIG_DMR_TMS from CONFIG_DMR_SMS), and none sits under a conditional on one. None of them may be a
  reserved name (_X, NDEBUG), which system headers read. So the core files a test builds with CONFIG_P25=1 and one
  built without it share every object but the few that CONFIG_P25 changes.

An object is rebuilt when it is missing or older than its source or a header it included. Tests built in parallel
(make -j) wait for each other through a lock per source instead of compiling one twice; a new object is written to a
temporary name and renamed.

Anything else (no -o, no .c source, or -c / -E / -S / -M given) goes to the compiler unchanged. HOSTCC_DIRECT=1 in
the environment turns the front end off. Each link's build time goes to out/times/build/<test> for `make times`.
"""
import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OBJ_DIR = os.path.join("out", "obj")
TIMES_DIR = os.path.join("out", "times", "build")

# Compile-only flags, left off the link; those that take the next argument as their value are in TAKES_VALUE
COMPILE_ONLY = ("-I", "-D", "-U", "-std=", "-include", "-isystem", "-iquote", "-W", "-M")
TAKES_VALUE = ("-I", "-D", "-U", "-include", "-isystem", "-iquote", "-MF", "-MT", "-MQ", "-x")
PASS_THROUGH = ("-c", "-E", "-S", "-M", "-MM", "-x")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# A ## paste in a macro: the literal parts next to it (IPGW_##range##_NET: "IPGW_" before, "_NET" after)
PASTE = re.compile(r"([A-Za-z0-9_]*)\s*##\s*([A-Za-z0-9_]*)")


def run(argv):
	return subprocess.call(argv)


def parse(args):
	"""(out, flags, defs, sources, link): defs the -D / -U switches in order, link the arguments with each source
	replaced by its index"""
	flags, defs, sources, link, out = [], [], [], [], None
	i = 0
	while i < len(args):
		a = args[i]
		if a == "-o" and i + 1 < len(args):
			out = args[i + 1]
			link += [a, out]
			i += 2
			continue
		if a in TAKES_VALUE and i + 1 < len(args):
			a = a + args[i + 1]
			i += 1
		if a.startswith(("-D", "-U")):
			defs.append(a)
		elif a.endswith(".c") and not a.startswith("-"):
			sources.append(a)
			link.append(len(sources) - 1)
		elif a.startswith(("-l", "-L", "-Wl,")) or a.endswith((".o", ".a")):
			link.append(a)
		else:
			flags.append(a)
		i += 1
	return out, flags, defs, sources, link


def macro_table(defs):
	"""{name: definition}: -DX=1 and -DX both define X, -UX makes it None (explicitly not defined)"""
	table = {}
	for d in defs:
		spec = d[2:]
		name = re.match(r"[A-Za-z_][A-Za-z0-9_]*", spec)
		if name is None:
			continue
		name = name.group(0)
		if d.startswith("-U"):
			table[name] = None
		else:
			table[name] = spec[len(name):] if "=" in spec or "(" in spec else "=1"
	return table


_files = {}


def strip_comments(text):
	"""text with comments blanked (newlines kept) and backslash-newlines joined, strings left alone"""
	out, i, n = [], 0, len(text)
	while i < n:
		c = text[i]
		if c == "\\" and text.startswith("\n", i + 1):
			i += 2
			continue
		if c == "/" and text.startswith("*", i + 1):
			j = text.find("*/", i + 2)
			j = n if j < 0 else j + 2
			out.append(" " + "\n" * text.count("\n", i, j))
			i = j
			continue
		if c == "/" and text.startswith("/", i + 1):
			j = text.find("\n", i)
			i = n if j < 0 else j
			continue
		if c in "\"'":
			j = i + 1
			while j < n and text[j] != c and text[j] != "\n":
				j += 2 if text[j] == "\\" else 1
			out.append(text[i:j + 1])
			i = j + 1
			continue
		out.append(c)
		i += 1
	return "".join(out)


DIRECTIVE = re.compile(r"\s*#\s*([A-Za-z_]*)(.*)")
DEFINE = re.compile(r"\s*([A-Za-z_][A-Za-z0-9_]*)(\([^)]*\))?(.*)")


def structure(path):
	"""(branches, entries) of a file, or None when it can't be read or its conditionals don't nest. branches[b] is
	the names a conditional branch depends on (its condition and the earlier ones of its #if chain); entries are
	(kind, enclosing branches, names, macro): kind "define" for #define / #undef of macro, "error" for #error and
	#warning, else "text" (a line of code, an #include, #pragma, ...: what reaches the compiler)"""
	try:
		mtime = os.stat(path).st_mtime
	except OSError:
		return None
	hit = _files.get(path)
	if hit is not None and hit[0] == mtime:
		return hit[1]
	try:
		with open(path, "rb") as f:
			text = strip_comments(f.read().decode("latin-1"))
	except OSError:
		return None
	branches, entries, stack = [], [], []	# stack: (branch, names of the chain so far)
	result = None
	for line in text.split("\n"):
		m = DIRECTIVE.match(line)
		if m is None:
			names = IDENT.findall(line)
			if names:
				entries.append(("text", tuple(b for b, _ in stack), frozenset(names), None))
			continue
		kind, rest = m.group(1), m.group(2)
		names = set(IDENT.findall(rest))
		if kind in ("if", "ifdef", "ifndef"):
			branches.append(frozenset(names))
			stack.append((len(branches) - 1, frozenset(names)))
		elif kind in ("elif", "elifdef", "elifndef", "else"):
			if not stack:
				break
			chain = stack.pop()[1] | names
			branches.append(chain)
			stack.append((len(branches) - 1, chain))
		elif kind == "endif":
			if not stack:
				break
			stack.pop()
		elif kind in ("define", "undef"):
			d = DEFINE.match(rest)
			if d is None:
				break
			body = d.group(3) or ""
			pastes = PASTE.findall(body) if "##" in body else []
			entries.append(("define", tuple(b for b, _ in stack), frozenset(IDENT.findall(body)), (d.group(1), pastes)))
		elif kind in ("error", "warning"):
			entries.append(("error", tuple(b for b, _ in stack), frozenset(names), None))
		elif kind or rest.strip():
			entries.append(("text", tuple(b for b, _ in stack), frozenset(names | {kind}), None))
	else:
		if not stack:
			result = (branches, entries)
	_files[path] = (mtime, result)
	return result


def may_paste(names, pastes):
	for before, after in pastes:
		if before == "" and after == "":
			return True	# x ## y of two parameters: could make any name
		for name in names:
			if (before and name.startswith(before)) or (after and name.endswith(after)):
				return True
	return False


def deps_of(obj):
	try:
		with open(obj[:-2] + ".d") as f:
			text = f.read()
	except OSError:
		return None
	return text.replace("\\\n", " ").partition(": ")[2].split()


def fresh(obj):
	"""True when obj exists and is newer than every file its dependency list names"""
	deps = deps_of(obj)
	if deps is None:
		return False
	try:
		t = os.stat(obj).st_mtime
		return all(os.stat(d).st_mtime <= t for d in deps)
	except OSError:
		return False


def same_text(obj, theirs, ours, check):
	"""True when the preprocessor gives the compiler the same text for obj's source with our switches as with
	theirs: no line that reaches the compiler names a macro whose definition may differ (a switch that differs, a
	macro defined from one or under a conditional on one), and none sits under such a conditional. A header whose
	#error checks such a macro (core/config.h's switch checks) is preprocessed alone with our switches instead:
	check(header) is False when that fails"""
	diff = {n for n in set(theirs) | set(ours) if theirs.get(n, "absent") != ours.get(n, "absent")}
	if any(n.startswith("_") or n == "NDEBUG" for n in diff):
		return False
	files = []
	for d in deps_of(obj) or []:
		st = structure(d)
		if st is None:
			return False
		files.append((d, st))
	if not files:
		return False
	tainted = set(diff)
	changed = True
	while changed:	# the macros that may differ, until no more
		changed = False
		for _, (branches, entries) in files:
			for kind, under, names, macro in entries:
				if kind != "define" or macro[0] in tainted:
					continue
				if (names & tainted or any(branches[b] & tainted for b in under)
						or (macro[1] and may_paste(tainted, macro[1]))):
					tainted.add(macro[0])
					changed = True
	errors = []
	for path, (branches, entries) in files:
		for kind, under, names, macro in entries:
			if kind != "define" and (names & tainted or any(branches[b] & tainted for b in under)):
				if kind == "text":
					return False
				if path not in errors:
					errors.append(path)
	return all(check(path) for path in errors)


_headers = {}


def header_ok(cc, flags, defs, header):
	"""True when header alone preprocesses with these flags and switches (its #error checks pass)"""
	if header not in _headers:
		argv = cc + flags + defs + ["-E", "-P", "-x", "c", "-o", os.devnull, "-"]
		p = subprocess.run(argv, input=('#include "%s"\n' % os.path.abspath(header)).encode(),
			stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
		_headers[header] = p.returncode == 0
	return _headers[header]


def compile_one(cc, flags, defs, src, obj):
	tmp = "%s.%d.tmp" % (obj, os.getpid())
	argv = cc + flags + defs + ["-c", src, "-o", tmp, "-MMD", "-MT", obj, "-MF", tmp + ".d"]
	status = run(argv)
	if status != 0:
		for f in (tmp, tmp + ".d"):
			if os.path.exists(f):
				os.remove(f)
		return status
	os.replace(tmp + ".d", obj[:-2] + ".d")
	os.replace(tmp, obj)
	return 0


def find_object(cc, flags, defs, src, block):
	"""The object for src (built here, by another test, or one another test built that serves), or an int: the
	compiler's failed status, or None when another process holds the source's lock (block=False)"""
	base = hashlib.sha1("\0".join(cc + flags).encode()).hexdigest()[:12]
	rel = os.path.relpath(os.path.abspath(src), ROOT)
	if rel.startswith(".."):
		rel = "_abs" + os.path.abspath(src)
	where = os.path.join(OBJ_DIR, base, os.path.splitext(rel)[0])
	ours = macro_table(defs)
	exact = os.path.join(where, hashlib.sha1("\0".join(defs).encode()).hexdigest()[:12] + ".o")
	if fresh(exact):
		return exact
	os.makedirs(where, exist_ok=True)
	with open(os.path.join(where, "lock"), "a") as lock:
		try:
			fcntl.flock(lock, fcntl.LOCK_EX | (0 if block else fcntl.LOCK_NB))
		except BlockingIOError:
			return None
		try:
			if fresh(exact):
				return exact
			for name in sorted(os.listdir(where)):
				obj = os.path.join(where, name)
				if not name.endswith(".o") or obj == exact or not fresh(obj):
					continue
				try:
					with open(obj[:-2] + ".defs") as f:
						theirs = macro_table(json.load(f))
				except (OSError, ValueError):
					continue
				if same_text(obj, theirs, ours, lambda h: header_ok(cc, flags, defs, h)):
					return obj
			with open(exact[:-2] + ".defs", "w") as f:
				json.dump(defs, f)
			status = compile_one(cc, flags, defs, src, exact)
			return exact if status == 0 else status
		finally:
			fcntl.flock(lock, fcntl.LOCK_UN)


def main():
	argv = sys.argv[1:]
	ncc = 1
	while ncc < len(argv) and not argv[ncc].startswith("-") and not argv[ncc].endswith(".c"):
		ncc += 1	# a compiler given with a launcher ("ccache cc"); the rest are its arguments
	cc, args = argv[:ncc], argv[ncc:]
	out, flags, defs, sources, link = parse(args)
	if (os.environ.get("HOSTCC_DIRECT") == "1" or out is None or not sources
			or any(a in PASS_THROUGH for a in args)):
		return run(argv)
	start = time.time()
	objs = [None] * len(sources)
	for block in (False, True):	# first the sources no other test is compiling, then wait for the rest
		for i, src in enumerate(sources):
			if objs[i] is None:
				got = find_object(cc, flags, defs, src, block)
				if isinstance(got, int):
					return got
				objs[i] = got
	keep = [f for f in flags if not f.startswith(COMPILE_ONLY) or f.startswith("-Wl,")]
	status = run(cc + keep + [objs[a] if isinstance(a, int) else a for a in link])
	if status == 0:
		os.makedirs(TIMES_DIR, exist_ok=True)
		with open(os.path.join(TIMES_DIR, os.path.basename(out)), "w") as f:
			f.write("%.2f\n" % (time.time() - start))
	return status


if __name__ == "__main__":
	sys.exit(main())
