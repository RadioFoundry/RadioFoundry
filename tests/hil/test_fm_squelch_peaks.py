# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""FM squelch through voice peaks on a 12.5 kHz channel (opt-in: --allow-rf, --bench-api; docs/spec/radio-features.md
3.5). Speech above about 2 kHz (fricatives) raised some receivers' noise reading over the close line while the
carrier stayed up, and the squelch closed on every one.

test_fm_squelch_voice_peaks (rf, bench_service): the VFO on --fm-mhz at 12.5 kHz, squelch level 1, no RX tone. The
bench board sends ten speech-like syllables (1 kHz at 1.5 kHz deviation, 300 ms) with a 120 ms high burst after each
(2 kHz or 3 kHz at full deviation, 2.5 kHz). The radio's state is read every 40 ms while the carrier is up. Pass: the
squelch opens and doesn't close once between the first open and the end of the transmission; after the bench drops the
carrier, the squelch closes within CLOSE_AFTER_S. The VFO's bandwidth and squelch level are put back."""
import time

import pytest

from rfoundry_hil import skip
from hil_config import rig

SYLLABLE = {"kind": "tone", "hz": 1000, "s": 0.3, "level": 0.6}    # 1.5 kHz at dev_hz 2500
BURSTS = {"2k": 2000, "3k": 3000}
DEV_HZ = 2500
CLOSE_AFTER_S = 0.5     # the bench's done record to the squelch shut (one poll, the link's reads, the record's lag)


def _program(burst_hz):
    prog = []
    for _ in range(10):
        prog.append(SYLLABLE)
        prog.append({"kind": "tone", "hz": burst_hz, "s": 0.12, "level": 1.0})
    return prog


def _step_squelch(radio, target):
    for _ in range(12):
        cur = int(radio.core()["squelch"])
        if cur == target:
            return
        radio.link.squelch_step(-1 if cur > target else 1)
        time.sleep(0.1)
    raise AssertionError(f"squelch didn't reach level {target}")


@pytest.mark.feature("fm", "vfo_fm")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.needs_switch("VFO_FM")
@pytest.mark.parametrize("burst", list(BURSTS))
def test_fm_squelch_voice_peaks(radio, sdr_bench, pytestconfig, burst):
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    L = radio.link
    with radio.vfo_session():
        L.vfo_set(hz)
        try:
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", 3, what="VFO tuned")
        except AssertionError:
            skip("other_board", f"the radio doesn't tune {mhz:.4f} MHz: not one of its bands")
        bw0 = radio.view(["channel"]).get("ch.bw")
        sq0 = int(radio.core()["squelch"])
        try:
            if bw0 != "12k5":
                L.bandwidth(False)
                radio.wait_for(lambda v: v.get("ch.bw") == "12k5", sections=("channel",), what="12.5 kHz")
            _step_squelch(radio, 1)
            time.sleep(0.5)
            samples = []
            with sdr_bench.lease():
                tx = sdr_bench.fm_tx_start(mhz, _program(BURSTS[burst]), max_s=9.0, dev_hz=DEV_HZ, lead_s=0.5,
                                           tail_s=0.2)
                t0 = time.monotonic()
                while tx.poll() is None and time.monotonic() - t0 < 12:
                    v = radio.core()
                    samples.append((round(time.monotonic() - t0, 2), v.get("state"), v.get("noise"),
                                    v.get("rssi_dbm")))
                    time.sleep(0.04)
                done = tx.wait(20)
                t_done = time.monotonic()
                while radio.core().get("state") == "rx" and time.monotonic() - t_done < 3:
                    time.sleep(0.02)
                close_s = time.monotonic() - t_done
        finally:
            _step_squelch(radio, sq0)
            if bw0 == "25k":
                L.bandwidth(True)
    states = [s[1] for s in samples]
    first = next((i for i, s in enumerate(states) if s == "rx"), None)
    last = max((i for i, s in enumerate(states) if s == "rx"), default=None)
    drops = 0
    if first is not None:
        drops = sum(1 for i in range(first + 1, last + 1) if states[i] != "rx" and states[i - 1] == "rx")
    print(f"\n{mhz:.4f} MHz, {burst} bursts: {len(samples)} reads, open at "
          f"{samples[first][0] if first is not None else None} s, closed {drops} times while keyed, shut "
          f"{close_s:.2f} s after the bench's done record; bench {done}")
    print("  " + " ".join(f"{s[0]}:{(s[1] or '?')[:2]}/{s[2]}/{s[3]}" for s in samples))
    assert not done.get("cut"), f"the bench cut its transmission: {done}"
    assert first is not None, f"the bench's carrier on {mhz:.4f} MHz never opened the squelch at level 1"
    assert drops == 0, f"the squelch closed {drops} times on {burst} voice peaks while the carrier was up"
    assert close_s < CLOSE_AFTER_S, f"the squelch closed {close_s:.2f} s after the carrier, over {CLOSE_AFTER_S} s"
