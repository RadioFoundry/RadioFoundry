#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The feature checklist (tests/hil/features.toml) per radio: what each radio must pass, the bring-up run that
checks it, and the records of those runs (docs/testing/results/<board>/). tests/hil/README.md, "Bring-up mode".

  tools/bringup.py list --radio ID              the radio's checklist with the last result per feature
  tools/bringup.py run --radio ID [--feature F[,F]] [-- PYTEST HIL OPTIONS]
                                                run the required tests under the radio lock and record the result
  tools/bringup.py status [--radio ID] [--json] every radio and feature
  tools/bringup.py check                        host-side consistency (tools/ci.sh)

A radio's state for a feature is resolved, never typed:
  n/a          the switch is in the board's BOARD_UNSUPPORTED (a reason is required)
  unsupported  board.toml [features.<id>] state = "unsupported": planned, not working yet (a reason is required)
  off          the board's default build leaves the switch off: nothing to check
  required     anything else: the feature's tests must pass on the radio
A required feature's status never understates it. The newest run record that ran it decides: pass, fail, partial
(excused skips or tests not run) or "needs re-run" (spec or source files changed since its commit). Without one, the
board's seed record (<board>/seed.json, the evidence from before its first run) decides, marked as a seed. With
neither the feature is "built, not yet recorded". Only a failing record shows a failure.

Python 3.11 or later (tomllib). pytest runs on whatever Python has it (--pytest-python, default this one when it has
pytest, else python3): the bring-up plugin (tests/hil/bringup_plugin.py) reads the JSON plan this tool writes."""
import argparse
import datetime
import fcntl
import json
import os
import re
import subprocess
import sys
import tempfile

if sys.version_info < (3, 11):
    sys.exit(f"bringup.py needs Python 3.11 or later (tomllib); this is {sys.version.split()[0]}. "
             "Run it with python3.11 or newer.")
import tomllib  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FEATURES_TOML = os.path.join(ROOT, "tests", "hil", "features.toml")
RESULTS = os.path.join(ROOT, "docs", "testing", "results")
RECORD_FORMAT = 1
FIELDS = {"id", "switch", "summary", "spec", "source", "tests", "optional_tests", "planned_tests", "needs", "owner",
          "host_tests", "doc_claims"}
REQUIRED_FIELDS = ("id", "summary", "spec", "tests")
NEEDS = ("radio", "rf", "bench", "second_radio", "owner_action", "usb_switch", "codeplug_write", "reboot")
OVERRIDE_STATES = ("n/a", "unsupported", "required")
# How list shows a resolved state: "required" is the checklist's word, "built" what it means to a reader
SHOWN = {"required": "built"}
SEED_RESULTS = ("pass", "fail")
# Test markers and fixtures -> the need they imply (check: a feature's needs cover its tests')
MARKER_NEEDS = {"rf": "rf", "radio_tx": "rf", "bench_service": "bench", "usb_switch": "usb_switch",
                "codeplug_write": "codeplug_write", "reboot": "reboot", "manual": "owner_action"}
FIXTURE_NEEDS = {"radio": "radio", "sdr_bench": "bench", "bench_station": "second_radio"}


def die(msg):
    sys.exit(f"bringup.py: {msg}")


# ── board data ──
#
# TODO(registry): at integration with the board registry (tools/boards.py, plan commit 1) this whole section becomes
# boards.load(): unsupported and config from the Board's derived build values, overrides from its [features.*]
# table, debuglink_names from its identity. Everything below reads boards only through _boards().

def _make_value(board, target):
    r = subprocess.run(["make", "-s", "--no-print-directory", f"BOARD={board}", target], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode != 0:
        die(f"make BOARD={board} {target} failed: {r.stderr.strip()[-400:]}")
    return r.stdout


def _unsupported_reason(board):
    """The comment block right above BOARD_UNSUPPORTED in the board's config.mk: the reason until board.toml has
    one (TODO(registry): board.toml [features.<id>] reason)."""
    path = os.path.join(ROOT, "boards", board, "config.mk")
    if not os.path.exists(path):
        return ""
    lines = open(path).read().splitlines()
    for i, line in enumerate(lines):
        if re.match(r"\s*BOARD_UNSUPPORTED\s*[:+?]?=", line):
            j = i
            while j > 0 and lines[j - 1].startswith("#"):
                j -= 1
            return " ".join(x.lstrip("#").strip() for x in lines[j:i]).strip()
    return ""


def _debuglink_names(board):
    """The board names VERSION can report: board_t .name and BOARD_NAME strings in the board's sources."""
    names = set()
    d = os.path.join(ROOT, "boards", board)
    for fn in sorted(os.listdir(d)):
        if fn.endswith((".c", ".h", ".mk")):
            text = open(os.path.join(d, fn), errors="replace").read()
            names.update(re.findall(r'\.name\s*=\s*"([^"]+)"', text))
            names.update(re.findall(r'#\s*define\s+BOARD_NAME\s+"([^"]+)"', text))
    return sorted(names)


