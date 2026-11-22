# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The HIL tests' hooks and fixtures (README.md): the radio and the SDR bench, the radio restored after every test,
opt-in skips, skip kinds on the reports, the boards' own tests (boards/<id>/hil) skipped on other radios, the tiers
and --affected selection, and the run's record (build/hil/). conftest.py registers this module as a plugin, so all of
it reaches tests outside tests/hil too."""
import datetime
import glob
import json
import os
import re
import subprocess
import sys
import time

import pytest

import bringup_plugin
from rfoundry_hil import (DebugLinkError, Radio, SdrBench, board_id, find_network_radios, find_ports, need_radio_tx,
                          skip, skip_kind, skip_mark)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TIERS = ("smoke", "standard", "bringup")
RECORD_FORMAT = 1


def own_board(path):
    """The board a test file belongs to (boards/<id>/hil/test_*.py), or None for a shared test."""
    parts = os.path.relpath(str(path), ROOT).split(os.sep)
    return parts[1] if len(parts) == 4 and parts[0] == "boards" and parts[2] == "hil" else None


def tier_of(item):
    """A test's tier: smoke, bringup (its markers), else standard."""
    if item.get_closest_marker("smoke"):
        return "smoke"
    return "bringup" if item.get_closest_marker("bringup") else "standard"


def whole_directory(config):
    """Whether the command line names only the test directories (tests/hil and the boards' own), no file or node."""
    dirs = {HERE} | {os.path.abspath(d) for d in glob.glob(os.path.join(ROOT, "boards", "*", "hil"))}
    return all("::" not in str(a) and os.path.abspath(str(a)) in dirs for a in config.args)


def run_tier(config):
    """The tier the run selects, or None for no tier filter. A run of the whole directory defaults to standard, so the
    bring-up tier is opt-in; a command that picks its tests (files, nodes, -k, -m), bring-up mode (which picks by
    feature) and tools/bringup.py's collection (--bringup-collect) get what they asked for, as before the tiers."""
    t = config.getoption("--tier", default=None)
    if t:
        return t
    if config.getoption("--affected", default=None):
        return "standard"
    if (config.getoption("--bringup", default=None) or config.getoption("--bringup-collect", default=None)
            or config.option.keyword or config.option.markexpr):
        return None
    return "standard" if whole_directory(config) else None


