# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Speaker tests by microphone (README.md, "Speaker tests"): opt-in with --microphone, an ALSA capture device
(arecord -D) whose microphone is at the radio's speaker; every test skips (opt_in) without one. Each plays a known
sequence of tones and silences and checks the recording with audio_check.check_tones: every tone as long as asked
(nothing lost at its start or end), its level flat, each silence silent, and no pop or noise burst around it.

test_speaker_beeps: every beep the debug link can ask for, one recording each, patterns read from core/radio/beep.c.
The first beep finds the amplifier as the radio left it; on a radio that keeps it warm (speaker_hang_s), the next
ones find it warm. A board's [hil.limits] can allow, with its reason, beep_lead_pops pops ahead of each beep's first
tone (an amplifier's own turn-on pop, which the beep's silent lead keeps off the tone) and beep_dropouts tone dropouts
in the run; each one allowed is printed.

test_speaker_beep_cold: on a radio with an amplifier hang (the beep.warm view key), waits for the hang to run out,
then the shortest beep (key): the lead-in must cover the amplifier's start.

test_speaker_sweep: the speaker test's sweep, 150 Hz to 3.8 kHz (BEEP_SWEEP): prints the speaker's response along it
and fails when the audio jumps on the way to the speaker. By hand on a radio whose cable mutes the speaker:
`rfoundry_debug.py radio beep sweep 15` starts it 15 s later, so the cable can come out first; record it and run
`audio_check.py FILE.wav sweep`.

test_fm_rx_speaker (rf, bench_service): the bench board's FM transmitter sends a carrier, then tone pulses of exact
lengths at several frequencies (--fm-mhz, no sub-audio tone); the radio's VFO receives them. Besides the tones'
checks, nothing may pop as the squelch opens or closes, and a squelch tail after the carrier may last no longer than
the core's squelch takes to close (one poll to see the carrier gone, then RADIO_SQL_CLOSE_POLLS polls of RADIO_POLL_MS,
core/radio/radio_sm.h). A tail is measured while it stays within 10 dB of its peak (audio_check.burst_ms): the room's
echo after it fades for a time set by the room, the volume and the microphone's place. Squelch tail
elimination is not done (docs/spec/radio-features.md 3.5), so the noise until that close is expected; a longer one
(a board whose noise reading lags the carrier) fails, unless the board's [hil.limits] fm_squelch_tail_ms holds it to a
longer one with its reason."""
import math
import os
import shutil
import subprocess
import time

import pytest

from hil_config import rig
import rfoundry_hil
from rfoundry_hil import skip

try:
    import audio_check as A
except ImportError as e:        # audio_check needs numpy
    skip("bench", f"the speaker analysis needs numpy: {e}", module=True)

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
        if p.poll() is not None:     # it couldn't open it: unplugged, renamed or busy, the rig's and not the radio's
            skip("bench", f"the microphone isn't usable: arecord -D {self.device}: "
                          f"{p.stderr.read().decode(errors='replace').strip()}")
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
    m = Microphone(dev, str(tmp_path_factory.mktemp("speaker")))
    m.processed = bool(pytestconfig.getoption("--mic-processed"))
    if m.processed:
        print("--mic-processed: tone starts only; lengths and silences are not measured by microphone")
    return m


def _judge(mic, path, segs, **kw):
    """The recording's check: the starts alone when the microphone's processing spoils the rest (--mic-processed)"""
    return A.check_wav_onsets(path, segs) if mic.processed else A.check_wav(path, segs, **kw)


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


def _stream(radio):
    """The PCM stream's late and dropped blocks and the blocks it played (audio.late, audio.dropped, audio.blocks), or
    None on firmware that doesn't count them"""
    a = radio.view(["audio"])
    if "audio.late" not in a:
        return None
    return int(a["audio.late"]), int(a.get("audio.dropped") or 0), int(a.get("audio.blocks") or 0)


def _block_ms():
    """AUDIO_BLOCK_MS, read from the source so the test copies no number"""
    import re
    src = open(os.path.join(A.ROOT, "hal", "hal_audio.h")).read()
    return int(re.search(r"#define\s+AUDIO_BLOCK_MS\s+(\d+)", src).group(1))


