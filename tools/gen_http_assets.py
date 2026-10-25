#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Write the HTTP status server's asset table from the files of core/net/http_assets (run by make).

    gen_http_assets.py [--gzip] OUT.h FILE...

Every FILE is served at "/<its name>", index.html also at "/" (docs/spec/tcp-services.md 5). Each is minified first
(minify(): comments and the whitespace nothing needs out of HTML, CSS, JavaScript, SVG and JSON; names are kept). With
--gzip
(CONFIG_NET_HTTP_GZIP) each is stored gzip compressed, deterministically (level 9, mtime 0, no file name), and
served with Content-Encoding: gzip; without it, as it is. OUT.h holds one byte array per file and the table
http_assets[] of http_asset_t (core/net/http_assets.h). It is rewritten only when its content changes, and then the
script prints "changed".

    gen_http_assets.py --check DUMP_DIR FILE...

The host test's half (tests/host/httpAssetsTest.c writes each table entry's bytes to DUMP_DIR/<index>.bin and its
path, type and gzip flag to DUMP_DIR/table.txt): every entry decompresses (or is) the minified source file, at the
right path with the right type; with node on the PATH, every minified script still parses (node --check).
"""
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css",
    ".js": "text/javascript",
    ".svg": "image/svg+xml",
    ".json": "application/json",
    ".txt": "text/plain; charset=utf-8",
    ".png": "image/png",
    ".ico": "image/x-icon",
}


def content_type(path):
    return TYPES.get(os.path.splitext(path)[1].lower(), "application/octet-stream")


def url_path(path):
    name = os.path.basename(path)
    return "/" if name == "index.html" else "/" + name


WORD = re.compile(r"[A-Za-z0-9_$\\]")
# A / after these (or at the start) begins a regular expression, after anything else it divides
REGEX_AFTER = set("(,=:[!&|?{};+-*%<>~^\n")
REGEX_WORDS = ("return", "typeof", "case", "in", "of", "void", "delete", "throw", "new")
# A line break is not needed after these or before those: no statement can end there (ASI)
NL_AFTER = set("{;,([=:?&|+-*/<>!")
NL_BEFORE = set("})].,;:?&|=")


def min_js(src):
    """JavaScript without comments, indentation or blank lines. Strings, template literals and regular expressions are
    copied as they are; a space stays only between two word characters (or two + or two -); a line break stays where
    automatic semicolon insertion may need it."""
    out = []
    i, n = 0, len(src)

    def last():
        return out[-1][-1] if out and out[-1] else ""

    while i < n:
        c = src[i]
        if src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
        elif src.startswith("/*", i):
            i = src.index("*/", i) + 2
        elif c in "'\"`":
            j = i + 1
            while src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif c == "/" and (last() == "" or last() in REGEX_AFTER or
                           re.search(r"\b(%s)$" % "|".join(REGEX_WORDS), "".join(out[-3:]))):
            j, cls = i + 1, False
            while cls or src[j] != "/":
                cls = (cls or src[j] == "[") and src[j] != "]"
                j += 2 if src[j] == "\\" else 1
            j += 1
            while j < n and src[j].isalpha():
                j += 1
            out.append(src[i:j])
            i = j
        elif c in " \t\r\n":
            j = i
            while j < n and src[j] in " \t\r\n":
                j += 1
            nl = "\n" in src[i:j]
            prev, nxt = last(), src[j] if j < n else ""
            if nl and prev and nxt and prev not in NL_AFTER and nxt not in NL_BEFORE and not src.startswith("//", j):
                out.append("\n")
            elif prev and nxt and (WORD.match(prev) and WORD.match(nxt) or prev == nxt and prev in "+-"):
                out.append(" ")
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out).strip()


def min_css(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"\s+", " ", src)
    src = re.sub(r" ?([{};,>]) ?", r"\1", src)
    src = re.sub(r"([{;]) ?([-\w]+) ?: ?", r"\1\2:", src)	# a declaration's colon, not a selector's :hover
    return src.replace(";}", "}").strip()


def min_markup(src):
    """HTML or SVG: no comments, no XML declaration, no whitespace between tags or at line starts; <style> and
    <script> minified as CSS and JavaScript."""
    parts = re.split(r"(<style>.*?</style>|<script>.*?</script>)", src, flags=re.S)
    out = []
    for p in parts:
        if p.startswith("<style>"):
            out.append("<style>" + min_css(p[7:-8]) + "</style>")
        elif p.startswith("<script>"):
            out.append("<script>" + min_js(p[8:-9]) + "</script>")
        else:
            p = re.sub(r"<!--.*?-->|<\?xml.*?\?>", "", p, flags=re.S)
            p = re.sub(r"\s*\n\s*", "\n", p)
            p = re.sub(r">\n<", "><", p)
            out.append(p.strip("\n") if not out else p)
    return "".join(out).strip() + "\n"


def minify(path, data):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".html", ".svg"):
        return min_markup(data.decode("utf-8")).encode("utf-8")
    if ext == ".css":
        return min_css(data.decode("utf-8")).encode("utf-8")
    if ext == ".js":
        return min_js(data.decode("utf-8")).encode("utf-8")
    if ext == ".json":
        return json.dumps(json.loads(data), separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return data


def source(path):
    with open(path, "rb") as fh:
        return minify(path, fh.read())


def pack(data, use_gzip):
    # gzip.compress with mtime 0 writes no file name and a fixed OS byte: the same bytes on every machine
    return gzip.compress(data, compresslevel=9, mtime=0) if use_gzip else data


def generate(out, files, use_gzip):
    files = sorted(files, key=lambda f: (url_path(f) != "/", url_path(f)))
    lines = ["// Generated by tools/gen_http_assets.py from core/net/http_assets; do not edit",
             "#define HTTP_ASSETS_GZIP %d" % (1 if use_gzip else 0)]
    rows = []
    for i, f in enumerate(files):
        body = pack(source(f), use_gzip)
        lines.append("// %s: %s" % (url_path(f), os.path.basename(f)))
        lines.append("static const uint8_t http_asset_%d[%d] = {" % (i, len(body)))
        for k in range(0, len(body), 16):
            lines.append("\t" + ", ".join("0x%02x" % b for b in body[k:k + 16]) + ",")
        lines.append("};")
        rows.append('\t{ "%s", "%s", http_asset_%d, %d, %d },' % (url_path(f), content_type(f), i, len(body),
                                                             1 if use_gzip else 0))
    lines.append("static const http_asset_t http_assets[] = {")
    lines += rows
    lines.append("};")
    lines.append("#define HTTP_ASSET_COUNT %d" % len(files))
    text = "\n".join(lines) + "\n"
    try:
        with open(out, encoding="utf-8") as fh:
            if fh.read() == text:
                return
    except OSError:
        pass
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("changed")


def check(dump, files):
    by_path = {url_path(f): f for f in files}
    with open(os.path.join(dump, "table.txt"), encoding="utf-8") as fh:
        table = [line.rstrip("\n").split("\t") for line in fh if line.strip()]
    bad = 0
    if sorted(r[1] for r in table) != sorted(by_path):
        print("FAIL paths %s, sources %s" % (sorted(r[1] for r in table), sorted(by_path)))
        bad += 1
    for index, path, ctype, gz in table:
        with open(os.path.join(dump, index + ".bin"), "rb") as fh:
            data = fh.read()
        src = by_path.get(path)
        if src is None:
            continue
        want = source(src)
        bad += parses(src, want)
        got = gzip.decompress(data) if gz == "1" else data
        if got != want or ctype != content_type(src) or (gz == "1" and data != pack(want, True)):
            print("FAIL %s: %d bytes (gzip %s) do not give %s" % (path, len(data), gz, src))
            bad += 1
        else:
            print("ok %s %s %d -> %d bytes%s" % (path, ctype, len(want), len(data), " gzip" if gz == "1" else ""))
    return bad


def parses(path, data):
    """1 if a minified script no longer parses (node --check on every <script> and .js); 0 without node."""
    node = shutil.which("node")
    text = data.decode("utf-8", "replace")
    scripts = re.findall(r"<script>(.*?)</script>", text, flags=re.S) if path.endswith(".html") else \
        [text] if path.endswith(".js") else []
    if node is None or not scripts:
        return 0
    bad = 0
    for js in scripts:
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as fh:
            fh.write(js)
        r = subprocess.run([node, "--check", fh.name], capture_output=True, text=True)
        os.unlink(fh.name)
        if r.returncode != 0:
            print("FAIL %s: the minified script does not parse: %s" % (path, r.stderr.strip()[:400]))
            bad = 1
    return bad


def main(argv):
    if argv[:1] == ["--check"]:
        return 1 if check(argv[1], argv[2:]) else 0
    use_gzip = argv[:1] == ["--gzip"]
    if use_gzip:
        argv = argv[1:]
    generate(argv[0], argv[1:], use_gzip)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
