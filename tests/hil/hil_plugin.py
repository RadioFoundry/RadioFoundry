# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The HIL tests' hooks and fixtures (README.md): the radio and the SDR bench, the radio restored after every test,
opt-in skips, skip kinds on the reports, and the boards' own tests (boards/<id>/hil) skipped on other radios.
conftest.py registers this module as a plugin, so all of it reaches tests outside tests/hil too."""
import os

import pytest

import bringup_plugin
from rfoundry_hil import (DebugLinkError, Radio, SdrBench, board_id, find_network_radios, find_ports, need_radio_tx,
                          skip, skip_kind, skip_mark)

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def own_board(path):
    """The board a test file belongs to (boards/<id>/hil/test_*.py), or None for a shared test."""
    parts = os.path.relpath(str(path), ROOT).split(os.sep)
    return parts[1] if len(parts) == 4 and parts[0] == "boards" and parts[2] == "hil" else None


def marked(item, name):
    """The test carries the marker (its own, its class's or its module's). Not item.keywords, which also holds the
    names of the directories above the test: a checkout in a directory named rf would be taken for the rf marker"""
    return item.get_closest_marker(name) is not None


def pytest_collection_modifyitems(config, items):
    for item in items:
        if (marked(item, "rf") or marked(item, "radio_tx")) and not config.getoption("--allow-rf"):
            if marked(item, "audio_smoke"):
                item.add_marker(skip_mark("opt_in", "DMR AUDIO NOT CHECKED: transmits; run the bench pass with "
                                                    "--allow-rf --ambe FILE (README.md, Default bench pass)"))
            else:
                item.add_marker(skip_mark("opt_in", "transmits: run with --allow-rf"))
        elif marked(item, "usb_switch") and not config.getoption("--allow-usb-switch"):
            item.add_marker(skip_mark("opt_in", "re-enumerates the radio as a network adapter: run with "
                                                "--allow-usb-switch"))
        elif marked(item, "codeplug_write") and not config.getoption("--allow-codeplug-write"):
            item.add_marker(skip_mark("opt_in", "writes the codeplug: run with --allow-codeplug-write"))
        elif marked(item, "reboot") and not config.getoption("--allow-reboot"):
            item.add_marker(skip_mark("opt_in", "restarts the radio: run with --allow-reboot"))
        elif marked(item, "manual") and not config.getoption("--manual") and \
                not (marked(item, "by_ear") and config.getoption("--microphone")):
            item.add_marker(skip_mark("opt_in", "needs a person at the radio: run with --manual -s" +
                                      (", or --microphone to listen with it" if marked(item, "by_ear") else "")))


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
    if request.node.get_closest_marker("radio_tx") is not None and "radio" in request.fixturenames:
        need_radio_tx(request.getfixturevalue("radio"))


@pytest.fixture(autouse=True)
def restore_radio(request):
    """Every test that uses the radio leaves it as it found it (channel / VFO, zone, power, squelch, monitor), and runs
    with every transmission blocked unless marked radio_tx (Radio.tx_gate). A radio_tx test is blocked again after it,
    so nothing between tests (module fixtures, a module without the radio's serial link) can transmit."""
    # test_tcp_services.py's module fixture "net" has the radio on the network (its serial link closed) for the whole
    # module and puts it back on serial itself: nothing to snapshot over serial then. test_ppp_link.py's "ppp" has PPP
    # on the serial port, which a debug link frame there would end (ppp.md 4); its tests change nothing
    if "radio" not in request.fixturenames or "net" in request.fixturenames or "ppp" in request.fixturenames:
        yield
        return
    r = request.getfixturevalue("radio")
    may_transmit = request.node.get_closest_marker("radio_tx") is not None
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