def _play(radio, mic, name, segs):
    """The recording's check, with the stream's own fault added when it played a block late or dropped one: the
    radio's, whatever the microphone made of it"""
    length = sum(ms for _, ms in segs) / 1000
    s0 = _stream(radio)
    p, path = mic.start(length + 1.2)
    radio.link.beep(name)
    r = _judge(mic, mic.finish(p, path), segs)
    s1 = _stream(radio)
    if s0 is not None and s1 is not None:
        if s1[:2] != s0[:2]:
            r.problems.append(f"the audio stream played {s1[0] - s0[0]} block(s) late and dropped {s1[1] - s0[1]} "
                              f"(audio.late, audio.dropped)")
        # the pattern's length, where the microphone can't say: the stream ran at least that long
        need = math.ceil(length * 1000 / _block_ms())
        if mic.processed and s1[2] - s0[2] < need:
            r.problems.append(f"the audio stream played {s1[2] - s0[2]} blocks, the pattern needs at least {need} "
                              f"(audio.blocks)")
    elif mic.processed:
        print(f"{name}: this firmware has no stream counters: its length and silences are unchecked")
    return r, path


@pytest.mark.feature("beep")
def test_speaker_beeps(radio, microphone):
    _beep_ready(radio)
    from rfoundry_debug import BEEPS
    pats = A.beep_patterns()
    names = [n for n in BEEPS if n in pats]
    assert names, "no beep both in the debug link and in beep.c"
    limits = rfoundry_hil.board_hil(rfoundry_hil.board_id(radio.version().get("board"))).get("limits", {})
    lead_pops, dropouts = (limits.get(k) or {"value": 0, "reason": None} for k in ("beep_lead_pops", "beep_dropouts"))
    problems, heard, dropped = [], 0, 0
    for name in names:
        r, path = _play(radio, microphone, name, pats[name])
        mine = _check(r, name, path)
        first = r.tones[0].start_s if r.tones and r.tones[0].start_s is not None else None
        ahead = [p for p in r.pops if first is not None and p[0] + p[2] / 1000 <= first + 0.005]
        for t0, over, ms in ahead[:int(lead_pops["value"])]:
            mine = [p for p in mine if f"a pop or noise burst at {t0:.3f} s" not in p]
            print(f"{name}: the pop {over:.0f} dB over the floor ahead of the first tone allowed (the board's "
                  f"[hil.limits] beep_lead_pops: {lead_pops['reason']})")
        for p in [p for p in mine if ": a dropout:" in p]:
            if dropped < int(dropouts["value"]):
                dropped += 1
                mine.remove(p)
                print(f"{p}: allowed (the board's [hil.limits] beep_dropouts: {dropouts['reason']})")
        problems += mine
        heard += not r.quiet
        radio.wait_for(lambda v: v.get("beep.busy") == "0", 3, sections=("features",), what="the beep finished")
    if not heard:
        _quiet("every beep")
    assert not problems, "\n".join(problems)    # one beep not heard among others is a fault, not the room


@pytest.mark.feature("beep", optional=True)    # only a board with an amplifier hang has one to check
def test_speaker_beep_cold(radio, microphone):
    _beep_ready(radio)
    a = _audio(radio)
    if "beep.warm" not in a:
        skip("no_hw", "no amplifier hang on this radio (no beep.warm view key)")
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


@pytest.mark.feature("beep", optional=True)    # a radio whose beeps are its own tones (beep.tone 1) has no sweep
def test_speaker_sweep(radio, microphone):
    """The speaker test's sweep (BEEP_SWEEP, a tone rising through the band): the speaker's response along it, printed
    for the radio's records, and the sweep whole: audio that jumps (samples lost, repeated or out of order on the way
    to the speaker) spreads energy across the band, which a tone check at a few pitches can miss"""
    _beep_ready(radio)
    if _audio(radio).get("beep.tone") == "1":
        skip("no_hw", "the beeps are the radio's own tones (beep.tone 1): it plays fixed pitches, no sweep")
    from rfoundry_debug import DebugLinkError
    lo, hi, ms = A.sweep_spec()
    length = sum(m for _, m in A.beep_patterns()["sweep"]) / 1000
    p, path = microphone.start(length + 1.5)
    try:
        radio.link.beep("sweep")
    except DebugLinkError as e:
        microphone.finish(p, path)
        skip("not_built", f"no sweep in this firmware ({e})")
    x, fs = A.load_wav(microphone.finish(p, path))
    r = A.check_sweep(x, fs, lo, hi, ms)
    print(f"sweep ({path}):\n{r.report()}")
    if r.quiet:
        _quiet("the sweep")
    assert not r.problems, "\n".join(r.problems)