_BOARDS = None


def _boards():
    """Every board: id -> {unsupported, unsupported_reason, config, overrides, debuglink_names}. The one place this
    tool reads board data (TODO(registry): boards.load())."""
    global _BOARDS
    if _BOARDS is not None:
        return _BOARDS
    out = {}
    bdir = os.path.join(ROOT, "boards")
    for b in sorted(os.listdir(bdir)):
        if not os.path.exists(os.path.join(bdir, b, "board.mk")):
            continue
        config = {}
        for line in _make_value(b, "print-config").splitlines():
            k, _, v = line.partition("=")
            if k:
                config[k.strip()] = v.strip()
        overrides = {}
        toml = os.path.join(bdir, b, "board.toml")
        if os.path.exists(toml):
            with open(toml, "rb") as f:
                overrides = tomllib.load(f).get("features", {})
        out[b] = {"unsupported": set(_make_value(b, "print-unsupported").split()),
                  "unsupported_reason": _unsupported_reason(b), "config": config, "overrides": overrides,
                  "debuglink_names": _debuglink_names(b)}
    _BOARDS = out
    return out


# ── the checklist ──

def switches():
    """The switches of config/features.mk, in order."""
    text = open(os.path.join(ROOT, "config", "features.mk")).read()
    m = re.search(r"^FEATURES\s*:?=((?:.*\\\n)*.*)", text, re.M)
    if not m:
        die("no FEATURES list in config/features.mk")
    return m.group(1).replace("\\\n", " ").split()


def load_features(path=FEATURES_TOML):
    with open(path, "rb") as f:
        data = tomllib.load(f)
    feats = data.get("feature", [])
    for e in feats:
        for k in ("source", "tests", "optional_tests", "planned_tests", "needs", "host_tests", "doc_claims"):
            e.setdefault(k, [])
        e.setdefault("owner", {})
        e.setdefault("switch", None)
    return feats


_TREE = None


def _tree_texts():
    """Source text of core/, drivers/, platform/ and hal/ once (default source lookup)."""
    global _TREE
    if _TREE is None:
        _TREE = {}
        for top in ("core", "drivers", "platform", "hal"):
            for dp, _, fns in os.walk(os.path.join(ROOT, top)):
                for fn in fns:
                    if fn.endswith((".c", ".h", ".S", ".mk")):
                        p = os.path.join(dp, fn)
                        _TREE[os.path.relpath(p, ROOT)] = open(p, errors="replace").read()
    return _TREE


