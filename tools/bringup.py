#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The feature checklist (tests/hil/features.toml) per radio: what each radio must pass, the bring-up run that
checks it, and the records of those runs (docs/testing/results/<board>/). tests/hil/README.md, "Bring-up mode".

  tools/bringup.py list --radio ID              the radio's checklist with the last result per feature
  tools/bringup.py run --radio ID [--feature F[,F]] [-- PYTEST HIL OPTIONS]
                                                run the required tests under the radio lock and record the result
  tools/bringup.py status [--radio ID] [--json] every radio and feature
  tools/bringup.py check                        host-side consistency (tools/ci.sh)

A radio's state for a feature is resolved, never typed:
  n/a          the switch is in the board's BOARD_UNSUPPORTED: the hardware can't (config.mk, the reason on its line)
  not written  the switch is in the board's BOARD_NOT_WRITTEN: the hardware can, the board's code isn't written yet
  unsupported  board.toml [features] <id> state = "unsupported": built, known not to work yet (a reason is required)
  off          the board's default build leaves the switch off: nothing to check
  required     anything else: the feature's tests must pass on the radio
The rule for the two config.mk lists: docs/build-config.md, "Unsupported, not written, unconfirmed". A test skips a
feature that isn't built as not_built (n/a and not written alike: the firmware leaves it out).
A required feature's status never understates it. The newest run record that ran it decides: pass, fail, partial
(excused skips) or "needs re-run" (its spec or source paths differ in the tree from the hashes the record keeps).
Without one, the board's seed record (<board>/seed.json, the evidence from before its first run) decides, marked as a
seed. With neither the feature is "built, not yet recorded". Only a failing record shows a failure.

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
FIELDS = {"switch", "hardware", "spec", "source", "planned_tests", "manual"}
OVERRIDE_STATES = ("unsupported",)
# How list shows a resolved state: "required" is the checklist's word, "built" what it means to a reader
SHOWN = {"required": "built"}
SEED_RESULTS = ("pass", "fail")


def die(msg):
    sys.exit(f"bringup.py: {msg}")


# ── board data ──
#
# Everything below reads boards only through _boards(), and _boards() only through the board registry
# (tools/boards.py): the board list, the debug link names, config.mk's BOARD_UNSUPPORTED and BOARD_NOT_WRITTEN with
# each switch's reason,
# board.toml [features] (the unsupported marks) and each switch's default (by make).

_BOARDS = None


def _boards():
    """Every board: id -> {unsupported: {switch: reason}, not_written: {switch: reason}, config, overrides,
    debuglink_names}."""
    global _BOARDS
    if _BOARDS is not None:
        return _BOARDS
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import boards as registry
    sws = switches()
    out = {}
    try:
        for bid, b in registry.load(ROOT).items():
            mk = registry.make_vars(ROOT, bid, [f"CONFIG_{s}" for s in sws])
            out[bid] = {"unsupported": b.unsupported, "not_written": b.not_written,
                        "config": {s: mk.get(f"CONFIG_{s}", "") for s in sws},
                        "overrides": b.get("features", {}), "debuglink_names": b.debuglink_names()}
    except registry.RegistryError as e:
        die(f"board registry: {e}")
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
    """The checklist as a list of entries, each with its id ([feature.<id>])."""
    with open(path, "rb") as f:
        data = tomllib.load(f)
    feats = []
    for fid, e in data.get("feature", {}).items():
        e = dict(e, id=fid)
        for k in ("source", "planned_tests", "manual"):
            e.setdefault(k, [])
        e.setdefault("switch", None)
        feats.append(e)
    return feats


