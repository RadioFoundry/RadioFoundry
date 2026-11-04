#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Build the firmware's CJK glyph table from an outline font that permits it.

The radio draws one byte a character from its fixed-cell Latin fonts (core/ui/font.h). A CJK catalog needs a second
face: a square 1-bit bitmap per ideograph, looked up by code point, with its own advance width. This tool rasterizes
the characters a catalog uses into a 16x16 cell - the height of the 8x16 Latin font, so CJK and Latin share a line -
and writes the C tables the firmware links (core/ui/cjk_font_data.c) plus a PNG preview.

    tools/cjkfont.py --font Font.ttf --weight 500 --po core/ui/lang/zh.po \
        --out-c core/ui/cjk_font_data.c --out-png /tmp/cjk.png
    tools/cjkfont.py --font Font.ttf --text "确认 取消 信道" --out-c /tmp/cjk.c --out-png /tmp/cjk.png

The cell is 16x16 pixels because smaller is not readable: at 12x12 an outline font loses the strokes of all but the
simplest characters (a hand-tuned 12x12 bitmap font would do it, but none with a license that suits this project).

What it writes is data derived from the font, so the font's license must allow embedding bitmaps; an SIL Open Font
License font (Noto Sans SC, Source Han Sans) does, and its notice goes at the top of the generated file. Do not run
this on a vendor font that forbids it.
"""
import argparse
import re
import sys

from PIL import Image, ImageDraw, ImageFont

CELL = 16				# pixels square; the height of the 8x16 Latin font
BITS_PER_ROW = CELL
ROW_BYTES = (BITS_PER_ROW + 7) // 8
GLYPH_BYTES = CELL * ROW_BYTES

# A variable font needs its weight picked: the default instance is too thin at 12-16 px, where a stroke is one pixel
def load_font(path, size, weight):
    f = ImageFont.truetype(path, size)
    if weight:
        try:
            f.set_variation_by_axes([weight])
        except (OSError, ValueError):
            print("cjkfont: %s has no weight axis; using its default" % path, file=sys.stderr)
    return f


# The glyph bytes of font.h: the Latin fonts draw them (one byte each), so the CJK face does not need them
MACRO_CHARS = "\u2026\u00b7\u2192\u00b0\u2912\u2913"


# From a catalog: every character a msgstr holds that is not ASCII, which is what the Latin fonts cannot draw
def chars_from_po(path):
    text = open(path, encoding="utf-8").read()
    out = set()
    for m in re.finditer(r'^msgstr "((?:[^"\\]|\\.)*)"$', text, re.M):
        s = m.group(1).replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")
        out.update(ch for ch in s if ord(ch) > 0x7E and ch not in MACRO_CHARS)
    return out


def codepoint_ok(cp):
    # The BMP only: a catalog's UTF-8 never carries a surrogate, and every CJK block is below U+10000
    return 0 < cp <= 0xFFFF and not 0xD800 <= cp <= 0xDFFF


def rasterize(font, ch, supersample, threshold):
    """One character as a CELL x CELL 1-bit image, its ink centered in the square cell."""
    big = CELL * supersample
    im = Image.new("L", (big, big), 0)
    ImageDraw.Draw(im).text((big // 4, big // 4), ch, font=font, fill=255, anchor="lt")
    box = im.getbbox()
    if box is None:
        return None			# a space, or a character the font has no glyph for
    ink = im.crop(box)
    canvas = Image.new("L", (big, big), 0)
    canvas.paste(ink, ((big - ink.width) // 2, (big - ink.height) // 2))
    cell = canvas.resize((CELL, CELL), Image.LANCZOS)
    return cell.point(lambda v: 255 if v >= threshold else 0)


def to_rows(cell):
    rows = []
    for y in range(CELL):
        row = bytearray(ROW_BYTES)
        for x in range(CELL):
            if cell.getpixel((x, y)):
                row[x >> 3] |= 0x80 >> (x & 7)
        rows.append(bytes(row))
    return rows


def c_bytes(ch):
    """The character's UTF-8 bytes, as the comment that marks its glyph in the generated table."""
    return "".join("\\x%02X" % b for b in ch.encode("utf-8"))


