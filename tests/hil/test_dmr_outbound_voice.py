# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Two-slot voice on a repeater outbound (opt-in RF: --allow-rf, a bench board, --outbound-mhz). The bench board sends
what a repeater's downlink carries: BS bursts with the CACH on both slots, with voice calls on them (bench_tx.py
outbound --voice, up to 8 calls). The radio under test sits on a duplex DMR channel whose RX is that frequency and
must show the call on its own timeslot, from the right talker and talkgroup, and nothing from the other slot or from
another color code. The radio never transmits.

The board's own records are checked too: one `call` record per call sent, no late bursts. A board that can't push voice
on an outbound refuses the job (`unsupported`), which is a skip.

| Case | The bench sends | The radio on each timeslot |
|---|---|---|
| alone | one call, on TS1 or on TS2 | the call on its own slot; hears nothing on the other |
| both | a call on TS1 and one on TS2 at the same time | its own slot's call, not the other's |
| overlap | TS2 starts 1.5 s into a TS1 call | its own slot's call, not the other's |
| late | one call with no headers, joined at burst A-F | the call from the embedded LC, marked late entry |
| wrong_cc | one call on the radio's slot, sent with another color code | no call |

Needs --outbound-mhz (or outbound_mhz in the rig's file) and a duplex DMR channel receiving it; --dmr-cc is the
channel's color code (the radio's own is read from the channel when it shows one)."""
import json
import time

import pytest

from rfoundry_hil import counter_deltas, skip
from hil_config import rig

SUPERFRAME_S = 0.360
SRC = {1: 9990301, 2: 9990302}      # talker per slot, so a call heard on the wrong slot shows by its source
DST = {1: 9990401, 2: 9990402}      # the talkgroups; the channel's RX group list must hold them or the radio hears
                                    # every group (hear_all)
LEAD_S = 5.0                        # the radio locks to the outbound well before the first call


def _call(slot, start_s, superframes=3, **fields):
    c = {"slot": slot, "src": SRC[slot], "dst": DST[slot], "group": True, "start_s": start_s,
         "superframes": superframes}
    c.update(fields)
    return c


def _records(job):
    """The JSON records among a bench job's output lines."""
    out = []
    for line in job.lines:
        if line.lstrip().startswith("{"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def _span_s(c):
    """When a call is over, in seconds from the job's start: headers 60 ms, superframes 360 ms, terminators 60 ms."""
    return c["start_s"] + c.get("headers", 3) * 0.06 + c["superframes"] * SUPERFRAME_S + c.get("terminators", 3) * 0.06


def _setup(radio, pytestconfig):
    if "DMR" not in radio.features():
        skip("not_built", "built without DMR")
    freq = rig(pytestconfig, "outbound-mhz", "the repeater outbound the bench board sends on:")
    cc = rig(pytestconfig, "dmr-cc")
    radio.go_home()
    try:
        radio.find_channel(round(freq * 1e6), "dmr")
    except AssertionError as e:
        skip("opt_in", f"the radio has no DMR channel receiving {freq} MHz: {e}")
    core = radio.core()
    if core.get("tx_hz") == core.get("rx_hz"):
        skip("opt_in", f"the channel on {freq} MHz is simplex: the outbound needs a duplex (repeater) channel")
    d = radio.view(["dmr"])
    if d.get("dmr.cc") not in (None, str(cc)):
        skip("opt_in", f"the channel's color code is {d.get('dmr.cc')}, --dmr-cc says {cc}")
    return freq, cc


def _run(radio, bench, pytestconfig, calls, radio_ts, cc_offset=0, expect_up=True):
    """The calls on the outbound, with the radio on timeslot radio_ts; returns (radio deltas, radio view after, the
    board's records)."""
    freq, cc = _setup(radio, pytestconfig)
    if bench.api is None:
        skip("bench", "an outbound with voice needs a bench board (--bench-api)")
    if radio.view(["dmr"]).get("dmr.ts") != str(radio_ts):
        radio.link.dmr_fields(ts=radio_ts)
    sent_cc = (cc + cc_offset) % 16
    seconds = round(max(_span_s(c) for c in calls) + 3.0, 1)
    voice = json.dumps(calls, separators=(",", ":"))
    try:
        with bench.lease():
            radio.reset_counters()
            _, before = radio.counters()
            job = bench._api_job("bench_tx", f"--freq {freq} --cc {sent_cc} outbound --seconds {seconds} "
                                              f"--voice '{voice}'", seconds)
            if expect_up:       # another color code is no outbound of ours: the radio never reports it up
                radio.wait_for(lambda x: x.get("dmr.repeater_up") == "1", timeout=15, sections=("dmr",),
                               what="the radio hearing the outbound")
            else:
                time.sleep(LEAD_S - 1.0)
            bench.listen_result(job, timeout=seconds + 30)
            time.sleep(1.5)                 # the last call times out in the radio
            _, after = radio.counters()
            view = radio.view(["dmr_call"])
    finally:
        radio.link.clear_temporary()
    if job.returncode not in (0, None):
        text = "".join(job.lines)
        if "unsupported" in text:
            skip("bench", "the bench board can't push voice on an outbound (unsupported)")
        raise AssertionError(f"the bench job ended with {job.returncode}: {text[-400:]}")
    d = counter_deltas(before, after, ["call.rx_calls", "call.rx_late_entries", "call.rx_voice_bursts", "modem.crc"])
    recs = _records(job)
    print("radio deltas:", d, "last:", {k: v for k, v in view.items() if k.startswith(("last.", "call."))})
    return d, view, recs


def _board_calls(recs, calls):
    """The board's side: one call record per call sent (index, slot, src, dst), none of its bursts late."""
    rows = [r for r in recs if isinstance(r.get("call"), dict) or ("index" in r and "bursts" in r)]
    rows = [r.get("call", r) if isinstance(r.get("call"), dict) else r for r in rows]
    assert len(rows) == len(calls), f"the board reported {len(rows)} calls for {len(calls)} sent: {rows}"
    for sent, got in zip(calls, rows):
        assert got.get("slot") == sent["slot"], (sent, got)
        assert got.get("src") == sent["src"] and got.get("dst") == sent["dst"], (sent, got)
        if "late_entry" in got:
            assert bool(got["late_entry"]) == (sent.get("late", -1) >= 0), (sent, got)
    summary = [r["voice"] for r in recs if isinstance(r.get("voice"), dict)]
    if summary:
        assert summary[-1].get("calls", len(calls)) == len(calls), summary
        assert summary[-1].get("late", 0) == 0, f"the board sent bursts late: {summary[-1]}"


def _heard(d, view, slot, ts, late=False):
    """The radio on timeslot ts after calls on slot: heard (one call from that talker and group) or silent."""
    if slot != ts:
        assert d["call.rx_calls"] == 0, f"the radio on TS{ts} heard a call sent on TS{slot}: {d}"
        return
    assert d["call.rx_calls"] == 1, d
    assert view.get("last.src") == str(SRC[slot]) and view.get("last.dst") == str(DST[slot]), view
    assert view.get("last.group") == "1", view
    assert d["call.rx_voice_bursts"] > 0 and d["modem.crc"] == 0, d
    if late:
        assert d["call.rx_late_entries"] == 1, d


MARKS = [pytest.mark.feature("dmr_voice"), pytest.mark.rf, pytest.mark.bench_service, pytest.mark.bench_modem]


def _marked(f):
    for m in reversed(MARKS):
        f = m(f)
    return f


@_marked
@pytest.mark.parametrize("ts", [1, 2])
@pytest.mark.parametrize("slot", [1, 2])
def test_outbound_voice_alone(radio, sdr_bench, pytestconfig, slot, ts):
    calls = [_call(slot, LEAD_S)]
    d, view, recs = _run(radio, sdr_bench, pytestconfig, calls, ts)
    _board_calls(recs, calls)
    _heard(d, view, slot, ts)


@_marked
@pytest.mark.parametrize("ts", [1, 2])
def test_outbound_voice_both_slots(radio, sdr_bench, pytestconfig, ts):
    calls = [_call(1, LEAD_S, 4), _call(2, LEAD_S, 4)]
    d, view, recs = _run(radio, sdr_bench, pytestconfig, calls, ts)
    _board_calls(recs, calls)
    _heard(d, view, ts, ts)


@_marked
@pytest.mark.parametrize("ts", [1, 2])
def test_outbound_voice_slot2_mid_call(radio, sdr_bench, pytestconfig, ts):
    """TS2 starts 1.5 s into a TS1 call: the slot already carrying a call doesn't disturb the new one."""
    calls = [_call(1, LEAD_S, 6), _call(2, LEAD_S + 1.5, 3)]
    d, view, recs = _run(radio, sdr_bench, pytestconfig, calls, ts)
    _board_calls(recs, calls)
    _heard(d, view, ts, ts)


@_marked
@pytest.mark.parametrize("late", [0, 3, 5])
def test_outbound_voice_late_entry(radio, sdr_bench, pytestconfig, late):
    """No headers, the first burst the receiver gets is A..F (late 0-5): the call is taken from the embedded LC,
    on the slot of each parity so both are covered."""
    slot = 1 + late % 2
    calls = [_call(slot, LEAD_S, 6, headers=0, late=late)]
    d, view, recs = _run(radio, sdr_bench, pytestconfig, calls, slot)
    _board_calls(recs, calls)
    _heard(d, view, slot, slot, late=True)


@_marked
def test_outbound_voice_wrong_cc(radio, sdr_bench, pytestconfig):
    """The outbound's color code isn't the channel's: the radio doesn't hear the repeater's calls."""
    calls = [_call(1, LEAD_S)]
    d, view, recs = _run(radio, sdr_bench, pytestconfig, calls, 1, cc_offset=1, expect_up=False)
    assert d["call.rx_calls"] == 0, f"the radio showed a call sent with another color code: {d}"
