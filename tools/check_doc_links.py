#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Check the relative links, images and anchors in the repository's Markdown.

    tools/check_doc_links.py              every *.md file git tracks or would track
    tools/check_doc_links.py FILE...      only these files

For each Markdown link [text](target), image ![alt](target) and HTML <img src="..."> / <a href="..."> outside code:
  - a relative path must name a file or directory that exists in the working tree;
  - an anchor (#heading or file.md#heading) must match a heading of that file, using GitHub's heading ids
    (lower case, punctuation dropped, spaces to hyphens, -1 / -2 for repeats).
Links with a scheme (https:, mailto:) are not fetched. Exit status 0 when everything resolves, 1 otherwise.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

FENCE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE = re.compile(r"`+[^`]*`+")
MD_LINK = re.compile(r"!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HTML_LINK = re.compile(r"<(?:img|a)\b[^>]*?\b(?:src|href)\s*=\s*\"([^\"]+)\"", re.I)
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
HTML_ANCHOR = re.compile(r"<a\s+(?:name|id)\s*=\s*\"([^\"]+)\"", re.I)


def slug(text):
    """GitHub's heading id: markup removed, lower case, keep letters, digits, spaces, hyphens, underscores."""
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)   # links: keep the text
    text = re.sub(r"<[^>]+>", "", text)                        # inline HTML
    text = text.replace("`", "").lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def lines_outside_code(path):
    """(line number, text) of every line outside fenced code, with inline code removed."""
    fence = None
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            m = FENCE.match(line)
            if m:
                if fence is None:
                    fence = m.group(1)
                elif m.group(1) == fence:
                    fence = None
                continue
            if fence is None:
                yield n, INLINE_CODE.sub("", line.rstrip("\n"))


_anchor_cache = {}


def anchors(path):
    if path not in _anchor_cache:
        ids, seen = set(), {}
        for _, line in lines_outside_code(path):
            for a in HTML_ANCHOR.findall(line):
                ids.add(a)
            m = HEADING.match(line)
            if not m:
                continue
            base = slug(m.group(2))
            k = seen.get(base, 0)
            seen[base] = k + 1
            ids.add(base if k == 0 else f"{base}-{k}")
        _anchor_cache[path] = ids
    return _anchor_cache[path]


def check(path):
    errors = []
    here = os.path.dirname(path)
    rel = os.path.relpath(path, ROOT)
    for n, line in lines_outside_code(path):
        for target in MD_LINK.findall(line) + HTML_LINK.findall(line):
            if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
                continue                                   # https:, mailto:, ...
            file_part, _, anchor = target.partition("#")
            file_part = file_part.split("?", 1)[0]
            if file_part.startswith("/"):
                dest = os.path.join(ROOT, file_part.lstrip("/"))
            elif file_part:
                dest = os.path.normpath(os.path.join(here, file_part))
            else:
                dest = path
            if not os.path.exists(dest):
                errors.append(f"{rel}:{n}: {target}: no such file")
                continue
            if anchor and dest.endswith(".md") and os.path.isfile(dest):
                if anchor not in anchors(dest):
                    errors.append(f"{rel}:{n}: {target}: no heading #{anchor} in {os.path.relpath(dest, ROOT)}")
    return errors


def tracked_markdown():
    out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", "*.md"], cwd=ROOT, capture_output=True, check=True).stdout
    files = [os.path.join(ROOT, f) for f in out.decode().split("\0") if f]
    return [f for f in files if os.path.isfile(f) and not os.path.relpath(f, ROOT).startswith("third_party/")]


def main(argv):
    files = [os.path.abspath(f) for f in argv] if argv else tracked_markdown()
    errors = []
    for f in files:
        errors += check(f)
    for e in errors:
        print(e)
    print(f"check_doc_links: {len(files)} files, {len(errors)} broken links")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