def write_preview(glyphs, path, zoom=3, gap=3):
    cols = 16
    step = CELL * zoom + gap
    nrows = (len(glyphs) + cols - 1) // cols
    sheet = Image.new("L", (cols * step + gap, nrows * step + gap), 0)
    d = ImageDraw.Draw(sheet)
    for i, rows in enumerate(glyphs):
        x0 = gap + (i % cols) * step
        y0 = gap + (i // cols) * step
        for y, row in enumerate(rows):
            for x in range(CELL):
                if row[x >> 3] & (0x80 >> (x & 7)):
                    d.rectangle([x0 + x * zoom, y0 + y * zoom, x0 + x * zoom + zoom - 1,
                                 y0 + y * zoom + zoom - 1], fill=255)
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser(description="Build the firmware's CJK glyph table (docs/dev/translating.md)")
    ap.add_argument("--font", required=True, help="an outline font whose license allows embedding bitmaps")
    ap.add_argument("--weight", type=int, default=500, help="the variable font's weight axis (0: its default)")
    ap.add_argument("--supersample", type=int, default=2, help="render this much larger, then shrink (default 2)")
    ap.add_argument("--threshold", type=int, default=100, help="coverage that counts as ink, 0-255 (default 100)")
    ap.add_argument("--po", help="take the characters from a gettext catalog's msgstr lines")
    ap.add_argument("--text", help="take the characters from this text instead")
    ap.add_argument("--font-name", default="", help="the font and its license, for the generated file's notice")
    ap.add_argument("--out-c", required=True, help="the C file to write")
    ap.add_argument("--out-png", help="a preview image to write")
    args = ap.parse_args()

    if not args.po and not args.text:
        ap.error("give --po or --text")
    chars = chars_from_po(args.po) if args.po else set(args.text)
    chars = sorted(ch for ch in chars if codepoint_ok(ord(ch)) and not ch.isspace())
    if not chars:
        ap.error("no characters to build")

    font = load_font(args.font, CELL * args.supersample, args.weight)
    glyphs, missing = [], []
    for ch in chars:
        cell = rasterize(font, ch, args.supersample, args.threshold)
        rows = to_rows(cell) if cell is not None else None
        if rows is None or not any(any(row) for row in rows):
            missing.append(ch)
            continue
        glyphs.append((ord(ch), ch, rows))
    if missing:
        print("cjkfont: %d character(s) the font does not draw: %s" % (len(missing), "".join(missing)), file=sys.stderr)
    if not glyphs:
        ap.error("every character came out blank")

    if args.out_png:
        write_preview([rows for _, _, rows in glyphs], args.out_png)

    with open(args.out_c, "w", encoding="utf-8", newline="\n") as f:
        f.write("// SPDX-License-Identifier: MIT\n")
        f.write("// Copyright (c) 2026 The RadioFoundry Authors\n")
        f.write("//\n")
        f.write("// Generated by tools/cjkfont.py from %s; do not edit.\n" % (args.font_name or args.font))
        f.write("// The font's license covers this data: it is a bitmap of its glyphs, not part of RadioFoundry's\n")
        f.write("// MIT-licensed code. Regenerate it, never hand-edit it.\n")
        f.write("// %d glyphs of %dx%d pixels, %d bytes each, sorted by code point (core/ui/cjk_font.h)\n"
                % (len(glyphs), CELL, CELL, GLYPH_BYTES))
        f.write('#include "cjk_font.h"\n\n')
        f.write("#if CONFIG_LANG_ZH\n")
        f.write("const uint16_t cjk_font_cp[] = {\n")
        for i, (cp, _, _) in enumerate(glyphs):
            f.write("\t0x%04X,%s" % (cp, "\n" if i % 8 == 7 or i == len(glyphs) - 1 else " "))
        f.write("};\n\n")
        f.write("const uint8_t cjk_font_bits[] = {\n")
        for cp, ch, rows in glyphs:
            f.write("\t/* U+%04X \"%s\" */\n" % (cp, c_bytes(ch)))
            for row in rows:
                f.write("\t" + ", ".join("0x%02X" % b for b in row) + ",\n")
        f.write("};\n\n")
        f.write("const uint16_t cjk_font_count = %d;\n" % len(glyphs))
        f.write("#endif\n")

    print("cjkfont: %d glyphs of %dx%d, %d bytes of bitmap, %d bytes of code points"
          % (len(glyphs), CELL, CELL, len(glyphs) * GLYPH_BYTES, len(glyphs) * 2))


if __name__ == "__main__":
    main()
