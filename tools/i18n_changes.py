#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Warns of English text a branch changed while a translation of it stayed the same (docs/dev/translating.md), on the
radio (core/ui/lang/*.po) and the web site (site/i18n/*.json). Often that is right (a small edit of the English), so
it is a warning, never a failure: each one shows the old and new English and every language's text, for a reviewer to
decide. A missing translation is a failure, of tools/i18n.py --check and python3 -m rfoundrysite.i18n.

    tools/i18n_changes.py [BASE]    BASE: the commit to compare with; by default the pull request's base branch in
                                    GitHub Actions (fetched), else the merge base with origin/main

In GitHub Actions each warning is an annotation (::warning) on the pull request.
"""
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import i18n  # noqa: E402

ROOT = i18n.ROOT
SITE = os.path.join("site", "i18n")


def git(*args):
    r = subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def base_commit():
    ref = os.environ.get("GITHUB_BASE_REF")
    if ref:
        # the checks job checks out the merge commit alone
        if git("fetch", "-q", "--no-tags", "--depth=1", "origin", ref) is None:
            return None
        return git("rev-parse", "FETCH_HEAD")
    return git("merge-base", "HEAD", "origin/main")


def at_base(base, rel):
    """The file's text at the base commit, or None"""
    r = subprocess.run(["git", "-C", ROOT, "show", "%s:%s" % (base, rel)], capture_output=True)
    return r.stdout.decode("utf-8") if r.returncode == 0 else None


def line_of(rel, needle):
    try:
        with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                if line.startswith(needle):
                    return n
    except OSError:
        pass
    return 1


def radio(base):
    """[(file, line, old English, new English, [(code, old translation, new translation)])]"""
    head, old = {}, {}
    for code, path in i18n.catalogs().items():
        text = at_base(base, os.path.relpath(path, ROOT))
        if text is None:
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".po", encoding="utf-8", delete=False) as f:
            f.write(text)
        try:
            old[code] = {m: s for m, s, fz in i18n.read_po(f.name)[1] if s and fz is False}
        finally:
            os.unlink(f.name)
        head[code] = {m: s for m, s, fz in i18n.read_po(path)[1]}
    if not old:
        return []
    first = sorted(head)[0]
    was = i18n.reworded(sorted({m for c in old.values() for m in c}), list(head[first]))
    rel = os.path.relpath(i18n.catalogs()[first], ROOT)
    out = []
    for new, en in sorted(was.items()):
        langs = [(c, old[c].get(en, ""), head[c].get(new, "")) for c in sorted(head) if c in old]
        if any(a and a == b for _, a, b in langs):
            out.append((rel, line_of(rel, "msgid " + i18n.po_quote(new)), en, new, langs))
    return out


def site(base):
    meta_path = os.path.join(ROOT, SITE, "languages.json")
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)
    default = meta["default"]

    def pair(code):
        rel = os.path.join(SITE, code + ".json")
        text = at_base(base, rel)
        with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
            return (json.loads(text) if text else None), json.load(f)
    en_old, en_new = pair(default)
    if en_old is None:
        return []
    cats = {l["code"]: pair(l["code"]) for l in meta["languages"] if l["code"] != default}
    rel = os.path.join(SITE, default + ".json")
    out = []
    for key in sorted(en_new):
        if key.startswith("_") or key not in en_old or en_old[key] == en_new[key]:
            continue
        langs = [(c, (o or {}).get(key, ""), n.get(key, "")) for c, (o, n) in sorted(cats.items())]
        if any(a and a == b for _, a, b in langs):
            out.append((rel, line_of(rel, ' "%s":' % key), en_old[key], en_new[key], langs))
    return out


def esc(s, prop=False):
    s = s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    return s.replace(":", "%3A").replace(",", "%2C") if prop else s


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else base_commit()
    if not base or git("rev-parse", "--verify", "-q", base + "^{commit}") is None:
        print("i18n changes: no base commit to compare with: skipped")
        return 0
    found = radio(base) + site(base)
    actions = os.environ.get("GITHUB_ACTIONS") == "true"
    for rel, line, old, new, langs in found:
        body = ["English: %r -> %r" % (old, new)]
        for code, a, b in langs:
            body.append("%s: %r%s" % (code, b, " (unchanged: still right?)" if a and a == b else
                                      " (updated)" if b else " (not translated)"))
        if actions:
            print("::warning file=%s,line=%d,title=%s::%s" % (esc(rel, True), line,
                  esc("English changed, a translation didn't", True), esc("\n".join(body))))
        else:
            print("%s:%d: English changed, a translation didn't" % (rel, line))
            for b in body:
                print("    " + b)
    print("i18n changes: %d string(s) whose English changed while a translation didn't%s"
          % (len(found), ": check each (a warning, not a failure)" if found else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
