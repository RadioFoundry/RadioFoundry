# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Bring-up mode for the HIL tests (README.md, "Bring-up mode"): pytest --bringup FEATURE[,FEATURE]|all runs the
required and optional tests of those features from the checklist (features.toml), and a required test that skips
for a reason its feature doesn't declare fails instead. tools/bringup.py run drives it, writes the plan it reads
(--bringup-plan) and turns the results it writes (--bringup-out) into a record under docs/testing/results/.

Which skips are excused (the skip kinds are odr_hil.SKIP_KINDS):
- other_board: always (a test for another board).
- bench: only when the feature needs the SDR bench. A run without a radio is never excused: the radio fixture
  fails in bring-up mode (no_radio below).
- opt_in: only when the feature needs something the run didn't allow (needs below, NEED_FLAGS). With every need
  allowed, a missing option (a frequency, a recording) is the run's fault and fails.
- not_built, no_hw, state, and a skip without a kind: never. Built without the switch, a radio lacking the
  capability and a test that couldn't set itself up are what the checklist is there to catch.
An optional test never fails for a skip, and an excused skip is recorded but doesn't verify the feature.

This file runs on the Python pytest runs on (3.9 here and there), so it reads features.toml only when tomllib is
there (3.11 and later); tools/bringup.py passes a JSON plan instead."""
import json
import os
import platform
import re

import pytest

from odr_hil import skip, skip_kind

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# A feature need -> the run options that allow it (any one of them). radio and bench have none: the radio fixture and
# the bench fixture decide (an unreachable bench is a bench skip, excused for a feature that needs the bench)
NEED_FLAGS = {
    "radio": (),
    "bench": (),
    "rf": ("--allow-rf",),
    "usb_switch": ("--allow-usb-switch",),
    "codeplug_write": ("--allow-codeplug-write",),
    "reboot": ("--allow-reboot",),
    "owner_action": ("--manual",),
    "second_radio": ("--split-rx-port", "--bench-station"),
}
# The VERSION keys a record keeps (docs/spec/debuglink.md 3.1): no uptime, nothing that names one unit
FIRMWARE_KEYS = ("proto", "view", "git", "board", "features", "codec", "display")
# Run options a record keeps: what was allowed and where it ran on air, never a port or a host
RECORD_OPTIONS = ("--allow-rf", "--allow-service-stop", "--allow-usb-switch", "--allow-codeplug-write",
                  "--allow-reboot", "--manual", "--dmr-freq", "--dmr-cc", "--dmr-tg", "--fm-mhz", "--tx-power")


def add_options(parser):
    """conftest.py's pytest_addoption calls this; conftest.py's pytest_configure registers Plugin."""
    g = parser.getgroup("odr-bringup", "OpenDigitalRadio bring-up mode (tools/bringup.py)")
    g.addoption("--bringup", default=None, metavar="FEATURES",
                help="bring-up mode: run the checklist tests of these features (comma separated, or all); a "
                     "required test that skips for a reason its feature doesn't declare fails")
    g.addoption("--bringup-plan", default=None, metavar="FILE",
                help="the features as JSON (tools/bringup.py writes it); default: features.toml (Python 3.11+)")
    g.addoption("--bringup-out", default=None, metavar="FILE", help="write the bring-up results here as JSON")
    g.addoption("--bringup-collect", default=None, metavar="FILE",
                help="write every collected test's markers and fixtures as JSON (tools/bringup.py check)")


def no_radio(config, reason):
    """The radio fixture's way out without a radio: a bench skip normally, a failure in bring-up mode (a bring-up
    run that found no radio verified nothing)."""
    __tracebackhide__ = True
    if config.getoption("--bringup", default=None):
        pytest.fail(f"bring-up: {reason}", pytrace=False)
    skip("bench", reason)


def node_id(item):
    """The item's node id relative to the repository (pytest's own is relative to its rootdir, which follows the
    command line), or to the rootdir for a test outside the repository."""
    path = os.path.abspath(str(item.path))
    base = ROOT if path.startswith(ROOT + os.sep) else str(item.config.rootpath)
    rest = item.nodeid.split("::", 1)[1] if "::" in item.nodeid else item.name
    return os.path.relpath(path, base).replace(os.sep, "/") + "::" + rest


def base_id(nid):
    """A parametrized id without its parameters: the checklist lists the test, every case of it counts."""
    return re.sub(r"\[.*\]$", "", nid)


# Options whose values name a port or a host: their values never reach a record
PORT_OPTIONS = ("--radio-port", "--split-rx-port", "--split-ms-port", "--bench-station")


def sanitize(text, secrets=()):
    """A skip or failure reason fit for a committed record: no serial ports, device serials or host names."""
    for s in secrets:
        text = text.replace(s, "<port>")
    text = re.sub(r"\w+://\S+", "<url>", text)
    text = re.sub(r"/dev/\S+", "<port>", text)
    text = re.sub(r"(?i)\b(?:COM\d+|usbmodem\w*|serial[=: ]+\S+)", "<port>", text)
    text = re.sub(r"\b[\w-]+\.local\b", "<host>", text)
    return text[:400]


def load_plan(config):
    path = config.getoption("--bringup-plan")
    if path:
        with open(path) as f:
            return json.load(f)
    try:
        import tomllib
    except ImportError:
        raise pytest.UsageError("--bringup without --bringup-plan reads features.toml, which needs Python 3.11 or "
                                "later: run tools/bringup.py run, or give --bringup-plan")
    with open(os.path.join(HERE, "features.toml"), "rb") as f:
        data = tomllib.load(f)
    return {"features": {e["id"]: {k: e.get(k, []) for k in ("tests", "optional_tests", "needs")}
                         for e in data.get("feature", [])}}


def granted(config):
    """The needs this run allows."""
    out = set()
    for need, flags in NEED_FLAGS.items():
        if not flags or any(config.getoption(f, default=None) for f in flags):
            out.add(need)
    return out


class BringUp:
    def __init__(self, config):
        self.config = config
        plan = load_plan(config)
        feats = plan.get("features", {})
        want = config.getoption("--bringup")
        names = list(feats) if want == "all" else [w.strip() for w in want.split(",") if w.strip()]
        unknown = [n for n in names if n not in feats]
        if unknown:
            raise pytest.UsageError(f"--bringup: no such feature in the checklist: {', '.join(unknown)}")
        self.features = names
        self.granted = granted(config)
        self.tests = {}         # base node id -> {"required": [features], "optional": [features]}
        for name in names:
            for key, kind in (("tests", "required"), ("optional_tests", "optional")):
                for t in feats[name].get(key, []):
                    self.tests.setdefault(t, {"required": [], "optional": []})[kind].append(name)
        self.needs = {name: set(feats[name].get("needs", [])) for name in names}
        self.results = {}
        self.seen = set()
        self.secrets = [v for v in (config.getoption(o, default=None) for o in PORT_OPTIONS) if v]

    def role(self, nid):
        return self.tests.get(base_id(nid))

    def excused(self, nid, kind):
        """Whether a skip of this kind is excused for every feature that requires the test."""
        feats = self.role(nid)["required"]
        if kind == "other_board":
            return True
        if kind == "bench":
            return all("bench" in self.needs[f] for f in feats)
        if kind == "opt_in":
            return all(self.needs[f] - self.granted for f in feats)
        return False

    def record(self, nid, report, required, excused=None):
        r = self.results.setdefault(nid, {"id": nid, "features": sorted(set(sum(self.role(nid).values(), []))),
                                          "required": required, "outcome": "passed"})
        if report.failed:
            r["outcome"] = "failed"
            text = report.longreprtext.strip().splitlines()
            r["reason"] = sanitize(text[-1] if text else "", self.secrets)
        elif report.skipped and r["outcome"] == "passed":
            r["outcome"] = "skipped"
            r["skip_kind"] = getattr(report, "skip_kind", None)
            r["excused"] = bool(excused)
            longrepr = report.longrepr
            reason = longrepr[2] if isinstance(longrepr, tuple) else str(longrepr)
            r["reason"] = sanitize(reason[len("Skipped: "):] if reason.startswith("Skipped: ") else reason,
                                   self.secrets)


class Plugin:
    """The hooks, registered by conftest.py's pytest_configure (a conftest below the rootdir can't name plugins)."""

    def __init__(self, config):
        if config.getoption("--bringup", default=None):
            config._odr_bringup = BringUp(config)

    def pytest_collection_modifyitems(self, session, config, items):
        out = config.getoption("--bringup-collect", default=None)
        if out:
            found = {}
            for item in items:
                found[node_id(item)] = {"markers": sorted({m.name for m in item.iter_markers()}),
                                        "fixtures": sorted(getattr(item, "fixturenames", []))}
            with open(out, "w") as f:
                json.dump(found, f, indent=1, sort_keys=True)
        bu = getattr(config, "_odr_bringup", None)
        if bu is None:
            return
        keep, drop = [], []
        for item in items:
            nid = node_id(item)
            if bu.role(nid):
                item._odr_bringup_id = nid
                keep.append(item)
                bu.seen.add(base_id(nid))
            else:
                drop.append(item)
        if drop:
            config.hook.pytest_deselected(items=drop)
            items[:] = keep

    @pytest.hookimpl(wrapper=True, tryfirst=True)
    def pytest_runtest_makereport(self, item, call):
        """In bring-up mode, a required test's skip that its features don't excuse becomes a failure. tryfirst: it
        wraps conftest's wrapper, which has put the kind on the report by then."""
        report = yield
        bu = getattr(item.config, "_odr_bringup", None)
        nid = getattr(item, "_odr_bringup_id", None)
        if bu is None or nid is None:
            return report
        if not hasattr(report, "skip_kind") and report.skipped and not hasattr(report, "wasxfail"):
            report.skip_kind = skip_kind(call.excinfo.value if call.excinfo else report)
        required = bool(bu.role(nid)["required"])
        excused = None
        if report.skipped and not hasattr(report, "wasxfail") and required:
            kind = report.skip_kind
            excused = bu.excused(nid, kind)
            if not excused:
                longrepr = report.longrepr
                reason = longrepr[2] if isinstance(longrepr, tuple) else str(longrepr)
                report.outcome = "failed"
                report.longrepr = (f"bring-up: a required test of {', '.join(bu.role(nid)['required'])} skipped "
                                   f"({kind or 'no kind'}), which its needs don't excuse: {reason}")
        if report.when == "call" or report.outcome != "passed":
            bu.record(nid, report, required, excused)
        return report

    def pytest_sessionfinish(self, session, exitstatus):
        config = session.config
        bu = getattr(config, "_odr_bringup", None)
        out = config.getoption("--bringup-out", default=None)
        if bu is None or not out:
            return
        options = {}
        for opt in RECORD_OPTIONS:
            v = config.getoption(opt, default=None)
            if v not in (None, False):
                options[opt.lstrip("-")] = v
        fw = getattr(config, "_odr_firmware", None) or {}
        doc = {
            "features": bu.features,
            "granted": sorted(bu.granted),
            "missing": sorted(t for t in bu.tests if t not in bu.seen),
            "tests": [bu.results[k] for k in sorted(bu.results)],
            "firmware": {k: fw[k] for k in FIRMWARE_KEYS if k in fw},
            "options": options,
            "tools": {"python": platform.python_version(), "pytest": pytest.__version__},
            "exitstatus": int(exitstatus),
        }
        with open(out, "w") as f:
            json.dump(doc, f, indent=1, sort_keys=True)
            f.write("\n")


def remember_firmware(config, radio):
    """The radio fixture hands over the firmware's VERSION for the record (bring-up mode only)."""
    if getattr(config, "_odr_bringup", None) is not None:
        try:
            config._odr_firmware = dict(radio.version())
        except Exception:      # a record without the firmware is still a record; the tests show what went wrong
            config._odr_firmware = None

