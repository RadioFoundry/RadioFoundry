# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The HIL tests' hooks and fixtures (README.md): the radio and the SDR bench, the radio restored after every test,
opt-in skips, skip kinds on the reports, and the boards' own tests (boards/<id>/hil) skipped on other radios.
conftest.py registers this module as a plugin, so all of it reaches tests outside tests/hil too."""
import os

import pytest

import bringup_plugin
from odr_hil import DebugLinkError, Radio, SdrBench, board_id, find_ports, skip, skip_kind, skip_mark

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def own_board(path):
    """The board a test file belongs to (boards/<id>/hil/test_*.py), or None for a shared test."""
    parts = os.path.relpath(str(path), ROOT).split(os.sep)
    return parts[1] if len(parts) == 4 and parts[0] == "boards" and parts[2] == "hil" else None


def pytest_collection_modifyitems(config, items):
    for item in items:
        if ("rf" in item.keywords or "radio_tx" in item.keywords) and not config.getoption("--allow-rf"):
            item.add_marker(skip_mark("opt_in", "transmits: run with --allow-rf"))
        elif "usb_switch" in item.keywords and not config.getoption("--allow-usb-switch"):
            item.add_marker(skip_mark("opt_in", "re-enumerates the radio as a network adapter: run with "
                                                "--allow-usb-switch"))
        elif "codeplug_write" in item.keywords and not config.getoption("--allow-codeplug-write"):
            item.add_marker(skip_mark("opt_in", "writes the codeplug: run with --allow-codeplug-write"))
        elif "reboot" in item.keywords and not config.getoption("--allow-reboot"):
            item.add_marker(skip_mark("opt_in", "restarts the radio: run with --allow-reboot"))
        elif "manual" in item.keywords and not config.getoption("--manual"):
            item.add_marker(skip_mark("opt_in", "needs a person at the radio: run with --manual -s"))


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item, call):
    """A skip's kind (odr_hil.skip) on its report (report.skip_kind) and in the test's user properties, which the
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
    if not port and not find_ports():
        bringup_plugin.no_radio(pytestconfig, "no radio on USB (1FC9:0094): plug it in or give --radio-port")
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
    yield r
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
def restore_radio(request):
    """Every test that uses the radio leaves it as it found it (channel / VFO, zone, power, squelch, monitor)."""
    # test_tcp_services.py's module fixture "net" has the radio on the network (its serial link closed) for the whole
    # module and puts it back on serial itself: nothing to snapshot over serial then
    if "radio" not in request.fixturenames or "net" in request.fixturenames:
        yield
        return
    r = request.getfixturevalue("radio")
    snap = r.snapshot()
    try:
        yield
    finally:
        try:
            r.link.ptt(False)       # whatever happened, no PTT held
        except DebugLinkError:
            pass
        r.restore(snap)


@pytest.fixture(scope="session")
def sdr_bench(pytestconfig):
    b = SdrBench(allow_rf=pytestconfig.getoption("--allow-rf"),
                 allow_service_stop=pytestconfig.getoption("--allow-service-stop"),
                 tx_power=pytestconfig.getoption("--tx-power"), gain=pytestconfig.getoption("--sdr-gain"),
                 wait_max=pytestconfig.getoption("--bench-wait-max"))
    if not b.reachable():
        skip("bench", f"SDR bench {b.host} not reachable over ssh, or its tools missing (tools/bench/dmr_bench.py "
                      "sync)")
    return b


@pytest.fixture(scope="session")
def bench_station(pytestconfig, sdr_bench):
    """The name of the bench station to use as the second radio (--bench-station); skips without one."""
    name = pytestconfig.getoption("--bench-station")
    if not name:
        skip("opt_in", "no bench station: give --bench-station NAME (odr-bench info on the bench lists them)")
    st = next((s for s in sdr_bench.info().get("stations", []) if s["name"] == name), None)
    if st is None or not st.get("present"):
        skip("bench", f"bench station {name} is not on the bench (or the bench has no benchd)")
    return name


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
