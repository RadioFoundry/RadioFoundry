# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Radio.reconnect and Radio.serial_back, which tests use after a restart or a USB personality switch. No radio: these run anywhere, and
tools/ci.sh runs them. On a bench with several radios, a reconnect that took any port found by USB ID sent the rest of
the run (watchdog resets, a reboot, crash tests) to another session's radio."""
import pytest

import rfoundry_hil
from rfoundry_hil import DebugLinkError, Radio


class FakeLink:
    """A debug link on one of the fake ports: uids maps a port to its chip UID; a missing port doesn't answer."""
    uids = {}
    opened = []

    def __init__(self, port, timeout=None):
        FakeLink.opened.append(port)
        if port not in FakeLink.uids:
            raise DebugLinkError(f"{port}: no answer")
        self.port = port

    def ping(self):
        return True

    def chip(self):
        return {"uid": FakeLink.uids[self.port]}

    def close(self):
        pass


@pytest.fixture
def bench(monkeypatch):
    """Ours on A (uid 1), another radio on B (uid 2); find_ports lists both"""
    FakeLink.uids = {"A": (1, 0, 0), "B": (2, 0, 0)}
    FakeLink.opened = []
    monkeypatch.setattr(rfoundry_hil, "DebugLink", FakeLink)
    monkeypatch.setattr(rfoundry_hil, "find_ports", lambda: ["B", "A"])
    monkeypatch.setattr(rfoundry_hil.time, "sleep", lambda s: None)
    return FakeLink


def _gone(bench, port):
    del bench.uids[port]


def test_pinned_port_only(bench):
    r = Radio("A")
    _gone(bench, "A")           # restarting: its port doesn't answer
    bench.opened = []
    with pytest.raises(AssertionError):
        r.reconnect(timeout=0.01)
    assert "B" not in bench.opened, bench.opened
    bench.uids["A"] = (1, 0, 0)
    r.reconnect(timeout=1)
    assert r.port == "A"


def test_found_by_usb_id_needs_the_same_chip(bench, monkeypatch):
    r = Radio("A")
    r.pinned = False            # as when the port was found by USB ID
    _gone(bench, "A")
    with pytest.raises(AssertionError):
        r.reconnect(timeout=0.01)       # B answers but is another radio
    assert r.port == "A"
    bench.uids["C"] = (1, 0, 0)         # ours, back on a new port
    monkeypatch.setattr(rfoundry_hil, "find_ports", lambda: ["B", "C"])
    r.reconnect(timeout=1)
    assert r.port == "C"


def test_found_by_usb_id_checks_its_old_port_too(bench):
    r = Radio("A")
    r.pinned = False
    bench.uids["A"] = (2, 0, 0)         # re-enumerated: another radio has our old port's name now
    _gone(bench, "B")
    with pytest.raises(AssertionError):
        r.reconnect(timeout=0.01)


def test_serial_back_only_on_this_radio(bench, monkeypatch):
    r = Radio("A")
    r.pinned = False
    _gone(bench, "A")                   # a network adapter now: only another radio's port is there
    assert not r.serial_back(0.01)
    bench.uids["C"] = (1, 0, 0)         # back as a serial port, under a new name
    monkeypatch.setattr(rfoundry_hil, "find_ports", lambda: ["B", "C"])
    assert r.serial_back(1)
    assert r.port == "C" and r.link.port == "C"


def test_released_keeps_a_link_reopened_inside(bench, monkeypatch):
    r = Radio("A")
    r.pinned = False
    with r.released():
        _gone(bench, "A")
        bench.uids["C"] = (1, 0, 0)
        monkeypatch.setattr(rfoundry_hil, "find_ports", lambda: ["B", "C"])
        assert r.serial_back(1)
        inside = r.link
        bench.opened = []
    assert r.link is inside and bench.opened == []
