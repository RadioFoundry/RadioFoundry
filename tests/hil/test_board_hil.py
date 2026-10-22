# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The HIL tests' board data without a radio (odr_hil.board_names, board_id, board_hil): every board's VERSION name
maps back to it, and every expectation names a board and a known key."""
import os

import odr_hil


def test_every_board_name_maps_back():
    names = odr_hil.board_names()
    assert names, "no boards found under boards/"
    for b, ns in names.items():
        assert ns, f"boards/{b}: no board_t .name or BOARD_NAME string"
        for n in ns:
            assert odr_hil.board_id(n) == b, n
    assert odr_hil.board_id("no such radio") is None


def test_expectations_name_a_board_and_known_keys():
    for b, hil in odr_hil._HIL_UNTIL_REGISTRY.items():
        assert os.path.exists(os.path.join(odr_hil.ROOT, "boards", b, "board.mk")), b
        assert set(hil) <= set(odr_hil.HIL_KEYS), (b, set(hil) - set(odr_hil.HIL_KEYS))
        assert odr_hil.board_hil(b) == {**hil, **odr_hil.board_hil(b)}
    assert odr_hil.board_hil(None) == {}


class FakeLink:
    def __init__(self, lines):
        self.lines = lines

    def diag(self):
        return None, self.lines


class FakeRadio:
    def __init__(self, name, lines, features=("HOSTLINK", "DEBUGLINK", "DMR"), display="240x320x4"):
        self.link, self.name, self.feats, self.display = FakeLink(lines), name, set(features), display

    def version(self, fresh=False):
        return {"board": self.name, "display": self.display}

    def features(self):
        return self.feats


def outcome(fn, radio):
    """passed, failed or the skip kind of one test_health check run against a fake radio."""
    import test_health
    try:
        fn(test_health.Board(radio))
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
    name = sorted(odr_hil.board_names()["dm32"])[0]
    good = FakeRadio(name, ["flash C84018 cal factory", "txc 131", "pa 900 1000 1100 1200", "bat 2000 7400",
                            "codec 049"])
    for fn in (h.test_display, h.test_build_features, h.test_data_flash, h.test_calibration, h.test_battery,
               h.test_codec_present):
        assert outcome(fn, good) == "passed", fn.__name__
    bad = FakeRadio(name, ["flash EF4018 cal none", "txc 128", "pa 1408 1408 1408 1408", "bat 2000 3000",
                           "codec 041"], features=("USB", "DMR"), display="160x128x1")
    for fn in (h.test_display, h.test_build_features, h.test_data_flash, h.test_calibration, h.test_battery,
               h.test_codec_present):
        assert outcome(fn, bad) == "failed", fn.__name__
    # a board without expectations: nothing to compare (other_board); a line the firmware doesn't print: not_built
    other = FakeRadio("no such radio", [])
    assert outcome(h.test_display, other) == "other_board"
    assert outcome(h.test_receiver_alive, other) == "not_built"