def default_source(switch):
    """What the Makefile builds for the switch (inside an ifeq / ifneq on it, or a -$(CONFIG_X) line), else the files
    that test CONFIG_<switch> (core/config.h, which names them all, left out)."""
    if not switch:
        return []
    found = []
    stack = []
    cont = False
    for line in open(os.path.join(ROOT, "Makefile")).read().splitlines():
        s = line.strip()
        if re.match(r"if(n?eq|n?def)\b", s):
            stack.append(f"$(CONFIG_{switch})" in s)
            continue
        if s == "endif" and stack:
            stack.pop()
            continue
        mine = any(stack) or f"-$(CONFIG_{switch})" in s or cont
        cont = mine and s.endswith("\\") and not any(stack)
        if mine:
            for tok in re.findall(r"[A-Za-z0-9_./-]+", s):
                if "/" in tok and os.path.exists(os.path.join(ROOT, tok)) and tok not in found:
                    found.append(tok)
    if found:
        return found
    pat = re.compile(rf"\bCONFIG_{switch}\b")
    return sorted(p for p, t in _tree_texts().items() if p != os.path.join("core", "config.h") and pat.search(t))


def feature_paths(e):
    return [e["spec"]] + (e["source"] or default_source(e["switch"]))


def resolve(board, e):
    """(state, reason) of a feature on a board."""
    bd = _boards()[board]
    ov = bd["overrides"].get(e["id"])
    if e["switch"] and e["switch"] in bd["unsupported"]:
        if ov and ov.get("state") == "unsupported":
            return "unsupported", ov.get("reason", "")
        return "n/a", (ov or {}).get("reason") or bd["unsupported_reason"]
    if ov:
        return ov.get("state", "required"), ov.get("reason", "")
    if e["switch"] and bd["config"].get(e["switch"]) != "y":
        return "off", "the board's default build leaves it off"
    return "required", ""


# ── records ──

def git(*args, check=True):
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    if check and r.returncode != 0:
        die(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r


def load_records(results=RESULTS):
    """board -> {"runs": run records newest first, "seed": the seed record or None}. A seed record
    (<board>/seed.json, kind "seed") carries the evidence the repository had before the board's first recorded run."""
    out = {}
    if not os.path.isdir(results):
        return out
    for b in sorted(os.listdir(results)):
        d = os.path.join(results, b)
        if not os.path.isdir(d):
            continue
        runs, seed = [], None
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".json"):
                with open(os.path.join(d, fn)) as f:
                    r = json.load(f)
                r["_file"] = os.path.relpath(os.path.join(d, fn), ROOT)
                if r.get("kind") == "seed":
                    seed = r
                else:
                    runs.append(r)
        out[b] = {"runs": sorted(runs, key=lambda r: r.get("date", ""), reverse=True), "seed": seed}
    return out


def stale(commit, e):
    """Files under the feature's spec or source changed since the commit (empty: not stale)."""
    if not commit or git("cat-file", "-e", f"{commit}^{{commit}}", check=False).returncode != 0:
        return ["(the record's commit is not in this repository)"]
    r = git("diff", "--name-only", commit, "HEAD", "--", *feature_paths(e), check=False)
    return r.stdout.split()


NOT_RECORDED = "built, not yet recorded"


def run_status(rec, e):
    """pass, fail or partial (excused skips, tests not run) of one run record for a feature."""
    res = {}
    for t in rec.get("tests", []):
        res.setdefault(re.sub(r"\[.*\]$", "", t["id"]), []).append(t)
    got = [res.get(t) for t in e["tests"]]
    if any(x["outcome"] == "failed" for v in got if v for x in v):
        return "fail"
    if all(v and all(x["outcome"] == "passed" for x in v) for v in got):
        return "pass"
    return "partial"


