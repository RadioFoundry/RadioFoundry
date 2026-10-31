# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The docs blocks generated from the board registry (tools/boards.py docs), and the radio survey's rows.

A generated block sits between two markers and is rewritten whole:

    <!-- generated: tools/boards.py docs <block>; edit <source> -->
    ...
    <!-- end generated -->

A YAML file's markers are comments at the block's indent (# generated: ..., # end generated), and a block inside a
Markdown comment has bare markers (generated: ..., end generated), since comments don't nest.

The survey (docs/spec/radios.md, "Summary table") is hand-written but for one cell: the Port cell of a row that names
a board is generated from the board's [status]. Every other row's Port cell says how far the port is:

    Planned[. note]                     Planned, ○ marks
    Planned, hardware needed[. note]    Planned, hardware needed
    In progress, #N[, MARKS][. note]    In progress, linked to pull request N
    Not planned[. note]                 not listed outside the survey

MARKS says what pull request N already has, as a comma list of builds, host tests and on the radio. The README shows
each as ◐ ("in the pull request, not on main yet") until the board merges and its own [status] takes over. In
progress rows come before planned ones.

The note is the text the README and the user manual show after the status, so no second summary is written.

A board's "On the radio" mark: ● supported, ◐ when its bring-up records (docs/testing/results/<board>/) hold a pass,
○ when nothing has passed on a radio yet.
"""
import os
import re
import subprocess

REPO_URL = "https://github.com/radiofoundry/radiofoundry"
SURVEY = "docs/spec/radios.md"
SURVEY_HEADING = "Summary table"
PORT_COLUMN = "Port"
BEGIN = "<!-- generated: tools/boards.py docs %s; edit %s -->"
END = "<!-- end generated -->"
# a block's marker style: (begin, end), each "%s" for the marker text
MARKERS = {"html": ("<!-- %s -->", "<!-- %s -->"), "yaml": ("# %s", "# %s"), "bare": ("%s", "%s")}
BEGIN_RE = re.compile(r"^<!-- generated: tools/boards.py docs ([\w-]+);.*-->$")
PR_MARKS = ("builds", "host tests", "on the radio")
PORT_RE = re.compile(r"^(planned, hardware needed|planned|in progress, #(\d+)((?:, (?:%s))*)|not planned)"
                     r"(?:\.\s+(.*))?$" % "|".join(PR_MARKS), re.I)
# how a board's [status] level reads in the survey's Port cell
LEVEL_WORD = {"supported": "Supported", "testing": "Testing", "bring-up": "Bring-up"}
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
    link = "[%s](%s)" % (d["name"], os.path.relpath(board_page(b), dst_dir or "."))
    return link + "".join(", " + a for a in also_names(b)) + (" (mobile)" if d["kind"] == "mobile" else "")


def also_names(b):
    """The names a board is also sold as, the maker left off where it repeats the board's."""
    d = b.data
    return [a[len(d["maker"]) + 1:] if a.startswith(d["maker"] + " ") else a for a in d["also"]]


def ordered(boards):
    return sorted(boards.values(), key=lambda b: b.data["order"])


def names_board(radio, boards):
    """The boards a survey row names (its model, as a whole word, among the row's radios). A board that is part_of
    another one the row names is that radio's other build, not a second radio: the row is the radio's."""
    hits = [b for b in boards.values()
            if re.search(r"(?<![\w-])%s(?![\w-])" % re.escape(b.data["model"]), plain(radio))]
    ids = {b.id for b in hits}
    return [b for b in hits if b.data.get("part_of") not in ids]


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
    "planned", "in_progress" or None for not planned), pr, pr_marks (what the pull request has, of PR_MARKS),
    hardware_needed, note (Markdown, links as the survey writes them)}. problems: a Port cell outside the fixed set,
    a row that names two boards, a board named by two rows, a hand-written status on a board's row (check 5)."""
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
            out.append({"radio": plain(radio), "board": b.id, "status": "board", "pr": None, "pr_marks": [],
                        "hardware_needed": False, "note": ""})
            continue
        m = PORT_RE.match(cell)
        if not m:
            problems.append("%s:%d: Port cell %r is none of: planned, planned, hardware needed, in progress, #N[, "
                            "%s], not planned (then an optional '. note')" % (SURVEY, line + 1, cell,
                                                                             ", ".join(PR_MARKS)))
            continue
        kind = m.group(1).lower()
        status = None if kind == "not planned" else "in_progress" if kind.startswith("in progress") else "planned"
        marks = [x.strip().lower() for x in (m.group(3) or "").split(",") if x.strip()]
        out.append({"radio": plain(radio), "board": None, "status": status,
                    "pr": int(m.group(2)) if m.group(2) else None, "pr_marks": marks,
                    "hardware_needed": kind.endswith("hardware needed"), "note": (m.group(4) or "").strip()})
    return out, problems


