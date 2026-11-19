#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Build Cyrillic supplements from the public-domain X11, Spleen and JetBrains Mono sources."""

import argparse
from pathlib import Path

from PIL import ImageFont

from fontconv import parse, render
from i18n import CYRILLIC
from latinfont import emit, from_outline, header


def add_bdf(lines, path, name):
    props, _, shapes = parse(path)
    bitmaps = [render(props, shapes[ord(char)]) for char in CYRILLIC]
    if any(not any(bitmap) for bitmap in bitmaps):
        raise ValueError(f"{path}: a Russian letter has no ink")
    lines.extend(emit(name, bitmaps))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("x11", type=Path, help="public-domain X11 5x8.bdf")
    ap.add_argument("spleen", type=Path)
    ap.add_argument("jbmono", type=Path)
    ap.add_argument("output", type=Path)
    a = ap.parse_args()

    tiny = ["// SPDX-License-Identifier: LicenseRef-X11-Fixed-Public-Domain",
            '// X11 5x8 fixed font. BDF COPYRIGHT: "Public domain font.  Share and enjoy."',
            "// https://github.com/olikraus/u8g2/blob/master/tools/font/bdf/5x8.bdf", ""]
    add_bdf(tiny, a.x11, "spleen_5x8_cyrillic")
    spleen = header("BSD-2-Clause", "https://github.com/fcambus/spleen/tree/2.1.0", a.spleen / "LICENSE")
    spleen.append("#if !CONFIG_FONT_AA || defined(FONT_BOTH)")
    for size in ("6x12", "8x16", "12x24", "16x32"):
        if size == "16x32":
            spleen.append("#if CONFIG_FONT_XL")
        add_bdf(spleen, a.spleen / f"spleen-{size}.bdf", f"spleen_{size}_cyrillic")
        if size == "16x32":
            spleen.append("#endif")
    spleen.extend(["#endif", ""])

    jb = header("OFL-1.1", "https://github.com/JetBrains/JetBrainsMono/tree/v2.304", a.jbmono / "OFL.txt")
    path = a.jbmono / "fonts/ttf/JetBrainsMono-Bold.ttf"
    probe = ImageFont.truetype(str(path), 100).getlength("0")
    jb.append("#if CONFIG_FONT_AA")
    for w, h, asc, switch in ((6, 12, 9, ""), (8, 16, 12, ""), (9, 18, 14, "CONFIG_FONT_BIG"),
                              (12, 24, 19, ""), (16, 32, 26, "CONFIG_FONT_XL")):
        font = ImageFont.truetype(str(path), 100 * w / probe)
        if switch:
            jb.append("#if " + switch)
        jb.extend(emit(f"jbmono_{w}x{h}_cyrillic", [from_outline(font, c, w, h, asc, 2) for c in CYRILLIC]))
        if switch:
            jb.append("#endif")
    jb.extend(["#endif", ""])
    for face, data in (("small", tiny), ("spleen", spleen), ("jbmono", jb)):
        (a.output / f"cyrillic_{face}.inc").write_bytes("\n".join(data).encode("utf-8"))


if __name__ == "__main__":
    main()