def feature_status(board, e, records):
    """(status, basis, detail) of a required feature on a board. Never understates: no record is "built, not yet
    recorded", only a failing record is a failure, and a stale one "needs re-run". basis is "run", "seed" or None.
    The newest run that covers the feature decides; a seed row stands until a run passes or fails it."""
    recs = records.get(board, {"runs": [], "seed": None})
    run = next((r for r in recs["runs"] if e["id"] in r.get("features", [])), None)
    row = ((recs["seed"] or {}).get("features") or {}).get(e["id"])
    planned = f"; {len(e['planned_tests'])} planned tests not written" if e["planned_tests"] else ""
    status = run_status(run, e) if run and e["tests"] else None
    if status in ("pass", "fail") or (status == "partial" and not row):
        at = f"{run.get('commit', '')[:8]} {run.get('date', '')[:10]}"
        changed = stale(run.get("commit"), e) if status != "fail" else []
        if changed:
            return "needs re-run", "run", f"{status} at {at}; changed since: {', '.join(changed[:3])}"
        return status, "run", at + planned
    if row:
        later = "; a later run was partial" if status == "partial" else ""
        detail = f"seed: {row.get('source') or recs['seed'].get('source', '')}{later}"
        if row.get("result") == "fail":
            return "fail", "seed", detail
        changed = stale(row["commit"], e) if row.get("commit") else []
        if changed:
            return "needs re-run", "seed", f"{detail}; changed since: {', '.join(changed[:3])}"
        return "pass", "seed", detail + planned
    return NOT_RECORDED, None, "no HIL test yet" if not e["tests"] else planned.lstrip("; ")


# ── commands ──

def cmd_list(args):
    feats = load_features(args.features)
    boards = _boards()
    if args.radio not in boards:
        die(f"no board {args.radio!r} (boards/*/board.mk)")
    records = load_records(args.results)
    rows = []
    for e in feats:
        state, reason = resolve(args.radio, e)
        status, basis, detail = (feature_status(args.radio, e, records) if state == "required"
                                 else (state, None, reason))
        rows.append({"id": e["id"], "switch": e["switch"] or "", "state": state, "status": status, "basis": basis,
                     "detail": detail, "tests": len(e["tests"]), "needs": e["needs"], "owner": e["owner"]})
    if args.json:
        print(json.dumps(rows, indent=1))
        return 0
    print(f"{'feature':22} {'state':11} {'tests':>5}  {'status':24} detail")
    for r in rows:
        detail = r["detail"] if len(r["detail"]) <= 70 else r["detail"][:67] + "..."
        mark = r["status"] + (" (seed)" if r["basis"] == "seed" else "")
        print(f"{r['id']:22} {SHOWN.get(r['state'], r['state']):11} {r['tests']:5}  {mark:24} {detail}")
    owner = [(r["id"], t, a) for r in rows if r["state"] == "required" for t, a in r["owner"].items()]
    if owner:
        print("\nowner actions (run with --manual; they're asked for during the run):")
        for fid, t, a in owner:
            print(f"  {fid}: {t.split('::')[-1]}: {a}")
    return 0


def cmd_status(args):
    feats = load_features(args.features)
    boards = sorted(_boards()) if not args.radio else [args.radio]
    records = load_records(args.results)
    table = {e["id"]: {} for e in feats}
    for b in boards:
        for e in feats:
            state, reason = resolve(b, e)
            status, basis, _ = feature_status(b, e, records) if state == "required" else (state, None, "")
            table[e["id"]][b] = {"status": status, "basis": basis}
    if args.json:
        print(json.dumps(table, indent=1))
        return 0
    w = max(24, *(len(b) for b in boards))
    print(f"{'feature':22} " + " ".join(f"{b:{w}}" for b in boards))
    for fid, row in table.items():
        print(f"{fid:22} " + " ".join(f"{row[b]['status'] + ('*' if row[b]['basis'] == 'seed' else ''):{w}}"
                                      for b in boards))
    if any(c["basis"] == "seed" for row in table.values() for c in row.values()):
        print("\n* rests on a seed record (docs/testing/results/<board>/seed.json), not on a recorded run")
    return 0


def _pytest_python(arg):
    if arg:
        return arg
    if subprocess.run([sys.executable, "-c", "import pytest"], capture_output=True).returncode == 0:
        return sys.executable
    return "python3"


