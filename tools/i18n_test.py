#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Tests of tools/i18n.py (docs/dev/translating.md): the marks and their keys, the generated tables (a key's
translation, else its English), a catalog kept in step by key (an English edit keeps the translation), the
migration of a catalog without keys, the rewrite of old-form marks, and the rename of keys."""
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

msgctxt "DONE"
msgid "Done"
msgstr "Hecho"

msgctxt "UNTRANSLATED"
msgid "Untranslated"
msgstr ""

#, fuzzy
msgctxt "FUZZY"
msgid "Fuzzy"
msgstr "Borroso"

msgctxt "VOLUME"
msgid "Volume "
msgstr "Volumen"

msgctxt "BAD"
msgid "Bad"
msgstr "中"
"""

SRC = """// SPDX-License-Identifier: MIT
static const tr_id_t names[] = { TRN(DONE, "Done"), TRN(UNTRANSLATED, "Untranslated"), TRX(RX, "RX") };
void f(void) { show(TR(FUZZY, "Fuzzy"), TR(VOLUME, "Volume "), TR(BAD, "Bad"), TR(DONE, "Done")); }
"""


def scan(src, rel="core/ui/x.c"):
    return i18n.scan(rel, src)


class Marks(unittest.TestCase):
    def test_source_references_use_forward_slashes(self):
        self.assertTrue(all("\\" not in path for path in i18n.source_files()))

    def test_key_and_english(self):
        found = scan(SRC)
        self.assertEqual([(m.kind, m.key, m.text) for m in found][:3],
                         [("TRN", "DONE", "Done"), ("TRN", "UNTRANSLATED", "Untranslated"), ("TRX", "RX", "RX")])
        source, trx = i18n.extract(found)
        self.assertEqual([k for k, _, _ in source], ["DONE", "UNTRANSLATED", "FUZZY", "VOLUME", "BAD"])
        self.assertEqual(trx, [("RX", "RX")])

    def test_glyph_macros_and_concatenation(self):
        found = scan('x = TR(MORE, "More" GLYPH_ELLIPSIS);\ny = TRN(ALT,\n\tTEXT_ALT_OPEN "Power" TEXT_ALT_NEXT "Pwr" '
                     'TEXT_ALT_CLOSE);\n')
        self.assertEqual([m.text for m in found], ["More…", "{Power|Pwr}"])

    def test_no_marks_in_the_tables_reader(self):
        with tempfile.TemporaryDirectory() as d:
            saved = i18n.ROOT
            i18n.ROOT = d
            try:
                os.makedirs(os.path.join(d, "core", "ui"))
                with open(os.path.join(d, "core", "ui", "i18n.c"), "w", encoding="utf-8") as f:
                    f.write('x = TR(ZONE, "Zone");\n')
                with self.assertRaisesRegex(i18n.I18nError, "tables' reader"):
                    i18n.marks(files=["core/ui/i18n.c"])
            finally:
                i18n.ROOT = saved

    def test_old_form_is_an_error(self):
        with self.assertRaisesRegex(i18n.I18nError, "takes a key"):
            scan('x = TR("Zone");\n')
        self.assertIsNone(i18n.scan("core/ui/x.c", 'x = TR("Zone");\n', old_ok=True)[0].key)

    def test_one_key_one_english(self):
        with self.assertRaisesRegex(i18n.I18nError, "one key, one English"):
            i18n.check_keys(scan('a = TR(ZONE, "Zone");\nb = TR(ZONE, "Zones");\n'))
        # the same English under two keys is fine (a later step names them by meaning)
        i18n.check_keys(scan('a = TR(ZONE, "Zone");\nb = TR(ZONE_TITLE, "Zone");\n'))

    def test_translated_or_never(self):
        with self.assertRaisesRegex(i18n.I18nError, "TRX in one place"):
            i18n.check_keys(scan('a = TRN(RX, "RX");\nb = TRX(RX, "RX");\n'))

    def test_taken_key(self):
        for key in ("NONE", "KEYS", "TX"):     # TX: the source has a TR_TX of its own
            with self.assertRaisesRegex(i18n.I18nError, "is taken"):
                i18n.check_keys(scan('a = TR(%s, "x");\n' % key))
        with self.assertRaisesRegex(i18n.I18nError, "takes a key"):
            scan('a = TR(GLYPH_DOT, "x");\n')     # a glyph macro is text, never a key

    def test_key_from_text(self):
        self.assertEqual(i18n.key_from_text("Couldn't save: try again"), "COULDNT_SAVE_TRY_AGAIN")
        self.assertEqual(i18n.key_from_text("{Power|Pwr}"), "POWER")
        self.assertEqual(i18n.key_from_text("12.5 kHz"), "N12_5_KHZ")
        self.assertEqual(i18n.key_from_text("…"), "TEXT")
        self.assertLessEqual(len(i18n.key_from_text("A very long text that goes on and on past the limit")),
                             i18n.KEY_MAX)


