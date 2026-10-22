# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The docs blocks generated from the board registry (tools/boards.py docs), and the radio survey's rows.

A generated block sits between two markers and is rewritten whole:

    <!-- generated: tools/boards.py docs <block>; edit <source> -->
    ...
    <!-- end generated -->

The survey (docs/spec/radios.md, "Summary table") is hand-written but for one cell: the Port cell of a row that names
a board is generated from the board's [status]. Every other row's Port cell says how far the port is:

    Planned[. note]                     Planned, ○ marks
    Planned, hardware needed[. note]    Planned, hardware needed
    In progress, #N[. note]             In progress, linked to pull request N
    Not planned[. note]                 not listed outside the survey

The note is the text the README and the user manual show after the status, so no second summary is written.
"""
import os
import re
import subprocess

REPO_URL = "https://github.com/sfowlr/OpenDigitalRadio"
SURVEY = "docs/spec/radios.md"
SURVEY_HEADING = "Summary table"
PORT_COLUMN = "Port"
BEGIN = "<!-- generated: tools/boards.py docs %s; edit %s -->"
END = "<!-- end generated -->"
BEGIN_RE = re.compile(r"^<!-- generated: tools/boards.py docs ([\w-]+);.*-->$")
PORT_RE = re.compile(r"^(planned, hardware needed|planned|in progress, #(\d+)|not planned)(?:\.\s+(.*))?$", re.I)
# how a board's [status] level reads: in the survey's Port cell, and as the README's "On the radio" mark
LEVEL_WORD = {"supported": "Supported", "testing": "Testing", "bring-up": "Bring-up"}
LEVEL_MARK = {"supported": "●", "testing": "◐", "bring-up": "◐"}
# the README's marks and statuses, lowest first: the floor check compares by these
MARK_RANK = {"○": 0, "◐": 1, "●": 2}
STATUS_RANK = {"planned": 1, "in_progress": 2, "board": 3}


class DocsError(Exception):
    pass


# ── Markdown ────────────────────────────────────────────────────────────────

def split_row(line):
    """The cells of a pipe table row (a backslash-escaped pipe stays in its cell)."""
    cells = re.split(r"(?<!\\)\|", line.strip())
    return [c.strip() for c in cells[1:-1]]


def tables(text):
    """[(heading, first line index, header cells, [(line index, cells)])] of every pipe table, with the nearest ## or
    ### heading above it."""
    out, heading, lines, i = [], "", text.split("\n"), 0
    while i < len(lines):
        m = re.match(r"^#{2,3}\s+(.*)", lines[i])
        if m:
            heading = m.group(1).strip()
        if lines[i].startswith("|") and i + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|\s*$", lines[i + 1]):
            start, head, rows = i, split_row(lines[i]), []
            i += 2
            while i < len(lines) and lines[i].startswith("|"):
                rows.append((i, split_row(lines[i])))
                i += 1
            out.append((heading, start, head, rows))
            continue
        i += 1
    return out


def plain(text):
    """A short Markdown cell as plain text: links to their text, no emphasis or code marks."""
    return re.sub(r"[*`]", "", re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)).strip()


def rebase_links(text, src_dir, dst_dir):
    """text's relative Markdown links, written in a file in src_dir, as dst_dir sees them (both repository
    relative)."""
    def one(m):
        target = m.group(2)
        if re.match(r"^([a-z]+:|#|/)", target):
            return m.group(0)
        path, frag = (target.split("#", 1) + [""])[:2]
        new = os.path.relpath(os.path.normpath(os.path.join(src_dir, path)), dst_dir or ".")
        return "[%s](%s%s)" % (m.group(1), new, "#" + frag if frag else "")
    return re.sub(r"\[([^\]]*)\]\(([^)\s]+)\)", one, text)


def pr_link(n):
    return "[#%d](%s/pull/%d)" % (n, REPO_URL, n)


# ── the board side ──────────────────────────────────────────────────────────

