# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The PA key-up calibration (tools/bench/pa_cal.py) against a simulated PA loop, no radio and no bench: tools/ci.sh
runs it. The rule in Python gives the firmware's own figures (tests/host/paKeyupTest.c), the fit finds a loop's
constants from its open-loop times, and a whole band's run ends with every level on time."""
import random
import types

import pytest

import pa_cal

DFLT = {"dead_pm": 900, "knee_us": 120, "slope_us": 250, "window_us": 1500, "settle_us": 300, "pa_us": 0xFFFF}


def test_plan_is_the_firmwares():
    k = pa_cal.plan(DFLT, 700, 1448, 760, 4095)
    assert (k["boost"], k["drive"], k["boost_us"], k["pa_us"], k["drive_us"]) == (764, 700, 1500, 1500, 300)
    c = dict(DFLT, knee_us=2000)
    assert pa_cal.plan(c, 767, 1448, 760, 4095)["boost"] == 1737        # 120 % of the top drive
    assert pa_cal.plan(c, 767, 1448, 760, 1500)["boost"] == 1500        # the board's limit
    c = dict(DFLT, pa_us=400)
    k = pa_cal.plan(c, 767, 1448, 760, 4095)
    assert (k["boost"], k["boost_us"], k["pa_us"], k["drive_us"]) == (767, 1354, 400, 400)
    k = pa_cal.plan(c, 1448, 1448, 760, 4095)
    assert (k["boost"], k["pa_us"]) == (0, 370)
    k = pa_cal.plan(dict(DFLT, pa_us=200), 700, 1448, 760, 4095)
    assert (k["boost"], k["pa_us"], k["drive_us"]) == (764, 200, 200)
    k = pa_cal.plan(DFLT, 600, 1448, 760, 4095)                         # in the dead zone
    assert (k["boost"], k["pa_us"]) == (0, 1500)
    fixed = {"dead_pm": 0, "knee_us": 0, "slope_us": 936, "window_us": 936, "settle_us": 0, "pa_us": 936}
    for d in (100, 880, 1725):
        k = pa_cal.plan(fixed, d, 1725, 870, 4095)
        assert (k["boost"], k["pa_us"]) == (0, 936)


def test_grid():
    g = pa_cal.grid([955, 1300, 2615], 2615, 6)
    assert g[0] == 955 and g[-1] == 2615 and 1300 in g and len(g) == 6 and g == sorted(set(g))
    g = pa_cal.grid([500, 500], 500, 6)                                 # levels on one drive: drives under it
    assert g[0] == 300 and g[-1] == 500 and len(g) == 6
    assert pa_cal.grid([0, 0], 500, 6) == []
    assert max(pa_cal.grid([400, 2000], 1000, 6)) <= 1000               # a level over the limit is left out


class Loop:
    """A PA power loop: the output is up once the drive over the dead zone has added up to q(d). A boost charges at
    `eff` of what the law says, which the open-loop times can't show."""

    def __init__(self, dead, knee, slope, top, eff=1.0, noise=0.0, seed=1):
        self.dead, self.knee, self.slope, self.top, self.eff, self.noise = dead, knee, slope, top, eff, noise
        self.rnd = random.Random(seed)

    def rise_us(self, k):
        """The -3 dB point against the first symbol for a key-up plan."""
        d = k["drive"]
        q = self.knee * (self.top - self.dead) + self.slope * (d - self.dead)
        if d <= self.dead:
            return None
        if k["boost"] > d:
            room = k["boost_us"] - k["drive_us"]
            got = (k["boost"] - self.dead) * room * self.eff
            t = q / ((k["boost"] - self.dead) * self.eff) if got >= q else room + (q - got) / (d - self.dead)
            start = k["boost_us"]
        else:
            t, start = q / (d - self.dead), k["boost_us"] if k["boost"] else k["pa_us"]
        return -start + t + pa_cal.TARGET_US + self.rnd.gauss(0, self.noise)


