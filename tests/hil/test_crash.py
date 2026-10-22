# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The crash log (docs/spec/crash-log.md): the view section, and on purpose crashes that must leave a record the next
start shows. The crash tests restart the radio: opt-in with --allow-reboot (marker reboot)."""
import time

import pytest

from odr_hil import DebugLink, DebugLinkError, as_int, find_ports, skip

CRASH_KIND_FAULT, CRASH_KIND_ASSERT = 1, 4


def _need(radio):
    if "CRASH_LOG" not in radio.features():
        skip("not_built", "built without CONFIG_CRASH_LOG")


def _reconnect(radio, timeout=30.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for port in [radio.port] + find_ports():
            try:
                radio.link = DebugLink(port, timeout=1.5)
                radio.link.ping()
                radio.port = port
                return
            except (DebugLinkError, OSError):
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