def board_page(b):
    """The page a board's rows link to: its bring-up checklist when it has one, else its hardware page."""
    return b.data["docs"].get("bringup") or b.data["docs"]["hardware"]


def board_title(b, dst_dir):
    """A board's name, linked to its page, then the names it is also sold as (the maker left off where it repeats)
    and "(mobile)" for a mobile radio."""
    d = b.data
    also = [a[len(d["maker"]) + 1:] if a.startswith(d["maker"] + " ") else a for a in d["also"]]
    link = "[%s](%s)" % (d["name"], os.path.relpath(board_page(b), dst_dir or "."))
    return link + "".join(", " + a for a in also) + (" (mobile)" if d["kind"] == "mobile" else "")


def ordered(boards):
    return sorted(boards.values(), key=lambda b: b.data["order"])


def names_board(radio, boards):
    """The board a survey row names (its model, as a whole word, among the row's radios), or None."""
    hits = [b for b in boards.values()
            if re.search(r"(?<![\w-])%s(?![\w-])" % re.escape(b.data["model"]), plain(radio))]
    return hits


# ── the survey ──────────────────────────────────────────────────────────────

def survey_table(text):
    for heading, start, head, rows in tables(text):
        if heading == SURVEY_HEADING and PORT_COLUMN in head:
            return start, head, rows
    raise DocsError("%s: no table with a %s column under '## %s'" % (SURVEY, PORT_COLUMN, SURVEY_HEADING))


def port_cell(b):
    """The survey's Port cell of a board's row."""
    return "%s ([%s](%s))" % (LEVEL_WORD[b.data["status"]["level"]], os.path.basename(board_page(b)),
                              os.path.relpath(board_page(b), os.path.dirname(SURVEY)))


def survey(root, boards):
    """(rows, problems). rows: the survey's radios in its order, {radio, board (id or None), status ("board",
    "planned", "in_progress" or None for not planned), pr, hardware_needed, note (Markdown, links as the survey
    writes them)}. problems: a Port cell outside the fixed set, a row that names two boards, a board named by two
    rows, a hand-written status on a board's row (check 5)."""
    text = open(os.path.join(root, SURVEY), encoding="utf-8").read()
    _, head, rows = survey_table(text)
    col, out, problems, seen = head.index(PORT_COLUMN), [], [], {}
    for line, r in rows:
        if len(r) != len(head):
            problems.append("%s:%d: %d cells, the header has %d" % (SURVEY, line + 1, len(r), len(head)))
            continue
        radio, cell = r[0], r[col]
        hits = names_board(radio, boards)
        if len(hits) > 1:
            problems.append("%s:%d: the row names boards %s" % (SURVEY, line + 1, ", ".join(b.id for b in hits)))
            continue
        if hits:
            b = hits[0]
            if b.id in seen:
                problems.append("%s:%d: boards/%s already has the row on line %d"
                                % (SURVEY, line + 1, b.id, seen[b.id]))
            seen[b.id] = line + 1
            if PORT_RE.match(plain(cell)):
                problems.append("%s:%d: %s names boards/%s, but its Port cell %r is a planned radio's: a board's "
                                "cell comes from its board.toml (tools/boards.py docs)" % (SURVEY, line + 1,
                                                                                         plain(radio), b.id, cell))
            out.append({"radio": plain(radio), "board": b.id, "status": "board", "pr": None,
                        "hardware_needed": False, "note": ""})
            continue
        m = PORT_RE.match(cell)
        if not m:
            problems.append("%s:%d: Port cell %r is none of: planned, planned, hardware needed, in progress, #N, not "
                            "planned (then an optional '. note')" % (SURVEY, line + 1, cell))
            continue
        kind = m.group(1).lower()
        status = None if kind == "not planned" else "in_progress" if kind.startswith("in progress") else "planned"
        out.append({"radio": plain(radio), "board": None, "status": status,
                    "pr": int(m.group(2)) if m.group(2) else None, "hardware_needed": kind.endswith("hardware needed"),
                    "note": (m.group(3) or "").strip()})
    return out, problems


