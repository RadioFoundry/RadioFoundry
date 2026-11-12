#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Write the web control page's per-radio files (CONFIG_NET_WEBCTL, docs/spec/tcp-services.md 5.1; run by make).

    gen_webctl_assets.py BOARD OUT_DIR

OUT_DIR/keys.json maps each key's name to its bit: the board's KEY_* enum (key_header: boards/BOARD/BOARD.h, else the
header of a board family directory its BOARD_DIRS builds), in order from 0, named in lower case without KEY_
("fn_left": 16). OUT_DIR/radio.svg is the radio's drawing,
site/static/img/radios/src/BOARD.svg, whose key-<name>, encoder and led ids the page makes clickable
(docs/dev/website.md "Radio images"); a radio without one gets no radio.svg and the page draws its keys as buttons.
OUT_DIR/mic.svg is its hand microphone, site/static/img/radios/src/BOARD-mic.svg, when it has a drawing of one; the
page shows it below the radio.
A file is rewritten only when its content changes. The script prints the files' paths, for make: a $(wildcard) of a
directory made while make reads the Makefile can miss them.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENUM = re.compile(r"enum\s*\{([^}]*\bKEY_COUNT\b[^}]*)\}", re.S)


def key_bits(header):
    """{name: bit} from the header's enum that ends in KEY_COUNT."""
    with open(header, encoding="utf-8") as f:
        text = re.sub(r"//[^\n]*|/\*.*?\*/", "", f.read(), flags=re.S)
    m = ENUM.search(text)
    if m is None:
        raise SystemExit("%s: no enum with KEY_COUNT" % header)
    names = [n.strip() for n in m.group(1).split(",") if n.strip()]
    if any("=" in n for n in names):
        raise SystemExit("%s: the key enum must count from 0 without values" % header)
    return {n[4:].lower(): i for i, n in enumerate(names) if n.startswith("KEY_") and n != "KEY_COUNT"}


def key_header(board):
    """The header with the board's key enum: boards/BOARD/BOARD.h, else the first header that has one in the board's
    directory (keys.h, when the board keeps its keys apart) or in a boards/<family> directory its board.mk lists in
    BOARD_DIRS (boards/uv380_family/uvf.h)."""
    own = os.path.join(ROOT, "boards", board, board + ".h")
    if os.path.exists(own):
        with open(own, encoding="utf-8") as fh:
            if ENUM.search(re.sub(r"//[^\n]*|/\*.*?\*/", "", fh.read(), flags=re.S)):
                return own
    dirs = [os.path.join("boards", board)]
    try:
        with open(os.path.join(ROOT, "boards", board, "board.mk"), encoding="utf-8") as f:
            mk = f.read().replace("\\\n", " ")
        for m in re.finditer(r"(?m)^\s*BOARD_DIRS\s*[:+]?=(.*)$", mk):
            dirs += [d.strip("/") for d in m.group(1).split() if re.fullmatch(r"boards/[\w-]+/?", d)]
    except OSError:
        pass
    for d in dirs:
        full = os.path.join(ROOT, d)
        for f in sorted(os.listdir(full)) if os.path.isdir(full) else ():
            if f.endswith(".h"):
                with open(os.path.join(full, f), encoding="utf-8") as fh:
                    if ENUM.search(re.sub(r"//[^\n]*|/\*.*?\*/", "", fh.read(), flags=re.S)):
                        return os.path.join(full, f)
    raise SystemExit("boards/%s: no header with the key enum (an enum ending in KEY_COUNT)" % board)


def write(path, data):
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    except OSError:
        pass
    with open(path, "wb") as f:
        f.write(data)
    return True


def main():
    board, out = sys.argv[1], sys.argv[2]
    keys = key_bits(key_header(board))
    os.makedirs(out, exist_ok=True)
    files = [os.path.join(out, "keys.json")]
    write(files[0], json.dumps(keys, separators=(",", ":")).encode())
    for src, name in ((board + ".svg", "radio.svg"), (board + "-mic.svg", "mic.svg")):
        svg = os.path.join(ROOT, "site", "static", "img", "radios", "src", src)
        dst = os.path.join(out, name)
        if os.path.exists(svg):
            with open(svg, "rb") as f:
                write(dst, f.read())
            files.append(dst)
        elif os.path.exists(dst):
            os.remove(dst)
    print(" ".join(files))


if __name__ == "__main__":
    main()
