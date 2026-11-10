# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""What keeps one run like the next (README.md, "The rig's configuration" and "Restored after every test"): a
board's table in the rig's file gives every run of that radio the same values, and a hardware option a restart reset
is set back. No radio: these run anywhere, and tools/ci.sh runs them."""
import types

import pytest

import hil_config
import rfoundry_hil as H


class FakeConfig:
    def __init__(self, path, board=None, **given):
        names = [hil_config.key(n) for n in list(hil_config.RIG) + list(hil_config.TX_OPTIONS)]
        self.option = types.SimpleNamespace(**dict.fromkeys(names), bench_api=None, radio_port=None,
                                            allow_reboot=False)
        for k, v in given.items():
            setattr(self.option, k, v)
        self._opts = {"--hil-config": str(path), "--hil-board": board, "--allow-rf": False}

    def getoption(self, name):
        if name in self._opts:
            return self._opts[name]
        return getattr(self.option, hil_config.key(name.lstrip("-")))


def rig_file(tmp_path, text):
    p = tmp_path / "hil.toml"
    p.write_text(text)
    return p


BOARDS = '''
dmr_freq = 446.075
callsign = "Q0TST"

[board.radio_a]
dmr_freq = 461.4625
bench_api = "bench-a"
'''


def test_board_table_applies_to_its_board_only(tmp_path):
    path = rig_file(tmp_path, BOARDS)
    c = FakeConfig(path, "radio_a")
    hil_config.configure(c)
    assert c.option.dmr_freq == 461.4625 and c.option.bench_api == "bench-a" and c.option.callsign == "Q0TST"
    c = FakeConfig(path, "radio_b")
    hil_config.configure(c)
    assert c.option.dmr_freq == 446.075 and c.option.bench_api is None
    c = FakeConfig(path)
    hil_config.configure(c)
    assert c.option.dmr_freq == 446.075


def test_command_line_beats_the_board_table(tmp_path):
    c = FakeConfig(rig_file(tmp_path, BOARDS), "radio_a", dmr_freq=446.1, bench_api="bench-b")
    hil_config.configure(c)
    assert c.option.dmr_freq == 446.1 and c.option.bench_api == "bench-b"


@pytest.mark.parametrize("line", ["allow_reboot = true", "radio_port = \"/dev/x\"", "no_such_option = 1"])
def test_board_table_refuses_flags_ports_and_unknown_keys(tmp_path, line):
    c = FakeConfig(rig_file(tmp_path, f"[board.radio_a]\n{line}\n"), "radio_a")
    with pytest.raises(pytest.UsageError):
        hil_config.configure(c)


class FakeLink:
    def __init__(self, opts):
        self.opts = opts
        self.set = []

    def hwopts(self):
        return [dict(o) for o in self.opts]

    def hwopt_set(self, i, v):
        self.set.append((i, v))
        self.opts[i]["value"] = v


def radio_with(opts):
    r = object.__new__(H.Radio)
    r.link = FakeLink(opts)
    return r


def test_a_reset_hardware_option_is_set_back():
    r = radio_with([{"name": "Board revision", "value": 0, "restart": True},
                    {"name": "Transmit (bring-up)", "value": 1, "restart": False},
                    {"name": "Nothing stored", "value": -1, "restart": False}])
    r.hwopt_baseline()
    # a restart: the power-cycle option back to its default, the restart one changed, the unstored one set
    r.link.opts[0]["value"], r.link.opts[1]["value"], r.link.opts[2]["value"] = 1, 0, 0
    r.hwopts_back()
    assert r.link.set == [(1, 1)]
    r.hwopts_back()
    assert r.link.set == [(1, 1)]       # nothing left to set


def test_no_hardware_options_no_requests():
    r = radio_with([])
    r.hwopt_baseline()
    r.hwopts_back()
    assert r.link.set == []


def test_reconnect_sets_the_options_back(monkeypatch):
    """A test that restarts the radio and keys at once (test_dmr_repeater_slots boot) needs the arm before teardown"""
    r = radio_with([{"name": "Transmit (bring-up)", "value": 1, "restart": False}])
    r.hwopt_baseline()
    fresh = FakeLink([{"name": "Transmit (bring-up)", "value": 0, "restart": False}])
    fresh.ping = lambda: None
    fresh.close = lambda: None
    r.link.close = lambda: None
    r.port, r.pinned, r.uid = "/dev/radio", True, None
    monkeypatch.setattr(H, "DebugLink", lambda port, timeout: fresh)
    r.reconnect(timeout=1.0)
    assert r.link is fresh and fresh.set == [(0, 1)]
