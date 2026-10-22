# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Bring-up mode for the HIL tests (README.md, "Bring-up mode"): pytest --bringup FEATURE[,FEATURE]|all runs the
tests marked @pytest.mark.feature("<id>") for those features, and a required test that skips for a reason its needs
don't excuse fails instead. tools/bringup.py run drives it and turns the results it writes (--bringup-out) into a
record under docs/testing/results/.

The marker: @pytest.mark.feature("<id>"[, "<id>" ...][, optional=True][, owner="what a person does"]). A test may
carry several (required for one feature, optional for another). A test's needs come from its markers and fixtures
(MARKER_NEEDS, FIXTURE_NEEDS).

Which skips are excused (the skip kinds are odr_hil.SKIP_KINDS):
- other_board: always (a test for another board).
- bench: only when the test needs the SDR bench. A run without a radio is never excused: the radio fixture fails in
  bring-up mode (no_radio below).
- opt_in: only when the test needs something the run didn't allow (NEED_FLAGS). With every need allowed, a missing
  option (a frequency, a recording) is the run's fault and fails.
- not_built, no_hw, state, and a skip without a kind: never. Built without the switch, a radio lacking the
  capability and a test that couldn't set itself up are what the checklist is there to catch.
An optional test never fails for a skip, and an excused skip is recorded but doesn't verify the feature."""
import json
import os
import platform
import re

import pytest

from odr_hil import skip, skip_kind

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# Test markers and fixtures -> the need they imply
MARKER_NEEDS = {"rf": "rf", "radio_tx": "rf", "bench_service": "bench", "usb_switch": "usb_switch",
                "codeplug_write": "codeplug_write", "reboot": "reboot", "manual": "owner_action",
                "second_radio": "second_radio"}
FIXTURE_NEEDS = {"radio": "radio", "sdr_bench": "bench", "bench_station": "second_radio"}
# A need -> the run options that allow it (any one of them). radio and bench have none: the radio fixture and
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
                help="bring-up mode: run the tests marked for these features (comma separated, or all); a "
                     "required test that skips for a reason its needs don't excuse fails")
    g.addoption("--bringup-out", default=None, metavar="FILE", help="write the bring-up results here as JSON")
    g.addoption("--bringup-collect", default=None, metavar="FILE",
                help="write every collected test's features, needs and markers as JSON (tools/bringup.py)")


def feature_marks(item):
    """[(feature, optional, owner, gate)] of a test's feature markers."""
    out = []
    for m in item.iter_markers("feature"):
        for fid in m.args:
            out.append((fid, bool(m.kwargs.get("optional")), m.kwargs.get("owner"), bool(m.kwargs.get("gate"))))
    return out


def needs_of(item):
    """What a test needs, from its markers and fixtures."""
    out = {MARKER_NEEDS[m.name] for m in item.iter_markers() if m.name in MARKER_NEEDS}
    return out | {FIXTURE_NEEDS[f] for f in getattr(item, "fixturenames", []) if f in FIXTURE_NEEDS}


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
        want = config.getoption("--bringup")
        self.all = want == "all"
        self.features = [] if self.all else [w.strip() for w in want.split(",") if w.strip()]
        self.granted = granted(config)
        self.tests = {}         # node id -> {"required": [features], "optional": [features], "gates": [features],
        #                                     "needs": set}
        self.gates = {}         # feature -> its capability gate's node id
        self.gate_failed = {}   # feature -> what its gate did instead of passing
        self.results = {}
        self.secrets = [v for v in (config.getoption(o, default=None) for o in PORT_OPTIONS) if v]

    def select(self, item, nid):
        """Whether the run covers the test; remembers its role."""
        req, opt, gates = [], [], []
        for fid, optional, _, gate in feature_marks(item):
            if self.all or fid in self.features:
                (opt if optional else req).append(fid)
                if gate and not optional:
                    gates.append(fid)
                    self.gates[fid] = nid
                if self.all and fid not in self.features:
                    self.features.append(fid)
        if not req and not opt:
            return False
        self.tests[nid] = {"required": sorted(set(req)), "optional": sorted(set(opt) - set(req)),
                           "gates": sorted(set(gates)), "needs": needs_of(item)}
        return True

    def role(self, nid):
        return self.tests.get(nid)

    def excused(self, nid, kind):
        """Whether a skip of this kind is excused for the test."""
        needs = self.tests[nid]["needs"]
        if kind == "other_board":
            return True
        if kind == "bench":
            return "bench" in needs
        if kind == "opt_in":
            return bool(needs - self.granted)
        return False

    def record(self, nid, report, required, excused=None):
        role = self.tests[nid]
        r = self.results.setdefault(nid, {"id": nid, "features": role["required"], "optional": role["optional"],
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
        config.addinivalue_line("markers", "feature(id, ..., optional=False, owner=None, gate=False): the checklist "
                                "feature(s) the test checks (tests/hil/features.toml); optional: run and recorded, "
                                "not gating; owner: what a person does for it; gate: the feature's capability gate, "
                                "run first in bring-up mode, its other required tests fail unless it passes")
        config.addinivalue_line("markers", "second_radio: needs a second radio (--split-rx-port or --bench-station)")
        if config.getoption("--bringup", default=None):
            config._odr_bringup = BringUp(config)

    def pytest_collection_modifyitems(self, session, config, items):
        out = config.getoption("--bringup-collect", default=None)
        if out:
            found = {}
            for item in items:
                found[node_id(item)] = {"markers": sorted({m.name for m in item.iter_markers()}),
                                        "needs": sorted(needs_of(item)),
                                        "features": [{"id": f, "optional": o, "owner": w, "gate": g}
                                                     for f, o, w, g in feature_marks(item)]}
            with open(out, "w") as f:
                json.dump(found, f, indent=1, sort_keys=True)
        bu = getattr(config, "_odr_bringup", None)
        if bu is None:
            return
        keep, drop = [], []
        for item in items:
            nid = node_id(item)
            if bu.select(item, nid):
                item._odr_bringup_id = nid
                keep.append(item)
            else:
                drop.append(item)
        if drop:
            config.hook.pytest_deselected(items=drop)
        # capability gates first, the rest in collection order
        items[:] = [i for i in keep if bu.tests[i._odr_bringup_id]["gates"]] + [
            i for i in keep if not bu.tests[i._odr_bringup_id]["gates"]]

    @pytest.hookimpl(tryfirst=True)
    def pytest_runtest_setup(self, item):
        """A required test of a feature whose capability gate didn't pass fails without running."""
        bu = getattr(item.config, "_odr_bringup", None)
        nid = getattr(item, "_odr_bringup_id", None)
        if bu is None or nid is None:
            return
        shut = [f for f in bu.tests[nid]["required"] if f in bu.gate_failed and bu.gates.get(f) != nid]
        if shut:
            pytest.fail("bring-up: capability gate failed, not run: " + "; ".join(
                f"{f}'s gate {bu.gates[f].split('::')[-1]} {bu.gate_failed[f]}" for f in shut), pytrace=False)

    @pytest.hookimpl(wrapper=True, tryfirst=True)
    def pytest_runtest_makereport(self, item, call):
        """In bring-up mode, a required test's skip that its features don't excuse becomes a failure. tryfirst: it
        wraps hil_plugin's wrapper, which has put the kind on the report by then."""
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
        if report.outcome != "passed":
            for f in bu.tests[nid]["gates"]:
                bu.gate_failed.setdefault(f, report.outcome)
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

