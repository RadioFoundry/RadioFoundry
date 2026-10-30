# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""
Slot timing through a duplex repeater, under the conditions that can upset it (ETSI TS 102 361-1 5.1.1, aligned
timing; docs/spec/dmr-modem.md). A subscriber on TSx must send under the outbound's TSx every time: whether it woke the
repeater or found it up, after a timeslot or channel change, a long idle, a reboot, or an outbound that dropped and
came back on a new slot phase, and all through a long call.

Opt-in like test_dmr_tx_split_repeater (--allow-rf, --allow-codeplug-write, --split-tx-mhz): the test channel's TX
frequency is set to --split-tx-mhz over CPS (backed up, restored and verified afterwards) and the bench modem is a
duplex repeater on the pair (bench_repeater.py: input --split-tx-mhz, output --dmr-freq). Its idle receiver doesn't
hear a radio's BS_Dwn_Act on this bench, so it is kept up by its beacon (a local wake-up with the hang time as long as
the beacon) or answers a wake-up on a timer.

Every call is judged by the voice bursts the repeater's input heard while the radio was keyed (the events' arrival on
the host): at least half of what the call's length should give, and at least 90% of them on the radio's timeslot. A
call on the other slot, or one the repeater couldn't put on its slot grid, fails the scenario. Calls alternate
between TS1 and TS2 (a temporary override, cleared afterwards) unless the scenario needs the channel's own.

| Scenario | The repeater | The radio |
|---|---|---|
| up_ts_changes | up all along | 10 calls, the timeslot changed 0-3 s before each key-up |
| long_idle | up all along | 80 s idle listening to the outbound, then a call on each slot |
| rekey | 4 s beacons 0.5 s apart: the outbound drops and comes back on a new phase | keys 0.2-0.8 s after a rekey, 6 times |
| channel_change | up all along | to the next channel for 2-5 s and back, keys within 0.5 s, 4 times |
| long_call | up all along | a 25 s call on each slot |
| boot | up all along | restarted, keys as soon as the link answers, then again once settled |
| wake_ts | down at each key-up, answers 0.6 s after it | the timeslot changed 1 s before, 5 calls |

