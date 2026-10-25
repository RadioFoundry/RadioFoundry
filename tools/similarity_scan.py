#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Token-shingle similarity scan of this tree against reference trees (docs/CLEANROOM.md).

    tools/similarity_scan.py [options] REF [REF...]

REF is a directory or a single file anywhere on disk (for example an aftermarket firmware tree; it is only read). Every
source file of this repository (or of --root) is compared with every source file under the REFs:

- comments are stripped, string literals become STR (and, with --ids, identifiers become ID and numbers NUM, which
  also finds copies with renamed variables, at the price of more hits on generic code);
- every run of --k tokens (a shingle) is hashed; a shingle of ours that also occurs in a reference file is a hit;
- overlapping hits are merged into regions, each reported with our lines and the reference file and line it matches;
- shingles that are mostly numbers and punctuation (lookup tables, register lists, standard constants) are left out
  unless --data; they are rarely evidence of copying and drown the report.

A hit is not proof of copying and no hit is not proof of independence: every region is reviewed by hand. The exit
status is 1 when any file has a region of at least --min shingles, 0 when none, 2 on a usage error.

Options:
    --root DIR       the tree to check (default: this repository)
    --k N            tokens per shingle (default 25)
    --min N          report regions of at least N shingles (default 1)
    --ids            normalize identifiers and numbers too
    --data           keep number / punctuation shingles
    --exclude GLOB   leave out matching paths (relative to --root or to the REF; repeatable). third_party/, build/
                     and .git/ are always left out of --root, and .git/ of the REFs
    --report FILE    also write the full report to FILE
    --json FILE      the regions as JSON (for scripts)
    --quiet          only the per-file summary lines
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import sys

EXTS = ('.c', '.h', '.cpp', '.cc', '.hpp', '.S', '.s', '.inc', '.py', '.ld')
TOKEN = re.compile(r'[A-Za-z_]\w*|0[xX][0-9A-Fa-f]+|\d+(?:\.\d+)?[uUlLfF]*|"(?:\\.|[^"\\\n])*"|'
                   r"'(?:\\.|[^'\\\n])*'|\S")
C_COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
PY_COMMENT = re.compile(r'#[^\n]*')
KEYWORDS = set('''auto break case char const continue default do double else enum extern float for goto if inline int
long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while
bool true false NULL uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t size_t define include ifdef
ifndef endif elif undef pragma def class import from as pass lambda yield with try except finally raise global
nonlocal in is not and or None True False self'''.split())
ROOT_SKIP = ('.git', 'build', 'third_party', '__pycache__', '.pytest_cache')
REF_SKIP = ('.git', '__pycache__')


def blank(m):
    """A comment replaced by as many newlines, so token line numbers stay right."""
    return '\n' * m.group(0).count('\n') + ' '


def tokens(path, ids):
    """[(token, line)] of a source file, comments removed, literals normalized."""
    try:
        with open(path, encoding='latin-1') as f:
            text = f.read()
    except OSError:
        return []
    text = (PY_COMMENT if path.endswith('.py') else C_COMMENT).sub(blank, text)
    out = []
    line, pos = 1, 0
    for m in TOKEN.finditer(text):
        line += text.count('\n', pos, m.start())
        pos = m.start()
        t = m.group(0)
        if t[0] in '"\'':
            t = 'STR'
        elif ids:
            if t[0].isdigit():
                t = 'NUM'
            elif (t[0].isalpha() or t[0] == '_') and t not in KEYWORDS:
                t = 'ID'
        out.append((t, line))
    return out


def is_data(toks):
    """Mostly numbers and punctuation: a table, not code."""
    words = sum(1 for t in toks if t[0].isalpha() or t[0] == '_')
    return words * 5 < len(toks)


def shingles(toks, k, data):
    """[(hash, index of the first token)]"""
    out = []
    words = [t for t, _ in toks]
    for i in range(len(words) - k + 1):
        run = words[i:i + k]
        if not data and is_data(run):
            continue
        out.append((hashlib.blake2b('\x1f'.join(run).encode(), digest_size=8).digest(), i))
    return out


