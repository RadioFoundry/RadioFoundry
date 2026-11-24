# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The speaker amplifier goes off after the first DMR key-up (opt-in: --allow-rf; radio_tx, the radio only, at its
lowest power on the DMR test channel --dmr-freq).

After a restart or a visit to FM, the radio's PCM audio stream isn't running. The first key-up's talk permit beep
then starts the stream itself and routes the speaker, and the DMR voice stream takes the running stream over as the
transmission starts. The voice stream must give the route back: a route left on kept the amplifier on, and the
speaker hissed until the next key or PTT (issue 705). The amplifier's users are the board's debug link key
`board.amp_users` (a bit per user, 0x00 while nothing plays); a board without the key skips.

- test_amp_off_after_restart_keyup: a restart, the DMR test channel, one 1.5 s over: the state is tx mid-over, the
  talk permit beep started the stream itself (beep.played grew, beep.shared didn't), and `board.amp_users` reads 0x00
  on every sample from 1 s to 4 s after the over.
- test_amp_off_after_fm_keyup: the same after a visit to the FM VFO (no TX there) and back to the DMR test channel,
  three times.
"""
import time

import pytest

from hil_config import rig
from rfoundry_hil import skip

SAMPLES_S = (1.0, 2.0, 3.0, 4.0)     # after the over: the beep's route hold is 4 blocks (80 ms), the stream's stop 200 ms


def _amp_users(radio):
    v = radio.view(["board"]).get("board.amp_users")
    return None if v is None else int(v, 16)


def _setup(radio, pytestconfig):
    if "DMR" not in radio.features():
        skip("not_built", "built without DMR")
    if _amp_users(radio) is None:
        skip("not_built", "the board reports no speaker amplifier users (board.amp_users)")
    return round(rig(pytestconfig, "dmr-freq") * 1e6)


def _dmr_channel(radio, hz):
    radio.go_home()
    radio.find_channel(hz, "dmr", simplex=True)
    radio.lowest_power()
    time.sleep(2.0)       # the radio settles on the channel (the scan of a first entry, the receiver's set-up)


def _beeps(radio):
    """(beep.played, beep.shared): every beep played, and those played into a stream already running"""
    return int(radio.view(None).get("beep.played", 0)), int(radio.view(["audio"]).get("beep.shared", 0))


def _key_and_check(radio, hz, what):
    """One 1.5 s over: the talk permit beep started the stream itself (beep.played grew, beep.shared didn't), so the
    voice stream took it over; then the amplifier's users sampled after it: every sample 0x00."""
    v = radio.view(None)
    played0, shared0 = _beeps(radio)
    assert v.get("tx_hz") == str(hz), f"tx_hz {v.get('tx_hz')} is not the test frequency {hz}"
    radio.link.ptt(True, 1800)
    time.sleep(0.8)
    mid = radio.view(None)
    time.sleep(0.7)
    radio.link.ptt(False)
    assert mid.get("state") == "tx", f"{what}: the radio did not transmit (state {mid.get('state')}, tx_deny " \
                                     f"{mid.get('tx_deny')}, txlim.refused {mid.get('txlim.refused')})"
    radio.wait_for(lambda s: s.get("state") != "tx", timeout=3, what="TX ended")
    played, shared = _beeps(radio)
    assert played > played0, f"{what}: no beep played for the over: are the key beep and Settings > Talk permit off?"
    assert shared == shared0, f"{what}: the beep played into a stream already running (beep.shared " \
                              f"{shared0} -> {shared}), so the voice stream never took a beep's stream over"
    t0 = time.monotonic()
    seen = []
    for t in SAMPLES_S:
        time.sleep(max(0.0, t - (time.monotonic() - t0)))
        seen.append(_amp_users(radio))
    print(f"{what}: board.amp_users at +{'/+'.join(str(t) for t in SAMPLES_S)} s: "
          f"{['0x%02x' % u for u in seen]}")
    assert all(u == 0 for u in seen), \
        f"{what}: the speaker amplifier stayed on after the over (board.amp_users {['0x%02x' % u for u in seen]})"


@pytest.mark.feature("beep")
@pytest.mark.feature("dmr_voice", optional=True)
@pytest.mark.radio_tx
def test_amp_off_after_restart_keyup(radio, pytestconfig):
    hz = _setup(radio, pytestconfig)
    radio.link.reboot(0, radio.link.chip()["token"])
    radio.link.close()
    time.sleep(2.0)
    radio.reconnect(30.0)
    radio.tx_gate(True)     # a restart cleared the test TX limit
    _dmr_channel(radio, hz)
    _key_and_check(radio, hz, "first key-up after a restart")


@pytest.mark.feature("beep")
@pytest.mark.feature("dmr_voice", optional=True)
@pytest.mark.radio_tx
def test_amp_off_after_fm_keyup(radio, pytestconfig):
    hz = _setup(radio, pytestconfig)
    for rep in range(3):
        with radio.vfo_session():
            time.sleep(2.0)
        radio.leave_vfo()
        _dmr_channel(radio, hz)
        _key_and_check(radio, hz, f"first key-up after FM, round {rep + 1}")
