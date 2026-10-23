# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The crash log (docs/spec/crash-log.md): the view section, and on purpose crashes that must leave a record the next
start shows. The crash tests restart the radio: opt-in with --allow-reboot (marker reboot)."""
import time

import pytest

from odr_hil import DebugLink, DebugLinkError, as_int, find_ports, skip

CRASH_KIND_FAULT, CRASH_KIND_ASSERT, CRASH_KIND_HANG = 1, 4, 5


def _need(radio):
    if "CRASH_LOG" not in radio.features():
        skip("not_built", "built without CONFIG_CRASH_LOG")


def _reconnect(radio, timeout=30.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for port in [radio.port] + find_ports():
            link = None
            try:
                link = DebugLink(port, timeout=1.5)
                link.ping()
                radio.link, radio.port = link, port
                return
            except (DebugLinkError, OSError):
                # A serial hub takes one client per port: a link left open would lock out every later try.
                if link is not None:
                    link.close()
                time.sleep(0.5)
    raise AssertionError("the radio did not come back after the crash")


def _crash(radio, kind):
    """Crash on purpose, wait for the restart, return the crash view."""
    radio.link.crash_clear()
    assert radio.view(["crash"]).get("crash.valid") == "0"
    up_before = as_int(radio.view(["link"])["uptime_ms"])
    radio.link.crash_test(kind)
    radio.link.close()
    time.sleep(2.0)
    _reconnect(radio)
    up_after = as_int(radio.view(["link"])["uptime_ms"])
    assert up_after < up_before or up_after < 30000, "the radio didn't restart"
    return radio.view(["crash"])


@pytest.mark.feature("crash_log")
def test_crash_view(radio):
    _need(radio)
    v = radio.view(["crash"])
    assert v.get("crash.built") == "1", v
    assert v.get("crash.valid") in ("0", "1"), v


@pytest.mark.feature("crash_log")
def test_crash_test_refused_without_token(radio):
    _need(radio)
    with pytest.raises(DebugLinkError) as e:
        radio.link.request(0x1B, bytes([1, 1, 0, 0, 0, 0]))
    assert e.value.status == 7     # refused: no crash without the token


@pytest.mark.feature("crash_log")
@pytest.mark.reboot
def test_crash_fault_kept(radio):
    """A trap instruction: a CPU exception, kept with the registers, and the radio restarts."""
    _need(radio)
    v = _crash(radio, "trap")
    assert v.get("crash.valid") == "1", "no record after the restart: the store didn't survive it " + str(v)
    assert as_int(v["crash.kind"]) == CRASH_KIND_FAULT, v
    assert as_int(v["crash.count"]) == 1, v
    assert as_int(v["crash.pc"], 0) != 0, v
    assert v.get("crash.task") == "host", v       # the debug link runs in the host link task
    assert as_int(v["crash.flags"]) & 0x04, v     # a task's stack is in RAM: its words were copied
    assert len(v.get("crash.stack", "").split(",")) == 16, v
    git = radio.version(fresh=True).get("git", "")
    if git[:8] and all(c in "0123456789abcdef" for c in git[:8]):
        assert v["crash.build"] == git[:8], (v, git)
    radio.link.crash_clear()


@pytest.mark.feature("crash_log")
@pytest.mark.reboot
def test_crash_assert_kept(radio):
    _need(radio)
    v = _crash(radio, "assert")
    assert v.get("crash.valid") == "1", v
    assert as_int(v["crash.kind"]) == CRASH_KIND_ASSERT, v
    assert as_int(v["crash.cause"]) > 0, v         # the line
    radio.link.crash_clear()
    assert radio.view(["crash"]).get("crash.valid") == "0"


@pytest.mark.feature("crash_log")
@pytest.mark.reboot
def test_crash_hang_kept(radio):
    """The hang check (crash-log.md 1.1): the host link's task spins with interrupts on, and the tick keeps a hang
    record of it before the watchdog would reset the radio."""
    _need(radio)
    wd = radio.view(["watchdog"])
    if wd.get("watchdog.built") != "1":
        skip("not_built", "built without CONFIG_WATCHDOG: nothing would end the hang")
    timeout = as_int(wd["watchdog.timeout_ms"])
    v = _crash(radio, "hang")
    assert v.get("crash.valid") == "1", "no hang record: the watchdog reset the radio first " + str(v)
    assert as_int(v["crash.kind"]) == CRASH_KIND_HANG, v
    ms = as_int(v["crash.cause"])
    assert timeout * 3 // 4 <= ms < timeout, (ms, timeout)   # three quarters of the timeout, before the reset
    assert v.get("crash.task") == "host", v       # the spinning task, not the tick's
    assert as_int(v["crash.pc"], 0) != 0, v
    radio.link.crash_clear()