def excluded(rel, patterns):
    return any(fnmatch.fnmatch(rel, p) or fnmatch.fnmatch(os.path.basename(rel), p) for p in patterns)


def walk(top, skip, patterns):
    if os.path.isfile(top):
        yield top
        return
    for d, dirs, files in os.walk(top):
        dirs[:] = sorted(x for x in dirs if x not in skip)
        for f in sorted(files):
            p = os.path.join(d, f)
            if f.endswith(EXTS) and not excluded(os.path.relpath(p, top), patterns):
                yield p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('refs', nargs='+', metavar='REF', help='reference directory or file')
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')))
    ap.add_argument('--k', type=int, default=25)
    ap.add_argument('--min', type=int, default=1)
    ap.add_argument('--ids', action='store_true')
    ap.add_argument('--data', action='store_true')
    ap.add_argument('--exclude', action='append', default=[])
    ap.add_argument('--report')
    ap.add_argument('--json')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()

    root = os.path.realpath(a.root)
    for r in a.refs:
        if not os.path.exists(r):
            ap.error(f'{r}: no such file or directory')
    if a.k < 5:
        ap.error('--k below 5 matches almost anything')

    # Index the references: hash -> (file number, line). A reference inside --root (or --root itself) is skipped,
    # or the tree would match itself.
    ref_files, index = [], {}
    for ref in a.refs:
        for p in walk(ref, REF_SKIP, a.exclude):
            rp = os.path.realpath(p)
            if rp == root or rp.startswith(root + os.sep):
                continue
            n = len(ref_files)
            ref_files.append(p)
            toks = tokens(p, a.ids)
            for h, i in shingles(toks, a.k, a.data):
                index.setdefault(h, (n, toks[i][1]))
    print(f'similarity_scan: {len(ref_files)} reference files, {len(index)} distinct shingles (k={a.k}'
          f'{", ids normalized" if a.ids else ""})', file=sys.stderr)
    if not ref_files:
        print('similarity_scan: no source files under the references', file=sys.stderr)
        return 2

    lines, results, files_hit = [], [], 0
    for p in walk(root, ROOT_SKIP, a.exclude):
        toks = tokens(p, a.ids)
        own = shingles(toks, a.k, a.data)
        if not own:
            continue
        # Merge hits whose token runs overlap or touch into regions
        regions, cur = [], None
        for h, i in own:
            ref = index.get(h)
            if ref is None:
                continue
            if cur and i <= cur['end'] + 1:
                cur['end'] = i + a.k - 1
                cur['shingles'] += 1
            else:
                cur = {'start': i, 'end': i + a.k - 1, 'shingles': 1, 'ref': ref}
                regions.append(cur)
        regions = [r for r in regions if r['shingles'] >= a.min]
        if not regions:
            continue
        files_hit += 1
        rel = os.path.relpath(p, root)
        covered = sum(r['end'] - r['start'] + 1 for r in regions)
        lines.append(f'{rel}: {len(regions)} region(s), {covered}/{len(toks)} tokens ({100 * covered / len(toks):.0f} %)')
        out = {'file': rel, 'tokens': len(toks), 'covered': covered, 'regions': []}
        for r in sorted(regions, key=lambda r: -r['shingles']):
            n, ref_line = r['ref']
            l0, l1 = toks[r['start']][1], toks[min(r['end'], len(toks) - 1)][1]
            out['regions'].append({'lines': [l0, l1], 'shingles': r['shingles'], 'ref': ref_files[n],
                                   'ref_line': ref_line})
            if not a.quiet:
                lines.append(f'    lines {l0}-{l1} ({r["shingles"]} shingles) ~ {ref_files[n]}:{ref_line}')
        results.append(out)

    text = '\n'.join(lines)
    if text:
        print(text)
    summary = f'similarity_scan: {files_hit} file(s) with regions of >= {a.min} shared shingle(s)'
    print(summary, file=sys.stderr)
    if a.report:
        with open(a.report, 'w') as f:
            f.write(text + ('\n' if text else '') + summary + '\n')
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'k': a.k, 'ids': a.ids, 'refs': a.refs, 'files': results}, f, indent=1)
    return 1 if files_hit else 0


if __name__ == '__main__':
    sys.exit(main())
