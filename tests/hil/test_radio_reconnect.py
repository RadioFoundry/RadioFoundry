# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Radio.reconnect and Radio.serial_back, which tests use after a restart or a USB personality switch. No radio: these run anywhere, and
tools/ci.sh runs them. On a bench with several radios, a reconnect that took any port found by USB ID sent the rest of
the run (watchdog resets, a reboot, crash tests) to another session's radio."""
from contextlib import nullcontext

import pytest

import rfoundry_hil
from rfoundry_hil import DebugLinkError, Radio


class FakeLink:
    """A debug link on one of the fake ports: uids maps a port to its chip UID; a missing port doesn't answer."""
    uids = {}
    opened = []
    closed = []
    booting = set()             # ports whose chip can't give its UID yet (an HR_C7000 just after boot)

    def __init__(self, port, timeout=None):
        FakeLink.opened.append(port)
        if port not in FakeLink.uids:
            raise DebugLinkError(f"{port}: no answer")
        self.port = port

    def ping(self):
        return True

    def view(self, sections):
        assert sections == ["link"]
        return {}               # older firmware does not report ui.draws

    def chip(self):
        if self.port in FakeLink.booting:
            raise DebugLinkError("INFO_CHIP: not ready")
        return {"uid": FakeLink.uids[self.port]}

    def close(self):
        FakeLink.closed.append(self)


@pytest.fixture
def bench(monkeypatch):
    """Ours on A (uid 1), another radio on B (uid 2); find_ports lists both"""
    FakeLink.uids = {"A": (1, 0, 0), "B": (2, 0, 0)}
    FakeLink.opened = []
    FakeLink.closed = []
    FakeLink.booting = set()
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


def test_reconnect_closes_the_old_link(bench):
    r = Radio("A")
    old = r.link
    r.reconnect(timeout=1)
    assert old in bench.closed and r.link is not old


def test_reconnect_waits_for_first_draw(bench, monkeypatch):
    draws = iter(["0", "0", "1"])

    def view(link, sections):
        assert sections == ["link"]
        return {"ui.draws": next(draws)}

    monkeypatch.setattr(bench, "view", view)
    Radio("A").reconnect(timeout=1)
    assert next(draws, None) is None


@pytest.mark.parametrize("reopen,interrupted", [(False, False), (True, False), (True, True)],
                         ids=["same-link", "reopened-link", "interrupted-after-reopen"])
@pytest.mark.parametrize("rx_tone,tx_tone", [("dcs:23", "ctcss:1000"), ("none:0", "none:0")])
def test_vfo_session_restores_the_frequency(bench, monkeypatch, reopen, interrupted, rx_tone, tx_tone):
    saved = {"vfo": "1", "mode": "dmr", "rx_hz": "446500000", "ch.tx_tone": tx_tone, "ch.rx_tone": rx_tone}
    state = saved.copy()

    class VfoLink(FakeLink):
        def version(self):
            return {"features": "VFO"}

        def view(self, sections):
            return super().view(sections) if sections == ["link"] else state

        def write(self, key, value):
            assert self not in bench.closed, f"{key} write on a closed link"
            state[key] = value

        def vfo_set(self, hz):
            self.write("rx_hz", str(hz))

        def dmr_fields(self, mode):
            self.write("mode", mode)

        def tone(self, which, kind, value):
            key = {1: "ch.tx_tone", 2: "ch.rx_tone"}[which]
            self.write(key, f"{kind}:{value}")

        def vfo(self, on):
            self.write("vfo", "1" if on else "0")

    monkeypatch.setattr(rfoundry_hil, "DebugLink", VfoLink)
    r = Radio("A")
    with pytest.raises(RuntimeError, match="interrupted") if interrupted else nullcontext():
        with r.vfo_session() as hz:
            assert state["mode"] == "fm" and state["ch.tx_tone"] == state["ch.rx_tone"] == "none:0"
            r.link.vfo_set(hz + 12500)
            r.link.tone(2, "ctcss", 1000)
            r.link.vfo(False)
            if reopen:
                r.reconnect(timeout=1)
            if interrupted:
                raise RuntimeError("interrupted")
    assert state == saved


def test_another_radio_is_asked_once(bench):
    r = Radio("A")
    r.pinned = False
    _gone(bench, "A")
    bench.opened = []
    with pytest.raises(AssertionError):
        r.reconnect(timeout=0.05)       # many passes: sleep is a no-op here
    assert bench.opened.count("B") == 1, bench.opened


def test_own_port_while_the_chip_cant_say(bench):
    r = Radio("A")
    r.pinned = False
    bench.booting.add("A")
    r.reconnect(timeout=1)
    assert r.port == "A"
    bench.booting.add("B")              # another port that can't say is never taken
    _gone(bench, "A")
    with pytest.raises(AssertionError):
        r.reconnect(timeout=0.01)


def test_pinned_port_with_another_chip(bench):
    r = Radio("A")
    bench.uids["A"] = (2, 0, 0)         # e.g. /dev/ttyACM0 handed to another radio
    with pytest.raises(AssertionError):
        r.reconnect(timeout=0.01)


def test_released_finds_this_radio_after_a_rename(bench, monkeypatch):
    r = Radio("A")
    r.pinned = False
    with r.released():
        bench.uids["A"] = (2, 0, 0)     # the body never reopened it; the old name is another radio's now
        bench.uids["C"] = (1, 0, 0)
        monkeypatch.setattr(rfoundry_hil, "find_ports", lambda: ["A", "C"])
    assert r.port == "C" and r.link.port == "C"
