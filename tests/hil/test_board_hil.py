# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The HIL tests' board data without a radio (odr_hil.board_names, board_id, board_hil): every board's VERSION name
maps back to it through the board registry, and the health checks pass, fail and skip on a fake radio built from a
board's own [hil] table."""
import odr_hil


def test_every_board_name_maps_back():
    names = odr_hil.board_names()
    assert names, "no boards in the registry"
    for b, ns in names.items():
        assert ns, f"boards/{b}: no debug link name"
        for n in ns:
            assert odr_hil.board_id(n) == b, n
    assert odr_hil.board_id("no such radio") is None


def test_expectations_use_known_keys():
    for b in odr_hil.board_names():
        hil = odr_hil.board_hil(b)
        assert set(hil) <= set(odr_hil.HIL_KEYS), (b, set(hil) - set(odr_hil.HIL_KEYS))
    assert odr_hil.board_hil(None) == {}


class FakeLink:
    def __init__(self, lines, view=None):
        self.lines, self.features = lines, view or {"battery": "ok"}

    def diag(self):
        return None, self.lines

    def view(self, sections=None):
        return dict(self.features)


class FakeRadio:
    def __init__(self, name, lines, features=("DMR",), display="1x1x1", view=None):
        self.link, self.name, self.feats, self.display = FakeLink(lines, view), name, set(features), display

    def version(self, fresh=False):
        return {"board": self.name, "display": self.display}

    def features(self):
        return self.feats


def outcome(fn, radio):
    """passed, failed or the skip kind of one test_health check run against a fake radio."""
    import test_health
    return result(lambda: fn(test_health.Board(radio)))


def result(call):
    """passed, failed or the skip kind of call()."""
    try:
        call()
    except AssertionError:
        return "failed"
    except BaseException as e:      # a skip (pytest's skip exception is not an Exception)
        kind = odr_hil.skip_kind(e)
        if kind is None:
            raise
        return kind
    return "passed"


def test_health_checks_read_the_expectations():
    import test_health as h
    # supply_mv is the one key no board has yet (no radio measures its supply): checked on a fake below
    full = [b for b in sorted(odr_hil.board_names())
            if set(odr_hil.HIL_KEYS) - set(odr_hil.board_hil(b)) <= {"supply_mv"} and odr_hil.board_hil(b)["calibration"]]
    assert full, "no board has every [hil] key: nothing to check the checks against"
    b = full[0]
    hil = odr_hil.board_hil(b)
    name = sorted(odr_hil.board_names()[b])[0]
    low, high = hil["battery_mv"]
    feats = set(hil["features"]) | {"DMR"}
    good = FakeRadio(name, [f"flash {hil['flash_jedec'][0]} cal factory", "txc 131", "pa 900 1000 1100 1200",
                            f"bat 2000 {(low + high) // 2}", f"codec {hil['codec'][0]}"],
                     features=feats, display=hil["display"])
    checks = (h.test_display, h.test_build_features, h.test_data_flash, h.test_calibration, h.test_voltage,
              h.test_codec_present)
    for fn in checks:
        assert outcome(fn, good) == "passed", fn.__name__
    bad = FakeRadio(name, ["flash 000000 cal none", "txc 128", "pa 1408 1408 1408 1408", f"bat 2000 {high + 1}",
                           "codec 999"], features=set(hil["no_features"]) | {"DMR"}, display="1x1x1")
    for fn in checks:
        assert outcome(fn, bad) == "failed", fn.__name__
    # a board without expectations: nothing to compare (other_board); a line the firmware doesn't print: not_built
    other = FakeRadio("no such radio", [])
    assert outcome(h.test_display, other) == "other_board"
    assert outcome(h.test_receiver_alive, other) == "not_built"


def test_voltage_check_battery_supply_none(monkeypatch):
    """test_voltage on a radio that measures its supply (supply_mv in the features section), one that measures
    neither (no_hw), and a supply reading on a board without a supply_mv expectation (other_board)."""
    import test_health as h
    b = sorted(odr_hil.board_names())[0]
    name = sorted(odr_hil.board_names()[b])[0]
    hil = dict(odr_hil.board_hil(b), supply_mv=[11000, 16000])
    monkeypatch.setattr(h, "board_hil", lambda bid: hil if bid == b else {})
    supply = {"battery": "none", "supply_mv": "13800"}
    assert outcome(h.test_voltage, FakeRadio(name, [], view=supply)) == "passed"
    assert outcome(h.test_voltage, FakeRadio(name, [], view=dict(supply, supply_mv="3000"))) == "failed"
    assert outcome(h.test_voltage, FakeRadio(name, [], view={"battery": "none"})) == "no_hw"
    monkeypatch.setattr(h, "board_hil", lambda bid: {})
    assert outcome(h.test_voltage, FakeRadio(name, [], view=supply)) == "other_board"


def test_board_tests_run_on_their_own_board_only():
    import hil_plugin
    root = odr_hil.ROOT
    assert hil_plugin.own_board(f"{root}/boards/x1/hil/test_bus.py") == "x1"
    assert hil_plugin.own_board(f"{root}/tests/hil/test_health.py") is None
    assert hil_plugin.own_board(f"{root}/boards/x1/tests/test_bus.py") is None
    b = sorted(odr_hil.board_names())[0]
    mine = FakeRadio(sorted(odr_hil.board_names()[b])[0], [])
    assert result(lambda: hil_plugin.only_on(b, mine)) == "passed"
    assert result(lambda: hil_plugin.only_on(b, FakeRadio("no such radio", []))) == "other_board"