def planned(rows):
    """The survey's radios without a board that are in progress, then those planned, each in the survey's order."""
    return sorted((r for r in rows if r["board"] is None and r["status"]), key=lambda r: r["status"] != "in_progress")


def planned_status(r, dst_dir):
    """A planned radio's status and note as the README and the user manual show them."""
    if r["status"] == "in_progress":
        s = "In progress (%s)." % pr_link(r["pr"])
    else:
        s = "Planned." + (" Hardware needed." if r["hardware_needed"] else "")
    note = rebase_links(r["note"], os.path.dirname(SURVEY), dst_dir)
    return s + (" " + note.rstrip(".") + "." if note else "")


# ── blocks ──────────────────────────────────────────────────────────────────

def ran_on_radio(rec):
    """True when a board's bring-up records ({"runs", "seed"}, tools/bringup.py load_records) hold any pass."""
    if any(t.get("outcome") == "passed" for r in rec.get("runs", []) for t in r.get("tests", [])):
        return True
    return any(isinstance(v, dict) and v.get("result") == "pass"
               for v in ((rec.get("seed") or {}).get("features") or {}).values())


def on_radio_mark(b, records):
    """A board's README "On the radio" mark: ● supported, ◐ something passed on a radio, ○ nothing yet."""
    if b.data["status"]["level"] == "supported":
        return "●"
    return "◐" if ran_on_radio(records.get(b.id, {})) else "○"


def readme_radios(root, boards, rows):
    import bringup
    records = bringup.load_records(os.path.join(root, "docs", "testing", "results"))
    out = ["| Radio | Builds | Host tests | On the radio | Support |", "|---|:---:|:---:|:---:|---|"]
    for b in ordered(boards):
        out.append("| %s | ● | ● | %s | %s |" % (board_title(b, ""), on_radio_mark(b, records),
                                               b.data["status"]["summary"]))
    for r in planned(rows):
        marks = ["◐" if m in r["pr_marks"] else "○" for m in PR_MARKS]
        out.append("| %s | %s | %s |" % (r["radio"], " | ".join(marks), planned_status(r, "")))
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


# the feature matrix's mark for a switch the radio's hardware can do but whose code isn't written for it yet
# (config.mk BOARD_NOT_WRITTEN); ✕ is only for what the hardware can't (docs/build-config.md)
NOT_WRITTEN_MARK = "…"


def feature_mark(b, s, cfg):
    """A switch's mark for a board in the feature matrix: ✕ the hardware can't, … not written yet, ● on by default,
    ? not known whether the radio has the part, ○ off."""
    if s in b.unsupported:
        return "✕"
    if s in getattr(b, "not_written", {}):
        return NOT_WRITTEN_MARK
    if cfg.get(s) == "y":
        return "●"
    return "?" if s in b.data["hardware"].get("unconfirmed", {}) else "○"


def column(b, bs):
    """A board's column heading: its model, and its CPU too where another board has the same model (the builds of one
    radio for two processors, board.toml part_of)."""
    if sum(o.data["model"] == b.data["model"] for o in bs) > 1:
        return "%s (%s)" % (b.data["model"], b.data["hardware"]["cpu"])
    return b.data["model"]


def feature_defaults(root, boards):
    """The feature matrix's table of every switch on every board: on by default, off, not written yet, or not available."""
    bs = ordered(boards)
    cfg = {b.id: dict(print_config(root, b.id)) for b in bs}
    order = [s for s, _ in print_config(root, bs[0].id)] if bs else []
    out = ["| Feature switch | %s |" % " | ".join(column(b, bs) for b in bs),
           "|---|" + ":-:|" * len(bs)]
    for s in order:
        marks = [feature_mark(b, s, cfg[b.id]) for b in bs]
        out.append("| `%s` | %s |" % (s, " | ".join(marks)))
    # not a switch: what the board measures (hal/board.h board_volts), for the battery or supply features
    out.append("| Voltage reading (board, no switch) | %s |" % " | ".join(VOLTS_WORD[b.volts] for b in bs))
    # not a switch either: board.toml [hardware] clock, the real-time clock (no driver uses it yet)
    out.append("| Clock kept through power-off (board, no switch) | %s |"
               % " | ".join(CLOCK_WORD[b.data["hardware"]["clock"]["rtc"]] for b in bs))
    return out


def yaml_scalar(v):
    """v as a YAML plain scalar, or double-quoted where a plain one would read as something else."""
    if re.search(r"^[-?:,\[\]{}#&*!|>'\"%@`]|: | #|^\s|\s$", v) or v.lower() in ("yes", "no", "true", "false", "null"):
        return '"%s"' % v.replace("\\", "\\\\").replace('"', '\\"')
    return v


