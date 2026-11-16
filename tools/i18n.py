#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The firmware's translations (docs/dev/translating.md): the strings the radio shows, marked in core/ with
TR(KEY, "English"), TRN(KEY, "English") or TRX(KEY, "English") (never translated), and one gettext catalog per
language in core/ui/lang/<code>.po, an entry per key (its msgctxt).

    tools/i18n.py --sync            bring every catalog up to date with the source (new keys added untranslated, keys
                                    gone from the source dropped, a key whose English changed keeps its translation); --sync es
                                    for one language. A catalog from before the keys gets them by its English
    tools/i18n.py --check           exit 1 when a catalog is out of date, has a string not translated or marked
                                    fuzzy, or a translation can't be drawn (tools/ci.sh); --allow-pending: not the
                                    untranslated and fuzzy ones (work in progress, never in a pull request)
    tools/i18n.py --stats           how much of each catalog is translated
    tools/i18n.py --gen OUT.h       the C tables core/ui/i18n.c includes, and tr_keys.h beside it (run by make; a file
                                    is rewritten only when its content changes, and "changed" is printed for OUT.h)
    tools/i18n.py --new CODE NAME   start a catalog: CODE an ISO 639-1 code, NAME the language's own name ("Español")
    tools/i18n.py --rewrite [FILE]  give each old-form mark TR("English") / TRN("English") a key made from its English
                                    (the same English keeps one key); by default in the whole source
    tools/i18n.py --rename OLD NEW  rename a key: at every mark (the source, tests/ and sim/) and in every catalog's
                                    msgctxt, so no translation is lost; --rename FILE for many, a line "OLD NEW"
                                    each (renamed together, so names may swap). Run --sync afterwards

A string in a catalog is written as UTF-8. The radio draws one byte a character: ASCII, the Latin-1 letters (U+00A0 to
U+00FF) and the extra glyphs of core/ui/font.h, written here as the characters they stand for. Text alternates
(TEXT_ALT_OPEN, _NEXT, _CLOSE) are written {longest|shorter|...}.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, "core", "ui", "lang")
SOURCE_DIRS = ["core"]

# core/ui/font.h: the macros a marked string may use, the byte each stands for and how a catalog writes it
MACROS = {
    "GLYPH_ELLIPSIS": (0x80, "\u2026"),
    "GLYPH_DOT": (0x81, "\u00b7"),
    "GLYPH_ARROW": (0x82, "\u2192"),
    "GLYPH_DEGREE": (0x83, "\u00b0"),
    "GLYPH_SHARE": (0x84, "\u2912"),
    "GLYPH_DOWNLOAD": (0x85, "\u2913"),
    "TEXT_ALT_OPEN": (0x1D, "{"),
    "TEXT_ALT_NEXT": (0x1F, "|"),
    "TEXT_ALT_CLOSE": (0x1E, "}"),
}
BYTE_TO_TEXT = {b: t for b, t in MACROS.values()}
TEXT_TO_BYTE = {t: b for b, t in MACROS.values()}
MAX_BYTES = 255


class I18nError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# The source: TR("...") and TRN("...") with string literals and the macros above

MARK = re.compile(r"(?<![A-Za-z0-9_])(TR[NX]?)\s*\(")
TOKEN = re.compile(r'\s+|/\*.*?\*/|//[^\n]*|"(?:[^"\\\n]|\\.)*"|[A-Za-z_][A-Za-z0-9_]*|\)|.', re.S)
SIMPLE_ESC = {"n": 10, "t": 9, "r": 13, "\\": 92, '"': 34, "'": 39, "?": 63, "a": 7, "b": 8, "f": 12, "v": 11, "0": 0}


def c_literal_bytes(lit, where):
    """The bytes of a C string literal (quotes included)"""
    out = bytearray()
    s = lit[1:-1]
    i = 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out += c.encode("ascii")
            i += 1
            continue
        e = s[i + 1]
        if e == "x":
            m = re.match(r"[0-9A-Fa-f]+", s[i + 2:])
            out.append(int(m.group(0), 16) & 0xFF)
            i += 2 + len(m.group(0))
        elif e in "01234567":
            m = re.match(r"[0-7]{1,3}", s[i + 1:])
            out.append(int(m.group(0), 8) & 0xFF)
            i += 1 + len(m.group(0))
        elif e in SIMPLE_ESC:
            out.append(SIMPLE_ESC[e])
            i += 2
        else:
            raise I18nError("%s: unknown escape \\%s" % (where, e))
    return bytes(out)


def bytes_to_text(b, where):
    """A marked string's bytes as a catalog writes them"""
    out = []
    for v in b:
        if v in BYTE_TO_TEXT:
            out.append(BYTE_TO_TEXT[v])
        elif 0x20 <= v < 0x7F:
            if chr(v) in "{|}":
                raise I18nError("%s: a marked string can't contain %r (it writes text alternates)" % (where, chr(v)))
            out.append(chr(v))
        else:
            raise I18nError("%s: byte 0x%02X in a marked string (English is ASCII plus font.h's glyphs)" % (where, v))
    return "".join(out)


def text_to_bytes(text, where):
    """A catalog string as the bytes the radio draws"""
    out = bytearray()
    for ch in text:
        if ch in TEXT_TO_BYTE:
            out.append(TEXT_TO_BYTE[ch])
        elif 0x20 <= ord(ch) < 0x7F or 0xA0 <= ord(ch) <= 0xFF:
            out.append(ord(ch))
        else:
            raise I18nError("%s: %r (U+%04X) can't be drawn: use ASCII, Latin-1 (U+00A0-U+00FF) or one of %s"
                    % (where, ch, ord(ch), " ".join(TEXT_TO_BYTE)))
    return bytes(out)