# the bench's FM program: a carrier first (the squelch opens on it), then pulses of exact lengths
FM_LEAD_S = 0.5


def squelch_close_ms():
    """The longest the core's squelch stays open after the carrier: up to one poll before a poll sees the carrier gone,
    then the close polls (read from radio_sm.h so the test copies no number), plus one pop analysis frame for where the
    burst's edges fall."""
    import re
    src = open(os.path.join(A.ROOT, "core", "radio", "radio_sm.h")).read()
    get = lambda name: int(re.search(rf"#define\s+{name}\s+(\d+)", src).group(1))  # noqa: E731
    return (get("RADIO_SQL_CLOSE_POLLS") + 1) * get("RADIO_POLL_MS") + A.POP_WIN_MS
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
    r = _judge(microphone, path, segs, guard_ms=(FM_LEAD_S * 1000 - 50, 900))
    problems = _check(r, f"FM {mhz} MHz", path)
    if r.quiet:
        _quiet(f"FM {mhz} MHz")
    end = r.tones[-1].end_s      # the program ends with its last tone: the carrier goes with it
    if end is not None:
        # a tail's own length, without the room's echo after it (audio_check.burst_ms): how long the squelch took
        x, fs = A.load_wav(path)
        freqs = [h for h, _ in segs if h]
        tails = [(t0, over, A.burst_ms(x, fs, freqs, t0, ms), ms) for t0, over, ms in r.pops if t0 > end + 0.02]
        allowed, why = squelch_close_ms(), None
        o = rfoundry_hil.board_hil(rfoundry_hil.board_id(radio.version().get("board"))).get("limits", {}).get(
            "fm_squelch_tail_ms")
        if o:
            allowed, why = o["value"], o["reason"]
            print(f"squelch tail limit {allowed:.0f} ms from the board's [hil.limits]: {why}")
        for i, (t0, over, ms, echo_ms) in enumerate(tails):
            problems = [p for p in problems if f"at {t0:.3f} s" not in p]     # named below instead
            what = (f"{ms:.0f} ms of noise {over:.0f} dB over the floor ({echo_ms:.0f} ms with the room's echo), "
                    f"{(t0 - end) * 1000:.0f} ms after the last tone")
            if i == 0 and ms <= allowed:
                print(f"squelch tail: {what} (within {allowed:.0f} ms{f': {why}' if why else ', the squelch close time'}"
                      ": expected)")
            else:
                problems.append(f"FM {mhz} MHz: a squelch tail: {what}" +
                                (f" (more than {allowed:.0f} ms{f': {why}' if why else ', the squelch close time'})"
                                 if i == 0 else ""))
    assert not problems, "\n".join(problems)


AM_LEAD_S = 0.5
AM_PROGRAM = [{"kind": "tone", "hz": 1000, "s": 3.0, "level": 1.0}]


@pytest.mark.feature("am_rx")
@pytest.mark.rf
@pytest.mark.bench_service
def test_am_rx_speaker(radio, sdr_bench, microphone, pytestconfig):
    """The bench board's AM (1 kHz at depth 0.3, --am-mhz) received in AM by the VFO: the speaker plays a steady
    1 kHz tone for the carrier's length, with nothing before or after it."""
    from test_am_rx import am_tx_start, am_vfo_ready, tune_am
    mhz, hz = am_vfo_ready(radio, pytestconfig)
    segs = A.program_segs(AM_PROGRAM)
    length = AM_PROGRAM[0]["s"] + 2 * AM_LEAD_S
    with radio.vfo_session():
        tune_am(radio, hz)
        time.sleep(0.5)
        with sdr_bench.lease():
            p, path = microphone.start(length + 3.0)
            done = am_tx_start(sdr_bench, mhz, AM_PROGRAM, depth=0.3, lead_s=AM_LEAD_S, tail_s=AM_LEAD_S).wait(length + 15)
            microphone.finish(p, path)
    print(f"bench {done}")
    assert not done.get("cut"), f"the bench cut its transmission: {done}"
    r = _judge(microphone, path, segs, guard_ms=(AM_LEAD_S * 1000 - 50, 900))
    problems = _check(r, f"AM {mhz} MHz", path)
    if r.quiet:
        _quiet(f"AM {mhz} MHz")
    assert not problems, "\n".join(problems)
