#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Tests of tools/i18n.py (docs/dev/translating.md): a string with no usable translation is left out of the generated
table, so the radio shows its English; a string whose English changed a little keeps its translation; the check lists
every string still to do."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import i18n  # noqa: E402

PO = """msgid ""
msgstr ""
"Language: xx\\n"
"X-Language-Name: Test\\n"

msgid "Done"
msgstr "Hecho"

msgid "Untranslated"
msgstr ""

#, fuzzy
msgid "Fuzzy"
msgstr "Borroso"

msgid "Volume "
msgstr "Volumen"

msgid "Bad"
msgstr "中"
"""


class Generate(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.saved = i18n.LANG_DIR
        i18n.LANG_DIR = self.dir.name
        with open(os.path.join(self.dir.name, "xx.po"), "w", encoding="utf-8") as f:
            f.write(PO)
        out = os.path.join(self.dir.name, "tables.h")
        i18n.generate(out)
        with open(out, encoding="utf-8") as f:
            self.h = f.read()

    def tearDown(self):
        i18n.LANG_DIR = self.saved
        self.dir.cleanup()

    def test_only_usable_translations_in_the_table(self):
        self.assertIn('"Hecho"', self.h)
        self.assertIn("1 strings", self.h)      # empty, fuzzy, a lost space, an undrawable character: English
        for text in ("Borroso", "Volumen"):
            self.assertNotIn(text, self.h)

    def test_default_language_switch(self):
        # an empty language setting is the language whose CONFIG_LANG_DEFAULT_<CODE> is on, else English
        self.assertIn("#if CONFIG_LANG_DEFAULT_XX\n#define LANG_DEFAULT\t1\n#else\n#define LANG_DEFAULT\tLANG_ENGLISH\n#endif",
                      self.h)

    def test_bad_translations_found(self):
        _, body = i18n.read_po(os.path.join(self.dir.name, "xx.po"))
        for msgid, msgstr, _ in body:
            errs = i18n.check_translation(msgid, msgstr, msgid) if msgstr else []
            self.assertEqual(bool(errs), msgid in ("Volume ", "Bad"), msgid)

    def test_changed_english_keeps_its_translation(self):
        path = os.path.join(self.dir.name, "xx.po")
        # "Done" reworded, a new string, "Bad" gone
        source = [("Done!", ["core/ui/x.c"]), ("Untranslated", ["core/ui/x.c"]), ("Fuzzy", ["core/ui/x.c"]),
                  ("Volume ", ["core/ui/x.c"]), ("Brand new text", ["core/ui/x.c"])]
        headers, entries, refs = i18n.synced(path, source)
        got = {m: (s, f) for m, s, f in entries}
        self.assertEqual(got["Done!"], ("Hecho", False))
        self.assertEqual(got["Brand new text"], ("", False))
        todo = "\n".join(i18n.pending("xx.po", entries))
        for text in ("'Untranslated': not translated", "'Fuzzy': marked fuzzy", "'Brand new text': not translated"):
            self.assertIn(text, todo)
        self.assertNotIn("Done", todo)
        self.assertEqual(i18n.pending("xx.po", [("Done", "Hecho", False)]), [])

    def test_fuzzy_note_survives_a_rewrite(self):
        path = os.path.join(self.dir.name, "xx.po")
        entries = [("Done!", "Hecho", "Done"), ("Fuzzy", "Borroso", True)]
        i18n.write_po(path, {"Language": "xx"}, entries, {})
        self.assertEqual(i18n.read_po(path)[1], entries)
        self.assertIn("made for 'Done'", "\n".join(i18n.pending("xx.po", entries)))

if __name__ == "__main__":
    unittest.main()