# A CJK catalog: the Latin fonts have no glyph for it, so the catalog says its text is UTF-8 that the text layer
# decodes, and the glyphs come from core/ui/cjk_font_data.c, which tools/cjkfont.py builds from the catalogs
# themselves. Every other catalog stays one byte a character (ASCII, Latin-1, font.h's glyphs).
CJK_LANGS = ("zh", "ja", "ko")
WIDE_HEADER = "X-Glyphs"
# A CJK catalog names itself in its own script, which a build without the CJK face draws as boxes: this header is the
# name such a build lists instead (Settings > Display > Language, docs/dev/translating.md)
LATIN_NAME_HEADER = "X-Language-Name-Latin"
CJK_TABLE = os.path.join(ROOT, "core", "ui", "cjk_font_data.c")
cjk_cps = None


def cjk_codepoints():
    """The code points the generated CJK font has; empty when it hasn't been built"""
    global cjk_cps
    if cjk_cps is None:
        try:
            with open(CJK_TABLE, encoding="utf-8") as f:
                src = f.read()
        except OSError:
            cjk_cps = frozenset()
            return cjk_cps
        m = re.search(r"cjk_font_cp\[\]\s*=\s*\{(.*?)\};", src, re.S)
        cjk_cps = frozenset(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{1,4})\b", m.group(1))) if m else frozenset()
    return cjk_cps


def text_to_bytes_wide(text, where):
    """A CJK catalog's string as the firmware stores it: UTF-8, each character either a byte the Latin font draws or a
    glyph of the CJK table. A character with no glyph is an error: the font is built from the catalogs themselves."""
    out = bytearray()
    have = cjk_codepoints()
    for ch in text:
        if ch in TEXT_TO_BYTE:
            out.append(TEXT_TO_BYTE[ch])
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ord(ch))
        elif ord(ch) <= 0x7FF:
            # The text layer reads only three-byte sequences as UTF-8; a two-byte one would be two Latin-1 bytes, and
            # Latin-1 text (a name, a message, the Spanish catalog) must stay readable in a build with the CJK face
            raise I18nError("%s: %r (U+%04X) is a two-byte UTF-8 character: a CJK catalog can't carry it (write it "
                            "in ASCII)" % (where, ch, ord(ch)))
        elif ord(ch) in have:
            out += ch.encode("utf-8")
        elif not have:
            raise I18nError("%s: %r (U+%04X) has no CJK glyph: build core/ui/cjk_font_data.c with tools/cjkfont.py "
                    "--po <catalog>" % (where, ch, ord(ch)))
        else:
            raise I18nError("%s: %r (U+%04X) has no CJK glyph: run tools/cjkfont.py --po <catalog> to add it"
                    % (where, ch, ord(ch)))
    return bytes(out)


def source_files():
    files = []
    for d in SOURCE_DIRS:
        for ext in ("c", "h"):
            files += glob.glob(os.path.join(ROOT, d, "**", "*." + ext), recursive=True)
    return sorted(os.path.relpath(f, ROOT).replace(os.sep, "/") for f in files)


# A key is a C identifier; TR_<KEY> is its number in the generated tr_keys.h. These names are taken: the "none" value
# and the generated header's own, and the glyph macros (a mark's text may start with one)
KEY = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")
RESERVED_KEYS = {"NONE", "KEYS", "TRANSLATED"} | set(MACROS)
# Where code defines TR_<NAME> identifiers of its own (a radio's TR_TX); a key must not make the same name, since
# nearly every file includes i18n.h through settings.h
IDENT_DIRS = ["core", "drivers", "platform", "boards", "hal"]
TR_IDENT = re.compile(r"(?<![A-Za-z0-9_])TR_([A-Za-z0-9_]+)")
taken_cache = None


def taken_keys():
    """The key names that would make a TR_<KEY> the source already uses for something else"""
    global taken_cache
    if taken_cache is None:
        found = set(RESERVED_KEYS)
        for d in IDENT_DIRS:
            for ext in ("c", "h"):
                for f in glob.glob(os.path.join(ROOT, d, "**", "*." + ext), recursive=True):
                    with open(f, encoding="utf-8", errors="replace") as fh:
                        src = fh.read()
                    if "TR_" in src:
                        found.update(TR_IDENT.findall(src))
        taken_cache = found
    return taken_cache


class Mark:
    """One TR / TRN / TRX in the source. key is None for the old form TR("...") (only --rewrite reads it); at is
    where the key goes: just after the opening parenthesis"""
    def __init__(self, kind, key, text, rel, line, at):
        self.kind, self.key, self.text, self.rel, self.line, self.at = kind, key, text, rel, line, at

    def where(self):
        return "%s:%d" % (self.rel, self.line)