test_dmr_repeater_burst_offset (the same options, and the SDR bench): where in the slot the radio's inbound bursts
fall. The centers of the inbound and outbound bursts are aligned, the inbound slots 30 ms after the outbound ones (ETSI
TS 102 361-1 4.1), and the 1 ms the standard allows is for propagation and drift (10.2.3.1.3), not for the radio's own
error. With the repeater up all along, the SDR bench decodes both sides of an 8 s call (burst start from the sync
correlation, one sample clock): the median of each inbound burst's offset from the nearest outbound burst plus 30 ms
must be within OFFSET_MAX_MS, over at least 50 bursts. The HR_C7000 radios set it with tx_pre_on
(docs/spec/modem-hrc7000.md 2.1).
"""
import bisect
import collections
import json
import random
import statistics
import time

import pytest

from rfoundry_hil import skip
from test_dmr_tx import _dmr_tx_setup, _wait_line

VOICE_PER_S = 1 / 0.060         # one voice burst per 60 ms frame on our slot
OFFSET_MAX_MS = 0.25            # the inbound burst against the outbound + 30 ms (test_dmr_repeater_burst_offset)


def _voice_by_slot(rpt, t0, t1):
    """Voice bursts the repeater's input heard between t0 and t1 (time.monotonic(), by the events' arrival)."""
    got = collections.Counter()
    for line, at in zip(list(rpt.lines), list(rpt.stamps)):
        if not t0 <= at <= t1:
            continue
        try:
            rx = json.loads(line).get("rx")
        except ValueError:
            continue
        if rx and "voice" in rx:
            got[rx["slot"]] += 1
    return got


class _Session:
    """The split channel, one bench repeater run, and the calls made through it."""

    def __init__(self, radio, sdr_bench, pytestconfig):
        self.radio, self.bench = radio, sdr_bench
        self.split = pytestconfig.getoption("--split-tx-mhz")
        self.freq, self.cc, _, d = _dmr_tx_setup(radio, pytestconfig)
        self.ts = int(d["dmr.ts"])
        self.calls = []

    def start(self, seconds, beacon, beacon_len, phase=1.0):
        self.rpt = self.bench.repeater_start(self.split, self.freq, self.cc, seconds=seconds,
                                             extra=f"--beacon {beacon} --beacon-len {beacon_len} --beacon-phase {phase}")
        return _wait_line(self.rpt, "beacon every", 20)

    def wait_up(self, timeout=15):
        self.radio.wait_for(lambda x: x.get("dmr.repeater_up") == "1", timeout=timeout, sections=("dmr",),
                            what="the radio hearing the repeater's outbound")

    def set_ts(self, ts):
        self.radio.link.dmr_fields(ts=ts)
        self.ts = ts

    def call(self, seconds, what):
        """A call of seconds on the radio's timeslot; PTT re-armed every second (the debug link's hold is 10 s)."""
        radio = self.radio
        t0 = time.monotonic()
        radio.link.ptt(True, 3000)
        try:
            while time.monotonic() - t0 < seconds:
                time.sleep(min(1.0, max(0.0, seconds - (time.monotonic() - t0))))
                if time.monotonic() - t0 < seconds:
                    radio.link.ptt(True, 3000)
        finally:
            radio.link.ptt(False)
        t1 = time.monotonic()
        radio.wait_for(lambda x: x.get("state") != "tx", timeout=5, what="TX ended")
        self.calls.append({"ts": self.ts, "t0": t0, "t1": t1, "s": seconds, "what": what})

    def judge(self):
        self.bench.repeater_result(self.rpt, timeout=180)
        bad = []
        for k, c in enumerate(self.calls):
            got = _voice_by_slot(self.rpt, c["t0"], c["t1"] + 0.8)
            n, want = sum(got.values()), c["s"] * VOICE_PER_S
            ok = n >= 0.5 * want and got.get(c["ts"], 0) >= 0.9 * n
            print(f"call {k + 1} TS{c['ts']} {c['s']:.1f} s ({c['what']}): repeater input {dict(got)}"
                  f"{'' if ok else '  <- FAIL'}")
            if not ok:
                bad.append(f"call {k + 1} on TS{c['ts']} ({c['what']}): {dict(got)}")
        keys = [json.loads(x).get("key") for x in self.rpt.lines if x.startswith("{") and '"key"' in x]
        print(f"repeater transmitter: {keys.count(True)} key-ups, {keys.count(False)} drops")
        assert self.calls, "no call was made"
        assert not bad, f"{len(bad)} of {len(self.calls)} calls not on the radio's slot: {bad}"


def _session(radio, sdr_bench, pytestconfig, backup_dir, body):
    split = pytestconfig.getoption("--split-tx-mhz")
    if not split:
        skip("opt_in", "give the split channel's TX frequency: --split-tx-mhz MHz (one you may transmit on)")
    if abs(split - pytestconfig.getoption("--dmr-freq")) < 0.0125:
        skip("opt_in", "--split-tx-mhz must differ from --dmr-freq")
    if radio.view().get("set.tx_inhibit") == "1":
        skip("state", "the radio's TX inhibit is on (set.tx_inhibit=1)")
    import cps_client
    from test_cps import codeplug_edited
    s = _Session(radio, sdr_bench, pytestconfig)
    number = int(radio.core()["channel_number"])
    tx_hz = round(split * 1e6)
    with codeplug_edited(radio, backup_dir, lambda img: cps_client.set_channel_tx(img, number, tx_hz), "split"):
        radio.wait_for(lambda x: x.get("tx_hz") == str(tx_hz), timeout=5, what="the channel's TX frequency")
        radio.lowest_power()
        try:
            with sdr_bench.lease():
                body(s, random.Random(250))
                s.judge()
        finally:
            radio.link.clear_temporary()


def _up_all_along(s, seconds):
    """The repeater keyed for the whole run: one beacon as long as the run."""
    s.start(seconds, beacon=seconds + 60, beacon_len=seconds)
    s.wait_up()


def _up_ts_changes(s, rnd):
    _up_all_along(s, 80)
    for delay in (0.0, 0.05, 0.1, 0.2, 0.3, 0.4, 0.8, 1.5, 2.2, 3.0):
        s.set_ts(3 - s.ts)
        time.sleep(delay)
        s.call(1.5, f"key-up {delay:.2f} s after the timeslot change")
        time.sleep(1.0)


def _long_idle(s, rnd):
    _up_all_along(s, 120)
    time.sleep(80)
    for _ in range(2):
        s.call(2.0, "after 80 s idle on the outbound")
        s.set_ts(3 - s.ts)
        time.sleep(1.0)


def _beacon(rpt, n, timeout):
    """(arrival, skipped) of the bench repeater's beacon n: skipped when its transmitter was already up."""
    tag, end = f'"beacon": {n},', time.monotonic() + timeout
    while time.monotonic() < end:
        for line, at in zip(list(rpt.lines), list(rpt.stamps)):
            if tag in line:
                return at, '"skipped"' in line
        time.sleep(0.05)
    raise AssertionError(f"no beacon {n} from the bench repeater in {timeout} s")


