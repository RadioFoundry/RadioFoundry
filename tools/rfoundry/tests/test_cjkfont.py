#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The CJK table must cover the catalogs and the reviewed channel-name list."""
import os
import re
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import cjkfont


class CjkFontTest(unittest.TestCase):
    def test_chars_file_ignores_comments(self):
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as f:
            # REUSE-IgnoreStart: a header written into the test file, not this file's
            f.write("# SPDX-License-Identifier: MIT\n中文\n\n北京 天津\n")
            # REUSE-IgnoreEnd
            path = f.name
        try:
            self.assertEqual(cjkfont.chars_from_file(path), set("中文北京天津"))
        finally:
            os.unlink(path)

    def test_table_covers_catalogs_and_names(self):
        with open(os.path.join(ROOT, "core/ui/cjk_font_data.c"), encoding="utf-8") as f:
            table = f.read()
        body = re.search(r"cjk_font_cp\[\]\s*=\s*\{(.*?)\};", table, re.S).group(1)
        have = set(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})\b", body))
        catalog = cjkfont.chars_from_po(os.path.join(ROOT, "core/ui/lang/zh.po"))
        names = cjkfont.chars_from_file(os.path.join(ROOT, "tools/cjk_names_zh.txt"))
        want = set(ord(ch) for ch in catalog | names if cjkfont.codepoint_ok(ord(ch)) and not ch.isspace())
        self.assertFalse(want - have, "missing glyphs: " + "".join(chr(cp) for cp in sorted(want - have)))


if __name__ == "__main__":
    unittest.main()