def scan(rel, src, old_ok=False):
    """The marks of one file"""
    out = []
    for m in MARK.finditer(src):
        line_start = src.rfind("\n", 0, m.start()) + 1
        head = src[line_start:m.start()].strip()
        if head == "#define" or head.startswith(("//", "*", "/*")):
            continue    # the definition of TR / TRN / TRX itself, or a comment; a macro whose value is TR(...) counts
        kind = m.group(1)
        where = "%s:%d" % (rel, src.count("\n", 0, m.start()) + 1)
        toks = []
        pos = m.end()
        while True:
            t = TOKEN.match(src, pos)
            if t is None:
                raise I18nError("%s: unterminated %s(" % (where, kind))
            tok = t.group(0)
            pos = t.end()
            if tok.isspace() or tok.startswith(("/*", "//")):
                continue
            if tok == ")":
                break
            toks.append(tok)
        key = None
        if len(toks) >= 2 and toks[1] == "," and KEY.match(toks[0]) and toks[0] not in MACROS:
            key = toks[0]
            toks = toks[2:]
        elif not old_ok:
            raise I18nError("%s: %s() takes a key and the English: %s(KEY, \"English\") (tools/i18n.py --rewrite "
                            "gives an old-form mark one)" % (where, kind, kind))
        parts = bytearray()
        for tok in toks:
            if tok.startswith('"'):
                parts += c_literal_bytes(tok, where)
            elif tok in MACROS:
                parts.append(MACROS[tok][0])
            else:
                raise I18nError("%s: %s() takes a key, then string literals and font.h's glyph macros only, not %r "
                                "(use tr() on a tr_id_t for text chosen at run time)" % (where, kind, tok))
        if not parts:
            raise I18nError("%s: %s() without English" % (where, kind))
        out.append(Mark(kind, key, bytes_to_text(bytes(parts), where), rel, int(where.rsplit(":", 1)[1]), m.end()))
    return out


def marks(old_ok=False, files=None):
    out = []
    for rel in files if files is not None else source_files():
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
            src = f.read()
        if "TR" in src:
            out += scan(rel, src, old_ok)
    return out


def check_keys(found):
    """A key has one English and is either translated (TR, TRN) or never (TRX); its name is free. Returns
    {key: (text, translated)}"""
    keys = {}
    taken = taken_keys()
    for mk in found:
        if mk.key is None:
            continue
        if mk.key in taken:
            raise I18nError("%s: key %s: TR_%s is taken (i18n.h, a glyph macro, or an identifier in the source): "
                            "pick another" % (mk.where(), mk.key, mk.key))
        text, tr = keys.setdefault(mk.key, (mk.text, mk.kind != "TRX"))
        if text != mk.text:
            raise I18nError("%s: key %s is %r here and %r elsewhere: one key, one English (give one a key of its "
                            "own)" % (mk.where(), mk.key, mk.text, text))
        if tr != (mk.kind != "TRX"):
            raise I18nError("%s: key %s is TRX in one place and translated in another: pick one"
                            % (mk.where(), mk.key))
    return keys


def extract(found=None):
    """([(key, english text, [files])] the catalogs translate, in the order the source first uses them,
    [(key, english text)] of the TRX marks); found: the marks (default: the source's)"""
    if found is None:
        found = marks()
    check_keys(found)
    src, trx, seen = {}, {}, set()
    for mk in found:
        if mk.kind == "TRX":
            trx.setdefault(mk.key, mk.text)
            continue
        files = src.setdefault(mk.key, (mk.text, []))[1]
        if mk.rel not in files:
            files.append(mk.rel)
    return [(k, t, f) for k, (t, f) in src.items()], list(trx.items())


# ---------------------------------------------------------------------------------------------------------------
# The rewrite: the old form TR("...") / TRN("...") to TR(KEY, "..."), with a key made from the English. A key already
# in the source for the same English is reused; one already taken is never made again. The names are only a start: any
# key can be renamed by hand later, and nothing depends on how one was made

GLYPH_TEXT = set(t for _, t in MACROS.values())
KEY_MAX = 28


def key_from_text(text):
    """A key name for this English: its words upper-case, joined with _, cut at a word to KEY_MAX characters"""
    t = re.sub(r"\{([^|}]*)[^}]*\}", r"\1", text)    # text alternates: the first (longest) one
    t = "".join(c for c in t if c not in GLYPH_TEXT).replace("'", "")
    words = re.findall(r"[A-Za-z0-9]+", t)
    key = ""
    for w in words:
        if key and len(key) + 1 + len(w) > KEY_MAX:
            break
        key = (key + "_" + w) if key else w
    key = key.upper()[:KEY_MAX] or "TEXT"
    return "N" + key if key[0].isdigit() else key


def rewrite(files=None):
    """Gives every old-form mark in the files (default: the source) a key. Returns the number rewritten"""
    every = marks(old_ok=True)
    if files is not None:
        every += marks(old_ok=True, files=[f for f in files if f not in set(source_files())])
    keys = check_keys(every)
    by_text = {}
    for k, (t, tr) in sorted(keys.items()):
        if tr:
            by_text.setdefault(t, k)
    taken = taken_keys() | set(keys)
    done = 0
    todo = {}
    for mk in every:
        if mk.key is None and (files is None or mk.rel in files):
            todo.setdefault(mk.rel, []).append(mk)
    # Plain words first, so "Zone" is ZONE and "Zone: " or "{Zone|}" the one with a number
    new = []
    for rel in sorted(todo):
        new += [mk.text for mk in todo[rel] if mk.text not in by_text and mk.text not in new]
    for text in sorted(new, key=lambda t: not re.fullmatch(r"[A-Za-z0-9]+( [A-Za-z0-9]+)*", t)):
        base = key_from_text(text)
        key, n = base, 2
        while key in taken:
            key, n = "%s_%d" % (base, n), n + 1
        taken.add(key)
        by_text[text] = key
    for rel in sorted(todo):
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8") as f:
            src = f.read()
        for mk in sorted(todo[rel], key=lambda m: m.at, reverse=True):
            sep = "," if src[mk.at:mk.at + 1].isspace() else ", "
            src = src[:mk.at] + by_text[mk.text] + sep + src[mk.at:]
            done += 1
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(src)
    return done


