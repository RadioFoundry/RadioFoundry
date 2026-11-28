# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Copyright (c) 2026 RadioFoundry Theme Pack contributors
"""Build and inspect data-only RadioFoundry theme packs."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"RFTP"
VERSION = 1
HEADER = struct.Struct("<4sHHIHHI")
ENTRY = struct.Struct("<B23s18H")
MAX_ERASE_UNIT = 4096
MAX_THEMES = 10  # IDs 4..13 follow the four stock themes.
COLOR_FIELDS = (
    "bg", "fg", "dim", "line", "tx", "rx", "warn", "idle", "pill_fg", "pill_dark_fg", "focus",
    "hint_bg", "priv", "idle_fg", "focus_fg", "chnum", "strip_bg", "strip_warn",
)


class PackError(ValueError):
    pass


def rgb565(value: object, where: str) -> int:
    if not isinstance(value, str) or len(value) != 7 or value[0] != "#":
        raise PackError(f"{where}: expected a color like #12ABEF")
    try:
        red, green, blue = (int(value[i:i + 2], 16) for i in (1, 3, 5))
    except ValueError as exc:
        raise PackError(f"{where}: invalid hexadecimal color {value!r}") from exc
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)


def read_source(path: Path) -> list[tuple[str, tuple[int, ...]]]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PackError(f"{path}: {exc}") from exc
    if not isinstance(doc, dict) or doc.get("format") != "rfoundry-theme-pack" or doc.get("version") != VERSION:
        raise PackError("source must declare format 'rfoundry-theme-pack' and version 1")
    themes = doc.get("themes")
    if not isinstance(themes, list) or not 1 <= len(themes) <= MAX_THEMES:
        raise PackError(f"themes must contain 1..{MAX_THEMES} entries")
    out = []
    names = set()
    for i, theme in enumerate(themes):
        where = f"themes[{i}]"
        if not isinstance(theme, dict) or set(theme) != {"name", "colors"}:
            raise PackError(f"{where}: expected only name and colors")
        name = theme["name"]
        if (not isinstance(name, str) or not name or len(name.encode("utf-8")) > 23 or
                any(ord(c) < 0x20 or ord(c) == 0x7F for c in name)):
            raise PackError(f"{where}.name: use 1..23 UTF-8 bytes without ASCII control characters")
        if name.casefold() in names:
            raise PackError(f"{where}.name: duplicate theme name {name!r}")
        names.add(name.casefold())
        colors = theme["colors"]
        if not isinstance(colors, dict) or set(colors) != set(COLOR_FIELDS):
            missing = sorted(set(COLOR_FIELDS) - set(colors)) if isinstance(colors, dict) else list(COLOR_FIELDS)
            extra = sorted(set(colors) - set(COLOR_FIELDS)) if isinstance(colors, dict) else []
            raise PackError(f"{where}.colors: missing {missing}; unknown {extra}")
        vals = tuple(rgb565(colors[key], f"{where}.colors.{key}") for key in COLOR_FIELDS)
        out.append((name, vals))
    return out


def encode(themes: list[tuple[str, tuple[int, ...]]]) -> bytes:
    payload = bytearray()
    for name, colors in themes:
        encoded = name.encode("utf-8")
        payload += ENTRY.pack(len(encoded), encoded.ljust(23, b"\0"), *colors)
    total = HEADER.size + len(payload)
    if total > MAX_ERASE_UNIT:
        raise PackError(f"pack is {total} bytes; maximum supported size is {MAX_ERASE_UNIT}")
    header = HEADER.pack(MAGIC, VERSION, HEADER.size, len(payload), len(themes), 0, zlib.crc32(payload))
    return header + payload


def inspect(data: bytes) -> tuple[int, list[str]]:
    if len(data) < HEADER.size:
        raise PackError("file is shorter than the pack header")
    magic, version, header_len, payload_len, count, reserved, crc = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or header_len != HEADER.size or reserved != 0:
        raise PackError("unknown or malformed theme pack header")
    if count < 1 or count > MAX_THEMES or payload_len != count * ENTRY.size:
        raise PackError("theme count or payload length is invalid")
    if len(data) != header_len + payload_len or len(data) > MAX_ERASE_UNIT:
        raise PackError("file length does not match the header or exceeds one erase unit")
    payload = data[header_len:]
    if zlib.crc32(payload) != crc:
        raise PackError("CRC-32 does not match")
    names = []
    for i in range(count):
        row = ENTRY.unpack_from(payload, i * ENTRY.size)
        name_len, name_bytes = row[:2]
        if not 1 <= name_len <= 23 or name_bytes[name_len:] != b"\0" * (23 - name_len):
            raise PackError(f"theme {i}: invalid name field")
        try:
            name = name_bytes[:name_len].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise PackError(f"theme {i}: name is not valid UTF-8") from exc
        if any(ord(c) < 0x20 or ord(c) == 0x7F for c in name):
            raise PackError(f"theme {i}: name contains ASCII control characters")
        names.append(name)
    return count, names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build", help="compile a JSON theme pack")
    build.add_argument("source", type=Path)
    build.add_argument("output", type=Path)
    check = sub.add_parser("inspect", help="validate and list a compiled theme pack")
    check.add_argument("pack", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "build":
            data = encode(read_source(args.source))
            args.output.write_bytes(data)
            count, names = inspect(data)
            print(f"Wrote {args.output}: {count} theme(s), {len(data)} bytes")
            for name in names:
                print(f"  {name}")
        else:
            data = args.pack.read_bytes()
            count, names = inspect(data)
            print(f"Valid RadioFoundry theme pack: {count} theme(s), {len(data)} bytes")
            for name in names:
                print(f"  {name}")
    except (OSError, PackError) as exc:
        print(f"theme_pack: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
