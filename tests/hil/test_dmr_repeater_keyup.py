# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Key-ups through a real duplex DMR repeater, timed around the end of its hang (opt-in RF: --allow-rf, a bench
receiver, --repeater-out-mhz and --repeater-in-mhz). Issue 524: through a repeater, a voice key-up was refused
("Modem refused") or keyed but never repeated, a few times in every ten, and more often when keyed as the repeater's
hang ended.

The repeater is a real one, not a bench job: it must answer the radio's wake-up (BS_Dwn_Act) and repeat what it hears,
as a repeater in service does. The radio's DMR test channel (--dmr-freq) is made duplex for the run (RX
--repeater-out-mhz, TX --repeater-in-mhz, color code --repeater-cc; --allow-codeplug-write, restored after). The bench
receiver watches both frequencies at once: what the radio sent on the input, what the repeater sent back on its
output.

The first key-up wakes the repeater and measures its hang: from the release to the output's last burst. The next ones
are keyed at offsets from the last release, inside the hang, just before and just after its end, and well past it:

| Key-up | The radio must |
|---|---|
| inside the hang | key at once on the repeater's slot |
| at the hang's end | key whether the repeater is still up or just dropped: wake it again when it dropped |
| past the hang | wake the repeater, then key |

Each key-up is judged on what was on the air: refused (call.tx_denied), not on the air (under 5 voice bursts from the
radio on the input), not repeated (under 5 voice bursts on the output), or ok. Every key-up must be ok. The radio's
phases while keyed and the wake-ups sent are printed for each."""
import collections
import time

import pytest

from rfoundry_hil import skip
from hil_config import rig
from test_dmr_audio_smoke import find_dmr_channel

KEY_S = 1.5             # each key-up's length
HANG_MAX_S = 20.0       # a repeater that holds its output longer is never down between key-ups here
OFFSETS = (-0.4, -0.15, 0.05, 0.25, 0.6, None, 3.0)    # s from the hang's end; None: 1 s after the release
LATE_S = 0.1            # a key-up this late against its offset is printed as such (the bench's results came late)


def _voice(rows, cc):
    return [r for r in rows if r.get("voice") and r.get("cc") in (None, cc)]


def _deltas(a, b):
    return {k: b.get(k, 0) - a.get(k, 0) for k in set(a) | set(b)
            if isinstance(b.get(k, 0), int) and b.get(k, 0) != a.get(k, 0)}


@pytest.mark.feature("dmr_voice", optional=True)
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.codeplug_write
def test_dmr_repeater_keyup(radio, sdr_bench, pytestconfig, backup_dir):
    out_mhz = pytestconfig.getoption("--repeater-out-mhz")
    in_mhz = pytestconfig.getoption("--repeater-in-mhz")
    if not out_mhz or not in_mhz:
        skip("opt_in", "a duplex DMR repeater that answers a wake-up: --repeater-out-mhz MHz (its output) and "
                       "--repeater-in-mhz MHz (its input, one you may transmit on)")
    import cps_client
    from test_cps import codeplug_edited
    if "DMR" not in radio.features():
        skip("not_built", "built without DMR")
    cc = pytestconfig.getoption("--repeater-cc")
    if cc is None:
        cc = rig(pytestconfig, "dmr-cc")
    keyups = pytestconfig.getoption("--repeater-keyups")
    radio.go_home()
    find_dmr_channel(radio, round(rig(pytestconfig, "dmr-freq") * 1e6))
    radio.lowest_power()
    d = radio.view(["dmr", "core"])
    if int(d["dmr.own_id"]) == 0:
        skip("state", "the radio has no DMR ID (dmr.own_id=0)")
    if radio.view().get("set.tx_inhibit") == "1":
        skip("state", "the radio's TX inhibit is on (set.tx_inhibit=1)")
    number = int(d["channel_number"])
    out_hz, in_hz = round(out_mhz * 1e6), round(in_mhz * 1e6)

    def edit(img):
        cps_client.set_channel_rx(img, number, out_hz)
        cps_client.set_channel_tx(img, number, in_hz)
        cps_client.set_channel_cc(img, number, cc)

    def watch(at, after_s=2.0):
        """Both frequencies watched from now to after_s past the release of a key-up at time.monotonic() `at`"""
        _, before = radio.counters()
        seconds = max(0.0, at - time.monotonic()) + KEY_S + after_s
        return before, sdr_bench.bursts_start(in_mhz, seconds=seconds), sdr_bench.bursts_start(out_mhz, seconds=seconds)

    def judge(before, lis_in, lis_out, samples, released_utc):
        rows_in, _ = sdr_bench.bursts_result(lis_in)
        rows_out, _ = sdr_bench.bursts_result(lis_out)
        _, after = radio.counters()
        dl = _deltas(before, after)
        v_in, v_out = _voice(rows_in, cc), _voice(rows_out, cc)
        errs = sorted({str(s[2]) for s in samples} | {str(radio.view(["dmr"]).get("dmr.tx_error"))})
        if dl.get("call.tx_denied", 0):
            verdict = "refused"
        elif len(v_in) < 5:
            verdict = "not on the air"
        elif len(v_out) < 5:
            verdict = "not repeated"
        else:
            verdict = "ok"
        phases = []
        for s in samples:
            if not phases or phases[-1] != s[1]:
                phases.append(s[1])
        last_out = max((r["t"] for r in rows_out if r.get("t") is not None), default=None)
        return {"verdict": verdict, "phases": phases, "errors": errs, "wakeups": sum(r.get("kind") == "CSBK"
                for r in rows_in), "voice_in": len(v_in), "voice_out": len(v_out),
                "hang": None if last_out is None else round(last_out - released_utc, 2),
                "denied": dl.get("call.tx_denied", 0)}

    def key_and_release(at):
        """PTT for KEY_S at time.monotonic() `at` (at once when past). (key-up time, release time, the release's
        time.time(): the bench's burst times are UTC, the radio's phases while keyed)"""
        while time.monotonic() < at:
            time.sleep(0.002)
        t_key = time.monotonic()
        radio.link.ptt(True, int(KEY_S * 1000) + 300)
        samples = []
        while time.monotonic() < t_key + KEY_S:
            x = radio.view(["dmr"])
            samples.append((round(time.monotonic() - t_key, 2), x.get("dmr.phase"), x.get("dmr.tx_error")))
            time.sleep(0.1)
        radio.link.ptt(False)
        released, released_utc = time.monotonic(), time.time()
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=5, what="TX ended")
        return t_key, released, released_utc, samples

    results = []
    with codeplug_edited(radio, backup_dir, edit, "repeater"):
        radio.wait_for(lambda x: x.get("tx_hz") == str(in_hz) and x.get("rx_hz") == str(out_hz), timeout=5,
                       what="the channel's frequencies")
        radio.tx_gate(True)
        with sdr_bench.lease():
            # The hang: the output watched until well past it
            w = watch(time.monotonic(), HANG_MAX_S)
            _, released, released_utc, samples = key_and_release(time.monotonic())
            first = judge(*w, samples, released_utc)
            print(f"first key-up: {first}")
            assert first["verdict"] == "ok", f"the first key-up, the repeater woken: {first}"
            hang = first["hang"]
            if hang is None or hang > HANG_MAX_S - 1:
                skip("state", f"the repeater's output didn't drop within {HANG_MAX_S - 1:.0f} s of the release "
                              f"(hang {hang})")
            print(f"repeater hang {hang:.2f} s after the release")
            # Down again, then one more wake-up: the key-ups below are timed from the release before each, and this
            # one's results come 2 s after its release, in time for a hang of about 3 s or more
            time.sleep(max(0.0, released + hang + 2 - time.monotonic()))
            w = watch(time.monotonic())
            _, released, released_utc, samples = key_and_release(time.monotonic())
            r = judge(*w, samples, released_utc)
            r.update(gap=None, offset=None, late=0.0)
            results.append(r)
            print(f"key-up after the hang: {r['verdict']}; phases {r['phases']}, wake-ups {r['wakeups']}")
            for k in range(keyups):
                off = OFFSETS[k % len(OFFSETS)]
                gap = 1.0 if off is None else hang + off
                at = released + gap
                w = watch(at)
                t_key, released, released_utc, samples = key_and_release(at)
                late = t_key - at
                r = judge(*w, samples, released_utc)
                r.update(gap=round(gap, 2), offset=off, late=round(late, 2))
                results.append(r)
                print(f"key-up {k + 1} of {keyups}, {gap:.2f} s after the last release"
                      f"{f' ({late:.2f} s late)' if late > LATE_S else ''}: {r['verdict']}; phases {r['phases']}, "
                      f"tx_error {r['errors']}, wake-ups {r['wakeups']}, voice in/out {r['voice_in']}/{r['voice_out']}")
    by = collections.Counter(r["verdict"] for r in results)
    print(f"{dict(by)}")
    bad = [f"{r['gap']} s: {r['verdict']} {r['errors']}" for r in results if r["verdict"] != "ok"]
    assert not bad, f"{len(bad)} of {len(results)} key-ups failed (hang {hang:.2f} s): {bad}"
