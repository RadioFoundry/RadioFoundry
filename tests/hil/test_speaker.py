# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Speaker tests by microphone (README.md, "Speaker tests"): opt-in with --microphone, an ALSA capture device
(arecord -D) whose microphone is at the radio's speaker; every test skips (opt_in) without one. Each plays a known
sequence of tones and silences and checks the recording with audio_check.check_tones: every tone as long as asked
(nothing lost at its start or end), its level flat, each silence silent, and no pop or noise burst around it.

test_speaker_beeps: every beep the debug link can ask for, one recording each, patterns read from core/radio/beep.c.
The first beep finds the amplifier as the radio left it; on a radio that keeps it warm (speaker_hang_s), the next
ones find it warm.

test_speaker_beep_cold: on a radio with an amplifier hang (the beep.warm view key), waits for the hang to run out,
then the shortest beep (key): the lead-in must cover the amplifier's start.

test_fm_rx_speaker (rf, bench_service): the bench board's FM transmitter sends a carrier, then tone pulses of exact
lengths at several frequencies (--fm-mhz, no sub-audio tone); the radio's VFO receives them. Besides the tones'
checks, nothing may pop as the squelch opens or closes, and no squelch tail may be heard after the carrier."""
import os
import shutil
import subprocess
import time

import pytest

import audio_check as A
from hil_config import rig
from rfoundry_hil import skip

RATE = 16000


class Microphone:
    def __init__(self, device, where):
        self.device = device
        self.dir = where
        self.n = 0

    def start(self, seconds):
        """Start a recording of seconds; returns (process, path). The device is opened before this returns, so the
        caller can play its sound right away."""
        self.n += 1
        path = os.path.join(self.dir, f"rec{self.n}.wav")
        p = subprocess.Popen(["arecord", "-q", "-D", self.device, "-f", "S16_LE", "-r", str(RATE), "-c", "1",
                              "-d", str(int(seconds + 0.999)), path], stderr=subprocess.PIPE)
        time.sleep(0.3)     # arecord opens the device
        if p.poll() is not None:
            raise RuntimeError(f"arecord -D {self.device}: {p.stderr.read().decode(errors='replace').strip()}")
        return p, path

    def finish(self, p, path):
        p.wait(timeout=30)
        if p.returncode != 0:
            raise RuntimeError(f"arecord -D {self.device}: {p.stderr.read().decode(errors='replace').strip()}")
        return path


@pytest.fixture(scope="module")
def microphone(pytestconfig, tmp_path_factory):
    """The recordings stay in pytest's temporary directory (its last few runs are kept) for a look by hand
    (python3 tests/hil/audio_check.py FILE.wav BEEP)"""
    dev = rig(pytestconfig, "microphone", "the speaker tests need")
    if not shutil.which("arecord"):
        skip("no_hw", "arecord (alsa-utils) isn't installed")
    return Microphone(dev, str(tmp_path_factory.mktemp("speaker")))


def _check(r, what, path):
    print(f"{what} ({path}):\n{r.report()}")
    return [f"{what}: {p}" for p in r.problems]


def _quiet(what):
    skip("state", f"{what}: the microphone hears too little of the speaker (volume, beep level, placement)")


def _beep_ready(radio):
    f = radio.view(["features"])
    if f.get("beep.built") != "1":
        skip("not_built", "built without CONFIG_BEEP")
    if int(f.get("set.beep_level") or 0) == 0:
        skip("state", "beep level 0")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")


def _audio(radio):
    """The beep keys of every view section the firmware has (beep.warm is missing on one without an amplifier
    hang): the whole view, as this checkout's section list may be older than the radio's"""
    return {k: v for k, v in radio.view(None).items() if k.startswith("beep.")}


def _play(radio, mic, name, segs):
    length = sum(ms for _, ms in segs) / 1000
    p, path = mic.start(length + 1.2)
    radio.link.beep(name)
    return A.check_wav(mic.finish(p, path), segs), path


@pytest.mark.feature("beep")
def test_speaker_beeps(radio, microphone):
    _beep_ready(radio)
    from rfoundry_debug import BEEPS
    pats = A.beep_patterns()
    names = [n for n in BEEPS if n in pats]
    assert names, "no beep both in the debug link and in beep.c"
    problems, heard = [], 0
    for name in names:
        r, path = _play(radio, microphone, name, pats[name])
        problems += _check(r, name, path)
        heard += not r.quiet
        radio.wait_for(lambda v: v.get("beep.busy") == "0", 3, sections=("features",), what="the beep finished")
    if not heard:
        _quiet("every beep")
    assert not problems, "\n".join(problems)    # one beep not heard among others is a fault, not the room


@pytest.mark.feature("beep")
def test_speaker_beep_cold(radio, microphone):
    _beep_ready(radio)
    a = _audio(radio)
    if "beep.warm" not in a:
        skip("not_built", "no amplifier hang on this firmware (no beep.warm view key)")
    if a["beep.warm"] != "0":
        end = time.monotonic() + 90
        print("waiting for the amplifier hang to run out")
        while _audio(radio).get("beep.warm") != "0":
            if time.monotonic() > end:
                skip("state", "the amplifier stayed warm for 90 s (something keeps the radio busy)")
            time.sleep(1)
    pats = A.beep_patterns()
    r, path = _play(radio, microphone, "key", pats["key"])
    problems = _check(r, "key, amplifier cold", path)
    if r.quiet:
        _quiet("key")
    assert _audio(radio).get("beep.warm") == "1", "the beep didn't warm the amplifier"
    assert not problems, "\n".join(problems)


# the bench's FM program: a carrier first (the squelch opens on it), then pulses of exact lengths
FM_LEAD_S = 0.5
FM_PROGRAM = [{"kind": "silence", "s": FM_LEAD_S},
              {"kind": "tone", "hz": 1000, "s": 0.30, "level": 0.6},
              {"kind": "silence", "s": 0.20},
              {"kind": "tone", "hz": 600, "s": 0.15, "level": 0.6},
              {"kind": "silence", "s": 0.20},
              {"kind": "tone", "hz": 1500, "s": 0.08, "level": 0.6},
              {"kind": "silence", "s": 0.30},
              {"kind": "tone", "hz": 1000, "s": 0.50, "level": 0.6}]


@pytest.mark.feature("fm")
@pytest.mark.rf
@pytest.mark.bench_service
def test_fm_rx_speaker(radio, sdr_bench, microphone, pytestconfig):
    if "VFO_FM" not in radio.features():
        skip("not_built", "built without the FM VFO")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    L = radio.link
    segs = A.program_segs(FM_PROGRAM)[1:]           # the tones from the first one on
    length = sum(p["s"] for p in FM_PROGRAM)
    with radio.vfo_session():
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="VFO tuned")
        tone0 = radio.view(["channel"])["ch.rx_tone"]
        try:
            if tone0 != "none:0":
                L.tone(2, "none", 0)
            time.sleep(0.5)
            with sdr_bench.lease():
                p, path = microphone.start(length + 3.0)
                done = sdr_bench.fm_tx_start(mhz, FM_PROGRAM).wait(length + 15)
                microphone.finish(p, path)
        finally:
            if tone0 != "none:0":
                k, v = tone0.split(":")
                L.tone(2, k, int(v))
    print(f"bench {done}")
    assert not done.get("cut"), f"the bench cut its transmission: {done}"
    # the squelch's opening is in the guard before the first tone, its closing (and any tail) in the one after
    r = A.check_wav(path, segs, guard_ms=(FM_LEAD_S * 1000 - 50, 900))
    problems = _check(r, f"FM {mhz} MHz", path)
    if r.quiet:
        _quiet(f"FM {mhz} MHz")
    end = r.tones[-1].end_s      # the program ends with its last tone: the carrier goes with it
    if end is not None:
        for t0, over, ms in r.pops:
            if t0 > end + 0.02:
                problems = [p for p in problems if f"at {t0:.3f} s" not in p]     # named below instead
                problems.append(f"FM {mhz} MHz: a squelch tail: {ms:.0f} ms of noise {over:.0f} dB over the floor, "
                                f"{(t0 - end) * 1000:.0f} ms after the carrier ended")
    assert not problems, "\n".join(problems)
