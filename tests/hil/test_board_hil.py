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
    def __init__(self, lines):
        self.lines = lines

    def diag(self):
        return None, self.lines


class FakeRadio:
    def __init__(self, name, lines, features=("DMR",), display="1x1x1"):
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
    full = [b for b in sorted(odr_hil.board_names()) if set(odr_hil.board_hil(b)) == set(odr_hil.HIL_KEYS)
            and odr_hil.board_hil(b)["calibration"]]
    assert full, "no board has every [hil] key: nothing to check the checks against"
    b = full[0]
    hil = odr_hil.board_hil(b)
    name = sorted(odr_hil.board_names()[b])[0]
    low, high = hil["battery_mv"]
    feats = set(hil["features"]) | {"DMR"}
    good = FakeRadio(name, [f"flash {hil['flash_jedec'][0]} cal factory", "txc 131", "pa 900 1000 1100 1200",
                            f"bat 2000 {(low + high) // 2}", f"codec {hil['codec'][0]}"],
                     features=feats, display=hil["display"])
    checks = (h.test_display, h.test_build_features, h.test_data_flash, h.test_calibration, h.test_battery,
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
