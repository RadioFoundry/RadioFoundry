# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""APRS AFSK transmit with sound at the microphone (docs/spec/aprs.md 13): the microphone must be muted while the
tones go out, so nothing but the tones modulates the carrier. Manual and opt-in: --manual -s --allow-rf
--allow-service-stop and the rig's callsign, a person at the radio.

No audio source can stand in for the microphone. Every tone the HIL tools can make joins the audio path after the
codec ADC, and a tone on the programming cable's serial line (test_mic_uart_inject.py) reaches the DMR microphone
path but not FM transmit where it was measured, so it never rides on the APRS tones, fixed or not. On radios with a
programming cable in the speaker/mic jack the cable also cuts the internal microphone, and the debug link goes with
the cable. So the person sends each position from the radio's own menu, cable out:

1. With the cable in, the test sets up the APRS test frequency, path and lowest power as test_aprs.py does
   (`aprs_air`: --aprs-test-mhz, path none, the RAM test callsign), starts a WINDOW_S bench board AFSK decode and
   prints "GO".
2. The person unplugs the cable, opens Tools > APRS, picks Send position now and waits for the transmission to end,
   then plugs the cable back in.
   - Over 1 (reference): a quiet room, nobody speaking.
   - Over 2: a steady loud whistle, or loud speech, at the microphone from before Send position now until the
     radio stops transmitting.
3. The test waits for the debug link (up to 3 min), checks aprs.rf_sent went up by one, and reads the board's decode.

Pass: each over gives exactly one frame with a valid FCS from <call>-<ssid>, the tones in range (check_tones), and
over 2's deviation (peak and rms over the frame) no more than SOUND_RATIO above over 1's. A live microphone adds
the voice to the tones: the frame fails to decode or the deviation jumps. Printed: both overs' deviation, twist and
tone levels. Every APRS setting is put back afterwards (aprs_session).
"""
import time

import pytest

from rfoundry_hil import skip
from test_aprs import (_aprs, _n, aprs_air, check_tones, print_capture,  # noqa: F401  (aprs_air: the fixture)
                       print_frame)
from test_mic_gain import wait_link

# Unplug, Tools > APRS > Send position now, the engine's wait for its next service pass (5.7 s once, test_aprs.py
# CAPTURE_S), the transmission: press Send soon after GO
WINDOW_S = 15
SOUND_RATIO = 1.25   # over 2's deviation / over 1's: the frames are the same, so a muted microphone leaves them equal

OVERS = (
    ("quiet", "Keep the room quiet; nobody speaks"),
    ("sound", "Whistle steadily or speak loudly at the microphone from before Send position now until the radio "
              "stops transmitting"),
)


def _over(radio, sdr_bench, s, n, name, how):
    v0 = _aprs(radio)
    print(f"\n=== over {n}/{len(OVERS)} ({name}): {s.hz / 1e6:.4f} MHz, lowest power ===")
    with sdr_bench.lease():
        job = sdr_bench.aprs_rx_start(s.hz / 1e6, seconds=WINDOW_S)
        print(f"GO: {WINDOW_S} s from now. Unplug the cable, then Tools > APRS > Send position now. {how}. "
              "When the transmission ends, plug the cable back in.", flush=True)
        s.expected_rf += 1
        res = sdr_bench.aprs_rx_result(job)
    wait_link(radio)
    v = radio.wait_for(lambda x: x.get("aprs.rf_active") == "0", timeout=10, sections=("aprs",),
                       what="APRS transmission over")
    sent = _n(v, "aprs.rf_sent") - _n(v0, "aprs.rf_sent")
    assert sent == 1, f"over {n} ({name}): aprs.rf_sent went up by {sent}, not 1: was Send position now pressed " \
                      f"once? {v}"
    print_capture(res)
    if "segments" in res and not res["segments"]:
        pytest.fail(f"over {n} ({name}): the bench board heard no carrier in its {WINDOW_S} s window: was Send "
                    "position now pressed soon enough after GO?")
    ours = [f for f in res["frames"] if f["src"] == f"{s.call}-{s.ssid}"]
    for f in ours:
        print_frame(f)
        if "rms_dev_hz" in f:
            print(f"  rms deviation {f['rms_dev_hz']:.0f} Hz")
    assert len(ours) == 1, (f"over {n} ({name}): expected one frame from {s.call}-{s.ssid} with a valid FCS, got "
                            f"{len(ours)}; bad FCS {res.get('fcs_bad')}, segments {res.get('segments')}" +
                            (": sound at the microphone reached the transmission" if name == "sound" else ""))
    check_tones(ours[0])
    return ours[0]


@pytest.mark.feature("aprs_afsk", owner=(
        "take the programming cable out, send a position from Tools > APRS, once in a quiet room and once whistling "
        "or speaking loudly at the microphone"))
@pytest.mark.manual
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_aprs_tx_microphone_muted(radio, sdr_bench, aprs_air):
    s = aprs_air
    frames = {}
    for n, (name, how) in enumerate(OVERS, 1):
        frames[name] = _over(radio, sdr_bench, s, n, name, how)
        time.sleep(1.0)
    q, l = frames["quiet"], frames["sound"]
    keys = [k for k in ("peak_dev_hz", "rms_dev_hz") if k in q and k in l and q[k]]
    if not keys:
        skip("bench", "the bench board reports no deviation for the frames")
    print("\n" + "  ".join(f"{k}: quiet {q[k]:.0f} Hz, sound {l[k]:.0f} Hz ({l[k] / q[k]:.2f}x)" for k in keys))
    for k in keys:
        assert l[k] <= q[k] * SOUND_RATIO, (f"{k} {l[k]:.0f} Hz with sound at the microphone, {q[k]:.0f} Hz without: "
                                            f"the microphone modulates the APRS transmission")