# ---------------------------------------------------------------------------------------------------------------
# The rename: a key's new name at every mark and in every catalog's msgctxt. The catalogs match by key alone, so a
# key renamed only in the source would lose its translations at the next --sync

RENAME_DIRS = ["tests", "sim"]    # marks outside the source the catalogs are made from: they use its keys


def rename(pairs):
    """Renames keys, [(old, new)], all at once (a name freed by one pair may be taken by another). Returns
    (marks rewritten, catalog entries rewritten)"""
    every = marks()
    keys = check_keys(every)
    names = dict(pairs)
    if len(names) != len(pairs):
        raise I18nError("rename: a key is listed twice")
    taken = taken_keys()
    after = set(keys) - set(names)
    for old, new in pairs:
        if old not in keys:
            raise I18nError("rename: no key %s in the source" % old)
        if not KEY.match(new) or new in MACROS:
            raise I18nError("rename: %s is not a key name (a C identifier)" % new)
        if new in taken:
            raise I18nError("rename: TR_%s is taken (i18n.h, a glyph macro, or an identifier in the source)" % new)
        if new in after:
            raise I18nError("rename: %s is a key already (listed twice as a new name, or one that stays)" % new)
        after.add(new)
    extra = []
    for d in RENAME_DIRS:
        for ext in ("c", "h"):
            extra += glob.glob(os.path.join(ROOT, d, "**", "*." + ext), recursive=True)
    every += marks(old_ok=True, files=sorted(os.path.relpath(f, ROOT).replace(os.sep, "/") for f in extra))
    todo = {}
    for mk in every:
        if mk.key in names:
            todo.setdefault(mk.rel, []).append(mk)
    done = 0
    for rel in sorted(todo):
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8") as f:
            src = f.read()
        for mk in sorted(todo[rel], key=lambda m: m.at, reverse=True):
            at = mk.at
            while not src.startswith(mk.key, at):    # white space or a comment between the parenthesis and the key
                at += 1
            src = src[:at] + names[mk.key] + src[at + len(mk.key):]
            done += 1
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(src)
    entries = 0
    ctxt = re.compile(r'^msgctxt "([^"]*)"$', re.M)

    def renamed(m):
        nonlocal entries
        if m.group(1) not in names:
            return m.group(0)
        entries += 1
        return 'msgctxt "%s"' % names[m.group(1)]

    for path in catalogs().values():
        with open(path, encoding="utf-8") as f:
            text = f.read()
        out = ctxt.sub(renamed, text)
        if out != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(out)
    return done, entries


# ---------------------------------------------------------------------------------------------------------------
# Catalogs: gettext .po, written by this tool (one line a string, references by file only so that edits elsewhere in a
# file don't touch the catalog)

def po_quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'


def po_unquote(s, where):
    if not (s.startswith('"') and s.endswith('"')):
        raise I18nError("%s: expected a quoted string" % where)
    out, i, s = [], 0, s[1:-1]
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"'}.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def read_po(path):
    """(headers dict, [(key, msgid, msgstr, fuzzy)]); key is the msgctxt (None in a catalog from before the keys);
    fuzzy is False, True, or the English the translation was made for (a "#| msgid" line: the source's English changed
    since)"""
    entries = []
    cur = None
    field = None
    fuzzy = False
    ctxt = None
    rel = os.path.relpath(path, ROOT)

    def flush():
        nonlocal cur
        if cur is not None and "msgid" in cur:
            entries.append((cur.get("msgctxt"), cur["msgid"], cur.get("msgstr", ""), cur.get("fuzzy", False)))
        cur = None

    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    for n, line in enumerate(lines, 1):
        where = "%s:%d" % (rel, n)
        s = line.strip()
        if not s:
            flush()
            fuzzy = False
            ctxt = None
            continue
        if s.startswith("#"):
            if s.startswith("#,") and "fuzzy" in s:
                fuzzy = fuzzy or True
            elif s.startswith("#| msgid "):
                fuzzy = po_unquote(s[len("#| msgid "):], where)
            continue
        if s.startswith("msgid_plural") or s.startswith("msgstr["):
            raise I18nError("%s: plural forms aren't supported" % where)
        m = re.match(r"(msgctxt|msgid|msgstr)\s+(\".*\")$", s)
        if m:
            if m.group(1) == "msgctxt":
                flush()
                ctxt = po_unquote(m.group(2), where)
                field = None
                continue
            if m.group(1) == "msgid":
                flush()
                cur = {"fuzzy": fuzzy}
                if ctxt is not None:
                    cur["msgctxt"] = ctxt
                fuzzy = False
                ctxt = None
            elif cur is None:
                raise I18nError("%s: msgstr without msgid" % where)
            field = m.group(1)
            cur[field] = po_unquote(m.group(2), where)
        elif s.startswith('"') and cur is not None and field is not None:
            cur[field] += po_unquote(s, where)
        else:
            raise I18nError("%s: can't read %r" % (where, s))
    flush()
    headers = {}
    body = []
    for key, msgid, msgstr, fz in entries:
        if msgid == "" and key is None:
            for h in msgstr.split("\n"):
                if ":" in h:
                    k, _, v = h.partition(":")
                    headers[k.strip()] = v.strip()
        else:
            body.append((key, msgid, msgstr, fz))
    return headers, body


HEADER_ORDER = ["Project-Id-Version", "Language", "X-Language-Name", LATIN_NAME_HEADER, "MIME-Version",
        "Content-Type", "Content-Transfer-Encoding"]