def planned(rows):
    """The survey's radios without a board that are planned or in progress."""
    return [r for r in rows if r["board"] is None and r["status"]]


def planned_status(r, dst_dir):
    """A planned radio's status and note as the README and the user manual show them."""
    if r["status"] == "in_progress":
        s = "In progress (%s)." % pr_link(r["pr"])
    else:
        s = "Planned." + (" Hardware needed." if r["hardware_needed"] else "")
    note = rebase_links(r["note"], os.path.dirname(SURVEY), dst_dir)
    return s + (" " + note.rstrip(".") + "." if note else "")


# ── blocks ──────────────────────────────────────────────────────────────────

def readme_radios(boards, rows):
    out = ["| Radio | Builds | Host tests | On the radio | Support |", "|---|:---:|:---:|:---:|---|"]
    for b in ordered(boards):
        out.append("| %s | ● | ● | %s | %s |" % (board_title(b, ""), LEVEL_MARK[b.data["status"]["level"]],
                                               b.data["status"]["summary"]))
    for r in planned(rows):
        out.append("| %s | ○ | ○ | ○ | %s |" % (r["radio"], planned_status(r, "")))
    return out


def manual_radios(boards, rows):
    """The user manual's radio index: each board's section on the page, what it is also sold as, what works."""
    out = ["| Radio | Also sold as | What works |", "|---|---|---|"]
    for b in ordered(boards):
        d = b.data
        anchor = re.sub(r"[^\w\- ]", "", d["name"].lower()).replace(" ", "-")
        out.append("| [%s](#%s) | %s | %s |" % (d["name"], anchor, ", ".join(d["also"]), d["status"]["summary"]))
    return out


def manual_planned(rows):
    out = ["| Radio | Status |", "|---|---|"]
    for r in planned(rows):
        out.append("| %s | %s |" % (r["radio"], planned_status(r, "docs/user")))
    return out


def print_config(root, bid):
    """[(switch, "y" or "n")] of a board's default build, in config/features.mk's order."""
    r = subprocess.run(["make", "-s", "--no-print-directory", "BOARD=" + bid, "print-config"], cwd=root,
                       capture_output=True, text=True)
    if r.returncode:
        raise DocsError("make BOARD=%s print-config failed: %s" % (bid, r.stderr.strip()[-300:]))
    return [tuple(l.split("=", 1)) for l in r.stdout.split()]


def feature_defaults(root, boards):
    """The feature matrix's table of every switch on every board: on by default, off, or not available."""
    bs = ordered(boards)
    cfg = {b.id: dict(print_config(root, b.id)) for b in bs}
    order = [s for s, _ in print_config(root, bs[0].id)] if bs else []
    out = ["| Feature switch | %s |" % " | ".join(b.data["model"] for b in bs),
           "|---|%s" % ":-:|" * len(bs)]
    for s in order:
        marks = ["✕" if s in b.unsupported else "●" if cfg[b.id].get(s) == "y" else "○" for b in bs]
        out.append("| `%s` | %s |" % (s, " | ".join(marks)))
    return out


# file, block, source the marker names, generator(root, boards, rows)
BLOCKS = (
    ("README.md", "radios", "boards/*/board.toml [status] and docs/spec/radios.md",
     lambda root, boards, rows: readme_radios(boards, rows)),
    ("docs/user/23-radios.md", "radios", "boards/*/board.toml [status]",
     lambda root, boards, rows: manual_radios(boards, rows)),
    ("docs/user/23-radios.md", "planned", "the Port cells of docs/spec/radios.md",
     lambda root, boards, rows: manual_planned(rows)),
    ("docs/feature-matrix.md", "feature-defaults", "config/features.mk and boards/*/config.mk",
     lambda root, boards, rows: feature_defaults(root, boards)),
)