def collected(args, extra=()):
    """Every collected HIL test: node id -> {features: [{id, optional, owner}], needs, markers} (the plugin's
    --bringup-collect), or None without pytest."""
    py = _pytest_python(args.pytest_python)
    if subprocess.run([py, "-c", "import pytest"], capture_output=True).returncode != 0:
        return None
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "collect.json")
        env = {k: v for k, v in os.environ.items() if k != "RFOUNDRY_PORT"}
        r = subprocess.run([py, "-m", "pytest", *args.tests, "--collect-only", "-q", "-p", "no:cacheprovider",
                            *args.pytest_arg, *extra, "--bringup-collect", out], cwd=ROOT, capture_output=True,
                           text=True, env=env)
        if r.returncode != 0 or not os.path.exists(out):
            die(f"pytest --collect-only failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
        with open(out) as f:
            return json.load(f)


def tests_by_feature(found):
    """feature id -> [{id, optional, owner, needs, markers}] from a collection."""
    out = {}
    for nid, info in (found or {}).items():
        for m in info["features"]:
            out.setdefault(m["id"], []).append({"id": nid, "optional": m["optional"], "owner": m["owner"],
                                                "needs": info["needs"], "markers": info["markers"]})
    return out


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
        return "n/a", bd["unsupported"][e["switch"]]
    if e["switch"] and e["switch"] in bd["not_written"]:
        return "not written", bd["not_written"][e["switch"]]
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


NO_GIT = "not a git checkout: the staleness check needs history"
_GIT_CHECKOUT = None


def git_checkout():
    """Whether ROOT is a git work tree (a `git archive` export isn't): records and staleness need its history."""
    global _GIT_CHECKOUT
    if _GIT_CHECKOUT is None:
        try:
            r = git("rev-parse", "--show-toplevel", check=False)
            _GIT_CHECKOUT = r.returncode == 0 and os.path.realpath(r.stdout.strip()) == os.path.realpath(ROOT)
        except OSError:	# no git at all
            _GIT_CHECKOUT = False
    return _GIT_CHECKOUT


def no_git_note(to=sys.stdout):
    """Says, after a listing, that nothing could be checked for staleness."""
    if not git_checkout():
        print(f"\n({NO_GIT}: statuses are as recorded, and none reads 'needs re-run')", file=to)


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


_HASHES = {}
_TREE_REV = None


def tree_rev():
    """The tree as it is: HEAD, or with uncommitted changes a commit object of the tracked files (git stash create;
    nothing in the tree, the index or the refs changes)."""
    global _TREE_REV
    if _TREE_REV is None:
        dirty = git("diff", "--quiet", "--ignore-submodules=all", "HEAD", "--", ".", check=False).returncode != 0
        _TREE_REV = (git("stash", "create", check=False).stdout.strip() or "HEAD") if dirty else "HEAD"
    return _TREE_REV


def source_hashes(e, rev=None):
    """path -> git object id of the feature's spec and source paths at rev (a blob or a tree; None: not there). By
    content, not ancestry: main is squash-merged, so the commit a record was made on may be gone from history while
    the files it checked are unchanged."""
    rev = rev or tree_rev()
    paths = [p.rstrip("/") for p in feature_paths(e)]
    key = (rev, tuple(paths))
    if key not in _HASHES:
        out = dict.fromkeys(paths)
        for line in git("ls-tree", rev, "--", *paths, check=False).stdout.splitlines():
            meta, _, path = line.partition("\t")
            if path in out:
                out[path] = meta.split()[2]
        _HASHES[key] = out
    return _HASHES[key]


def stale(hashes, e):
    """The feature's paths whose content in the tree differs from the recorded hashes (empty: not stale)."""
    if not git_checkout():
        return []		# nothing to compare with; the listing says so (no_git_note)
    if not hashes:
        return ["(no source hashes recorded)"]
    now = source_hashes(e)
    return sorted(p for p in set(now) | set(hashes) if now.get(p) != hashes.get(p))


NOT_RECORDED = "built, not yet recorded"


def run_status(rec, fid):
    """pass, fail or partial (excused skips) of one run record for a feature, or None: no required test of it ran."""
    got = [t for t in rec.get("tests", []) if fid in t.get("features", [])]
    if not got:
        return None
    if any(t["outcome"] == "failed" for t in got):
        return "fail"
    return "pass" if all(t["outcome"] == "passed" for t in got) else "partial"


def feature_status(board, e, records, tests=None):
    """(status, basis, detail) of a required feature on a board (tests: its collected tests, None when unknown).
    Never understates: no record is "built, not yet recorded", only a failing record is a failure, and a stale one
    "needs re-run". basis is "run", "seed" or None.
    The newest run that covers the feature decides; a seed row stands until a run passes or fails it."""
    recs = records.get(board, {"runs": [], "seed": None})
    run = next((r for r in recs["runs"] if e["id"] in r.get("features", [])), None)
    row = ((recs["seed"] or {}).get("features") or {}).get(e["id"])
    planned = f"; {len(e['planned_tests'])} planned tests not written" if e["planned_tests"] else ""
    status = run_status(run, e["id"]) if run else None
    if status in ("pass", "fail") or (status == "partial" and not row):
        at = f"{run.get('commit', '')[:8]} {run.get('date', '')[:10]}"
        changed = stale(run.get("sources", {}).get(e["id"]), e) if status != "fail" else []
        if changed:
            return "needs re-run", "run", f"{status} at {at}; changed since: {', '.join(changed[:3])}"
        return status, "run", at + planned
    if row:
        later = "; a later run was partial" if status == "partial" else ""
        detail = f"seed: {row.get('source') or recs['seed'].get('source', '')}{later}"
        if row.get("result") == "fail":
            return "fail", "seed", detail
        # a row's commit the repository still has gives the content it vouched for; without one the row stands
        known = row.get("commit") and has_commit(row["commit"])
        changed = stale(source_hashes(e, row["commit"]), e) if known else []
        if changed:
            return "needs re-run", "seed", f"{detail}; changed since: {', '.join(changed[:3])}"
        return "pass", "seed", detail + planned
    return NOT_RECORDED, None, "no HIL test yet" if tests == [] else planned.lstrip("; ")


# ── commands ──

def cmd_list(args):
    feats = load_features(args.features)
    boards = _boards()
    if args.radio not in boards:
        die(f"no board {args.radio!r} (boards/*/board.mk)")
    records = load_records(args.results)
    found = collected(args)
    by_feat = tests_by_feature(found)
    rows = []
    for e in feats:
        state, reason = resolve(args.radio, e)
        tests = by_feat.get(e["id"], []) if found is not None else None
        required = [t for t in tests or [] if not t["optional"]]
        status, basis, detail = (feature_status(args.radio, e, records, required if found is not None else None)
                                 if state == "required" else (state, None, reason))
        owner = {t["id"]: t["owner"] for t in tests or [] if t["owner"]}
        rows.append({"id": e["id"], "switch": e["switch"] or "", "state": state, "status": status, "basis": basis,
                     "detail": detail, "tests": len(required) if found is not None else None,
                     "needs": sorted({n for t in tests or [] for n in t["needs"]}), "owner": owner,
                     "manual": e["manual"]})
    if args.json:
        print(json.dumps(rows, indent=1))
        no_git_note(sys.stderr)
        return 0
    print(f"{'feature':22} {'state':11} {'tests':>5}  {'status':24} detail")
    for r in rows:
        detail = r["detail"] if len(r["detail"]) <= 70 else r["detail"][:67] + "..."
        mark = r["status"] + (" (seed)" if r["basis"] == "seed" else "")
        n = "?" if r["tests"] is None else r["tests"]
        print(f"{r['id']:22} {SHOWN.get(r['state'], r['state']):11} {n:>5}  {mark:24} {detail}")
    print_owner([r for r in rows if r["state"] == "required"])
    if found is None:
        print("\n(pytest not installed: tests, needs and owner actions unknown; --pytest-python)")
    no_git_note()
    return 0


def print_owner(rows, head="owner actions (run with --manual; they're asked for during the run):"):
    acts = [(r["id"], t.split("::")[-1], a) for r in rows for t, a in r["owner"].items()]
    acts += [(r["id"], "", a) for r in rows for a in r["manual"]]
    if acts:
        print("\n" + head)
        for fid, t, a in acts:
            print(f"  {fid}: {t + ': ' if t else ''}{a}")


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
        no_git_note(sys.stderr)
        return 0
    w = max(24, *(len(b) for b in boards))
    print(f"{'feature':22} " + " ".join(f"{b:{w}}" for b in boards))
    for fid, row in table.items():
        print(f"{fid:22} " + " ".join(f"{row[b]['status'] + ('*' if row[b]['basis'] == 'seed' else ''):{w}}"
                                      for b in boards))
    if any(c["basis"] == "seed" for row in table.values() for c in row.values()):
        print("\n* rests on a seed record (docs/testing/results/<board>/seed.json), not on a recorded run")
    no_git_note()
    return 0


def _pytest_python(arg):
    if arg:
        return arg
    if subprocess.run([sys.executable, "-c", "import pytest"], capture_output=True).returncode == 0:
        return sys.executable
    return "python3"


def lock_path(arg, pytest_args):
    """The radio lock of this run's radio: --lock, else $RFOUNDRY_RADIO_LOCK ("{port}" in it becomes the port), else
    ~/.rfoundry-radio-<port>.lock. The port is --radio-port, else $RFOUNDRY_PORT, else "usb" (the radio found on USB), so a bench
    with two radios runs one bring-up on each."""
    if arg:
        return arg
    port = os.environ.get("RFOUNDRY_PORT") or "usb"
    for i, a in enumerate(pytest_args):
        if a == "--radio-port" and i + 1 < len(pytest_args):
            port = pytest_args[i + 1]
        elif a.startswith("--radio-port="):
            port = a.split("=", 1)[1]
    slug = re.sub(r"[^A-Za-z0-9.-]+", "_", port).strip("_.") or "usb"
    env = os.environ.get("RFOUNDRY_RADIO_LOCK")
    if env:
        return env.replace("{port}", slug)
    return os.path.expanduser(f"~/.rfoundry-radio-{slug}.lock")


class RadioLock:
    """The radio lock (lock_path; docs/dev/ci.md): an flock on the file, as the hil workflow's lockf -k / flock takes
    it, held for the whole run."""

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
    if not args.no_record and not git_checkout():
        die(f"{NO_GIT}, and a record needs the commit: run from a clone, or pass --no-record")
    by_id = {e["id"]: e for e in feats}
    pytest_args = list(args.pytest_args)
    if pytest_args[:1] == ["--"]:
        pytest_args = pytest_args[1:]
    by_feat = tests_by_feature(collected(args))
    if args.feature:
        names = [n.strip() for n in args.feature.split(",") if n.strip()]
        for n in names:
            if n not in by_id:
                die(f"no feature {n!r} in the checklist")
            state, reason = resolve(args.radio, by_id[n])
            if state != "required":
                print(f"bringup.py: {n} is {state} on {args.radio} ({reason}): running it anyway", file=sys.stderr)
    else:
        names = [e["id"] for e in feats if resolve(args.radio, e)[0] == "required" and by_feat.get(e["id"])]
    if not names:
        die("nothing to run")
    rows = [{"id": n, "owner": {t["id"]: t["owner"] for t in by_feat.get(n, []) if t["owner"]},
             "manual": by_id[n]["manual"]} for n in names]
    print_owner(rows, "owner actions in this run" + ("" if "--manual" in pytest_args else
                                                     " (skipped without --manual; they don't verify the feature then)")
                + ":")
    tmp = tempfile.mkdtemp(prefix="rfoundry-bringup-")
    out_path = os.path.join(tmp, "out.json")
    cmd = [_pytest_python(args.pytest_python), "-m", "pytest", *args.tests, "-v", "-rs", "-p", "no:cacheprovider",
           *args.pytest_arg, "--bringup", ",".join(names), "--bringup-out", out_path, *pytest_args]
    lock = lock_path(args.lock, pytest_args)
    if args.dry_run:
        print(f"lock: {lock}")
        print(" ".join(cmd))
        return 0
    with RadioLock(lock, args.lock_wait):
        rc = subprocess.run(cmd, cwd=ROOT).returncode
    if not os.path.exists(out_path):
        die(f"pytest wrote no results (exit {rc})")
    with open(out_path) as f:
        out = json.load(f)
    fw_board = out.get("firmware", {}).get("board")
    if fw_board and fw_board not in boards[args.radio]["debuglink_names"]:
        die(f"the radio reports board={fw_board}, not {args.radio} ({', '.join(boards[args.radio]['debuglink_names'])})"
            ": nothing recorded")
    if args.no_record:
        return rc
    head = git("rev-parse", "HEAD").stdout.strip()
    dirty = git("diff", "--quiet", "--ignore-submodules=all", "HEAD", "--", ".", check=False).returncode != 0
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    record = {"format": RECORD_FORMAT, "kind": "run", "board": args.radio, "commit": head, "dirty": dirty,
              "date": now.isoformat().replace("+00:00", "Z"), "features": names,
              "sources": {n: source_hashes(by_id[n]) for n in names},
              **{k: out[k] for k in ("granted", "tests", "firmware", "options", "tools")}}
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


def check(feats, boards, records, found, all_switches):
    """The consistency errors (strings), and notes. Missing or stale records are never errors: only a malformed
    checklist, marker, override or record is."""
    errs, notes = [], []
    by_switch = {}
    for e in feats:
        where = f"feature {e['id']}"
        for k in sorted(set(e) - FIELDS - {"id"}):
            errs.append(f"{where}: unknown field {k}")
        if not e.get("spec"):
            errs.append(f"{where}: no spec")
        if not re.fullmatch(r"[a-z][a-z0-9_]*", e["id"]):
            errs.append(f"{where}: the id is not snake case")
        if e["switch"]:
            if e["switch"] not in all_switches:
                errs.append(f"{where}: switch {e['switch']} is not in config/features.mk")
            by_switch.setdefault(e["switch"], []).append(e["id"])
        for p in [e.get("spec"), *e["source"]]:
            if p and not os.path.exists(os.path.join(ROOT, p)):
                errs.append(f"{where}: no such path {p}")
        if found is not None:
            bases = {re.sub(r"\[.*\]$", "", nid) for nid in found}
            for t in e["planned_tests"]:
                if t in bases:
                    errs.append(f"{where}: planned test {t} exists now: drop it from planned_tests")
    by_id = {e["id"]: e for e in feats}
    for nid, info in (found or {}).items():
        marks = info["features"]
        if "feature" in info["markers"] and not marks:
            errs.append(f"{nid}: a feature marker without a feature id")
        for m in marks:
            if m["id"] not in by_id:
                errs.append(f"{nid}: marks feature {m['id']}, which tests/hil/features.toml doesn't have")
        for m in marks:
            if m.get("gate") and m["optional"]:
                errs.append(f"{nid}: an optional test can't be {m['id']}'s capability gate")
        if "manual" in info["markers"] and marks and not any(m["owner"] for m in marks):
            errs.append(f"{nid}: marked manual, but its feature marker has no owner= (what the person does)")
    gates = {}
    for nid, info in (found or {}).items():
        for m in info["features"]:
            if m.get("gate"):
                gates.setdefault(m["id"], set()).add(re.sub(r"\[.*\]$", "", nid))
    for fid, g in sorted(gates.items()):
        if len(g) > 1:
            errs.append(f"feature {fid} has more than one capability gate: {', '.join(sorted(g))}")
    for s in all_switches:
        if s not in by_switch:
            errs.append(f"switch {s} has no entry in tests/hil/features.toml")
        elif len(by_switch[s]) > 1:
            errs.append(f"switch {s} has more than one entry: {', '.join(by_switch[s])}")
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
        for e in feats:
            state, reason = resolve(b, e)
            if state in ("n/a", "not written", "unsupported") and not reason:
                errs.append(f"{b}: {e['id']} is {state} without a reason (" + (
                    f"boards/{b}/config.mk: BOARD_UNSUPPORTED += {e['switch']}  # <reason>" if state == "n/a" else
                    f"boards/{b}/config.mk: BOARD_NOT_WRITTEN += {e['switch']}  # <reason>" if state == "not written"
                    else
                    f"boards/{b}/board.toml [features] {e['id']} = {{ state = \"unsupported\", reason = \"...\" }}")
                    + ")")
    for b, recs in records.items():
        if b not in boards:
            errs.append(f"docs/testing/results/{b}: no such board")
        if recs["seed"]:
            errs += check_seed(recs["seed"], b, by_id)
        for r in recs["runs"]:
            where = r["_file"]
            if r.get("kind", "run") != "run":
                errs.append(f"{where}: kind {r.get('kind')!r} (a run record is \"run\", a seed record is seed.json)")
            for k in ("format", "board", "commit", "date", "features", "sources", "tests", "tools"):
                if k not in r:
                    errs.append(f"{where}: no {k}")
            if r.get("board") != b:
                errs.append(f"{where}: board {r.get('board')} in the directory of {b}")
            for f in r.get("features", []):
                if f not in by_id:
                    notes.append(f"{where}: feature {f} is no longer in the checklist")
            if re.search(r"/dev/|usbmodem|://|\bserial[=:]", json.dumps(r)):
                errs.append(f"{where}: names a port, host or serial number")
    # TODO(commit 9): the claim blocks in the docs (feature matrix, README, 23-radios.md) are generated from the
    # records; until they exist there is nothing to compare, so claims against records is not checked yet
    notes.append("claims against records: not checked until the docs' claim blocks are generated")
    return errs, notes


def has_commit(c):
    if not git_checkout():
        return False
    return git("cat-file", "-e", f"{c}^{{commit}}", check=False).returncode == 0


def check_seed(r, b, by_id):
    """A seed record: the board's evidence from before its first recorded run, one row per feature. A row's commit
    may be one the repository no longer has (a squash-merged branch): the row then stands until a run replaces it."""
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
    found = collected(args)
    errs, notes = check(feats, boards, records, found, all_switches)
    if found is None:
        notes.append("pytest not installed: the tests' feature markers not checked")
    for n in notes:
        print(f"note: {n}")
    for e in errs:
        print(f"error: {e}")
    by_feat = tests_by_feature(found)
    untested = [e["id"] for e in feats if not by_feat.get(e["id"])] if found is not None else []
    print(f"bringup check: {len(feats)} features ({sum(1 for e in feats if e['switch'])} with a switch, "
          f"{len(untested)} without a HIL test), {len(boards)} boards, "
          f"{sum(len(r['runs']) + bool(r['seed']) for r in records.values())} records, {len(errs)} errors")
    return 1 if errs else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--features", default=FEATURES_TOML, help="the checklist (default tests/hil/features.toml)")
    ap.add_argument("--results", default=RESULTS, help="the records (default docs/testing/results)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    py = argparse.ArgumentParser(add_help=False)
    py.add_argument("--pytest-python", help="the Python that has pytest (default this one, else python3)")
    py.add_argument("--tests", action="append", help=argparse.SUPPRESS)        # the unit test's own test files
    py.add_argument("--pytest-arg", action="append", default=[], help=argparse.SUPPRESS)    # and pytest options
    p = sub.add_parser("list", parents=[py], help="a radio's checklist with the last result per feature")
    p.add_argument("--radio", required=True, help="board id (boards/<id>)")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("status", help="every radio and feature")
    p.add_argument("--radio", help="one board only")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("run", parents=[py], help="run the radio's required tests and record the result")
    p.add_argument("--radio", required=True, help="board id (boards/<id>) of the radio on the debug link")
    p.add_argument("--feature", help="these features only (comma separated); default every required one")
    p.add_argument("--lock", help="the radio lock file (default $RFOUNDRY_RADIO_LOCK, \"{port}\" in it the port, else "
                                  "~/.rfoundry-radio-<port>.lock)")
    p.add_argument("--lock-wait", type=float, default=0, help="s to wait for the radio lock (default: fail at once)")
    p.add_argument("--no-record", action="store_true", help="run, but write no record")
    p.add_argument("--dry-run", action="store_true", help="print the pytest command, run nothing")
    p.add_argument("pytest_args", nargs=argparse.REMAINDER, help="-- then the HIL options (--allow-rf, --dmr-freq ...)")
    p = sub.add_parser("check", parents=[py], help="host-side consistency (tools/ci.sh)")
    args = ap.parse_args(argv)
    if hasattr(args, "tests"):
        args.tests = args.tests or [os.path.join("tests", "hil")]
    return {"list": cmd_list, "status": cmd_status, "run": cmd_run, "check": cmd_check}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