def affected_selection(config, base):
    """({feature: [why]}, changed paths) of the change from base's merge base with HEAD to the working tree."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import affected
    try:
        mb = affected.git(ROOT, "merge-base", base, "HEAD").strip()
        diff = affected.changed(ROOT, mb, None)
        return affected.affected_features(ROOT, mb, None, diff), set(diff)
    except subprocess.CalledProcessError as e:
        raise pytest.UsageError(f"--affected {base}: {' '.join(e.cmd)}: {e.stderr.strip()}")


def item_path(item):
    return os.path.relpath(str(item.path), ROOT).replace(os.sep, "/")


def select(config, items):
    """Drop the tests outside the run's tier and, with --affected, those the change doesn't reach."""
    tier = run_tier(config)
    base = config.getoption("--affected", default=None)
    keep, drop, note = [], [], []
    if tier:
        allowed = TIERS[:TIERS.index(tier) + 1]
        for item in items:
            (keep if tier_of(item) in allowed else drop).append(item)
        note.append(f"tier {tier}: {len(keep)} tests, {len(drop)} deselected")
        items[:], keep = keep, []
    if base:
        feats, paths = affected_selection(config, base)
        before = len(drop)
        for item in items:
            mine = {f for m in item.iter_markers("feature") for f in m.args}
            hit = tier_of(item) == "smoke" or (mine & set(feats)) or item_path(item) in paths
            (keep if hit else drop).append(item)
        items[:] = keep
        note.append(f"affected since {base}: {len(keep)} tests ({len(drop) - before} deselected); features: "
                    + (", ".join(sorted(feats)) or "none, smoke only"))
    if drop:
        config.hook.pytest_deselected(items=drop)
    tr = config.pluginmanager.get_plugin("terminalreporter")
    for n in note:
        if tr:
            tr.write_line(f"rfoundry-hil: {n}")
    config._rfoundry_selection = {"tier": tier, "affected": base}


def pytest_collection_modifyitems(config, items):
    select(config, items)
    for item in items:
        if ("rf" in item.keywords or "radio_tx" in item.keywords) and not config.getoption("--allow-rf"):
            if "audio_smoke" in item.keywords:
                item.add_marker(skip_mark("opt_in", "DMR AUDIO NOT CHECKED: transmits; run the bench pass with "
                                                    "--allow-rf (README.md, Default bench pass)"))
            else:
                item.add_marker(skip_mark("opt_in", "transmits: run with --allow-rf"))
        elif "usb_switch" in item.keywords and not config.getoption("--allow-usb-switch"):
            item.add_marker(skip_mark("opt_in", "re-enumerates the radio as a network adapter: run with "
                                                "--allow-usb-switch"))
        elif "codeplug_write" in item.keywords and not config.getoption("--allow-codeplug-write"):
            item.add_marker(skip_mark("opt_in", "writes the codeplug: run with --allow-codeplug-write"))
        elif "reboot" in item.keywords and not config.getoption("--allow-reboot"):
            item.add_marker(skip_mark("opt_in", "restarts the radio: run with --allow-reboot"))
        elif "manual" in item.keywords and not config.getoption("--manual") and \
                not ("by_ear" in item.keywords and config.getoption("--microphone")):
            item.add_marker(skip_mark("opt_in", "needs a person at the radio: run with --manual -s" +
                                      (", or --microphone to listen with it" if "by_ear" in item.keywords else "")))


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item, call):
    """A skip's kind (rfoundry_hil.skip) on its report (report.skip_kind) and in the test's user properties, which the
    JUnit XML writes: what a bring-up run and its result records read."""
    report = yield
    if report.skipped and not hasattr(report, "wasxfail"):
        report.skip_kind = skip_kind(call.excinfo.value if call.excinfo else report)
        if report.skip_kind and ("skip_kind", report.skip_kind) not in item.user_properties:
            item.user_properties.append(("skip_kind", report.skip_kind))
    return report


@pytest.fixture(scope="session")
def radio(pytestconfig):
    port = pytestconfig.getoption("--radio-port")
    if not port and not find_ports() and not find_network_radios():
        bringup_plugin.no_radio(pytestconfig, "no radio on USB (1FC9:0094, or one in USB network mode answering on "
                                            "UDP): plug it in or give --radio-port "
                                            "(DMR audio not checked either)")
    try:
        r = Radio(port)
    except DebugLinkError as e:
        bringup_plugin.no_radio(pytestconfig, f"radio not reachable: {e}")
    try:
        r.ping()
    except DebugLinkError as e:
        r.close()
        bringup_plugin.no_radio(pytestconfig, f"the radio doesn't answer the debug link (firmware without "
                                              f"CONFIG_DEBUGLINK?): {e}")
    bringup_plugin.remember_firmware(pytestconfig, r)
    try:
        pytestconfig._rfoundry_hil_version = dict(r.version())    # for the run's record
    except DebugLinkError:
        pass
    if r.auto_lock_off():
        print("rfoundry-hil: auto lock was on (a run cut short in test_auto_lock?): set back to Off")
    lang = r.language_english()
    if lang:
        print(f"rfoundry-hil: the radio's language was {lang}: set back to English (the tests read English text)")
    r.tx_gate(False)        # blocked from the start: module fixtures run before any test's own gate
    yield r
    try:
        r.tx_gate_end()
    except (DebugLinkError, OSError) as e:     # serial errors are OSErrors: the port may be gone
        print(f"rfoundry-hil: couldn't clear the test TX limit ({e}); a restart clears it")
    r.close()


@pytest.fixture(autouse=True)
def board_only(request):
    """A board's own test runs only on that board's radio: other_board anywhere else."""
    bid = own_board(request.node.path)
    if bid is None:
        return
    only_on(bid, request.getfixturevalue("radio"))


def only_on(bid, radio):
    """Skip other_board unless the radio is board bid."""
    name = radio.version().get("board", "")
    if board_id(name) != bid:
        skip("other_board", f"a test of boards/{bid}; this radio is {board_id(name) or name or 'unknown'}")


@pytest.fixture(autouse=True)
def radio_can_tx(request):
    """A radio_tx test on a radio that can't transmit: not_built."""
    if "radio_tx" in request.keywords and "radio" in request.fixturenames:
        need_radio_tx(request.getfixturevalue("radio"))


@pytest.fixture(autouse=True)
def restore_radio(request):
    """Every test that uses the radio leaves it as it found it (channel / VFO, zone, power, squelch, monitor), and runs
    with every transmission blocked unless marked radio_tx (Radio.tx_gate). A radio_tx test is blocked again after it,
    so nothing between tests (module fixtures, a module without the radio's serial link) can transmit."""
    # test_tcp_services.py's module fixture "net" has the radio on the network (its serial link closed) for the whole
    # module and puts it back on serial itself: nothing to snapshot over serial then; so does test_radio_udp.py's
    # "udp_ctl", whose test puts the radio back where it was. test_ppp_link.py's "ppp" has PPP on the serial port, which
    # a debug link frame there would end (ppp.md 4); its tests change nothing
    if "radio" not in request.fixturenames or any(f in request.fixturenames for f in ("net", "udp_ctl", "ppp")):
        yield
        return
    r = request.getfixturevalue("radio")
    may_transmit = "radio_tx" in request.keywords
    r.tx_gate(may_transmit)
    snap = r.snapshot()
    try:
        yield
    finally:
        try:
            r.link.ptt(False)       # whatever happened, no PTT held
        except DebugLinkError:
            pass
        r.restore(snap)
        if may_transmit:
            try:
                r.tx_gate(False)
            except DebugLinkError:
                pass