def _rekey(s, rnd):
    # Beacons 4.5 s apart, 4 s long: the outbound is down about 0.5 s between two, and comes back on its own phase.
    # The radio keys into a beacon only when the one before was on air too (it was locked to that one). A call holds
    # the transmitter for the hang time after it, so the next beacon is skipped
    at = s.start(130, beacon=4.5, beacon_len=4.0)
    n, prev_on = 0, False
    while len(s.calls) < 6:
        n += 1
        on, skipped = _beacon(s.rpt, n, 30)
        if skipped or not prev_on:
            prev_on = not skipped
            if prev_on:
                s.set_ts(3 - s.ts)      # changed during the beacon the radio locks to
            continue
        time.sleep(max(0.0, on + rnd.uniform(0.2, 0.8) - time.monotonic()))
        s.call(1.2, f"{time.monotonic() - on:.2f} s after a rekey (beacon {n}, {on - at:.1f} s in)")
        prev_on = False


def _channel_change(s, rnd):
    _up_all_along(s, 80)
    number = s.radio.core()["channel_number"]
    for k in range(4):
        s.radio.link.channel_step(1)
        s.radio.wait_for(lambda c: c.get("channel_number") != number, timeout=5, what="the next channel")
        time.sleep(rnd.uniform(2.0, 5.0))
        s.radio.link.channel_step(-1)
        s.radio.wait_for(lambda c: c.get("channel_number") == number, timeout=5, what="back on the test channel")
        delay = rnd.uniform(0.0, 0.5)
        time.sleep(delay)
        s.call(1.5, f"key-up {delay:.2f} s after coming back from the next channel")
        time.sleep(1.0)


def _long_call(s, rnd):
    _up_all_along(s, 75)
    for _ in range(2):
        s.call(25.0, "a 25 s call")
        s.set_ts(3 - s.ts)
        time.sleep(2.0)


def _boot(s, rnd):
    from test_cps import reconnect
    _up_all_along(s, 90)
    for settle in (0.0, 4.0):
        s.radio.link.reboot(0, s.radio.link.chip()["token"])
        s.radio.link.close()
        time.sleep(2.0)
        reconnect(s.radio)
        s.radio.wait_for(lambda c: c.get("state") not in (None, "tx"), timeout=15, what="the radio after the restart")
        s.radio.lowest_power()
        s.ts = int(s.radio.view(["dmr"])["dmr.ts"])        # the override is gone with the restart
        time.sleep(settle)
        s.call(2.0, f"keyed {settle:.0f} s after the restart")
        time.sleep(1.0)


