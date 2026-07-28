#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Token-shingle similarity scan of this tree against reference trees.

Strips comments and whitespace, normalizes literals, hashes every run of
K tokens and reports files that share runs with any file in the reference
trees. A hit is not proof of copying; every hit is reviewed by hand.

    tools/similarity_scan.py [--k 25] [--min 1] REF_DIR [REF_DIR...]
"""
import argparse, hashlib, os, re, sys

EXTS = ('.c', '.h', '.cpp', '.S', '.s', '.py')
TOKEN = re.compile(r'[A-Za-z_]\w*|0[xX][0-9A-Fa-f]+|\d+|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\S')
COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/|#[^\n]*$', re.S | re.M)

def tokens(path):
    try:
        text = open(path, encoding='latin-1').read()
    except OSError:
        return []
    if path.endswith('.py'):
        text = re.sub(r'#[^\n]*', '', text)
    else:
        text = re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)
    out = []
    for t in TOKEN.findall(text):
        if t[0] in '"\'':
            t = 'STR'
        out.append(t)
    return out

def shingles(toks, k):
    return {hashlib.blake2b(' '.join(toks[i:i + k]).encode(), digest_size=8).digest()
            for i in range(len(toks) - k + 1)}

def walk(root):
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ('.git', 'build', 'third_party')]
        for f in files:
            if f.endswith(EXTS):
                yield os.path.join(d, f)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--k', type=int, default=25, help='tokens per shingle')
    ap.add_argument('--min', type=int, default=1, help='shared shingles to report')
    ap.add_argument('--root', default=os.path.join(os.path.dirname(__file__), '..'))
    ap.add_argument('refs', nargs='+')
    a = ap.parse_args()

    index = {}
    for ref in a.refs:
        for p in walk(ref):
            for h in shingles(tokens(p), a.k):
                index.setdefault(h, p)
    print(f'reference shingles: {len(index)}', file=sys.stderr)

    hits = 0
    for p in walk(a.root):
        own = shingles(tokens(p), a.k)
        shared = [index[h] for h in own if h in index]
        if len(shared) >= a.min:
            hits += 1
            refs = sorted(set(shared))
            print(f'{os.path.relpath(p, a.root)}: {len(shared)}/{len(own)} shingles shared, e.g. {refs[0]}'
                  + (f' (+{len(refs) - 1} more files)' if len(refs) > 1 else ''))
    print(f'similarity_scan: {hits} file(s) with hits', file=sys.stderr)
    return 1 if hits else 0

if __name__ == '__main__':
    sys.exit(main())
