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
    # the fastest approach: a level over it up on drive 836, then its own; the levels under it as before
    e = dict(DFLT, ease_pm=200)
    assert pa_cal.plan(e, 767, 1448, 760, 4095) == pa_cal.plan(DFLT, 767, 1448, 760, 4095)
    assert pa_cal.plan(e, 700, 1448, 760, 4095) == pa_cal.plan(DFLT, 700, 1448, 760, 4095)
    assert pa_cal.plan(e, 1023, 1448, 760, 4095) == {"boost": 836, "drive": 1023, "boost_us": 990, "pa_us": 990, "drive_us": 137}
    assert pa_cal.plan(dict(e, pa_us=400), 1448, 1448, 760, 4095) == {"boost": 836, "drive": 1448, "boost_us": 1053, "pa_us": 400, "drive_us": 200}
    assert pa_cal.plan(dict(e, knee_us=2000), 700, 1448, 760, 4095)["boost"] == 836    # a boost stops there too
    short = dict(e, window_us=600, settle_us=100)                       # the window is the boosts'
    assert pa_cal.plan(short, 1448, 1448, 760, 4095)["boost_us"] == 1053
    assert pa_cal.plan(short, 1448, 1448, 760, 4095, 800)["boost_us"] == 800    # the board's earliest point
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
        return {"pa_keyup": "/".join(str(self.c.get(n, 0)) for n in pa_cal.NAMES),
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
                              pa_us=None, ease_pm=None, max_pct=100, points=6, rounds=4, confirm=True, cc=1, ts=1)
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


def _ramp(rise3, rise6, on, sym1):
    return {"ramp": {"rise_ms": {"-3": rise3, "-6": rise6, "-20": on}, "overshoot_db": 0.0, "sym1_db": sym1,
                     "settled_db": -20.0}}


def test_a_creeping_level_is_timed_by_its_edge():
    a = types.SimpleNamespace(dbfs_to_dbm=None)
    # A plain edge: the -3 dB point, 0.06 ms after the -6 dB crossing
    m = pa_cal.summarize([_ramp(-0.25, -0.31, -0.45, -0.3)] * 3, a)
    assert not m["creep"] and m["rise_us"] == pytest.approx(-250)
    # Within 6 dB at once, the last 3 dB over a millisecond: the edge stands in, and the -3 dB point is kept beside it
    m = pa_cal.summarize([_ramp(0.55, -0.43, -0.62, -3.3)] * 3, a)
    assert m["creep"] and m["rise_us"] == pytest.approx(-430 + pa_cal.CREEP_US) and m["rise3_us"] == pytest.approx(550)
    assert pa_cal.judge(m) == []                                        # not late, and its first symbol is the creep's
    assert "sym1" in pa_cal.judge(dict(m, sym1_db=-5.0))                # but not any level
    assert "sym1" in pa_cal.judge(dict(m, creep=False))                 # and a plain edge that low is the key-up's


def test_judge_on_the_air_before_the_slot():
    m = {"rise_us": -540.0, "over_db": 0.0, "sym1_db": -2.5, "on_us": -1308.0}
    assert pa_cal.judge(m, True) == ["before-slot"]                     # boosted or not
    assert pa_cal.judge(dict(m, on_us=-1042.0), True) == []


def test_a_key_down_in_the_slot_after_fails_the_band():
    tail = lambda end_ms: {"t0_ms": -2.0, "dt_ms": 0.05, "db": [0.0 if -2.0 + i * 0.05 < end_ms else -50.0 for i in range(120)]}
    assert round(pa_cal.off_air_us(tail(0.5))) == 450                   # the last sample within 20 dB
    assert pa_cal.overrun({"off_us": pa_cal.off_air_us(tail(0.5))}) == []
    assert pa_cal.overrun({"off_us": pa_cal.off_air_us(tail(1.9))}) == ["after-slot"]
    assert pa_cal.off_air_us(None) is None and pa_cal.overrun({}) == []
    assert pa_cal.end_db(tail(0.5)) == 0.0 and pa_cal.overrun({"end_db": pa_cal.end_db(tail(-0.2))}) == ["cut-short"]
    noisy = tail(0.5)
    noisy["db"][100] = -12.0                                            # one point of noise 3 ms after the carrier
    assert round(pa_cal.off_air_us(noisy)) == 450
    assert round(pa_cal.off_air_us(tail(9.0))) == 3950                  # never left: the envelope's end


def test_a_weak_signals_key_down_is_judged_over_its_noise():
    # 16 dB over the noise: the carrier falls at +0.25 ms and the noise stays within 20 dB of the burst's level
    dt = 0.05
    db = [0.0] * 45 + [-16.0 + (i % 3) for i in range(75)]
    weak = {"t0_ms": -2.0, "dt_ms": dt, "db": db}
    assert round(pa_cal.off_air_us(weak)) == 200
    assert pa_cal.overrun({"off_us": pa_cal.off_air_us(weak)}) == []