def _wake_ts(s, rnd):
    # The repeater is down at each key-up and keys its outbound 0.6 s after it, as if it had heard the wake-up; a
    # beacon lasts its hang time, so it is down again well before the next (test_dmr_tx_split_repeater, WAKE_ANSWER_S)
    period, phase, answer = 14.0, 6.0, 0.6
    at = s.start(phase + 5 * period + 4, beacon=period, beacon_len=3.0, phase=phase)
    for k in range(5):
        due = at + phase + k * period - answer
        time.sleep(max(0.0, due - 1.0 - time.monotonic()))
        assert s.radio.view(["dmr"]).get("dmr.repeater_up") == "0", "the repeater is up before its answer"
        s.set_ts(3 - s.ts)
        time.sleep(max(0.0, due - time.monotonic()))
        s.call(1.5, "woke the repeater, timeslot changed 1 s before")


SCENARIOS = {"up_ts_changes": _up_ts_changes, "long_idle": _long_idle, "rekey": _rekey,
             "channel_change": _channel_change, "long_call": _long_call, "boot": _boot, "wake_ts": _wake_ts}


@pytest.mark.feature("dmr_voice")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.codeplug_write
@pytest.mark.parametrize("scenario", list(SCENARIOS))
def test_dmr_repeater_slots(radio, sdr_bench, pytestconfig, backup_dir, scenario):
    _session(radio, sdr_bench, pytestconfig, backup_dir, SCENARIOS[scenario])


def burst_offsets_ms(inbound, outbound):
    """Each inbound burst's offset in ms from the nearest outbound burst 30 ms before it (records with `t`, the sync's
    time in s on one clock). The outbound has a burst every 30 ms while it is up, so an offset is within +-15 ms; one
    with no outbound burst within 5 ms (the outbound missed one) is left out."""
    out = sorted(r["t"] for r in outbound)
    offs = []
    for r in inbound:
        t = r["t"] - 0.030
        k = bisect.bisect_left(out, t)
        near = [out[j] for j in (k - 1, k) if 0 <= j < len(out)]
        if near:
            d = (t - min(near, key=lambda v: abs(v - t))) * 1e3
            if abs(d) < 5.0:
                offs.append(d)
    return offs


@pytest.mark.feature("dmr_voice")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.sdr_rx
@pytest.mark.codeplug_write
def test_dmr_repeater_burst_offset(radio, sdr_bench, pytestconfig, backup_dir):
    def body(s, rnd):
        for f in (s.split, s.freq):
            if not s.bench.can_watch(f):
                skip("bench", f"the SDR bench can't decode bursts on {f} MHz (no shared daemon, or outside its window)")
        _up_all_along(s, 40)
        ins = s.bench.bursts_start(s.split, 14, cc=s.cc)
        outs = s.bench.bursts_start(s.freq, 14, cc=s.cc)
        s.call(8.0, "burst offset")
        inbound, _ = s.bench.bursts_result(ins)
        outbound, _ = s.bench.bursts_result(outs)
        offs = burst_offsets_ms(inbound, outbound)
        print(f"inbound {len(inbound)} bursts, outbound {len(outbound)}, matched {len(offs)}")
        assert len(offs) >= 50, f"only {len(offs)} inbound bursts matched to the outbound (8 s is about 133)"
        med = statistics.median(offs)
        print(f"inbound vs outbound + 30 ms: median {med:+.3f} ms, min {min(offs):+.3f}, max {max(offs):+.3f}")
        assert abs(med) <= OFFSET_MAX_MS, f"the inbound bursts are {med:+.3f} ms off the outbound's (+-{OFFSET_MAX_MS} ms)"
    _session(radio, sdr_bench, pytestconfig, backup_dir, body)