def fill(text, block, source, lines, where):
    """text with block's lines between its markers."""
    pat = re.compile(r"^<!-- generated: tools/boards.py docs %s;[^\n]*-->\n.*?^%s$" % (re.escape(block),
                                                                                       re.escape(END)), re.M | re.S)
    if len(pat.findall(text)) != 1:
        raise DocsError("%s: needs exactly one generated block %r (%s ... %s)" % (where, block,
                                                                             BEGIN % (block, source), END))
    return pat.sub(lambda m: "\n".join([BEGIN % (block, source)] + lines + [END]), text)


def survey_text(text, boards):
    """The survey with each board row's Port cell generated."""
    _, head, rows = survey_table(text)
    col, lines = head.index(PORT_COLUMN), text.split("\n")
    for line, r in rows:
        hits = names_board(r[0], boards)
        if len(hits) == 1 and len(r) == len(head):
            r = list(r)
            r[col] = port_cell(hits[0])
            lines[line] = "| " + " | ".join(r) + " |"
    return "\n".join(lines)


def generate(root, boards):
    """{path: new text} of every file with a generated part, and the survey's problems."""
    rows, problems = survey(root, boards)
    out = {}
    for rel, block, source, gen in BLOCKS:
        text = out.get(rel) or open(os.path.join(root, rel), encoding="utf-8").read()
        out[rel] = fill(text, block, source, gen(root, boards, rows), rel)
    out[SURVEY] = survey_text(open(os.path.join(root, SURVEY), encoding="utf-8").read(), boards)
    return out, problems


def docs(root, boards, check=False):
    """Writes the generated parts (or, with check, lists the files that differ). Returns the problems."""
    new, problems = generate(root, boards)
    if not check:
        problems = []		# what writing fixes (a board row's Port cell) is checked again below
    for rel, text in new.items():
        path = os.path.join(root, rel)
        old = open(path, encoding="utf-8").read()
        if old != text:
            if check:
                problems.append("%s is out of date: run tools/boards.py docs" % rel)
            else:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
    return problems if check else survey(root, boards)[1]


# ── the floor: nothing reads lower than a reference commit says ─────────────

MODEL_CODE = re.compile(r"\b[A-Z]{1,4}-?[A-Z]{0,3}\d+[A-Z0-9]*(?:-[A-Z0-9]+)?\b")


def readme_claims(text):
    """{model code: (status rank, [mark ranks])} of the README's radio table. A row's key is the first model code in
    its Radio cell; a board row (any filled mark) ranks above an in-progress one, above a planned one."""
    out = {}
    for heading, _, head, rows in tables(text):
        if heading != "Radios and status" or "On the radio" not in head:
            continue
        mi = [head.index(c) for c in ("Builds", "Host tests", "On the radio")]
        si = head.index("Support")
        for _, r in rows:
            if len(r) != len(head):
                continue
            m = MODEL_CODE.search(plain(r[0]))
            if not m:
                continue
            marks = [MARK_RANK.get(r[i], 0) for i in mi]
            sup = plain(r[si]).lower()
            status = ("board" if any(marks) else "in_progress" if sup.startswith("in progress") else
                      "planned" if sup.startswith("planned") else None)
            out[m.group(0)] = (STATUS_RANK.get(status, 0), marks)
    return out


def floor(root, ref, new_readme=None):
    """Problems: a radio the README at ref lists that the README now lists lower, or not at all."""
    r = subprocess.run(["git", "show", "%s:README.md" % ref], cwd=root, capture_output=True, text=True)
    if r.returncode:
        raise DocsError("git show %s:README.md: %s" % (ref, r.stderr.strip()))
    old = readme_claims(r.stdout)
    now = readme_claims(new_readme if new_readme is not None else
                        open(os.path.join(root, "README.md"), encoding="utf-8").read())
    problems = []
    for code, (st, marks) in old.items():
        if code not in now:
            problems.append("README.md: %s is listed at %s but not now" % (code, ref))
            continue
        nst, nmarks = now[code]
        if nst < st or any(n < o for n, o in zip(nmarks, marks)):
            problems.append("README.md: %s reads lower than at %s (status %d -> %d, marks %s -> %s)"
                            % (code, ref, st, nst, marks, nmarks))
    return problems
