# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The crash log (docs/spec/crash-log.md): the view section, and on purpose crashes that must leave a record the next
start shows. The crash tests restart the radio: opt-in with --allow-reboot (marker reboot)."""
import time

import pytest

from rfoundry_hil import DebugLinkError, as_int, skip

CRASH_KIND_FAULT, CRASH_KIND_ASSERT, CRASH_KIND_HANG = 1, 4, 5


def _need(radio):
    if "CRASH_LOG" not in radio.features():
        skip("not_built", "built without CONFIG_CRASH_LOG")


def _crash(radio, kind):
    """Crash on purpose, wait for the restart, return the crash view."""
    radio.link.crash_clear()
    assert radio.view(["crash"]).get("crash.valid") == "0"
    up_before = as_int(radio.view(["link"])["uptime_ms"])
    radio.link.crash_test(kind)
    radio.link.close()
    time.sleep(2.0)
    radio.reconnect(30.0, "the crash")
    up_after = as_int(radio.view(["link"])["uptime_ms"])
    assert up_after < up_before, f"the radio didn't restart (uptime {up_before} ms, then {up_after})"
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
    tasks_built = radio.view(["tasks"]).get("tasks.built") != "0"
    v = _crash(radio, "hang")
    assert v.get("crash.valid") == "1", "no hang record: the watchdog reset the radio first " + str(v)
    assert as_int(v["crash.kind"]) == CRASH_KIND_HANG, v
    ms = as_int(v["crash.cause"])
    assert timeout * 3 // 4 <= ms < timeout, (ms, timeout)   # three quarters of the timeout, before the reset
    assert v.get("crash.task") == "host", v       # the spinning task, not the tick's
    assert as_int(v["crash.pc"], 0) != 0, v
    # The other tasks (crash-log.md 1.2): not host (running) nor IDLE, each with a saved context that could be read.
    # The UI and radio tasks wait for work, so at least one entry is a kernel wait
    if tasks_built:
        n = as_int(v["crash.tasks"])
        others = dict(v[f"crash.t{k}"].rsplit(",", 1) for k in range(n))
        # The record has room for a few (crash-log.md 1.2): a radio with more tasks lists the first and says so
        more = as_int(v.get("crash.tasks_more"), 0) != 0
        assert n >= 2 and all(t in others or more for t in ("ui", "radi")), v
        assert "host" not in others and "IDLE" not in others, v
        where = [int(w, 16) for w in others.values()]
        assert all(where) and any(w & 1 for w in where), others
    radio.link.crash_clear()