def write_po(path, headers, entries, refs):
    """entries: [(key, msgid, msgstr, fuzzy)] in source order; refs: {key: [files]}"""
    code = headers.get("Language", "")
    out = ["# SPDX-License-Identifier: MIT",
           "# Copyright (c) 2026 The RadioFoundry Authors",
           "#",
           "# The radio's strings in %s (%s; docs/dev/translating.md). Kept in step with the source by"
           % (headers.get("X-Language-Name", code), code),
           "# tools/i18n.py --sync: translate the empty msgstr lines; an empty one shows the English.",
           "msgid \"\"",
           "msgstr \"\""]
    for k in HEADER_ORDER + sorted(set(headers) - set(HEADER_ORDER)):
        if k in headers:
            out.append(po_quote("%s: %s\n" % (k, headers[k])))
    for key, msgid, msgstr, fz in entries:
        out.append("")
        if key in refs:
            out.append("#: " + " ".join(refs[key]))
        if fz:
            out.append("#, fuzzy")
            if isinstance(fz, str):
                out.append("#| msgid " + po_quote(fz))
        if key is not None:
            out.append("msgctxt " + po_quote(key))
        out.append("msgid " + po_quote(msgid))
        out.append("msgstr " + po_quote(msgstr))
    text = "\n".join(out) + "\n"
    old = None
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            old = f.read()
    if old != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return True
    return False


def catalogs():
    """{code: path}, sorted by code"""
    return {os.path.splitext(os.path.basename(p))[0]: p for p in sorted(glob.glob(os.path.join(LANG_DIR, "*.po")))}


def synced(path, source):
    """The catalog's (headers, entries) brought up to date with the source [(key, english, files)], and the refs. An
    entry is the key's: a key whose English changed keeps its translation, still in use (tools/i18n_changes.py warns on
    the pull request); a fuzzy mark a translator set stays; a key gone from the source is dropped. An entry without a key (a catalog
    from before the keys) goes to the key with its English."""
    headers, body = read_po(path)
    by_key = {k: (m, s, fz) for k, m, s, fz in body if k is not None}
    by_text = {m: (m, s, fz) for k, m, s, fz in body if k is None}
    entries = []
    for key, text, _ in source:
        old = by_key.get(key) or by_text.get(text)
        if old is None:
            entries.append((key, text, "", False))
            continue
        msgid, msgstr, fz = old
        if isinstance(fz, str) and fz == text:
            fz = False      # marked as made for other English, which is the English again
        if not msgstr:
            fz = False
        entries.append((key, text, msgstr, fz))
    return headers, entries, {k: f for k, _, f in source}


# ---------------------------------------------------------------------------------------------------------------
# Checks on a translation

def alt_groups(text):
    """Number of text alternate groups; raises on unbalanced braces"""
    depth = groups = 0
    for ch in text:
        if ch == "{":
            if depth:
                raise ValueError("nested {")
            depth, groups = 1, groups + 1
        elif ch == "}":
            if not depth:
                raise ValueError("} without {")
            depth = 0
        elif ch == "|" and not depth:
            raise ValueError("| outside {...}")
    if depth:
        raise ValueError("{ without }")
    return groups


def check_translation(en, tr, where, wide=False):
    """Problems of one translation (a list of strings)"""
    errs = []
    try:
        b = text_to_bytes_wide(tr, where) if wide else text_to_bytes(tr, where)
        if len(b) > MAX_BYTES:
            errs.append("%s: longer than %d bytes" % (where, MAX_BYTES))
    except I18nError as e:
        errs.append(str(e))
    try:
        if alt_groups(tr) != alt_groups(en):
            errs.append("%s: %r has %d text alternate groups {...}, the English %d"
                    % (where, tr, alt_groups(tr), alt_groups(en)))
    except ValueError as e:
        errs.append("%s: %r: %s" % (where, tr, e))
    lead = lambda s: len(s) - len(s.lstrip(" "))
    trail = lambda s: len(s) - len(s.rstrip(" "))
    if lead(en) != lead(tr) or trail(en) != trail(tr):
        errs.append("%s: %r must start and end with the same spaces as %r (it is joined to other text)" % (where, tr, en))
    if en.endswith(":") != tr.endswith(":") and not tr.endswith(" :"):
        errs.append("%s: %r: keep the English's ending colon (a value follows it)" % (where, tr))
    return errs


def pending(rel, entries):
    """The strings still to do in a catalog: untranslated, or marked fuzzy (in doubt, not used). A pull request leaves
    none (docs/dev/translating.md)"""
    todo = [(m, f) for _, m, s, f in entries if not s or f]
    if not todo:
        return []
    out = ["%s: %d string(s) to translate or check before the merge (the radio shows them in English):"
           % (rel, len(todo))]
    for m, f in todo[:15]:
        if isinstance(f, str):
            out.append("    %r: marked fuzzy, made for %r: check the translation, then remove #, fuzzy and #|"
                       % (m, f))
        elif f:
            out.append("    %r: marked fuzzy: check the translation, then remove #, fuzzy" % m)
        else:
            out.append("    %r: not translated" % m)
    if len(todo) > 15:
        out.append("    ... and %d more (tools/i18n.py --stats)" % (len(todo) - 15))
    return out


