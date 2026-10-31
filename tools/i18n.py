#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The firmware's translations (docs/dev/translating.md): the strings the radio shows, marked in core/ with TR("...")
or TRN("..."), and one gettext catalog per language in core/ui/lang/<code>.po.

    tools/i18n.py --sync            bring every catalog up to date with the source (new strings added untranslated,
                                    strings gone from the source dropped); --sync es for one language
    tools/i18n.py --check           exit 1 when a catalog is out of date, has a string not translated or marked
                                    fuzzy, or a translation can't be drawn (tools/ci.sh); --allow-pending: not the
                                    untranslated and fuzzy ones (work in progress, never in a pull request)
    tools/i18n.py --stats           how much of each catalog is translated
    tools/i18n.py --gen OUT.h       the C tables core/ui/i18n.c includes (run by make; OUT.h is rewritten only when
                                    its content changes, and "changed" is printed then)
    tools/i18n.py --new CODE NAME   start a catalog: CODE an ISO 639-1 code, NAME the language's own name ("Español")

A string in a catalog is written as UTF-8. The radio draws one byte a character: ASCII, the Latin-1 letters (U+00A0 to
U+00FF) and the extra glyphs of core/ui/font.h, written here as the characters they stand for. Text alternates
(TEXT_ALT_OPEN, _NEXT, _CLOSE) are written {longest|shorter|...}.
"""
import difflib
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

MARK = re.compile(r"(?<![A-Za-z0-9_])(TRN?)\s*\(")
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


def source_files():
    files = []
    for d in SOURCE_DIRS:
        for ext in ("c", "h"):
            files += glob.glob(os.path.join(ROOT, d, "**", "*." + ext), recursive=True)
    return sorted(os.path.relpath(f, ROOT) for f in files)


def extract():
    """[(english text, [files])] in the order the source first uses them"""
    found = {}
    for rel in source_files():
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
            src = f.read()
        if "TR" not in src:
            continue
        for m in MARK.finditer(src):
            line_start = src.rfind("\n", 0, m.start()) + 1
            head = src[line_start:m.start()].strip()
            if head == "#define" or head.startswith(("//", "*", "/*")):
                continue    # the definition of TR / TRN itself, or a comment; a macro whose value is TR(...) counts
            lineno = src.count("\n", 0, m.start()) + 1
            where = "%s:%d" % (rel, lineno)
            pos = m.end()
            parts = bytearray()
            while True:
                t = TOKEN.match(src, pos)
                if t is None:
                    raise I18nError("%s: unterminated %s(" % (where, m.group(1)))
                tok = t.group(0)
                pos = t.end()
                if tok.isspace() or tok.startswith(("/*", "//")):
                    continue
                if tok == ")":
                    break
                if tok.startswith('"'):
                    parts += c_literal_bytes(tok, where)
                elif tok in MACROS:
                    parts.append(MACROS[tok][0])
                else:
                    raise I18nError("%s: %s() takes string literals and font.h's glyph macros only, not %r (use tr() "
                            "for text known only at run time)" % (where, m.group(1), tok))
            if not parts:
                raise I18nError("%s: empty %s()" % (where, m.group(1)))
            text = bytes_to_text(bytes(parts), where)
            found.setdefault(text, [])
            if rel not in found[text]:
                found[text].append(rel)
    return list(found.items())


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
    """(headers dict, [(msgid, msgstr, fuzzy)]); fuzzy is False, True, or the English the translation was made for
    (a "#| msgid" line: the source's English changed since)"""
    entries = []
    cur = None
    field = None
    fuzzy = False
    rel = os.path.relpath(path, ROOT)

    def flush():
        nonlocal cur
        if cur is not None and "msgid" in cur:
            entries.append((cur["msgid"], cur.get("msgstr", ""), cur.get("fuzzy", False)))
        cur = None

    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    for n, line in enumerate(lines, 1):
        where = "%s:%d" % (rel, n)
        s = line.strip()
        if not s:
            flush()
            fuzzy = False
            continue
        if s.startswith("#"):
            if s.startswith("#,") and "fuzzy" in s:
                fuzzy = fuzzy or True
            elif s.startswith("#| msgid "):
                fuzzy = po_unquote(s[len("#| msgid "):], where)
            continue
        if s.startswith("msgctxt"):
            raise I18nError("%s: msgctxt isn't supported" % where)
        if s.startswith("msgid_plural") or s.startswith("msgstr["):
            raise I18nError("%s: plural forms aren't supported" % where)
        m = re.match(r"(msgid|msgstr)\s+(\".*\")$", s)
        if m:
            if m.group(1) == "msgid":
                flush()
                cur = {"fuzzy": fuzzy}
                fuzzy = False
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
    for msgid, msgstr, fz in entries:
        if msgid == "":
            for h in msgstr.split("\n"):
                if ":" in h:
                    k, _, v = h.partition(":")
                    headers[k.strip()] = v.strip()
        else:
            body.append((msgid, msgstr, fz))
    return headers, body


HEADER_ORDER = ["Project-Id-Version", "Language", "X-Language-Name", "MIME-Version", "Content-Type",
        "Content-Transfer-Encoding"]


def write_po(path, headers, entries, refs):
    """entries: [(msgid, msgstr, fuzzy)] in source order; refs: {msgid: [files]}"""
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
    for msgid, msgstr, fz in entries:
        out.append("")
        if msgid in refs:
            out.append("#: " + " ".join(refs[msgid]))
        if fz:
            out.append("#, fuzzy")
            if isinstance(fz, str):
                out.append("#| msgid " + po_quote(fz))
        out.append("msgid " + po_quote(msgid))
        out.append("msgstr " + po_quote(msgstr))
    text = "\n".join(out) + "\n"
    old = None
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            old = f.read()
    if old != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return True
    return False


def catalogs():
    """{code: path}, sorted by code"""
    return {os.path.splitext(os.path.basename(p))[0]: p for p in sorted(glob.glob(os.path.join(LANG_DIR, "*.po")))}


def reworded(old, new):
    """{new string: the old string it replaces}: of the strings only in new, each one mostly the same words as one only
    in old (a small edit of the English)"""
    gone = [o for o in old if o not in set(new)]
    out = {}
    for text in new:
        if text in old:
            continue
        best, score = None, 0.6
        for o in gone:
            r = difflib.SequenceMatcher(None, o, text).ratio()
            if r > score:
                best, score = o, r
        if best is not None:
            gone.remove(best)
            out[text] = best
    return out


def synced(path, source):
    """The catalog's (headers, entries) brought up to date with the source, and the refs. A string whose English
    changed a little keeps its translation, still in use: an edit of the English often leaves the translation right.
    tools/i18n_changes.py warns a pull request of each one, with its translations."""
    headers, body = read_po(path)
    have = {msgid: (msgstr, fz) for msgid, msgstr, fz in body}
    was = reworded([m for m, s, _ in body if s], [t for t, _ in source])
    entries = []
    for text, _ in source:
        if text in have:
            entries.append((text,) + have[text])
        elif text in was:
            entries.append((text,) + have[was[text]])
        else:
            entries.append((text, "", False))
    return headers, entries, dict(source)


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


def check_translation(en, tr, where):
    """Problems of one translation (a list of strings)"""
    errs = []
    try:
        b = text_to_bytes(tr, where)
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
    todo = [(m, f) for m, s, f in entries if not s or f]
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

def fnv1a(b):
    h = 2166136261
    for v in b:
        h = ((h ^ v) * 16777619) & 0xFFFFFFFF
    return h


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


def tables():
    """What core/ui/i18n.c searches. Every language shares one sorted list of the hashes of the English any catalog
    translates; each language has its translations in that order, "" where it has none (or the English itself), and
    where every LANG_STRIDE-th one starts. Flash per string: 4 bytes once, the text and its NUL per language, plus 2
    bytes per LANG_STRIDE strings. Returns (keys, [(code, name, switch, translations by hash, text, offsets)])"""
    langs, owner = [], {}
    for code, path in catalogs().items():
        headers, body = read_po(path)
        name = headers.get("X-Language-Name", code)
        pairs = {}
        for msgid, msgstr, fz in body:
            if not msgstr or fz:
                continue
            where = "%s: %r" % (os.path.relpath(path, ROOT), msgid)
            if check_translation(msgid, msgstr, where):
                continue    # --check reports it; the build shows the English
            en = text_to_bytes(msgid, where)
            tr = text_to_bytes(msgstr, where)
            if tr == en:
                continue    # the English is shown anyway
            h = fnv1a(en)
            if h in owner and owner[h] != en:
                raise I18nError("%s: hash collides with %r: reword one of them" % (where, owner[h]))
            owner[h] = en
            pairs[h] = tr
        langs.append((code, name, "CONFIG_LANG_" + code.upper(), pairs))
    keys = sorted(owner)
    out = []
    for code, name, sw, pairs in langs:
        blob, offs = bytearray(), []
        for i, h in enumerate(keys):
            if i % LANG_STRIDE == 0:
                offs.append(len(blob))
            blob += pairs.get(h, b"") + b"\0"
        if len(blob) > 0xFFFF:
            raise I18nError("%s: more than 64 KB of text" % code)
        out.append((code, name, sw, pairs, bytes(blob), offs))
    return keys, out


def lookup(keys, blob, offs, en):
    """tr() in core/ui/i18n.c on one language's tables: the translation's bytes, or None for the English"""
    h = fnv1a(en)
    if h not in keys:
        return None
    i = keys.index(h)
    s = offs[i // LANG_STRIDE]
    for _ in range(i % LANG_STRIDE):
        s = blob.index(b"\0", s) + 1
    tr = blob[s:blob.index(b"\0", s)]
    return tr or None


def generate(out_path):
    lines = ["// Generated by tools/i18n.py from core/ui/lang/*.po; do not edit", ""]
    keys, built = tables()
    langs = [(code, name, sw) for code, name, sw, _, _, _ in built]
    if keys:
        lines.append("#if CONFIG_LANG_ANY")
        lines.append("// The English any catalog translates, by hash: %d strings" % len(keys))
        lines.append("#define LANG_STRINGS\t%d" % len(keys))
        lines.append("#define LANG_STRIDE\t%d" % LANG_STRIDE)
        lines.append("static const uint32_t lang_hash[] = {")
        for i in range(0, len(keys), 8):
            lines.append("\t" + " ".join("0x%08Xu," % h for h in keys[i:i + 8]))
        lines.append("};")
        lines.append("#endif")
        lines.append("")
    for code, name, sw, pairs, blob, offs in built:
        if not pairs:
            lines.append("// %s: no translations, every lookup the English" % name)
            lines.append("#define LANG_%s_TABLE\tNULL" % code.upper())
            lines.append("")
            continue
        lines.append("#if %s" % sw)
        lines.append("// %s: %d strings, %d bytes of text" % (name, len(pairs), len(blob)))
        lines.append("static const uint16_t lang_%s_off[] = {" % code)
        for i in range(0, len(offs), 12):
            lines.append("\t" + " ".join("%d," % o for o in offs[i:i + 12]))
        lines.append("};")
        lines.append("static const char lang_%s_text[] =" % code)
        for h in keys:
            lines.append("\t%s \"\\0\"" % c_bytes(pairs.get(h, b"")))
        lines.append("\t;")
        lines.append("static const lang_table_t lang_%s_table = { lang_%s_off, lang_%s_text };" % (code, code, code))
        lines.append("#define LANG_%s_TABLE\t(&lang_%s_table)" % (code.upper(), code))
        lines.append("#else")
        lines.append("#define LANG_%s_TABLE\tNULL" % code.upper())
        lines.append("#endif")
        lines.append("")
    lines.append("// Every language a catalog exists for, English first; built = its switch is on")
    lines.append("#define LANG_KNOWN\t%d" % (len(langs) + 1))
    lines.append("#define LANG_LIST\t{ \\")
    lines.append("\t{ \"en\", \"English\", true, NULL }, \\")
    for code, name, sw in langs:
        lines.append("\t{ \"%s\", %s, %s, LANG_%s_TABLE }, \\" % (code, c_bytes(text_to_bytes(name, code)), sw,
                                       code.upper()))
    lines.append("}")
    lines.append("// The language an empty setting means: the first CONFIG_LANG_DEFAULT_<CODE> on, else English")
    for i, (code, _, _) in enumerate(langs):
        lines.append("#%s CONFIG_LANG_DEFAULT_%s" % ("if" if i == 0 else "elif", code.upper()))
        lines.append("#define LANG_DEFAULT\t%d" % (i + 1))
    if langs:
        lines.append("#else")
    lines.append("#define LANG_DEFAULT\tLANG_ENGLISH")
    if langs:
        lines.append("#endif")
    text = "\n".join(lines) + "\n"
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    old = None
    if os.path.exists(out_path):
        with open(out_path, encoding="latin-1") as f:
            old = f.read()
    if old != text:
        with open(out_path, "w", encoding="latin-1") as f:
            f.write(text)
        print("changed")


# ---------------------------------------------------------------------------------------------------------------

def main():
    args = sys.argv[1:]
    try:
        if args[:1] == ["--gen"] and len(args) == 2:
            generate(args[1])
            return 0
        if args[:1] == ["--new"] and len(args) == 3:
            code, name = args[1], args[2]
            if not re.fullmatch(r"[a-z]{2}", code) or code == "en":
                raise I18nError("CODE: an ISO 639-1 code other than en, two lower-case letters")
            path = os.path.join(LANG_DIR, code + ".po")
            if os.path.exists(path):
                raise I18nError("%s exists" % os.path.relpath(path, ROOT))
            text_to_bytes(name, "NAME")
            os.makedirs(LANG_DIR, exist_ok=True)
            source = extract()
            write_po(path, {"Project-Id-Version": "RadioFoundry", "Language": code, "X-Language-Name": name,
                    "MIME-Version": "1.0", "Content-Type": "text/plain; charset=UTF-8",
                    "Content-Transfer-Encoding": "8bit"},
                 [(t, "", False) for t, _ in source], dict(source))
            print("wrote %s; next: add the build switches CONFIG_LANG_%s and CONFIG_LANG_DEFAULT_%s "
                  "(docs/dev/translating.md, step 2)"
                  % (os.path.relpath(path, ROOT), code.upper(), code.upper()))
            return 0
        if args[:1] in (["--sync"], ["--check"], ["--stats"]):
            only = [a for a in args[1:] if not a.startswith("--")]
            source = extract()
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
                if args[0] == "--sync":
                    if write_po(path, headers, entries, refs):
                        print("updated %s" % rel)
                elif args[0] == "--check":
                    _, old = read_po(path)
                    if [(m, s, f) for m, s, f in old] != entries:
                        errs.append("%s: out of date with the source: run tools/i18n.py --sync" % rel)
                    for msgid, msgstr, fz in entries:
                        if msgstr and not fz:
                            errs += check_translation(msgid, msgstr, "%s: %r" % (rel, msgid))
                    errs += check_switches(code)
                    if "--allow-pending" not in sys.argv:
                        errs += pending(rel, entries)
                else:
                    done = sum(1 for _, s, f in entries if s and not f)
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
