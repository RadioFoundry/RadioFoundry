#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Write the web control page's per-radio files (CONFIG_NET_WEBCTL, docs/spec/tcp-services.md 5.1; run by make).

    gen_webctl_assets.py BOARD OUT_DIR

OUT_DIR/keys.json maps each key's name to its bit: the board's KEY_* enum in boards/BOARD/BOARD.h, in order from 0,
named in lower case without KEY_ ("fn_left": 16). OUT_DIR/radio.svg is the radio's drawing,
site/static/img/radios/src/BOARD.svg, whose key-<name>, encoder and led ids the page makes clickable
(docs/dev/website.md "Radio images"); a radio without one gets no radio.svg and the page draws its keys as buttons.
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
    keys = key_bits(os.path.join(ROOT, "boards", board, board + ".h"))
    os.makedirs(out, exist_ok=True)
    files = [os.path.join(out, "keys.json")]
    write(files[0], json.dumps(keys, separators=(",", ":")).encode())
    svg = os.path.join(ROOT, "site", "static", "img", "radios", "src", board + ".svg")
    dst = os.path.join(out, "radio.svg")
    if os.path.exists(svg):
        with open(svg, "rb") as f:
            write(dst, f.read())
        files.append(dst)
    elif os.path.exists(dst):
        os.remove(dst)
    print(" ".join(files))


if __name__ == "__main__":
    main()
