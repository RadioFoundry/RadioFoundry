# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""What keeps one run like the next (README.md, "The rig's configuration" and "Restored after every test"): a
board's table in the rig's file gives every run of that radio the same values. No radio: these run anywhere, and tools/ci.sh runs them."""
import sys
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


# reading a rig file needs tomllib (Python 3.11)
needs_toml = H.skip_if(sys.version_info < (3, 11), "bench", "reading the rig's file needs Python 3.11 or later")


@pytest.fixture(autouse=True)
def _run_config_kept(monkeypatch):
    """configure() makes its config the run's (hil_config.CONFIG): put the run's own back after each test, or every
    later test's transmission is checked against a fake config's tx_mhz (none) and refused"""
    monkeypatch.setattr(hil_config, "CONFIG", hil_config.CONFIG)


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


@needs_toml
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


@needs_toml
def test_command_line_beats_the_board_table(tmp_path):
    c = FakeConfig(rig_file(tmp_path, BOARDS), "radio_a", dmr_freq=446.1, bench_api="bench-b")
    hil_config.configure(c)
    assert c.option.dmr_freq == 446.1 and c.option.bench_api == "bench-b"


@needs_toml
@pytest.mark.parametrize("line", ["allow_reboot = true", "radio_port = \"/dev/x\"", "no_such_option = 1"])
def test_board_table_refuses_flags_ports_and_unknown_keys(tmp_path, line):
    c = FakeConfig(rig_file(tmp_path, f"[board.radio_a]\n{line}\n"), "radio_a")
    with pytest.raises(pytest.UsageError):
        hil_config.configure(c)
