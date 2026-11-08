# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The speaker, by ear: a person at the radio (--manual -s) says whether each sound comes out of the loudspeaker.

Nothing on the radio can tell whether its speaker plays, so these tests ask. The person answers on the radio itself:
press any key except PTT while the sound plays (heard), or press nothing (not heard). A key pressed is seen in
input.raw_keys; the radio fixture puts the channel, VFO and settings back afterwards, whatever the key did.

Without --manual, the microphone at the speaker (--microphone, as in test_speaker.py) listens instead: it records the
room just before the sound starts, then the sound, and the sound is heard when its loudest stretch (the 90th
percentile of 50 ms windows) is EAR_DB over the room's median. The volume
is set over the debug link (an override on a knob radio, given back after); a radio whose volume can't be set asks
for its own volume turned up.

They also show that the speaker plays with the programming cable in, on a radio whose host link runs on the cable
(the run itself goes through it).

- test_speaker_beep: a beep, repeated.
- test_speaker_fm_noise: the FM VFO on --fm-mhz with the squelch held open (monitor): the receiver's noise.
- test_speaker_dmr_voice (--allow-rf, the bench modem; --ambe for speech): a bench group call on the DMR test channel.
- test_speaker_second_receiver (the same): dual watch with the main receiver on another channel, so the call comes
  out of the second receiver (radio-features.md 2.2). Skips on a radio with one receiver."""
import contextlib
import threading
import time

import pytest

from rfoundry_hil import as_int, skip
from hil_config import rig

ANSWER_S = 12.0     # how long a sound plays and the person has to answer
ROOM_S = 2.0        # the microphone: the room recorded before the sound
EAR_DB = 10.0       # the microphone: the sound's loudest stretch over the room's median, to count as heard


def _keys(radio):
    return as_int(radio.link.view(["link"]).get("input.raw_keys"), 0)


def _heard(radio, ear, what, seconds=ANSWER_S, during=None, start=None):
    """Ask whether `what` is heard: True once a key goes down within `seconds` (a person), or when the microphone hears
    the speaker EAR_DB over the room (ear, the bench microphone). start(): starts the sound, once the room is
    measured. during(): called every 1.5 s while waiting (a beep again); keys still held from the step before are let
    go first."""
    if ear is not None:
        return _mic_heard(ear, what, seconds, during, start)
    end = time.monotonic() + 5
    while _keys(radio) != 0 and time.monotonic() < end:
        time.sleep(0.1)
    if start is not None:
        start()
    print(f"\n>>> Listen: {what}. Press any key except PTT if you hear it; press nothing if you don't "
          f"({seconds:.0f} s)", flush=True)
    end, nxt = time.monotonic() + seconds, 0.0
    while time.monotonic() < end:
        if during is not None and time.monotonic() >= nxt:
            during()
            nxt = time.monotonic() + 1.5
        if _keys(radio) != 0:
            print("    heard", flush=True)
            return True
        time.sleep(0.1)
    print("    not heard", flush=True)
    return False


def _levels(path):
    """dBFS of each 50 ms window of a recording"""
    import numpy as np
    import audio_check as A
    x, fs = A.load_wav(path)
    n = int(fs * 0.05)
    w = x[: len(x) // n * n].reshape(-1, n)
    return A.db(np.sqrt((w ** 2).mean(axis=1)))


def _mic_heard(mic, what, seconds, during, start):
    import numpy as np
    p, path = mic.start(ROOM_S)
    room = float(np.median(_levels(mic.finish(p, path))))
    p, path = mic.start(seconds)
    if start is not None:
        start()
    end, nxt = time.monotonic() + seconds, 0.0
    while p.poll() is None and time.monotonic() < end + 5:
        if during is not None and time.monotonic() >= nxt:
            during()
            nxt = time.monotonic() + 1.5
        time.sleep(0.1)
    loud = float(np.percentile(_levels(mic.finish(p, path)), 90))
    heard = loud - room >= EAR_DB
    print(f"microphone, {what}: room {room:.1f} dBFS, sound {loud:.1f} dBFS ({loud - room:+.1f} dB, "
          f"{EAR_DB:g} needed): {'heard' if heard else 'not heard'} ({path})", flush=True)
    return heard


@contextlib.contextmanager
def _loud(radio):
    """The volume at three quarters for the block, then back."""
    f = radio.link.view(["features"])
    src = f.get("volume.src", "none")
    if src == "none":
        print("\n>>> This radio's volume can't be set from here: turn it up now", flush=True)
        time.sleep(5)
        yield
        return
    vmax = as_int(f.get("volume.max"), 16)
    old = "knob" if src == "knob" else as_int(f.get("volume.step"), 0)
    radio.link.volume(max(1, vmax * 3 // 4))
    try:
        yield
    finally:
        radio.link.volume(old)


@pytest.fixture(scope="module")
def ear(pytestconfig, tmp_path_factory):
    """Who listens: None for the person at the radio (--manual), else the bench microphone (--microphone; the
    by_ear marker lets these tests run without --manual then)"""
    if pytestconfig.getoption("--manual"):
        return None
    from test_speaker import Microphone      # it needs numpy; imported here so --manual runs don't
    import shutil
    if not shutil.which("arecord"):
        skip("no_hw", "arecord (alsa-utils) isn't installed")
    return Microphone(rig(pytestconfig, "microphone", "without --manual these tests need"),
                      str(tmp_path_factory.mktemp("ear")))


@pytest.fixture
def ready(radio):
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    if radio.link.view(["link"]).get("input.raw_keys") is None:
        skip("not_built", "firmware without input.raw_keys")
    return radio


@pytest.mark.feature("beep", owner="say whether the speaker plays a beep")
@pytest.mark.manual
@pytest.mark.by_ear
def test_speaker_beep(ready, ear):
    if "BEEP" not in ready.features():
        skip("not_built", "built without CONFIG_BEEP")
    if as_int(ready.link.view(["features"]).get("set.beep_level"), 0) == 0:
        skip("state", "beep level 0")     # no beep to hear, as in test_speaker.py
    with _loud(ready):
        assert _heard(ready, ear, "a beep every 1.5 s", during=lambda: ready.link.beep("key")), "no beep heard"


@pytest.mark.feature("fm", "vfo_fm", owner="say whether the speaker plays the FM receiver's noise")
@pytest.mark.manual
@pytest.mark.by_ear
def test_speaker_fm_noise(ready, ear, pytestconfig):
    if "VFO_FM" not in ready.features():
        skip("not_built", "built without the FM VFO")
    hz = round(rig(pytestconfig, "fm-mhz") * 1e6)
    L = ready.link
    with ready.vfo_session():
        L.vfo_set(hz)
        ready.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
        def open_squelch():
            L.monitor(True)
            ready.wait_for(lambda v: v.get("monitor") == "1", 3, what="the squelch held open")

        with _loud(ready):
            try:
                heard = _heard(ready, ear, f"FM noise (hiss) on {hz / 1e6:.4f} MHz, the squelch held open",
                               start=open_squelch)
            finally:
                L.monitor(False)
    assert heard, "no FM receiver noise heard with the squelch open"


def _call_heard(radio, ear, sdr_bench, pytestconfig, what):
    """A 20 superframe bench call on the DMR test channel (about 7 s, speech with --ambe) while the person listens."""
    freq, cc, tg = (rig(pytestconfig, o) for o in ("dmr-freq", "dmr-cc", "dmr-tg"))
    ambe = pytestconfig.getoption("--ambe")
    if ambe:
        ambe = sdr_bench.upload(ambe, "hil_speech.ambe")
    res = {}

    def call():
        try:
            sdr_bench.voice_call(freq, cc, tg, superframes=20, calls=1, ambe=ambe)
        except Exception as e:  # noqa: BLE001 (reported below, after the answer)
            res["error"] = e

    t = threading.Thread(target=call, daemon=True)
    with sdr_bench.lease(), _loud(radio):
        try:
            heard = _heard(radio, ear, what + (" (speech)" if ambe else " (the bench's test frames: a tone or silence)"),
                           start=t.start)
        finally:
            if t.ident is not None:     # started: the person's or the microphone's wait began
                t.join(60)
    if "error" in res:
        raise res["error"]
    return heard


@pytest.mark.feature("dmr_voice", owner="say whether the speaker plays a bench DMR call")
@pytest.mark.manual
@pytest.mark.by_ear
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_speaker_dmr_voice(ready, ear, sdr_bench, pytestconfig):
    hz = round(rig(pytestconfig, "dmr-freq") * 1e6)
    ready.find_channel(hz, "dmr")
    assert _call_heard(ready, ear, sdr_bench, pytestconfig, "a DMR call from the bench"), \
        "the bench call wasn't heard"


@pytest.mark.feature("dual_watch", owner="say whether the speaker plays a call the second receiver hears")
@pytest.mark.manual
@pytest.mark.by_ear
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_speaker_second_receiver(ready, ear, sdr_bench, pytestconfig):
    from test_radio_features import features, need_scan, scan_off, settings, wait_features
    need_scan(ready)
    if features(ready).get("dual.receivers") != "2":
        skip("not_built", "one receiver: dual watch alternates it")
    hz = round(rig(pytestconfig, "dmr-freq") * 1e6)
    pos = ready.find_channel(hz, "dmr")
    size = as_int(ready.core().get("zone_size"), 0)
    other = next((p for p in range(size) if p != pos), None)
    if other is None:
        skip("state", "the DMR test channel's zone has one channel")
    ready.link.zone_pos(other)
    ready.wait_for(lambda v: v.get("zone_pos") == str(other), what="the other channel")
    if ready.core().get("rx_hz") == str(hz):
        skip("state", "the other channel is on the test frequency too")
    with settings(ready, scan_resume=1):
        ready.link.scan("dual", pos)
        try:
            wait_features(ready, lambda v: v.get("dual.both") == "1", 12.0, "the second receiver on the test channel")
            heard = _call_heard(ready, ear, sdr_bench, pytestconfig, "a DMR call through the second receiver")
        finally:
            scan_off(ready)
    assert heard, "the call on the second receiver wasn't heard"