@pytest.mark.parametrize("dead,knee,slope,top,ref,levels", [
    (700, 130, 280, 2615, 955, [955, 1300, 1900, 2615]),        # a slow loop, a wide table
    (1250, 60, 400, 1900, 1400, [1400, 1500, 1700, 1900]),      # a dead zone close under the lowest level
    (0, 300, 200, 3000, 0, [600, 1200, 3000]),                  # no anchor: no dead zone to find
])
def test_fit_finds_the_loop(dead, knee, slope, top, ref, levels):
    loop = Loop(dead, knee, slope, top)
    ahead = 2500
    openc = {"dead_pm": 0, "knee_us": 0, "slope_us": ahead, "window_us": ahead, "settle_us": 0, "pa_us": 0xFFFF}
    pts = [(d, ahead + loop.rise_us(pa_cal.plan(openc, d, top, ref, 4095)) - pa_cal.TARGET_US)
           for d in pa_cal.grid(levels, top, 6)]
    pm, k, s, rms = pa_cal.fit(pts, top, ref)
    assert rms < 5
    assert abs(ref * pm // 1000 - dead) <= max(ref // 200, 2)
    assert abs(k - knee) <= 0.03 * knee + 3 and abs(s - slope) <= 0.03 * slope + 5


def test_fit_without_a_knee():
    # A loop that follows the drive: the same time at every level, so one lead and no boost
    pm, k, s, _ = pa_cal.fit([(500, 610.0), (900, 640.0), (1400, 600.0), (2000, 630.0)], 2000, 500)
    assert (pm, k, s) == (0, 0, 640)
    c = {"dead_pm": pm, "knee_us": k, "slope_us": s, "window_us": 1500, "settle_us": 300, "pa_us": 0xFFFF}
    assert pa_cal.plan(c, 500, 2000, 500, 4095) == {"boost": 0, "drive": 500, "boost_us": 0, "pa_us": 640, "drive_us": 0}


class FakeRadio:
    def __init__(self, loop, drives, top, ref, board, ahead, max_drive):
        self.loop, self.drives, self.top, self.ref, self.board = loop, drives, top, ref, board
        self.ahead, self.max_drive = ahead, max_drive
        self.c, self.level, self.drive, self.last, self.restored = dict(board), 0, 0, None, False
        self.link = types.SimpleNamespace(ptt_for=lambda s: None)

    def cli(self, cmd):
        pass

    def tune(self, mhz):
        pass

    ints = pa_cal.Radio.ints

    def view(self):
        k = self.last or {"boost": 0, "drive": 0, "boost_us": 0, "pa_us": 0, "drive_us": 0}
        return {"pa_keyup": "/".join(str(self.c[n]) for n in pa_cal.NAMES),
                "pa_keyup.limits": f"{self.ahead}/{self.max_drive}", "tx_drive": "/".join(map(str, self.drives)),
                "pa_keyup.last": "/".join(str(k[n]) for n in ("boost", "drive", "boost_us", "pa_us", "drive_us"))
                                 + f"/{self.top}/{self.ref}"}

    def setup(self, c, level, drive=0):
        assert c is None or (c["window_us"] <= self.ahead and (c["settle_us"] < c["window_us"] or not c["window_us"]))
        assert drive <= self.top, "never over the radio's own top level"
        self.c, self.level, self.drive = dict(c or self.board), level, drive

    def restore(self):
        self.restored = True

    def keyed(self):
        self.last = pa_cal.plan(self.c, self.drive or self.drives[self.level], self.top, self.ref, self.max_drive)
        return self.last


class FakeBench:
    def __init__(self, radio):
        self.radio = radio

    def open(self, mhz):
        pass

    def close(self):
        pass

    def measure(self, mhz, key):
        key()
        k = self.radio.keyed()
        r = self.radio.loop.rise_us(k)
        over = 4.0 if k["boost"] > k["drive"] * 2 and self.radio.c["settle_us"] < 200 else 0.5
        return {"n": 0 if r is None else 8, "rise_us": r, "over_db": over, "sym1_db": -0.2, "settled_dbfs": -20.0,
                "dbm": -20.0, "clip_pct": 0, "peak_dbfs": -18.0, "edge_res_us": 25}


def run_band(loop, drives, top, ref, board, ahead=2500, max_drive=4095, **opts):
    a = types.SimpleNamespace(levels="all", plot="", before=None, channel=False, refit=True, batch=0, key_s=0.5, max_in_dbm=-13.0, pad_db=57.0, window_us=0, settle_us=None,
                              pa_us=None, max_pct=100, points=6, rounds=4, confirm=True, cc=1, ts=1)
    a.__dict__.update(opts)
    cal = pa_cal.Cal.__new__(pa_cal.Cal)
    cal.a, cal.rows, cal.keys_n, cal.say = a, [], 0, lambda s: None
    cal.radio = FakeRadio(loop, drives, top, ref, board, ahead, max_drive)
    cal.bench = FakeBench(cal.radio)
    r = cal.band(446.05)
    cal.end()
    assert cal.radio.restored
    return r, cal


@pytest.mark.parametrize("eff,noise", [(1.0, 0.0), (0.85, 0.0), (1.1, 15.0)])
def test_a_band_ends_on_time(eff, noise):
    # Constants far from the loop's, a boost that charges a little off the law, and a little measurement noise
    loop = Loop(700, 250, 280, 2615, eff=eff, noise=noise)
    r, cal = run_band(loop, [955, 1300, 1900, 2615], 2615, 955, DFLT)
    assert r["ok"], r
    for x in r["levels"].values():
        boosted = x["plan"]["boost"] > x["drive"]
        assert (-1500 if boosted else pa_cal.EARLY_MAX_US) <= x["rise_us"] <= 0 and not x["bad"], r
    assert r["levels"][0]["plan"]["boost"] > 955                # the lowest level is too slow for the window: boosted
    assert r["levels"][3]["plan"]["boost"] == 0                 # the top one is not
    assert cal.keys_n <= 1 + 6 + 1 + 5 * 4 + 4                        # check, open loop, at most four verifies, confirm
    assert r["levels"][0]["rise_us"] <= -pa_cal.TOL_US        # a boosted level: early, by a margin


def test_a_band_without_a_knee_gets_one_lead():
    loop = Loop(0, 0, 620, 2000)
    fixed = {"dead_pm": 0, "knee_us": 0, "slope_us": 1000, "window_us": 1000, "settle_us": 0, "pa_us": 1000}
    r, cal = run_band(loop, [400, 900, 2000], 2000, 400, fixed, ahead=1250)
    c = r["constants"]
    assert r["ok"] and c["knee_us"] == 0 and c["dead_pm"] == 0 and abs(c["slope_us"] - 620) <= 5, r
    assert all(x["plan"]["boost"] in (0, x["drive"]) for x in r["levels"].values())


def test_a_dead_path_stops():
    loop = Loop(5000, 100, 100, 2000)                           # nothing comes up: no bursts
    with pytest.raises(pa_cal.Stop):
        run_band(loop, [400, 900, 2000], 2000, 400, DFLT)


def test_judge_own_overshoot():
    m = {"rise_us": -250, "over_db": 4.9, "sym1_db": -0.5}
    assert "overshoot" in pa_cal.judge(m)                               # the key-up's
    assert pa_cal.judge(m, False, 5.0) == []                            # the drive's own: noted, not counted
    assert "overshoot" in pa_cal.judge(dict(m, over_db=6.5), True, 5.0)  # a boost added to it
