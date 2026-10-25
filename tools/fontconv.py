#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Convert a fixed-cell BDF bitmap font, or a monospaced TrueType font, into the C data the UI draws with
(core/ui/font.h).

    tools/fontconv.py FONT.bdf NAME LICENSE_FILE SPDX_ID > core/ui/fonts/NAME.inc
    tools/fontconv.py --ttf WxH:ASCENT FONT.ttf NAME LICENSE_FILE SPDX_ID > core/ui/fonts/NAME.inc

Output: `NAME_bits[]`, glyphs in the order 0x20..0x7E then the extra code points below, and `NAME_index[]`, where
each glyph starts in it. A glyph is its first inked row, its number of rows from there to the last inked one, then
those rows of `ceil(width * bpp / 8)` bytes, MSB = leftmost pixel: the blank rows above and below are not stored. A BDF font gives 1 bit a
pixel. A TrueType font is scaled so its advance is the cell width, drawn anti-aliased with its baseline ASCENT rows
from the top, and kept as 2 bits a pixel (coverage 0..3; four levels look the same as more on the panels, and 16-color
frame buffers have no room for more blends). It needs Pillow with FreeType. Glyphs the font lacks are all zero and
their bit is set in `NAME_MISSING` (the UI then draws a fallback). The font's own copyright and license text are
carried into the header of the generated file.
"""
import re
import sys

# Extra glyphs after ASCII, in slot order (core/ui/font.h GLYPH_*)
EXTRAS = [0x2026, 0x00B7, 0x2192, 0x00B0]


def parse(path):
    props = {}
    comments = []
    glyphs = {}
    with open(path, encoding="ascii", errors="replace") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("COMMENT"):
            comments.append(line[8:] if len(line) > 8 else "")
        elif line.startswith("FONTBOUNDINGBOX"):
            props["bbx"] = [int(v) for v in line.split()[1:5]]
        elif line.startswith("FONT_ASCENT"):
            props["ascent"] = int(line.split()[1])
        elif line.startswith("FONT_DESCENT"):
            props["descent"] = int(line.split()[1])
        elif line.startswith("STARTCHAR"):
            enc = None
            bbx = None
            rows = []
            i += 1
            while not lines[i].startswith("ENDCHAR"):
                l = lines[i]
                if l.startswith("ENCODING"):
                    enc = int(l.split()[1])
                elif l.startswith("BBX"):
                    bbx = [int(v) for v in l.split()[1:5]]
                elif l.startswith("BITMAP"):
                    i += 1
                    while not lines[i].startswith("ENDCHAR"):
                        rows.append(lines[i].strip())
                        i += 1
                    break
                i += 1
            if enc is not None and bbx is not None:
                glyphs[enc] = (bbx, rows)
        i += 1
    return props, comments, glyphs


def render(props, glyph):
    cw, ch = props["bbx"][0], props["bbx"][1]
    ascent = props["ascent"]
    (gw, gh, gx, gy), rows = glyph
    cell = [[0] * cw for _ in range(ch)]
    top = ascent - (gy + gh)
    for r, hexrow in enumerate(rows[:gh]):
        bits = int(hexrow, 16) if hexrow else 0
        nbits = len(hexrow) * 4
        for c in range(gw):
            if bits >> (nbits - 1 - c) & 1:
                y, x = top + r, gx + c
                if 0 <= y < ch and 0 <= x < cw:
                    cell[y][x] = 1
    stride = (cw + 7) // 8
    out = []
    for y in range(ch):
        for b in range(stride):
            v = 0
            for k in range(8):
                x = b * 8 + k
                if x < cw and cell[y][x]:
                    v |= 0x80 >> k
            out.append(v)
    return out


def ttf_glyphs(path, cw, ch, ascent):
    """Coverage 0..3 per pixel of each glyph the font has, keyed by code point"""
    from PIL import Image, ImageDraw, ImageFont
    probe = ImageFont.truetype(path, 100)
    font = ImageFont.truetype(path, cw * 100 / probe.getlength("0"))
    cmap = set()
    try:
        from fontTools.ttLib import TTFont
        cmap = set(TTFont(path).getBestCmap())
    except ImportError:
        cmap = None
    glyphs = {}
    for code in list(range(0x20, 0x7F)) + EXTRAS:
        if cmap is not None and code not in cmap:
            continue
        img = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(img).text((0, ascent), chr(code), font=font, fill=255, anchor="ls")
        px = img.load()
        glyphs[code] = [[(px[x, y] * 3 + 127) // 255 for x in range(cw)] for y in range(ch)]
    return glyphs


def pack(cell, bpp):
    cw = len(cell[0])
    stride = (cw * bpp + 7) // 8
    per = 8 // bpp
    out = []
    for row in cell:
        for b in range(stride):
            v = 0
            for k in range(per):
                x = b * per + k
                if x < cw:
                    v |= row[x] << (8 - bpp * (k + 1))
            out.append(v)
    return out


def main():
    args = sys.argv[1:]
    ttf = None
    if len(args) == 6 and args[0] == "--ttf":
        size, asc = args[1].split(":")
        ttf = [int(v) for v in size.split("x")] + [int(asc)]
        args = args[2:]
    if len(args) != 4:
        sys.stderr.write(__doc__)
        return 2
    path, name, license_path, spdx = args
    if ttf:
        cw, ch, ascent = ttf
        glyphs, comments, bpp = ttf_glyphs(path, cw, ch, ascent), [], 2
        props = {"ascent": ascent}
    else:
        props, comments, glyphs = parse(path)
        cw, ch, bpp = props["bbx"][0], props["bbx"][1], 1
    with open(license_path, encoding="utf-8") as f:
        license_text = f.read().strip().splitlines()

    codes = list(range(0x20, 0x7F)) + EXTRAS
    missing = 0
    data = []
    for slot, code in enumerate(codes):
        cell = None
        if code in glyphs:
            cell = pack(glyphs[code], 2) if ttf else render(props, glyphs[code])
        if cell is not None and (any(cell) or code < 0x7F):
            data.append(cell)
        else:
            # Not in the font, or an extra drawn blank (Spleen 5x8's middle dot and degree): gfx.c draws it
            if code < 0x7F:
                raise SystemExit("font lacks ASCII 0x%02X" % code)
            missing |= 1 << (slot - 95)
            data.append([0] * (((cw * bpp + 7) // 8) * ch))

    w = sys.stdout.write
    w("// SPDX-License-Identifier: %s\n" % spdx)
    if ttf:
        for l in license_text:
            m = re.match(r"Copyright (\d.*)", l)
            if m:
                w("// SPDX-FileCopyrightText: %s\n" % m.group(1))
    for c in comments:
        c = c.strip().lstrip("/*").rstrip("*/").strip()
        if c.startswith("*"):
            c = c[1:].strip()
        if c.startswith("Copyright"):
            w("// SPDX-FileCopyrightText: %s\n" % c[len("Copyright"):].strip(" (c)"))
    w("//\n// Generated by tools/fontconv.py from %s. Do not edit.\n" % path.rsplit("/", 1)[-1])
    if comments:
        w("// Original header:\n")
    for c in comments:
        w("//   %s\n" % c.rstrip())
    w("//\n// License of the font data:\n//\n")
    for l in license_text:
        w(("//   %s" % l).rstrip() + "\n")
    w("\n")
    w("#define %s_W %d\n#define %s_H %d\n#define %s_ASCENT %d\n#define %s_MISSING 0x%02Xu\n#define %s_BPP %d\n\n"
      % (name.upper(), cw, name.upper(), ch, name.upper(), props["ascent"], name.upper(), missing, name.upper(), bpp))
    stride = (cw * bpp + 7) // 8
    index, offset, glyphs_out = [], 0, []
    for g in data:
        rows = [g[r * stride:(r + 1) * stride] for r in range(ch)]
        inked = [r for r in range(ch) if any(rows[r])]
        top, n = (inked[0], inked[-1] - inked[0] + 1) if inked else (0, 0)
        glyph = [top, n] + [v for r in rows[top:top + n] for v in r]
        index.append(offset)
        offset += len(glyph)
        glyphs_out.append(glyph)
    if offset > 0xFFFF:
        raise SystemExit("font too large for 16-bit offsets")
    w("static const uint8_t %s_bits[] = {\n" % name)
    for slot, (code, g) in enumerate(zip(codes, glyphs_out)):
        label = chr(code) if 0x20 < code < 0x7F and chr(code) not in "\\" else "U+%04X" % code
        w("\t" + ",".join("0x%02X" % v for v in g) + ",\t// %s\n" % label)
    w("};\n\n")
    w("static const uint16_t %s_index[] = {\n" % name)
    for i in range(0, len(index), 12):
        w("\t" + ",".join("%d" % v for v in index[i:i + 12]) + ",\n")
    w("};\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