def check_switches(code):
    """The language's build switches (CONFIG_LANG_<CODE>, CONFIG_LANG_DEFAULT_<CODE>) are in the places a switch must
    be (docs/dev/translating.md)"""
    sw = "LANG_" + code.upper()
    dflt = "LANG_DEFAULT_" + code.upper()
    errs = []
    with open(os.path.join(ROOT, "config", "features.mk"), encoding="utf-8") as f:
        mk = f.read()
    m = re.search(r"^FEATURES :=((?:.*\\\n)*.*)$", mk, re.M)
    with open(os.path.join(ROOT, "core", "config.h"), encoding="utf-8") as f:
        h = f.read()
    with open(os.path.join(ROOT, "docs", "build-config.md"), encoding="utf-8") as f:
        doc = f.read()
    for s in (sw, dflt):
        if not re.search(r"^CONFIG_%s \?=" % s, mk, re.M):
            errs.append("config/features.mk: no CONFIG_%s default" % s)
        if not m or s not in m.group(1).split():
            errs.append("config/features.mk: %s is not in FEATURES" % s)
        if "#define CONFIG_%s" % s not in h:
            errs.append("core/config.h: no CONFIG_%s default" % s)
        if "`CONFIG_%s`" % s not in doc:
            errs.append("docs/build-config.md: CONFIG_%s isn't listed" % s)
    if "$(error CONFIG_%s=y needs CONFIG_%s=y)" % (dflt, sw) not in mk:
        errs.append("config/features.mk: no check that CONFIG_%s needs CONFIG_%s" % (dflt, sw))
    m = re.search(r"#define CONFIG_LANG_ANY\s+\(([^)]*)\)", h)
    if not m or "CONFIG_" + sw not in m.group(1):
        errs.append("core/config.h: CONFIG_LANG_ANY doesn't include CONFIG_%s" % sw)
    return errs


# ---------------------------------------------------------------------------------------------------------------
# The C tables

def c_bytes(b):
    """A C string literal of these bytes (a hex escape ends its literal: the next character can't extend it)"""
    out, lit = [], ""
    for v in b:
        if 0x20 <= v < 0x7F and chr(v) not in '"\\?':
            lit += chr(v)
        else:
            lit += "\\x%02X" % v
            out.append(lit)
            lit = ""
    out.append(lit)
    return " ".join('"%s"' % s for s in out if s != "") or '""'


LANG_STRIDE = 8     # an offset for every this many strings; a lookup steps over the rest


def string_table(strings):
    """(text, offsets) of one language: the strings NUL-terminated in id order, where every LANG_STRIDE-th starts"""
    blob, offs = bytearray(), []
    for i, b in enumerate(strings):
        if i % LANG_STRIDE == 0:
            offs.append(len(blob))
        blob += b + b"\0"
    return bytes(blob), offs


def tables(found=None):
    """What core/ui/i18n.c reads. Every key has a number: 0 none, then the keys the catalogs translate (TR, TRN), by
    name, then the TRX ones, by name. The English table has every key's text; each language the translated keys', ""
    where it has none (or the English itself). Flash per string: the text and its NUL per language, plus 2 bytes per
    LANG_STRIDE strings. Returns (ids {key: number}, translated (the first TRX number), english (text, offsets),
    [(code, name, name in this build, latin name, switch, {number: translation}, text, offsets, wide)])"""
    source, trx = extract(found)
    english = {k: t for k, t, _ in source}
    english.update(trx)
    order = [None] + sorted(k for k, _, _ in source) + sorted(k for k, _ in trx)
    if len(order) > 0xFFFF:
        raise I18nError("more than 65535 keys: a tr_id_t is 16 bits")
    ids = {k: i for i, k in enumerate(order) if k is not None}
    translated = len(source) + 1
    en_bytes = [b""] + [text_to_bytes(english[k], k) for k in order[1:]]
    en_blob, en_offs = string_table(en_bytes)
    if len(en_blob) > 0xFFFF:
        raise I18nError("more than 64 KB of English text")
    out = []
    for code, path in catalogs().items():
        headers, body = read_po(path)
        name = headers.get("X-Language-Name", code)
        wide = headers.get(WIDE_HEADER) == "utf8"
        wide_name = text_to_bytes_wide(name, code) if wide else text_to_bytes(name, code)
        if wide:
            latin_name = headers.get(LATIN_NAME_HEADER, "")
            if not latin_name:
                raise I18nError("%s: a UTF-8 catalog needs a \"%s\" header (the name a build without the CJK face "
                                "lists, docs/dev/translating.md)" % (os.path.relpath(path, ROOT), LATIN_NAME_HEADER))
            text_to_bytes(latin_name, "%s: %s" % (os.path.relpath(path, ROOT), LATIN_NAME_HEADER))
        else:
            latin_name = name
        pairs = {}
        for key, msgid, msgstr, fz in body:
            where = "%s: %s %r" % (os.path.relpath(path, ROOT), key, msgid)
            if key not in ids or ids[key] >= translated or not msgstr or fz:
                continue
            if english[key] != msgid:
                continue    # the catalog is not synced with this English (--check fails); the build shows the English
            if check_translation(msgid, msgstr, where, wide):
                continue    # --check reports it; the build shows the English
            tr = text_to_bytes_wide(msgstr, where) if wide else text_to_bytes(msgstr, where)
            if tr == en_bytes[ids[key]]:
                continue    # the English is shown anyway
            pairs[ids[key]] = tr
        blob, offs = string_table([pairs.get(i, b"") for i in range(translated)])
        if len(blob) > 0xFFFF:
            raise I18nError("%s: more than 64 KB of text" % code)
        out.append((code, name, wide_name, latin_name, "CONFIG_LANG_" + code.upper(), pairs, blob, offs, wide))
    return ids, translated, (en_blob, en_offs), out