def issue_radios(boards):
    """The bug report form's Radio choices: each board, then the names it is also sold as."""
    return ["- " + yaml_scalar(" / ".join([b.data["name"]] + also_names(b))) for b in ordered(boards)]


def pr_radios(boards):
    """The pull request template's list of radios, by model (a radio built for two processors once)."""
    return [", ".join(dict.fromkeys(b.data["model"] for b in ordered(boards)))]


VOLTS_WORD = {"battery": "Battery", "supply": "Supply", "none": "None"}
CLOCK_WORD = {"backed": "Yes", "unbacked": "No (only while on)", "none": "No clock", "unknown": "?"}


# ── the feature matrix's "Checked on each radio": from the bring-up records ──

# a feature's status (tools/bringup.py feature_status, resolve) as the table reads it
CHECKED_WORD = {"pass": "pass", "fail": "fail", "partial": "partial", "off": "off", "n/a": "✕", "unsupported": "✕"}


def checked_cell(status, basis, detail):
    """One cell: the result, "(seed)" when it rests on a seed record, and ", needs re-run" when the feature's files
    changed since. Never reads as a pass without a record: no record is "not recorded"."""
    import bringup
    if status == bringup.NOT_RECORDED:
        return "not recorded"
    seed = " (seed)" if basis == "seed" else ""
    if status == "needs re-run":
        was = detail.split()[0] if basis == "run" and detail else "pass"	# a stale seed row is always a pass
        return "%s%s, needs re-run" % (was, seed)
    return CHECKED_WORD.get(status, status) + seed


def checked_on_radio(root, boards):
    """Every checklist feature (tests/hil/features.toml) on every board, from docs/testing/results/<board>/."""
    import bringup
    feats = bringup.load_features(os.path.join(root, "tests", "hil", "features.toml"))
    records = bringup.load_records(os.path.join(root, "docs", "testing", "results"))
    bs = ordered(boards)
    out = ["| Feature | %s |" % " | ".join(column(b, bs) for b in bs), "|---|" + ":-:|" * len(bs)]
    for e in feats:
        cells = []
        for b in bs:
            state, _ = bringup.resolve(b.id, e)
            cells.append(checked_cell(*bringup.feature_status(b.id, e, records)) if state == "required"
                         else CHECKED_WORD.get(state, state))
        out.append("| `%s` | %s |" % (e["id"], " | ".join(cells)))
    return out


# file, block, source the marker names, generator(root, boards, rows)[, marker style (MARKERS), default html]
BLOCKS = (
    ("README.md", "radios", "boards/*/board.toml [status] and docs/spec/radios.md",
     lambda root, boards, rows: readme_radios(root, boards, rows)),
    ("docs/user/23-radios.md", "radios", "boards/*/board.toml [status]",
     lambda root, boards, rows: manual_radios(boards, rows)),
    ("docs/user/23-radios.md", "planned", "the Port cells of docs/spec/radios.md",
     lambda root, boards, rows: manual_planned(rows)),
    ("docs/feature-matrix.md", "feature-defaults", "config/features.mk and boards/*/config.mk",
     lambda root, boards, rows: feature_defaults(root, boards)),
    ("docs/feature-matrix.md", "checked", "docs/testing/results/<board>/ (tools/bringup.py run)",
     lambda root, boards, rows: checked_on_radio(root, boards)),
    (".github/ISSUE_TEMPLATE/bug_report.yml", "radios", "boards/*/board.toml",
     lambda root, boards, rows: issue_radios(boards), "yaml"),
    (".github/PULL_REQUEST_TEMPLATE.md", "radios", "boards/*/board.toml",
     lambda root, boards, rows: pr_radios(boards), "bare"),
)


def fill(text, block, source, lines, where, style="html"):
    """text with block's lines between its markers, each line at the begin marker's indent."""
    begin, end = MARKERS[style]
    head = begin % ("generated: tools/boards.py docs %s; edit %s" % (block, source))
    tail = end % "end generated"
    pre, post = begin.split("%s")		# the begin marker matches whatever source it names
    opening = re.escape(pre + "generated: tools/boards.py docs %s;" % block) + "[^\n]*" + re.escape(post)
    pat = re.compile(r"^([ \t]*)%s\n.*?^\1%s$" % (opening, re.escape(tail)), re.M | re.S)
    if len(pat.findall(text)) != 1:
        raise DocsError("%s: needs exactly one generated block %r (%s ... %s)" % (where, block, head, tail))
    return pat.sub(lambda m: "\n".join(m.group(1) + l for l in [head] + lines + [tail]), text)


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
    for rel, block, source, gen, *style in BLOCKS:
        text = out.get(rel) or open(os.path.join(root, rel), encoding="utf-8").read()
        out[rel] = fill(text, block, source, gen(root, boards, rows), rel, *style)
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