def plan_for(feats, names):
    return {"features": {e["id"]: {"tests": e["tests"], "optional_tests": e["optional_tests"], "needs": e["needs"]}
                         for e in feats if e["id"] in names}}


class RadioLock:
    """The bench's radio lock (docs/dev/ci.md, ODR_RADIO_LOCK, default ~/.odr-radio.lock): an flock on the file, as
    the hil workflow's lockf -k / flock takes it, held for the whole run."""

    def __init__(self, path, wait):
        self.path, self.wait, self.f = path, wait, None

    def __enter__(self):
        import time
        self.f = open(self.path, "a")
        t0 = time.monotonic()
        while True:
            try:
                fcntl.flock(self.f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except BlockingIOError:
                if time.monotonic() - t0 >= self.wait:
                    self.f.close()
                    die(f"the radio lock {self.path} is held by another run (--lock-wait S to wait for it)")
                time.sleep(1)

    def __exit__(self, *exc):
        fcntl.flock(self.f, fcntl.LOCK_UN)
        self.f.close()


def cmd_run(args):
    feats = load_features(args.features)
    boards = _boards()
    if args.radio not in boards:
        die(f"no board {args.radio!r} (boards/*/board.mk)")
    by_id = {e["id"]: e for e in feats}
    if args.feature:
        names = [n.strip() for n in args.feature.split(",") if n.strip()]
        for n in names:
            if n not in by_id:
                die(f"no feature {n!r} in the checklist")
            state, reason = resolve(args.radio, by_id[n])
            if state != "required":
                print(f"bringup.py: {n} is {state} on {args.radio} ({reason}): running it anyway", file=sys.stderr)
    else:
        names = [e["id"] for e in feats if resolve(args.radio, e)[0] == "required" and e["tests"]]
    if not names:
        die("nothing to run")
    pytest_args = list(args.pytest_args)
    if pytest_args[:1] == ["--"]:
        pytest_args = pytest_args[1:]
    owner = [(n, t, a) for n in names for t, a in by_id[n]["owner"].items()]
    if owner:
        print("owner actions in this run" + ("" if "--manual" in pytest_args else
                                             " (skipped without --manual; they don't verify the feature then)") + ":")
        for n, t, a in owner:
            print(f"  {n}: {t.split('::')[-1]}: {a}")
    tmp = tempfile.mkdtemp(prefix="odr-bringup-")
    plan_path, out_path = os.path.join(tmp, "plan.json"), os.path.join(tmp, "out.json")
    with open(plan_path, "w") as f:
        json.dump(plan_for(feats, names), f)
    cmd = [_pytest_python(args.pytest_python), "-m", "pytest", *(args.tests or [os.path.join("tests", "hil")]),
           "-v", "-rs", "-p", "no:cacheprovider", "--bringup", ",".join(names), "--bringup-plan", plan_path,
           "--bringup-out", out_path, *pytest_args]
    if args.dry_run:
        print(" ".join(cmd))
        return 0
    with RadioLock(args.lock, args.lock_wait):
        rc = subprocess.run(cmd, cwd=ROOT).returncode
    if not os.path.exists(out_path):
        die(f"pytest wrote no results (exit {rc})")
    with open(out_path) as f:
        out = json.load(f)
    fw_board = out.get("firmware", {}).get("board")
    if fw_board and fw_board not in boards[args.radio]["debuglink_names"]:
        die(f"the radio reports board={fw_board}, not {args.radio} ({', '.join(boards[args.radio]['debuglink_names'])})"
            ": nothing recorded")
    if out.get("missing"):
        print(f"bringup.py: listed tests not collected: {', '.join(out['missing'])}", file=sys.stderr)
        rc = rc or 1
    if args.no_record:
        return rc
    head = git("rev-parse", "HEAD").stdout.strip()
    dirty = git("diff", "--quiet", "--ignore-submodules=all", "HEAD", "--", ".", check=False).returncode != 0
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    record = {"format": RECORD_FORMAT, "kind": "run", "board": args.radio, "commit": head, "dirty": dirty,
              "date": now.isoformat().replace("+00:00", "Z"), **{k: out[k] for k in
              ("features", "granted", "missing", "tests", "firmware", "options", "tools")}}
    d = os.path.join(args.results, args.radio)
    os.makedirs(d, exist_ok=True)
    suffix = f"-{names[0]}" if args.feature and len(names) == 1 else ""
    path = os.path.join(d, f"{now.strftime('%Y%m%d-%H%M%S')}-{head[:8]}{suffix}.json")
    with open(path, "w") as f:
        json.dump(record, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"bringup.py: recorded {os.path.relpath(path, ROOT)}" + (" (uncommitted changes in the tree)" if dirty
                                                                   else ""))
    return rc


def collected(pytest_python, tests_dir):
    """Every collected HIL test's markers and fixtures (the plugin's --bringup-collect), or None without pytest."""
    if subprocess.run([pytest_python, "-c", "import pytest"], capture_output=True).returncode != 0:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "collect.json")
        env = {k: v for k, v in os.environ.items() if k != "ODR_PORT"}
        r = subprocess.run([pytest_python, "-m", "pytest", tests_dir, "--collect-only", "-q", "-p", "no:cacheprovider",
                            "--bringup-collect", out], cwd=ROOT, capture_output=True, text=True, env=env)
        if r.returncode != 0 or not os.path.exists(out):
            die(f"pytest --collect-only failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
        with open(out) as f:
            return json.load(f)


def check(feats, boards, records, found, all_switches):
    """The consistency errors (strings), and notes."""
    errs, notes = [], []
    ids = [e["id"] for e in feats]
    for i in sorted({i for i in ids if ids.count(i) > 1}):
        errs.append(f"feature id {i} is used twice")
    by_switch = {}
    for e in feats:
        where = f"feature {e.get('id', '?')}"
        for k in sorted(set(e) - FIELDS):
            errs.append(f"{where}: unknown field {k}")
        for k in REQUIRED_FIELDS:
            if not e.get(k) and not (k == "tests" and "tests" in e):
                errs.append(f"{where}: no {k}")
        if not re.fullmatch(r"[a-z][a-z0-9_]*", e["id"]):
            errs.append(f"{where}: the id is not snake case")
        if e["switch"]:
            if e["switch"] not in all_switches:
                errs.append(f"{where}: switch {e['switch']} is not in config/features.mk")
            by_switch.setdefault(e["switch"], []).append(e["id"])
        for p in [e["spec"], *e["source"], *e["doc_claims"]]:
            if p and not os.path.exists(os.path.join(ROOT, p)):
                errs.append(f"{where}: no such path {p}")
        for h in e["host_tests"]:
            if not any(os.path.exists(os.path.join(ROOT, "tests", "host", h + x)) for x in (".c", ".py")):
                errs.append(f"{where}: no host test tests/host/{h}.c or .py")
        for n in e["needs"]:
            if n not in NEEDS:
                errs.append(f"{where}: unknown need {n} (one of {', '.join(NEEDS)})")
        listed = e["tests"] + e["optional_tests"]
        for t in sorted({t for t in listed if listed.count(t) > 1}):
            errs.append(f"{where}: {t} is listed twice")
        for t in e["owner"]:
            if t not in listed:
                errs.append(f"{where}: owner action for {t}, which the entry doesn't list")
        if e["owner"] and "owner_action" not in e["needs"]:
            errs.append(f"{where}: owner actions without owner_action in needs")
        if found is not None:
            by_base = {}
            for nid, info in found.items():
                by_base.setdefault(re.sub(r"\[.*\]$", "", nid), []).append(info)
            for t in listed:
                if t not in by_base:
                    errs.append(f"{where}: {t} is not collected by pytest")
                    continue
                derived = set()
                for info in by_base[t]:
                    derived |= {MARKER_NEEDS[m] for m in info["markers"] if m in MARKER_NEEDS}
                    derived |= {FIXTURE_NEEDS[x] for x in info["fixtures"] if x in FIXTURE_NEEDS}
                    if "manual" in info["markers"] and t not in e["owner"]:
                        errs.append(f"{where}: {t} is marked manual but has no owner action")
                missing = derived - set(e["needs"])
                if missing:
                    errs.append(f"{where}: {t} needs {', '.join(sorted(missing))}, which needs doesn't list")
            for t in e["planned_tests"]:
                if t in by_base:
                    errs.append(f"{where}: planned test {t} exists now: move it to tests")
    for s in all_switches:
        if s not in by_switch:
            errs.append(f"switch {s} has no entry in tests/hil/features.toml")
        elif len(by_switch[s]) > 1:
            errs.append(f"switch {s} has more than one entry: {', '.join(by_switch[s])}")
    by_id = {e["id"]: e for e in feats}
    for b, bd in boards.items():
        for fid, ov in bd["overrides"].items():
            where = f"boards/{b}/board.toml [features.{fid}]"
            if fid not in by_id:
                errs.append(f"{where}: no such feature")
                continue
            if ov.get("state") not in OVERRIDE_STATES:
                errs.append(f"{where}: state {ov.get('state')!r} is not one of {', '.join(OVERRIDE_STATES)} (a "
                            "supported mark comes from the records, never typed)")
            if not ov.get("reason"):
                errs.append(f"{where}: no reason")
            sw = by_id[fid]["switch"]
            if ov.get("state") == "n/a" and sw and sw not in bd["unsupported"]:
                errs.append(f"{where}: n/a, but {sw} is not in the board's BOARD_UNSUPPORTED")
        for e in feats:
            state, reason = resolve(b, e)
            if state in ("n/a", "unsupported") and not reason:
                errs.append(f"{b}: {e['id']} is {state} without a reason")
    for b, recs in records.items():
        if b not in boards:
            errs.append(f"docs/testing/results/{b}: no such board")
        if recs["seed"]:
            errs += check_seed(recs["seed"], b, by_id)
        for r in recs["runs"]:
            where = r["_file"]
            if r.get("kind", "run") != "run":
                errs.append(f"{where}: kind {r.get('kind')!r} (a run record is \"run\", a seed record is seed.json)")
            for k in ("format", "board", "commit", "date", "features", "tests", "tools"):
                if k not in r:
                    errs.append(f"{where}: no {k}")
            if r.get("board") != b:
                errs.append(f"{where}: board {r.get('board')} in the directory of {b}")
            if r.get("commit") and not has_commit(r["commit"]):
                errs.append(f"{where}: commit {r['commit'][:12]} is not in this repository")
            for f in r.get("features", []):
                if f not in by_id:
                    notes.append(f"{where}: feature {f} is no longer in the checklist")
            text = json.dumps(r)
            if re.search(r"/dev/|usbmodem|://|\bserial[=:]", text):
                errs.append(f"{where}: names a port, host or serial number")
    # TODO(commit 9): the claim blocks in the docs (feature matrix, README, 23-radios.md) are generated from the
    # records; until they exist there is nothing to compare, so claims against records is not checked yet
    notes.append("claims against records: not checked until the docs' claim blocks are generated")
    return errs, notes


def has_commit(c):
    return git("cat-file", "-e", f"{c}^{{commit}}", check=False).returncode == 0


def check_seed(r, b, by_id):
    """A seed record: the board's evidence from before its first recorded run, one row per feature."""
    where, errs = r["_file"], []
    if os.path.basename(where) != "seed.json":
        errs.append(f"{where}: a seed record is named seed.json")
    for k in ("format", "board", "source", "date", "features"):
        if k not in r:
            errs.append(f"{where}: no {k}")
    if r.get("board") != b:
        errs.append(f"{where}: board {r.get('board')} in the directory of {b}")
    if r.get("source") and not os.path.exists(os.path.join(ROOT, r["source"])):
        errs.append(f"{where}: source {r['source']} does not exist")
    rows = r.get("features", {})
    if not isinstance(rows, dict):
        return errs + [f"{where}: features is a table of feature id -> row"]
    for f, row in rows.items():
        if f not in by_id:
            errs.append(f"{where}: {f} is not a checklist feature")
        if not isinstance(row, dict) or row.get("result") not in SEED_RESULTS:
            errs.append(f"{where}: {f}: result is one of {', '.join(SEED_RESULTS)}")
            continue
        if row.get("commit") and not has_commit(row["commit"]):
            errs.append(f"{where}: {f}: commit {row['commit'][:12]} is not in this repository")
        if row.get("source") and not os.path.exists(os.path.join(ROOT, row["source"])):
            errs.append(f"{where}: {f}: source {row['source']} does not exist")
    if re.search(r"/dev/|usbmodem|://|\bserial[=:]", json.dumps(r)):
        errs.append(f"{where}: names a port, host or serial number")
    return errs


def cmd_check(args):
    feats = load_features(args.features)
    all_switches = switches()
    boards = _boards()
    records = load_records(args.results)
    found = collected(_pytest_python(args.pytest_python), args.tests_dir)
    errs, notes = check(feats, boards, records, found, all_switches)
    if found is None:
        notes.append("pytest not installed: listed tests not checked against the collection")
    for n in notes:
        print(f"note: {n}")
    for e in errs:
        print(f"error: {e}")
    untested = [e["id"] for e in feats if not e["tests"]]
    print(f"bringup check: {len(feats)} features ({sum(1 for e in feats if e['switch'])} with a switch, "
          f"{len(untested)} without a HIL test), {len(boards)} boards, "
          f"{sum(len(r['runs']) + bool(r['seed']) for r in records.values())} records, {len(errs)} errors")
    return 1 if errs else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--features", default=FEATURES_TOML, help="the checklist (default tests/hil/features.toml)")
    ap.add_argument("--results", default=RESULTS, help="the records (default docs/testing/results)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="a radio's checklist with the last result per feature")
    p.add_argument("--radio", required=True, help="board id (boards/<id>)")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("status", help="every radio and feature")
    p.add_argument("--radio", help="one board only")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("run", help="run the radio's required tests and record the result")
    p.add_argument("--radio", required=True, help="board id (boards/<id>) of the radio on the debug link")
    p.add_argument("--feature", help="these features only (comma separated); default every required one")
    p.add_argument("--pytest-python", help="the Python that has pytest (default this one, else python3)")
    p.add_argument("--tests", action="append", help=argparse.SUPPRESS)     # the unit test's own test files
    p.add_argument("--lock", default=os.environ.get("ODR_RADIO_LOCK", os.path.expanduser("~/.odr-radio.lock")),
                   help="the radio lock file ($ODR_RADIO_LOCK, default ~/.odr-radio.lock)")
    p.add_argument("--lock-wait", type=float, default=0, help="s to wait for the radio lock (default: fail at once)")
    p.add_argument("--no-record", action="store_true", help="run, but write no record")
    p.add_argument("--dry-run", action="store_true", help="print the pytest command, run nothing")
    p.add_argument("pytest_args", nargs=argparse.REMAINDER, help="-- then the HIL options (--allow-rf, --dmr-freq ...)")
    p = sub.add_parser("check", help="host-side consistency (tools/ci.sh)")
    p.add_argument("--pytest-python", help="the Python that has pytest (default this one, else python3)")
    p.add_argument("--tests-dir", default=os.path.join("tests", "hil"), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    return {"list": cmd_list, "status": cmd_status, "run": cmd_run, "check": cmd_check}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