class Generate(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.saved = i18n.LANG_DIR
        i18n.LANG_DIR = self.dir.name
        with open(os.path.join(self.dir.name, "xx.po"), "w", encoding="utf-8") as f:
            f.write(PO)
        self.found = scan(SRC)
        out = os.path.join(self.dir.name, "tables.h")
        i18n.generate(out, self.found)
        with open(out, encoding="utf-8") as f:
            self.h = f.read()
        with open(os.path.join(self.dir.name, i18n.KEYS_HEADER), encoding="utf-8") as f:
            self.keys = f.read()

    def tearDown(self):
        i18n.LANG_DIR = self.saved
        self.dir.cleanup()

    def test_keys_header(self):
        # translated keys by name, then the TRX ones; 0 is none
        for line in ("TR_BAD = 1,", "TR_DONE = 2,", "TR_VOLUME = 5,", "TR_RX = 6,", "#define TR_TRANSLATED\t6",
                     "#define TR_KEYS\t\t7"):
            self.assertIn(line, self.keys)

    def test_only_usable_translations_in_the_table(self):
        self.assertIn('"Hecho"', self.h)
        self.assertIn("1 strings", self.h)      # empty, fuzzy, a lost space, an undrawable character: English
        for text in ("Borroso", "Volumen"):
            self.assertNotIn(text, self.h)

    def test_lookup(self):
        ids, translated, english, built = i18n.tables(self.found)
        xx = built[0]
        got = {k: i18n.tr(ids, translated, english, xx, k) for k in ids}
        self.assertEqual(got, {"DONE": b"Hecho", "UNTRANSLATED": b"Untranslated", "FUZZY": b"Fuzzy",
                               "VOLUME": b"Volume ", "BAD": b"Bad", "RX": b"RX"})
        self.assertEqual({k: i18n.tr(ids, translated, english, None, k) for k in ids}["DONE"], b"Done")

    def test_translation_for_other_english_is_not_used(self):
        # a catalog not synced since the English changed: its translation was made for other text
        found = scan(SRC.replace('TRN(DONE, "Done")', 'TRN(DONE, "Done!")').replace('TR(DONE, "Done")',
                                                                                     'TR(DONE, "Done!")'))
        ids, translated, english, built = i18n.tables(found)
        self.assertEqual(i18n.tr(ids, translated, english, built[0], "DONE"), b"Done!")

    def test_languages_share_the_numbers(self):
        with open(os.path.join(self.dir.name, "yy.po"), "w", encoding="utf-8") as f:
            f.write(PO.replace("Language: xx", "Language: yy").replace('"Hecho"', '"Done"')
                    .replace('msgid "Untranslated"\nmsgstr ""', 'msgid "Untranslated"\nmsgstr "Non tradotto"'))
        ids, translated, english, built = i18n.tables(self.found)
        got = {code: [i18n.tr(ids, translated, english, lang, k) for k in ("DONE", "UNTRANSLATED")]
               for code, lang in ((b[0], b) for b in built)}
        self.assertEqual(got, {"xx": [b"Hecho", b"Untranslated"], "yy": [b"Done", b"Non tradotto"]})

    def test_used_keys_only(self):
        # a build that compiles only some marks (a switch off): the other keys keep their numbers, their text is gone
        # from every table, and the used ones look up as before
        ids, translated, english, built = i18n.tables(self.found)
        uids, utranslated, uenglish, ubuilt = i18n.tables(self.found, used={"DONE", "RX", "NOT_A_KEY"})
        self.assertEqual((uids, utranslated), (ids, translated))
        for k in ("DONE", "RX"):
            self.assertEqual(i18n.tr(uids, utranslated, uenglish, ubuilt[0], k),
                             i18n.tr(ids, translated, english, built[0], k))
        for k in ("UNTRANSLATED", "FUZZY", "VOLUME", "BAD"):
            self.assertEqual(i18n.tr(uids, utranslated, uenglish, ubuilt[0], k), b"")
        self.assertNotIn(b"Untranslated", uenglish[0])
        self.assertEqual(len(uenglish[0]), len(b"Done\0RX\0") + len(ids) - 1)

    def test_used_keys_from_scan_files(self):
        path = os.path.join(self.dir.name, "x.tr")
        with open(path, "w", encoding="utf-8") as f:
            f.write("TR_DONE\nTR_KEYS\nTR_RX\n")
        out = os.path.join(self.dir.name, "used.h")
        i18n.generate(out, self.found, i18n.read_used([path]))
        with open(out, encoding="utf-8") as f:
            h = f.read()
        self.assertIn("6 keys, 2 in this build", h)
        self.assertIn('"Hecho"', h)
        self.assertNotIn('"Volume "', h)

    def test_default_language_switch(self):
        # an empty language setting is the language whose CONFIG_LANG_DEFAULT_<CODE> is on, else English
        self.assertIn("#if CONFIG_LANG_DEFAULT_XX\n#define LANG_DEFAULT\t1\n#else\n#define LANG_DEFAULT\tLANG_ENGLISH\n#endif",
                      self.h)

    def test_bad_translations_found(self):
        _, body = i18n.read_po(os.path.join(self.dir.name, "xx.po"))
        for key, msgid, msgstr, _ in body:
            errs = i18n.check_translation(msgid, msgstr, msgid) if msgstr else []
            self.assertEqual(bool(errs), msgid in ("Volume ", "Bad"), msgid)


class Sync(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "xx.po")
        with open(self.path, "w", encoding="utf-8") as f:
            f.write(PO)

    def tearDown(self):
        self.dir.cleanup()

    def test_changed_english_keeps_its_translation(self):
        # DONE reworded, a new key, BAD gone
        source = [("DONE", "Done!", ["core/ui/x.c"]), ("UNTRANSLATED", "Untranslated", ["core/ui/x.c"]),
                  ("FUZZY", "Fuzzy", ["core/ui/x.c"]), ("VOLUME", "Volume ", ["core/ui/x.c"]),
                  ("NEW", "Brand new text", ["core/ui/x.c"])]
        headers, entries, refs = i18n.synced(self.path, source)
        got = {k: (m, s, f) for k, m, s, f in entries}
        self.assertEqual(got["DONE"], ("Done!", "Hecho", False))   # still in use: i18n_changes.py warns
        self.assertEqual(got["NEW"], ("Brand new text", "", False))
        self.assertNotIn("BAD", got)
        todo = "\n".join(i18n.pending("xx.po", entries))
        for text in ("'Untranslated': not translated", "'Fuzzy': marked fuzzy", "'Brand new text': not translated"):
            self.assertIn(text, todo)
        self.assertNotIn("'Done!'", todo)
        self.assertEqual(i18n.pending("xx.po", [("DONE", "Done", "Hecho", False)]), [])
        # written and read back the same
        i18n.write_po(self.path, headers, entries, refs)
        self.assertEqual(i18n.read_po(self.path)[1], entries)
        # a translator's "made for" mark stays until the English is that English again
        marked = [("DONE", "Done!", "Hecho", "Done")]
        i18n.write_po(self.path, headers, marked, refs)
        self.assertEqual(i18n.synced(self.path, [("DONE", "Done!", [])])[1], marked)
        self.assertEqual(i18n.synced(self.path, [("DONE", "Done", [])])[1], [("DONE", "Done", "Hecho", False)])

    def test_catalog_without_keys_migrates_by_english(self):
        with open(self.path, "w", encoding="utf-8") as f:
            f.write("\n".join(line for line in PO.split("\n") if not line.startswith("msgctxt")))
        self.assertTrue(all(k is None for k, _, _, _ in i18n.read_po(self.path)[1]))
        _, entries, _ = i18n.synced(self.path, [("DONE_ROW", "Done", []), ("FUZZY", "Fuzzy", [])])
        self.assertEqual(entries, [("DONE_ROW", "Done", "Hecho", False), ("FUZZY", "Fuzzy", "Borroso", True)])


class Rewrite(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.saved = i18n.ROOT, i18n.taken_cache
        i18n.ROOT = self.dir.name
        i18n.taken_cache = None
        os.makedirs(os.path.join(self.dir.name, "core", "ui"))
        self.path = os.path.join(self.dir.name, "core", "ui", "x.c")
        with open(self.path, "w", encoding="utf-8") as f:
            f.write('a = TR("Zone");\nb = TRN(\n\t"Zone: ");\nc = TR(ZONE_OLD, "Old zone");\nd = TR("Old zone");\n'
                    'e = TR("TX");\nint TR_TX;\n')

    def tearDown(self):
        i18n.ROOT, i18n.taken_cache = self.saved
        self.dir.cleanup()

    def test_rewrite_gives_keys_once(self):
        self.assertEqual(i18n.rewrite(), 4)
        with open(self.path, encoding="utf-8") as f:
            src = f.read()
        # plain words get the plain name, the same English keeps its key, a taken name is never made
        self.assertIn('a = TR(ZONE, "Zone");\nb = TRN(ZONE_2,\n\t"Zone: ");', src)
        self.assertIn('d = TR(ZONE_OLD, "Old zone");', src)
        self.assertIn('e = TR(TX_2, "TX");', src)
        self.assertEqual(i18n.rewrite(), 0)
        with open(self.path, encoding="utf-8") as f:
            self.assertEqual(f.read(), src)


class Rename(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.saved = i18n.ROOT, i18n.LANG_DIR, i18n.taken_cache
        i18n.ROOT = self.dir.name
        i18n.LANG_DIR = os.path.join(self.dir.name, "core", "ui", "lang")
        i18n.taken_cache = None
        os.makedirs(i18n.LANG_DIR)
        os.makedirs(os.path.join(self.dir.name, "tests"))
        self.src = os.path.join(self.dir.name, "core", "ui", "x.c")
        self.test = os.path.join(self.dir.name, "tests", "t.c")
        self.po = os.path.join(i18n.LANG_DIR, "xx.po")
        with open(self.src, "w", encoding="utf-8") as f:
            f.write(SRC + "int TR_TX;\n")
        with open(self.test, "w", encoding="utf-8") as f:
            f.write('CHECK(strcmp(TR( DONE, "Done"), "Hecho") == 0);\n')
        with open(self.po, "w", encoding="utf-8") as f:
            f.write(PO)

    def tearDown(self):
        i18n.ROOT, i18n.LANG_DIR, i18n.taken_cache = self.saved
        self.dir.cleanup()

    def test_rename_keeps_the_translations(self):
        # DONE and FUZZY swap names, RX (never translated) gets a new one: marks, the tests and the catalog follow
        self.assertEqual(i18n.rename([("DONE", "FUZZY"), ("FUZZY", "DONE"), ("RX", "LABEL_RX")]), (5, 2))
        with open(self.src, encoding="utf-8") as f:
            src = f.read()
        self.assertIn('TRN(FUZZY, "Done"), TRN(UNTRANSLATED, "Untranslated"), TRX(LABEL_RX, "RX")', src)
        self.assertIn('show(TR(DONE, "Fuzzy"), TR(VOLUME, "Volume "), TR(BAD, "Bad"), TR(FUZZY, "Done"));', src)
        with open(self.test, encoding="utf-8") as f:
            self.assertIn('TR( FUZZY, "Done")', f.read())
        by_key = {k: (en, tr) for k, en, tr, _ in i18n.read_po(self.po)[1]}
        self.assertEqual(by_key["FUZZY"], ("Done", "Hecho"))
        self.assertEqual(by_key["DONE"], ("Fuzzy", "Borroso"))

    def test_rename_refuses_a_name_in_use(self):
        for pairs in ([("DONE", "BAD")], [("DONE", "TX")], [("DONE", "A"), ("BAD", "A")], [("GONE", "A")],
                      [("DONE", "2X")]):
            with self.assertRaises(i18n.I18nError):
                i18n.rename(pairs)
        with open(self.po, encoding="utf-8") as f:
            self.assertEqual(f.read(), PO)


class Catalogs(unittest.TestCase):
    def test_every_translation_found(self):
        # The radio's catalogs through the lookup the firmware does: each usable translation at its key's number, the
        # rest the English
        ids, translated, english, built = i18n.tables()
        source, trx = i18n.extract()
        en = {k: i18n.text_to_bytes(t, k) for k, t, _ in source}
        en.update((k, i18n.text_to_bytes(t, k)) for k, t in trx)
        for lang in built:
            code, wide = lang[0], lang[8]
            _, body = i18n.read_po(i18n.catalogs()[code])
            to_bytes = i18n.text_to_bytes_wide if wide else i18n.text_to_bytes  # a CJK catalog is UTF-8
            have = {}
            for key, msgid, msgstr, fz in body:
                usable = msgstr and not fz and not i18n.check_translation(msgid, msgstr, msgid, wide)
                if usable and msgid == i18n.bytes_to_text(en[key], key):
                    have[key] = to_bytes(msgstr, msgid)
            for key in ids:
                self.assertEqual(i18n.tr(ids, translated, english, lang, key), have.get(key, en[key]), (code, key))
        for key, t in trx:
            self.assertGreaterEqual(ids[key], translated)


if __name__ == "__main__":
    unittest.main()