def lookup(blob, offs, i):
    """The string at number i of one language's table, as core/ui/i18n.c finds it"""
    s = offs[i // LANG_STRIDE]
    for _ in range(i % LANG_STRIDE):
        s = blob.index(b"\0", s) + 1
    return blob[s:blob.index(b"\0", s)]


def tr(ids, translated, english, lang, key):
    """tr() in core/ui/i18n.c: the bytes key shows in a language (one entry of tables(), or None for English)"""
    i = ids[key]
    if lang is not None and i < translated:
        s = lookup(lang[6], lang[7], i)
        if s:
            return s
    return lookup(english[0], english[1], i)


def emit_table(lines, code, blob, offs):
    lines.append("static const uint16_t lang_%s_off[] = {" % code)
    for i in range(0, len(offs), 12):
        lines.append("\t" + " ".join("%d," % o for o in offs[i:i + 12]))
    lines.append("};")
    lines.append("static const char lang_%s_text[] =" % code)
    p = 0
    while p < len(blob):
        e = blob.index(b"\0", p)
        lines.append("\t%s \"\\0\"" % c_bytes(blob[p:e]))
        p = e + 1
    lines.append("\t;")
    lines.append("static const lang_table_t lang_%s_table = { lang_%s_off, lang_%s_text };" % (code, code, code))


def write_if_changed(path, text):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    old = None
    if os.path.exists(path):
        # UTF-8 since a CJK language's name is in the comments (the strings themselves are escaped bytes); replace keeps
        # a header written by an older version readable
        with open(path, encoding="utf-8", errors="replace") as f:
            old = f.read()
    if old != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return True
    return False


KEYS_HEADER = "tr_keys.h"


def generate(out_path, found=None):
    """OUT.h: the tables core/ui/i18n.c includes; tr_keys.h beside it: the keys' numbers, which i18n.h includes"""
    ids, translated, english, built = tables(found)
    keys = ["// Generated by tools/i18n.py from the marks in core/; do not edit",
            "// A key's number (i18n.h): the ones the catalogs translate, then the TRX ones",
            "#ifndef TR_KEYS_H", "#define TR_KEYS_H", "", "enum {"]
    for k, i in sorted(ids.items(), key=lambda kv: kv[1]):
        keys.append("\tTR_%s = %d," % (k, i))
    keys += ["};", "#define TR_TRANSLATED\t%d\t// the first TRX key" % translated,
             "#define TR_KEYS\t\t%d" % (len(ids) + 1), "", "#endif"]
    write_if_changed(os.path.join(os.path.dirname(os.path.abspath(out_path)), KEYS_HEADER), "\n".join(keys) + "\n")

    lines = ["// Generated by tools/i18n.py from the marks in core/ and core/ui/lang/*.po; do not edit", ""]
    langs = [(code, name, wide_name, sw) for code, name, wide_name, _, sw, _, _, _, _ in built]
    lines.append("#define LANG_STRIDE\t%d" % LANG_STRIDE)
    lines.append("#if CONFIG_LANG_ANY")
    lines.append("// English: %d keys, %d bytes of text" % (len(ids), len(english[0])))
    emit_table(lines, "en", english[0], english[1])
    lines.append("#endif")
    lines.append("")
    for code, name, wide_name, latin_name, sw, pairs, blob, offs, wide in built:
        if not pairs:
            lines.append("// %s: no translations, every lookup the English" % name)
            lines.append("#define LANG_%s_TABLE\tNULL" % code.upper())
            lines.append("")
            continue
        lines.append("#if %s" % sw)
        lines.append("// %s: %d strings, %d bytes of text" % (name, len(pairs), len(blob)))
        emit_table(lines, code, blob, offs)
        lines.append("#define LANG_%s_TABLE\t(&lang_%s_table)" % (code.upper(), code))
        lines.append("#else")
        lines.append("#define LANG_%s_TABLE\tNULL" % code.upper())
        lines.append("#endif")
        lines.append("")
    # A CJK language names itself in its own script; a build without the CJK face would draw those bytes as boxes in
    # Settings > Display > Language, so it lists the Latin name instead
    for code, name, wide_name, latin_name, sw, pairs, blob, offs, wide in built:
        if not wide:
            continue
        lines.append("#if %s" % sw)
        lines.append("#define LANG_%s_NAME\t%s" % (code.upper(), c_bytes(wide_name)))
        lines.append("#else")
        lines.append("#define LANG_%s_NAME\t%s" % (code.upper(), c_bytes(text_to_bytes(latin_name, code))))
        lines.append("#endif")
    lines.append("// Every language a catalog exists for, English first; built = its switch is on, wide = its text is UTF-8")
    lines.append("#define LANG_KNOWN\t%d" % (len(langs) + 1))
    lines.append("#define LANG_LIST\t{ \\")
    lines.append("\t{ \"en\", \"English\", true, false, NULL }, \\")
    for code, name, wide_name, latin_name, sw, pairs, blob, offs, wide in built:
        lines.append("\t{ \"%s\", %s, %s, %s, LANG_%s_TABLE }, \\" % (
            code, "LANG_%s_NAME" % code.upper() if wide else c_bytes(wide_name), sw,
            "true" if wide else "false", code.upper()))
    lines.append("}")
    lines.append("// The language an empty setting means: the first CONFIG_LANG_DEFAULT_<CODE> on, else English")
    for i, (code, _, _, _) in enumerate(langs):
        lines.append("#%s CONFIG_LANG_DEFAULT_%s" % ("if" if i == 0 else "elif", code.upper()))
        lines.append("#define LANG_DEFAULT\t%d" % (i + 1))
    if langs:
        lines.append("#else")
    lines.append("#define LANG_DEFAULT\tLANG_ENGLISH")
    if langs:
        lines.append("#endif")
    if write_if_changed(out_path, "\n".join(lines) + "\n"):
        print("changed")


# ---------------------------------------------------------------------------------------------------------------

def main():
    args = sys.argv[1:]
    try:
        if args[:1] == ["--gen"] and len(args) == 2:
            generate(args[1])
            return 0
        if args[:1] == ["--new"] and len(args) in (3, 4):
            code, name = args[1], args[2]
            if not re.fullmatch(r"[a-z]{2}", code) or code == "en":
                raise I18nError("CODE: an ISO 639-1 code other than en, two lower-case letters")
            wide = code in CJK_LANGS
            if wide:
                if not name.strip():
                    raise I18nError("NAME: the language's own name")
                if len(args) != 4:
                    raise I18nError("LATIN-NAME: the name as a build without the CJK face lists it (ASCII, "
                                    "\"Chinese\" for 中文)")
                text_to_bytes(args[3], "LATIN-NAME")
            else:
                text_to_bytes(name, "NAME")
            path = os.path.join(LANG_DIR, code + ".po")
            if os.path.exists(path):
                raise I18nError("%s exists" % os.path.relpath(path, ROOT))
            os.makedirs(LANG_DIR, exist_ok=True)
            source, _ = extract()
            headers = {"Project-Id-Version": "RadioFoundry", "Language": code, "X-Language-Name": name,
                    "MIME-Version": "1.0", "Content-Type": "text/plain; charset=UTF-8",
                    "Content-Transfer-Encoding": "8bit"}
            if wide:
                headers[WIDE_HEADER] = "utf8"
                headers[LATIN_NAME_HEADER] = args[3]
            write_po(path, headers, [(k, t, "", False) for k, t, _ in source], {k: f for k, _, f in source})
            print("wrote %s; next: add the build switches CONFIG_LANG_%s and CONFIG_LANG_DEFAULT_%s "
                  "(docs/dev/translating.md, step 2)%s"
                  % (os.path.relpath(path, ROOT), code.upper(), code.upper(),
                     "; its glyphs come from tools/cjkfont.py --po %s" % os.path.relpath(path, ROOT) if wide else ""))
            return 0
        if args[:1] == ["--rewrite"]:
            files = [os.path.relpath(os.path.abspath(a), ROOT).replace(os.sep, "/") for a in args[1:]] or None
            print("i18n: %d mark(s) given a key" % rewrite(files))
            return 0
        if args[:1] == ["--rename"] and len(args) in (2, 3):
            if len(args) == 3:
                pairs = [(args[1], args[2])]
            else:
                with open(args[1], encoding="utf-8") as f:
                    lines = [line.split() for line in f if line.strip() and not line.startswith("#")]
                if any(len(line) != 2 for line in lines):
                    raise I18nError("%s: each line is \"OLD NEW\"" % args[1])
                pairs = [(a, b) for a, b in lines]
            print("i18n: %d mark(s) and %d catalog entr(ies) renamed; next: tools/i18n.py --sync" % rename(pairs))
            return 0
        if args[:1] in (["--sync"], ["--check"], ["--stats"]):
            only = [a for a in args[1:] if not a.startswith("--")]
            source, _ = extract()
            errs = []
            for code, path in catalogs().items():
                if only and code not in only:
                    continue
                rel = os.path.relpath(path, ROOT)
                headers, entries, refs = synced(path, source)
                if headers.get("Language") != code:
                    errs.append("%s: its Language header isn't %s" % (rel, code))
                if not headers.get("X-Language-Name"):
                    errs.append("%s: no X-Language-Name header (the language's own name)" % rel)
                wide = headers.get(WIDE_HEADER) == "utf8"
                if wide and not headers.get(LATIN_NAME_HEADER):
                    errs.append("%s: no %s header (the name a build without the CJK face lists, e.g. \"Chinese\")"
                                % (rel, LATIN_NAME_HEADER))
                if not wide:
                    bad = [c for _, _, s, _ in entries for c in s if ord(c) > 0xFF and c not in TEXT_TO_BYTE]
                    if bad:
                        errs.append("%s: %r can't be drawn by the Latin fonts: give the catalog a \"%s: utf8\" "
                                    "header and build the CJK font with tools/cjkfont.py" % (rel, bad[0], WIDE_HEADER))
                if args[0] == "--sync":
                    if write_po(path, headers, entries, refs):
                        print("updated %s" % rel)
                elif args[0] == "--check":
                    _, old = read_po(path)
                    if old != entries:
                        errs.append("%s: out of date with the source: run tools/i18n.py --sync" % rel)
                    for _, msgid, msgstr, fz in entries:
                        if msgstr and not fz:
                            errs += check_translation(msgid, msgstr, "%s: %r" % (rel, msgid), wide)
                    errs += check_switches(code)
                    if "--allow-pending" not in sys.argv:
                        errs += pending(rel, entries)
                else:
                    done = sum(1 for _, _, s, f in entries if s and not f)
                    print("%s %-12s %4d of %d translated (%d%%)" % (code, headers.get("X-Language-Name", ""), done,
                                               len(entries), 100 * done // max(1, len(entries))))
            for e in errs:
                print(e, file=sys.stderr)
            if args[0] == "--check" and not errs:
                print("i18n: %d strings, %d catalogs ok" % (len(source), len(catalogs())))
            return 1 if errs else 0
    except I18nError as e:
        print("i18n: %s" % e, file=sys.stderr)
        return 1
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