@pytest.fixture(scope="session")
def sdr_bench(pytestconfig):
    b = SdrBench(allow_rf=pytestconfig.getoption("--allow-rf"),
                 wait_max=pytestconfig.getoption("--bench-wait-max"), api=pytestconfig.getoption("--bench-api"),
                 api_rx=pytestconfig.getoption("--bench-api-rx"),
                 api_level=pytestconfig.getoption("--bench-api-level"))
    if not b.api:
        skip("bench", "no bench board: give --bench-api HOST[:PORT] (no bench test runs, the DMR audio smoke neither)")
    if not b.reachable():
        skip("bench", f"bench board {b.api} (receive {b.api_rx}) not answering on the line protocol, with a TX "
                      "frequency and its receiver running")
    yield b
    b.remove_run_dirs()


@pytest.fixture(scope="session")
def sdr_bench_2(pytestconfig):
    """A second bench transmitter (--bench-api-2), for a test that needs two signals at once"""
    api = pytestconfig.getoption("--bench-api-2")
    if not api:
        skip("bench", "no second bench transmitter: give --bench-api-2 HOST[:PORT]")
    b = SdrBench(allow_rf=pytestconfig.getoption("--allow-rf"), wait_max=pytestconfig.getoption("--bench-wait-max"),
                 api=api, api_level=pytestconfig.getoption("--bench-api-level"))
    if not b.reachable():
        skip("bench", f"the second bench board {b.api} not answering on the line protocol, with a TX frequency and "
                      "its receiver running")
    yield b
    b.remove_run_dirs()


@pytest.fixture
def shots(pytestconfig, tmp_path):
    d = pytestconfig.getoption("--shots")
    if d:
        os.makedirs(d, exist_ok=True)
        return d
    return str(tmp_path)


@pytest.fixture
def backup_dir(pytestconfig, tmp_path):
    """Where codeplug_write tests keep their codeplug backups (--codeplug-backup-dir, else the test's tmp dir)."""
    return pytestconfig.getoption("--codeplug-backup-dir") or str(tmp_path)


# ── the run's record ──

_RUN = None     # the session's run: start time and each test's result (pytest_runtest_logreport gets no config)


def pytest_sessionstart(session):
    global _RUN
    _RUN = {"t0": time.time(), "tests": {}}
    session.config._rfoundry_hil_run = _RUN


def pytest_runtest_logreport(report):
    """Each test's outcome (a failure in any phase, else a skip, else passed), its skip kind and its time over all
    phases."""
    if _RUN is None:
        return
    t = _RUN["tests"].setdefault(report.nodeid, {"id": report.nodeid, "outcome": "passed", "duration_s": 0.0})
    t["duration_s"] = round(t["duration_s"] + (report.duration or 0.0), 3)
    if report.failed:
        t["outcome"] = "failed"
        t.pop("skip_kind", None)
    elif report.skipped and t["outcome"] == "passed":
        t["outcome"] = "xfailed" if hasattr(report, "wasxfail") else "skipped"
        if getattr(report, "skip_kind", None):
            t["skip_kind"] = report.skip_kind


def slug(text):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("_.") or "unknown"


def tree_commit():
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def pytest_sessionfinish(session, exitstatus):
    """build/hil/<board>-<firmware commit>.json (or --hil-record): what ran on which firmware and how long each test
    took, to find the slow ones. Written when the run used the radio or was given a file; never a port or a host."""
    config = session.config
    run = getattr(config, "_rfoundry_hil_run", None)
    out = config.getoption("--hil-record", default=None)
    fw = getattr(config, "_rfoundry_hil_version", None)
    if run is None or config.option.collectonly or out == "none" or not (out or fw):
        return
    fw = fw or {}
    board = (board_id(fw.get("board", "")) if fw.get("board") else None) or fw.get("board") or "unknown"
    tree = tree_commit()
    fw_git = fw.get("git") or tree or "unknown"
    if not out:
        out = os.path.join(ROOT, "build", "hil", f"{slug(board)}-{slug(fw_git)}.json")
    sel = getattr(config, "_rfoundry_selection", {})
    doc = {
        "format": RECORD_FORMAT,
        "board": board,
        "firmware": {k: fw[k] for k in bringup_plugin.FIRMWARE_KEYS if k in fw},
        "tree": tree,
        "tier": sel.get("tier"),
        "affected": sel.get("affected"),
        "started": datetime.datetime.fromtimestamp(run["t0"], datetime.timezone.utc).isoformat(timespec="seconds"),
        "duration_s": round(time.time() - run["t0"], 1),
        "exitstatus": int(exitstatus),
        "tests": sorted(run["tests"].values(), key=lambda t: t["id"]),
    }
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")
    tr = config.pluginmanager.get_plugin("terminalreporter")
    if tr:
        tr.ensure_newline()
        tr.write_line(f"rfoundry-hil: record {os.path.relpath(out)}")
