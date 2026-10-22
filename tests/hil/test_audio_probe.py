# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""USB audio feasibility on the DM-1701: which HR_C6000 settings put FM audio on the I2S link between the chip and the
MCU (docs/spec/hardware-dm1701.md 21.3). Needs firmware built with CONFIG_AUDIO_PROBE=y (skipped otherwise): the debug
link's audio probe (docs/spec/debuglink.md 3.14) reads and writes HR_C6000 registers and reports the PCM levels of
both I2S channels. These are measurements, not regression tests: they print a table, and assert only that the
measurement ran. Every register they touch is read first and written back in a finally block; the radio's own
mode changes (RX / TX set-up) rewrite them anyway.

- test_probe_stream_in_fm (radio only): the I2S clock runs on an FM channel, so a PCM stream can exist in FM at all.
- test_probe_fm_rx_variants (radio only, NOAA audible as for test_fm): for each candidate setting, the RMS of both
  I2S channels with the NOAA signal and with the squelch closed on a quiet frequency. A setting whose RMS rises
  clearly with the signal carries FM receive audio to the MCU.
- test_probe_fm_tx_variants (--allow-rf --allow-service-stop): the radio keys FM on --fm-mhz with the microphone
  muted while the probe sends a 1 kHz tone into the I2S link; the SDR bench measures the voice band deviation per
  candidate setting. A setting with deviation well above the muted baseline puts MCU audio into the modulator.

The HR_C6000's own FM sample path (hardware-dm1701.md 21.3, candidate C): the manual's RAM tables give two 512-byte "FM data"
buffers in each of the TX and RX RAMs (page 2, 0x030-0x22F and 0x230-0x42F; the TX ones "can be written externally
for FM send"), and 0x82 has FM-mode flags bit 7 "sending data request" and bit 1 "receiving data". Neither the sample
format nor the control bit that picks the RAM over the codec is documented, so these tests try the likely ones:

- test_probe_fm_ram_rx (radio only, NOAA): how often each 0x82 flag comes with 0x81 as set and with the FM flags
  unmasked, then 4 s of audio from the RX RAM (8 kHz big endian 16-bit, found on the bench) with the NOAA signal, on
  a quiet frequency, and there with de-emphasis off. Speech shows as a varying envelope, noise as a steady one (the
  path is ungated); the noise tilt shows whether 0x34's de-emphasis comes before the tap.
- test_probe_fm_ram_tx_variants (--allow-rf --allow-service-stop): both TX RAM buffers hold a whole number of cycles
  of a 1 kHz tone (8 samples per cycle at 8 kHz), so a chip that replays them sends a steady tone without the MCU
  refilling them. Keyed with the microphone muted, per candidate control setting: the 0x82 flag rate and the voice
  band deviation the SDR bench measures.

The prompt tone path (manual appendix A1) and a register sweep, the last candidates for FM transmit:

- test_probe_music_fifo (radio only): the FIFO's drain rate with the path (0x06 bit 1) closed and open.
- test_probe_fm_tx_music (--allow-rf --allow-service-stop): a 1400 Hz tone through the FIFO, keyed, with and without
  the codec's DAC-ADC pass-through.
- test_probe_fm_tx_sweep (--allow-rf --allow-service-stop): each bit of the codec area registers flipped for 0.5 s
  while keyed, with tones on I2S and in the TX RAM; the SDR's tone timeline says which flip put which on air.

Results on the DM-1701 (2026-10-12): docs/spec/hardware-dm1701.md 21.3.1.
"""
import math
import re
import time

import pytest

from odr_hil import skip

PAGE = 4
TONE_HZ = 1000
TONE_AMP = 8000          # about -12 dBFS
PTT_S = 3.0

# FM receive candidates: (name, register, mask to set, mask to clear). HR_C6000 manual, register table (Chinese
# section): 0xE0 bit 6 LINEIN1 (the microphone on these radios), bit 5 LINEIN2 (wiring unknown), 0xE3 bit 0 the
# codec's internal DAC-ADC pass-through
RX_VARIANTS = [
    ("as set (0xE0=0x89)", None, 0, 0),
    ("LINEIN2 on (0xE0 |= 0x20)", 0xE0, 0x20, 0),
    ("LINEIN1 on (0xE0 |= 0x40)", 0xE0, 0x40, 0),
    ("codec pass-through (0xE3 |= 0x01)", 0xE3, 0x01, 0),
]

# FM transmit candidates: applied while keyed (the TX set-up writes 0xE0 = 0xC9), microphone (LINEIN1) muted in all.
# (name, [(register, set, clear)], amplitude, tone Hz): the LINEIN2 rows without a tone and at another frequency tell
# MCU audio from whatever else LINEIN2 might hear
TX_VARIANTS = [
    ("mic muted, no tone", [], 0, TONE_HZ),
    ("mic muted, tone", [], TONE_AMP, TONE_HZ),
    ("mic muted, tone, codec pass-through (0xE3 |= 0x01)", [(0xE3, 0x01, 0)], TONE_AMP, TONE_HZ),
    ("mic muted, no tone, LINEIN2 on (0xE0 |= 0x20)", [(0xE0, 0x20, 0)], 0, TONE_HZ),
    ("mic muted, tone, LINEIN2 on (0xE0 |= 0x20)", [(0xE0, 0x20, 0)], TONE_AMP, TONE_HZ),
    ("mic muted, 600 Hz tone, LINEIN2 on", [(0xE0, 0x20, 0)], TONE_AMP, 600),
]


# FM data buffers in page 2 (manual 5.3 tables): two halves of 512 bytes, written = TX RAM, read = RX RAM
RAM_PAGE = 2
RAM_FM = 0x030
RAM_FM_LEN = 1024
FM_FLAGS = 0x82          # 0x82 bit 7 "FM: sending data request", bit 1 "FM: receiving data"
FM_FLAG_MASK = FM_FLAGS  # in 0x81 (interrupt mask, 0 = masked): FM mode sets 0x19, which masks both

# FM transmit through the RAM: control settings tried while keyed with the microphone muted and the TX RAM holding
# the tone. (name, [(register, set mask, clear mask) or (register, None, value)], byte order of the tone)
RAM_TX_VARIANTS = [
    ("RAM tone, nothing else", [], "<"),
    ("RAM tone, FM flags unmasked (0x81 |= 0x82)", [(0x81, FM_FLAG_MASK, 0)], "<"),
    ("RAM tone, 0xDB |= 0x02 (send FM voice)", [(0x81, FM_FLAG_MASK, 0), (0xDB, 0x02, 0)], "<"),
    ("RAM tone, 0x60 = 0xC0 (voice + data request)", [(0x81, FM_FLAG_MASK, 0), (0x60, None, 0xC0)], "<"),
    ("RAM tone, 0x60 = 0x40 (data request)", [(0x81, FM_FLAG_MASK, 0), (0x60, None, 0x40)], "<"),
    ("RAM tone, 0x06 |= 0x01 (MCU vocoder control)", [(0x81, FM_FLAG_MASK, 0), (0x06, 0x01, 0)], "<"),
    ("RAM tone big endian, 0xDB |= 0x02", [(0x81, FM_FLAG_MASK, 0), (0xDB, 0x02, 0)], ">"),
    ("RAM tone BE, 0x06 |= 0x04 (CodeCMode external)", [(0x81, FM_FLAG_MASK, 0), (0x06, 0x04, 0)], ">"),
    ("RAM tone BE, 0x06 |= 0x08 (SPIFrom)", [(0x81, FM_FLAG_MASK, 0), (0x06, 0x08, 0)], ">"),
    ("RAM tone BE, 0x06 |= 0x10 (VocoderFrom)", [(0x81, FM_FLAG_MASK, 0), (0x06, 0x10, 0)], ">"),
    ("RAM tone BE, 0xE0 &= ~0x80 (chip controls codec)", [(0x81, FM_FLAG_MASK, 0), (0xE0, 0, 0x80)], ">"),
    ("RAM tone BE, 0xE2 |= 0x10 (codec ADC off)", [(0x81, FM_FLAG_MASK, 0), (0xE2, 0x10, 0)], ">"),
]
RAM_TX_SAVED = {0x81, 0xDB, 0x60, 0x06, 0xE0, 0xE2}


def tone_bytes(order, amp=TONE_AMP, n=RAM_FM_LEN // 2):
    """n samples of a TONE_HZ sine at 8 kHz, 16-bit in byte order order ('<' or '>')"""
    import struct
    x = [round(amp * math.sin(2 * math.pi * TONE_HZ * i / 8000)) for i in range(n)]
    return struct.pack(f"{order}{n}h", *x)


def ram_capture(link, seconds):
    """Continuous FM receive audio from the RX RAM buffers: read both halves twice in a row; the half that differs
    is being filled, the other one is complete. Big endian 16-bit, 8 kHz (measured: one half per 32 ms).
    Returns (samples, fraction of the time covered)."""
    import struct
    halves, last = [], None
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        a = link.probe_ram(RAM_PAGE, RAM_FM, RAM_FM_LEN)
        b = link.probe_ram(RAM_PAGE, RAM_FM, RAM_FM_LEN)
        moving = [a[:512] != b[:512], a[512:] != b[512:]]
        if moving[0] == moving[1]:
            continue
        done = b[512:] if moving[0] else b[:512]
        if done != last:
            halves.append(done)
            last = done
    el = time.monotonic() - t0
    x = [v for h in halves for v in struct.unpack(">256h", h)]
    return x, len(halves) * 0.032 / el


def audio_stats(x):
    """RMS, the coefficient of variation of the 50 ms envelope (speech about 0.5 and up, steady noise about 0.1) and
    the power at 2.5 kHz relative to 500 Hz in dB (the noise tilt: negative with de-emphasis)"""
    env = [math.sqrt(sum(v * v for v in x[i:i + 400]) / 400) for i in range(0, len(x) - 400, 400)]
    m = sum(env) / max(len(env), 1)
    cv = math.sqrt(sum((e - m) ** 2 for e in env) / max(len(env), 1)) / max(m, 1)
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / 255) for i in range(256)]

    def power(f):
        acc = 0.0
        for k in range(0, len(x) - 256, 256):
            seg = x[k:k + 256]
            mean = sum(seg) / 256
            re = sum((seg[i] - mean) * win[i] * math.cos(2 * math.pi * f * i / 8000) for i in range(256))
            im = sum((seg[i] - mean) * win[i] * math.sin(2 * math.pi * f * i / 8000) for i in range(256))
            acc += re * re + im * im
        return acc

    tilt = 10 * math.log10(max(power(2500), 1e-9) / max(power(500), 1e-9))
    return {"rms": math.sqrt(sum(v * v for v in x) / max(len(x), 1)), "cv": cv, "tilt": tilt}


def watch_text(w):
    bits = ", ".join(f"bit {b} {c * 1000 / max(w['elapsed_ms'], 1):.1f}/s" for b, (c, _) in w["bits"].items() if c)
    return f"{bits or 'no flags'}; RAM word changed {w['ram_changes']}x in {w['elapsed_ms']} ms"


@pytest.fixture
def probe(radio):
    from odr_debug import DebugLinkError
    try:
        radio.link.probe_status()
    except DebugLinkError as e:
        skip("not_built", f"firmware without the audio probe (build with CONFIG_AUDIO_PROBE=y): {e}")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    try:
        yield radio
    finally:
        try:
            radio.link.probe_stop()
        except DebugLinkError:
            pass


def regs_saved(link, regs):
    return {r: link.probe_reg(PAGE, r) for r in regs}


def apply(link, reg, set_mask, clear_mask):
    v = link.probe_reg(PAGE, reg)
    return link.probe_reg(PAGE, reg, (v | set_mask) & ~clear_mask & 0xFF)


def levels(link, settle=0.4, n=5):
    """Mean RMS and the largest peak of both channels over n status reads"""
    time.sleep(settle)
    acc = {"left": [0, 0], "right": [0, 0]}
    for _ in range(n):
        st = link.probe_status()
        for ch in acc:
            acc[ch][0] += st[ch]["rms"]
            acc[ch][1] = max(acc[ch][1], st[ch]["peak"])
        time.sleep(0.1)
    return {ch: (acc[ch][0] / n, acc[ch][1]) for ch in acc}


@pytest.mark.feature("audio_probe", optional=True)
def test_probe_stream_in_fm(probe, pytestconfig):
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    hz = round(pytestconfig.getoption("--noaa-mhz") * 1e6)
    with probe.vfo_session():
        L.vfo_set(hz)
        probe.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        L.probe_start(0, 0)
        b0 = L.probe_status()["blocks"]
        time.sleep(1.0)
        st = L.probe_status()
        print(f"FM mode: stream running={st['running']}, {st['blocks'] - b0} blocks in 1 s; levels {st}")
        assert st["running"] and st["blocks"] - b0 >= 40, f"no PCM stream on an FM channel: {st}"


def test_probe_fm_rx_variants(probe, pytestconfig):
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    noaa = round(pytestconfig.getoption("--noaa-mhz") * 1e6)
    quiet = noaa + 2_000_000          # 2 MHz away: nothing there at the bench, squelch closed (checked)
    rows = []
    with probe.vfo_session():
        saved = regs_saved(L, {0xE0, 0xE3, 0x36})
        try:
            L.probe_start(0, 0)
            for name, reg, set_mask, clear_mask in RX_VARIANTS:
                res = {}
                for tag, hz in (("signal", noaa), ("quiet", quiet)):
                    L.vfo_set(hz)
                    probe.wait_for(lambda v: v.get("rx_hz") == str(hz), what=f"VFO on {hz}")
                    if tag == "signal":
                        probe.wait_for(lambda v: v.get("state") == "rx", timeout=6, what="NOAA squelch open")
                    if reg is not None:
                        apply(L, reg, set_mask, clear_mask)
                    res[tag] = levels(L)
                    res[tag + "_state"] = probe.core().get("state")
                    for r, v in saved.items():
                        L.probe_reg(PAGE, r, v)
                rows.append((name, res))
        finally:
            for r, v in saved.items():
                L.probe_reg(PAGE, r, v)
            L.probe_stop()
    print("\nFM RX on I2S (RMS / peak, full scale 32767):")
    print(f"{'variant':<36} {'L signal':>14} {'L quiet':>14} {'R signal':>14} {'R quiet':>14}")
    for name, res in rows:
        cells = []
        for ch in ("left", "right"):
            for tag in ("signal", "quiet"):
                rms, peak = res[tag][ch]
                cells.append(f"{rms:6.0f}/{peak:6d}")
        print(f"{name:<36} " + " ".join(f"{c:>14}" for c in cells))
        for ch in ("left", "right"):
            s, q = res["signal"][ch][0], res["quiet"][ch][0]
            if s > 200 and s > 4 * max(q, 1):
                print(f"  -> {name}: {ch} channel carries receive audio (RMS {s:.0f} vs {q:.0f})")
    assert len(rows) == len(RX_VARIANTS)


def keyed_runs(probe, sdr_bench, pytestconfig, runs, tag, ptt_s=PTT_S, **analyze):
    """Key FM on --fm-mhz once per run with the microphone muted. Each run is (name, before, during): before() runs
    unkeyed, during() keyed once the TX set-up has written its registers and returns a note (or None). Returns
    [(name, (peak 99.5 %, peak 99.9 %, rms) of the voice band deviation in Hz or None, note)]."""
    import re
    L = probe.link
    mhz = pytestconfig.getoption("--fm-mhz")
    hz = round(mhz * 1e6)
    rows = []
    with probe.vfo_session():
        L.vfo_set(hz)
        probe.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        probe.lowest_power()
        try:
            for i, (name, before, during) in enumerate(runs):
                before()
                rdir = sdr_bench.run_dir(f"hil_aprobe_{tag}{i}")
                with sdr_bench.lease():
                    cap = sdr_bench.capture_start(rdir, mhz, seconds=ptt_s + 5)
                    L.ptt(True, int(ptt_s * 1000) + 1500)
                    try:
                        probe.wait_for(lambda v: v.get("state") == "tx", timeout=2, what="keyed")
                        time.sleep(0.2)          # after the TX set-up has written its registers
                        apply(L, 0xE0, 0, 0x40)  # microphone (LINEIN1) off
                        t0 = time.monotonic()
                        note = during()
                        time.sleep(max(0.0, ptt_s - (time.monotonic() - t0)))
                    finally:
                        L.ptt(False)
                    probe.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
                    sdr_bench.capture_wait()
                L.probe_stop()
                rc, text = sdr_bench.analyze_fm(cap, bw_khz=12.5, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz,
                                                tones=True, **analyze)
                m = re.search(r"voice 300-3000 Hz: peak (\d+) Hz \(99\.5%\), (\d+) Hz \(99\.9%\), rms (\d+) Hz", text)
                extra = re.findall(r"(SINAD [-\d.]+ dB|noise 4-7\.5 kHz: rms \d+ Hz|wideband tone rms \d+ Hz)", text)
                if extra:
                    note = "; ".join(([note] if note else []) + extra)
                tones = re.findall(r"tone (\d+(?:\.\d)?) Hz [\d.]+-[\d.]+ s \(([\d.]+) s\)", text)
                tone_note = "tones " + ", ".join(f"{hz} Hz {d} s" for hz, d in tones) if tones else "no steady tone"
                note = f"{note}; {tone_note}" if note else tone_note
                rows.append((name, tuple(int(x) for x in m.groups()) if m else None, note))
                if not m:
                    print(text)
        finally:
            L.ptt(False)
            L.probe_stop()
    return rows


def print_tx_rows(title, rows, base_row=0):
    print(f"\n{title} (voice band deviation, peak 99.5 % / rms, Hz):")
    base = rows[base_row][1][2] if rows and rows[base_row][1] else None
    for name, r, note in rows:
        print(f"{name:<52} {'-' if r is None else f'{r[0]:5d} / {r[2]:5d}'}{'  ' + note if note else ''}")
        if r and base is not None and r[2] > 4 * max(base, 20):
            print(f"  -> {name}: MCU audio reaches the modulator (rms {r[2]} Hz vs {base} Hz baseline)")


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_variants(probe, sdr_bench, pytestconfig):
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link

    def run(regs, amp, hz):
        def during():
            for reg, s_, c in regs:
                apply(L, reg, s_, c)
        return (lambda: L.probe_start(hz, amp)), during

    runs = [(name, *run(regs, amp, hz)) for name, regs, amp, hz in TX_VARIANTS]
    rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "tx")
    print_tx_rows("FM TX from the I2S link", rows)
    assert len(rows) == len(TX_VARIANTS)


def test_probe_fm_ram_rx(probe, pytestconfig):
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    noaa = round(pytestconfig.getoption("--noaa-mhz") * 1e6)
    quiet = noaa + 2_000_000
    res = {}
    with probe.vfo_session():
        saved = regs_saved(L, {0x81, 0x34})
        try:
            for tag, hz, regs in (("signal", noaa, {}), ("quiet", quiet, {}),
                                  ("quiet, de-emphasis off (0x34 &= ~0x20)", quiet, {0x34: saved[0x34] & ~0x20})):
                L.vfo_set(hz)
                probe.wait_for(lambda v: v.get("rx_hz") == str(hz), what=f"VFO on {hz}")
                if hz == noaa:
                    probe.wait_for(lambda v: v.get("state") == "rx", timeout=6, what="NOAA squelch open")
                for r, v in saved.items():
                    L.probe_reg(PAGE, r, v)
                masked = L.probe_watch(1000, FM_FLAGS, ram_addr=RAM_FM)
                apply(L, 0x81, FM_FLAG_MASK, 0)
                unmasked = L.probe_watch(1000, FM_FLAGS, ram_addr=RAM_FM)
                for r, v in regs.items():
                    L.probe_reg(PAGE, r, v)
                x, cover = ram_capture(L, 4.0)
                res[tag] = (masked, unmasked, cover, audio_stats(x))
        finally:
            for r, v in saved.items():
                L.probe_reg(PAGE, r, v)
    print("\nFM RX through the RX RAM FM buffers (page 2 0x030-0x42F, big endian 16-bit, 8 kHz):")
    for tag, (masked, unmasked, cover, st) in res.items():
        print(f"{tag}: 0x81 as set: {watch_text(masked)}; FM flags unmasked: {watch_text(unmasked)}")
        print(f"    {cover * 100:.0f} % captured, RMS {st['rms']:.0f}, envelope CV {st['cv']:.2f}, "
              f"2.5 kHz vs 500 Hz {st['tilt']:+.0f} dB")
    sig, q = res["signal"][3], res["quiet"][3]
    if sig["cv"] > 0.3 and q["cv"] < 0.2:
        print(f"  -> the RX RAM carries receive audio (envelope CV {sig['cv']:.2f} vs noise {q['cv']:.2f}), ungated")
    off = res["quiet, de-emphasis off (0x34 &= ~0x20)"][3]
    if off["tilt"] > q["tilt"] + 10:
        print(f"  -> the RX RAM tap follows 0x34's de-emphasis (noise tilt {q['tilt']:+.0f} -> {off['tilt']:+.0f} dB)")
    assert res["signal"][2] > 0.9, "RX RAM capture lost halves"


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_ram_tx_variants(probe, sdr_bench, pytestconfig):
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    saved = regs_saved(L, RAM_TX_SAVED)

    def restore():
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)

    def run(regs, data):
        def before():
            restore()            # each variant starts from the radio's own settings
            L.probe_ram(RAM_PAGE, RAM_FM, data=data)

        def during():
            L.probe_ram(RAM_PAGE, RAM_FM, data=data)   # again, in case the TX set-up cleared it
            for reg, s_, c in regs:
                if s_ is None:
                    L.probe_reg(PAGE, reg, c)
                else:
                    apply(L, reg, s_, c)
            return watch_text(L.probe_watch(1000, FM_FLAGS, ram_addr=RAM_FM))

        return before, during

    runs = [("mic muted, RAM silent (baseline)", *run([], bytes(RAM_FM_LEN)))]
    runs += [(name, *run(regs, tone_bytes(order) * 2)) for name, regs, order in RAM_TX_VARIANTS]
    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "ramtx")
    finally:
        restore()
    print_tx_rows("FM TX from the TX RAM FM buffers", rows)
    assert len(rows) == len(runs)


# --- FM transmit: the prompt tone path, and a sweep of the undocumented bits ---

MUSIC_HZ = 1400
MUSIC_MS = 2000


@pytest.mark.feature("audio_probe", optional=True)
def test_probe_music_fifo(probe, pytestconfig):
    """Radio only: with the prompt tone path open (0x06 bit 1), does the chip drain the FIFO (about 8000 samples a
    second = 250 writes of 32), and with it closed, not? Tells whether the path runs at all in FM."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    rows = []
    with probe.vfo_session():
        saved = regs_saved(L, {0x06})
        try:
            for name, v in (("path closed", saved[0x06] & ~0x02), ("path open (0x06 |= 0x02)", saved[0x06] | 0x02)):
                L.probe_reg(PAGE, 0x06, v)
                m = L.probe_music(1000, MUSIC_HZ, 0)        # silence: the speaker stays quiet
                rows.append((name, m))
        finally:
            for r, v in saved.items():
                L.probe_reg(PAGE, r, v)
    print("\nPrompt tone FIFO (page 3 address 0, 32 samples per write when 0x88 bit 0 reads 0):")
    for name, m in rows:
        rate = m["writes"] * 32 * 1000 / max(m["elapsed_ms"], 1)
        print(f"{name:<28} {m['writes']:5d} writes in {m['elapsed_ms']} ms = {rate:6.0f} samples/s, {m['polls']} polls")
    assert len(rows) == 2


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_music(probe, sdr_bench, pytestconfig):
    """The prompt tone path into the modulator: the FIFO feeds the codec DAC; with 0xE3 bit 0 the DAC loops into the
    ADC that feeds the FM transmit chain. Keyed with the microphone muted, a 1400 Hz tone through the FIFO."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    saved = regs_saved(L, {0x06, 0xE3})

    def restore():
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)

    def run(regs, le):
        def during():
            for reg, s_, c in regs:
                apply(L, reg, s_, c)
            m = L.probe_music(MUSIC_MS, MUSIC_HZ, TONE_AMP, little_endian=le)
            return f"{m['writes'] * 32 * 1000 / max(m['elapsed_ms'], 1):.0f} samples/s into the FIFO"
        return restore, during

    runs = [(name, *run(regs, le)) for name, regs, le in (
        ("FIFO, path closed (baseline)", [], False),
        ("FIFO, path open (0x06 |= 0x02)", [(0x06, 0x02, 0)], False),
        ("FIFO, path open, DAC-ADC pass-through (0xE3 |= 0x01)", [(0x06, 0x02, 0), (0xE3, 0x01, 0)], False),
        ("FIFO little endian, path open, pass-through", [(0x06, 0x02, 0), (0xE3, 0x01, 0)], True),
    )]
    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "music")
    finally:
        restore()
    print_tx_rows(f"FM TX from the prompt tone FIFO ({MUSIC_HZ} Hz)", rows)
    assert len(rows) == len(runs)


# Registers whose bits the sweep flips one at a time while keyed: the codec and audio routing area, none of the
# modulation, reference or synthesizer registers (0x04, 0x45-0x4A, 0x35, 0x3E stay as the TX set-up wrote them)
SWEEP_REGS = [0x25, 0x26, 0xE1, 0xE2, 0xE3, 0xE5, 0x0D, 0x0E, 0x0F, 0x38]
SWEEP_ON_S, SWEEP_GAP_S = 0.5, 0.25
SWEEP_PER_KEY = 8
SWEEP_MARK_S = 1.5        # LINEIN2's tone takes most of a second to build up
I2S_HZ = 600             # the I2S stream's tone during the sweep; the TX RAM holds TONE_HZ, the marker is LINEIN2's
MARK_HZ = (2300, 2800)  # LINEIN2's tone: 2.49 kHz one day, 2.60 kHz the next


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_sweep(probe, sdr_bench, pytestconfig):
    """Keyed with the microphone muted, a 600 Hz tone on the I2S stream and a 1000 Hz tone in both TX RAM halves
    (big endian, FM flags unmasked), each bit of SWEEP_REGS flipped for 0.5 s in turn. Each key-up starts with
    LINEIN2 on for 1.5 s, whose own 2.49 kHz tone marks the time (it shows up for the last 0.3-0.8 s of it); a tone the SDR finds during a flip says which bit
    and which source (600 Hz: I2S, 1000 Hz: TX RAM) reach the modulator."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    import re
    L = probe.link
    mhz = pytestconfig.getoption("--fm-mhz")
    hz = round(mhz * 1e6)
    flips = [(r, b) for r in SWEEP_REGS for b in range(8)]
    batches = [flips[i:i + SWEEP_PER_KEY] for i in range(0, len(flips), SWEEP_PER_KEY)]
    key_s = SWEEP_MARK_S + 0.2 + len(batches[0]) * (SWEEP_ON_S + SWEEP_GAP_S) + 0.3
    hits, unmapped, missing_marker = [], [], []
    with probe.vfo_session():
        L.vfo_set(hz)
        probe.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        probe.lowest_power()
        saved = regs_saved(L, set(SWEEP_REGS) | {0x81, 0xE0})
        try:
            for k, batch in enumerate(batches):
                for r, v in saved.items():
                    L.probe_reg(PAGE, r, v)
                L.probe_ram(RAM_PAGE, RAM_FM, data=tone_bytes(">") * 2)
                L.probe_start(I2S_HZ, TONE_AMP)
                rdir = sdr_bench.run_dir(f"hil_aprobe_sweep{k}")
                with sdr_bench.lease():
                    cap = sdr_bench.capture_start(rdir, mhz, seconds=key_s + 5)
                    L.ptt(True, int(key_s * 1000) + 1500)
                    try:
                        probe.wait_for(lambda v: v.get("state") == "tx", timeout=2, what="keyed")
                        time.sleep(0.2)
                        apply(L, 0xE0, 0, 0x40)          # microphone off
                        apply(L, 0x81, FM_FLAG_MASK, 0)
                        e0 = L.probe_reg(PAGE, 0xE0)
                        t_mark = time.monotonic()
                        L.probe_reg(PAGE, 0xE0, e0 | 0x20)
                        time.sleep(SWEEP_MARK_S)
                        L.probe_reg(PAGE, 0xE0, e0)
                        time.sleep(0.2)
                        slots = []
                        for r, b in batch:
                            v = L.probe_reg(PAGE, r)
                            t_on = time.monotonic() - t_mark
                            L.probe_reg(PAGE, r, v ^ (1 << b))
                            time.sleep(SWEEP_ON_S)
                            L.probe_reg(PAGE, r, v)
                            slots.append((r, b, v, t_on, time.monotonic() - t_mark))
                            time.sleep(SWEEP_GAP_S)
                    finally:
                        L.ptt(False)
                    probe.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
                    sdr_bench.capture_wait()
                L.probe_stop()
                rc, text = sdr_bench.analyze_fm(cap, bw_khz=12.5, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz,
                                                tones=True)
                runs = [(float(f), float(a), float(e)) for f, a, e in
                        re.findall(r"tone (\d+(?:\.\d)?) Hz ([\d.]+)-([\d.]+) s", text)]
                marks = [t for t in runs if MARK_HZ[0] <= t[0] <= MARK_HZ[1]]
                if not marks:
                    missing_marker.append(k)
                    print(f"key-up {k}: no marker tone; tones {runs}\n{text}")
                    continue
                t0 = marks[0][1]
                for f, a, e in runs:
                    if (f, a, e) == marks[0]:
                        continue
                    rel = a - t0
                    slot = next((s for s in slots if s[3] - 0.15 <= rel <= s[4] + 0.15), None)
                    src = "I2S" if abs(f - I2S_HZ) < 40 else "TX RAM" if abs(f - TONE_HZ) < 40 else "?"
                    if slot is None:
                        unmapped.append((k, f, rel, e - a))
                    else:
                        hits.append((slot[0], slot[1], slot[2], f, src, e - a))
        finally:
            L.ptt(False)
            L.probe_stop()
            for r, v in saved.items():
                L.probe_reg(PAGE, r, v)
    print(f"\nFM TX bit sweep: {len(flips)} flips in {len(batches)} key-ups")
    for r, b, v, f, src, d in hits:
        print(f"  -> 4:0x{r:02X} bit {b} (0x{v:02X} -> 0x{v ^ (1 << b):02X}): {f:.0f} Hz for {d:.2f} s, from {src}")
    for k, f, rel, d in unmapped:
        print(f"  key-up {k}: {f:.0f} Hz at +{rel:.2f} s for {d:.2f} s outside any flip")
    if not hits:
        print("  no flip put a tone on air")
    assert not missing_marker, f"key-ups without the marker tone: {missing_marker}"


# --- FM transmit: the mode register and the modulation offsets (hardware-dm1701.md 21.3.1, after the split duplex work) ---
#
# In DMR the chip ignored the TX RAM until 0x10's layer bits picked the physical layer mode (0x66), which the manual
# documents only for receive. FM sets 0x10 = 0x80 and its low bits were never varied in FM, and the FM TX RAM failure
# looks the same (the chip asks for samples, then ignores them). Page 1 0x50 / 0x51 (FM writes 0) and 0x01 (H/W
# config) were never swept either.

def tx_ram_tone():
    """Both FM buffers and the DMR physical layer area (0x000-0x047) of the TX RAM: one big endian 1 kHz tone"""
    return tone_bytes(">", n=RAM_FM // 2) + tone_bytes(">") * 2


def marked_steps(probe, sdr_bench, pytestconfig, steps, tag, per_key=8):
    """Keyed with the microphone muted, a 600 Hz tone on the I2S stream and a 1000 Hz tone in the TX RAM (FM flags
    unmasked), each step (label, page, register, value) written for SWEEP_ON_S and then restored. Each key-up starts
    with LINEIN2's marker tone, as in test_probe_fm_tx_sweep. Returns (hits, unmapped, key-ups without the marker,
    texts): a hit is (label, Hz, source, seconds)."""
    import re
    L = probe.link
    mhz = pytestconfig.getoption("--fm-mhz")
    hz = round(mhz * 1e6)
    batches = [steps[i:i + per_key] for i in range(0, len(steps), per_key)]
    key_s = SWEEP_MARK_S + 0.2 + per_key * (SWEEP_ON_S + SWEEP_GAP_S) + 0.3
    hits, unmapped, missing, texts = [], [], [], []
    with probe.vfo_session():
        L.vfo_set(hz)
        probe.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        probe.lowest_power()
        regs = {(p, r) for _, p, r, _ in steps} | {(PAGE, 0x81), (PAGE, 0xE0)}
        saved = {(p, r): L.probe_reg(p, r) for p, r in regs}
        try:
            for k, batch in enumerate(batches):
                for (p, r), v in saved.items():
                    L.probe_reg(p, r, v)
                L.probe_ram(RAM_PAGE, 0, data=tx_ram_tone())
                L.probe_start(I2S_HZ, TONE_AMP)
                rdir = sdr_bench.run_dir(f"hil_aprobe_{tag}{k}")
                with sdr_bench.lease():
                    for attempt in range(3):      # the bench service sometimes still holds the SDR for a moment
                        try:
                            cap = sdr_bench.capture_start(rdir, mhz, seconds=key_s + 5)
                            break
                        except RuntimeError:
                            if attempt == 2:
                                raise
                            time.sleep(5)
                    L.ptt(True, int(key_s * 1000) + 1500)
                    try:
                        probe.wait_for(lambda v: v.get("state") == "tx", timeout=2, what="keyed")
                        time.sleep(0.2)
                        apply(L, 0xE0, 0, 0x40)          # microphone off
                        apply(L, 0x81, FM_FLAG_MASK, 0)
                        L.probe_ram(RAM_PAGE, 0, data=tx_ram_tone())
                        e0 = L.probe_reg(PAGE, 0xE0)
                        t_mark = time.monotonic()
                        L.probe_reg(PAGE, 0xE0, e0 | 0x20)
                        time.sleep(SWEEP_MARK_S)
                        L.probe_reg(PAGE, 0xE0, e0)
                        time.sleep(0.2)
                        slots = []
                        for label, p, r, val in batch:
                            v = L.probe_reg(p, r)
                            if isinstance(val, tuple):           # ("xor", mask): flip bits of the live value
                                label = f"{label} (0x{v:02X} -> 0x{v ^ val[1]:02X})"
                                val = v ^ val[1]
                            t_on = time.monotonic() - t_mark
                            L.probe_reg(p, r, val)
                            time.sleep(SWEEP_ON_S)
                            L.probe_reg(p, r, v)
                            slots.append((label, t_on, time.monotonic() - t_mark))
                            time.sleep(SWEEP_GAP_S)
                    finally:
                        L.ptt(False)
                    probe.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
                    sdr_bench.capture_wait()
                L.probe_stop()
                rc, text = sdr_bench.analyze_fm(cap, bw_khz=12.5, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz,
                                                tones=True)
                texts.append((k, [s[0] for s in slots], text))
                runs = [(float(f), float(a), float(e)) for f, a, e in
                        re.findall(r"tone (\d+(?:\.\d)?) Hz ([\d.]+)-([\d.]+) s", text)]
                print(f"key-up {k}/{len(batches)} ({slots[0][0]} .. {slots[-1][0]}): tones "
                      f"{[(round(f), round(e - a, 2)) for f, a, e in runs]}", flush=True)  # survives a later failure
                marks = [t for t in runs if MARK_HZ[0] <= t[0] <= MARK_HZ[1]]
                if not marks:
                    missing.append(k)
                    continue
                t0 = marks[0][1]
                for f, a, e in runs:
                    if (f, a, e) == marks[0]:
                        continue
                    rel = a - t0
                    slot = next((s for s in slots if s[1] - 0.15 <= rel <= s[2] + 0.15), None)
                    src = "I2S" if abs(f - I2S_HZ) < 40 else "TX RAM" if abs(f - TONE_HZ) < 40 else "?"
                    if slot is None:
                        unmapped.append((k, f, rel, e - a))
                    else:
                        hits.append((slot[0], f, src, e - a))
        finally:
            L.ptt(False)
            L.probe_stop()
            for (p, r), v in saved.items():
                L.probe_reg(p, r, v)
    return hits, unmapped, missing, texts


def print_steps(title, n, hits, unmapped, missing, texts):
    print(f"\n{title}: {n} steps")
    for label, f, src, d in hits:
        print(f"  -> {label}: {f:.0f} Hz for {d:.2f} s, from {src}")
    for k, f, rel, d in unmapped:
        print(f"  key-up {k}: {f:.0f} Hz at +{rel:.2f} s for {d:.2f} s outside any step")
    if not hits:
        print("  no step put a tone on air")
    for k, labels, text in texts:
        segs = re.search(r"(\d+) keyed segment", text)
        if k in missing or (segs and segs.group(1) != "1"):
            print(f"  key-up {k} ({', '.join(labels)}): {'no marker; ' if k in missing else ''}"
                  f"{segs.group(1) if segs else '?'} keyed segments\n{text}")


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_mode_sweep(probe, sdr_bench, pytestconfig):
    """Experiment 1: every FM value of 0x10 (bit 7 set, 0x81-0xFF), keyed with tones on I2S (600 Hz) and in the TX
    RAM (1000 Hz). On the DM-1701 (2026-10-12): none put a tone on air, none dropped the carrier."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    steps = [(f"4:0x10 = 0x{v:02X}", PAGE, 0x10, v) for v in range(0x81, 0x100)]
    hits, unmapped, missing, texts = marked_steps(probe, sdr_bench, pytestconfig, steps, "mode")
    print_steps("FM TX mode sweep (0x10 values)", len(steps), hits, unmapped, missing, texts)
    assert not missing, f"key-ups without the marker tone: {missing}"


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_hwcfg_sweep(probe, sdr_bench, pytestconfig):
    """Each bit of 0x01 (H/W config: two-point modulation) and page 1 0x50 / 0x51 (FM writes 0) flipped while keyed,
    tones as in test_probe_fm_tx_mode_sweep."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    steps = [(f"{p}:0x{r:02X} bit {b}", p, r, ("xor", 1 << b)) for p, r in ((PAGE, 0x01), (1, 0x50), (1, 0x51))
             for b in range(8)]
    hits, unmapped, missing, texts = marked_steps(probe, sdr_bench, pytestconfig, steps, "hwcfg")
    print_steps("FM TX sweep (0x01, page 1 0x50 / 0x51 bits)", len(steps), hits, unmapped, missing, texts)
    assert not missing, f"key-ups without the marker tone: {missing}"


# Experiment 2: the modulation registers. 0x04 is rewritten in FM with the calibration "Mod2 offset", a DC level on
# the two-point modulation's reference path: if a step moves the carrier, the MCU can write audio into it. Each
# register gets one key-up: a base period, a few values, then a fast toggle from the host.
OFFSET_REGS = [0x04, 0x02, 0x03, 0x05, 0x47, 0x48, 0x46, 0x45]
OFFSET_HOLD_S = 0.4
OFFSET_TOGGLE_S = 1.5


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_offsets(probe, sdr_bench, pytestconfig):
    """Keyed with the microphone muted, per register: 0.6 s as set, then the value +8, +32, -8, -32 and with bit 7
    flipped (OFFSET_HOLD_S each, back to the base between them), then a toggle between the base and +32 as fast as
    the debug link writes. The SDR's carrier track (50 ms) shows which steps move the carrier and by how much."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    mhz = pytestconfig.getoption("--fm-mhz")
    hz = round(mhz * 1e6)
    deltas = [8, 32, -8, -32, "x80"]
    key_s = 0.6 + len(deltas) * 2 * OFFSET_HOLD_S + OFFSET_TOGGLE_S + 0.6
    rows = []
    with probe.vfo_session():
        L.vfo_set(hz)
        probe.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        probe.lowest_power()
        saved = regs_saved(L, set(OFFSET_REGS) | {0xE0})
        try:
            for k, reg in enumerate(OFFSET_REGS):
                for r, v in saved.items():
                    L.probe_reg(PAGE, r, v)
                rdir = sdr_bench.run_dir(f"hil_aprobe_offset{k}")
                with sdr_bench.lease():
                    cap = sdr_bench.capture_start(rdir, mhz, seconds=key_s + 5)
                    t_ptt = time.monotonic()
                    L.ptt(True, int(key_s * 1000) + 1500)
                    try:
                        probe.wait_for(lambda v: v.get("state") == "tx", timeout=2, what="keyed")
                        time.sleep(0.2)
                        apply(L, 0xE0, 0, 0x40)          # microphone off
                        base = L.probe_reg(PAGE, reg)
                        sched = [("base", 0.0, None)]
                        time.sleep(0.6)
                        for d in deltas:
                            val = base ^ 0x80 if d == "x80" else (base + d) & 0xFF
                            t_on = time.monotonic() - t_ptt
                            L.probe_reg(PAGE, reg, val)
                            time.sleep(OFFSET_HOLD_S)
                            L.probe_reg(PAGE, reg, base)
                            sched.append((f"0x{val:02X}", t_on, time.monotonic() - t_ptt))
                            time.sleep(OFFSET_HOLD_S)
                        t_on = time.monotonic() - t_ptt
                        n = 0
                        while time.monotonic() - t_ptt - t_on < OFFSET_TOGGLE_S:
                            L.probe_reg(PAGE, reg, (base + 32) & 0xFF if n % 2 == 0 else base)
                            n += 1
                        L.probe_reg(PAGE, reg, base)
                        el = time.monotonic() - t_ptt - t_on
                        sched.append((f"toggle {n} writes = {n / el:.0f}/s (square wave {n / el / 2:.0f} Hz)",
                                      t_on, t_on + el))
                        time.sleep(0.4)
                    finally:
                        L.ptt(False)
                    probe.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
                    sdr_bench.capture_wait()
                rc, text = sdr_bench.analyze_fm(cap, bw_khz=12.5, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz,
                                                tones=True, track=True)
                rows.append((reg, base, sched, text))
        finally:
            L.ptt(False)
            for r, v in saved.items():
                L.probe_reg(PAGE, r, v)
    print("\nFM TX modulation offsets (times from PTT; the track starts when the carrier comes up):")
    for reg, base, sched, text in rows:
        print(f"\n4:0x{reg:02X} as set 0x{base:02X}: " +
              "; ".join(f"{lab} {a:.2f}-{b:.2f} s" if b else lab for lab, a, b in sched))
        for line in text.splitlines():
            if re.match(r"\s*(track|carrier|voice|tone|segment|\S+: [\d.]+ s)", line):
                print("  " + line.strip())
    assert len(rows) == len(OFFSET_REGS)


# Tones played into 0x04 by the firmware (probe op MOD): (Hz, amplitude in register steps). 16 steps at about 75 Hz
# a step is about 1.2 kHz deviation
MOD_TONES = [(1000, 16), (300, 16), (2000, 16), (3000, 16), (1000, 32), (1000, 4)]
MOD_MS = 2000


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_mod_tone(probe, sdr_bench, pytestconfig):
    """FM transmit from the MCU through 0x04: keyed with the microphone muted, the firmware writes base + a sine into
    0x04 for 2 s; the SDR reports the tone and its deviation. The frequency response and the deviation per step."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link

    def run(hz, steps):
        def during():
            m = L.probe_mod(MOD_MS, hz, steps)
            return f"{m['writes'] * 1000 / max(m['elapsed_ms'], 1):.0f} writes/s around 0x{m['base']:02X}"
        return (lambda: None), during

    runs = [("0x04 as set (baseline)", lambda: None, lambda: None)]
    runs += [(f"0x04 tone {hz} Hz, {steps} steps", *run(hz, steps)) for hz, steps in MOD_TONES]
    rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "mod")
    print_tx_rows("FM TX from the MCU through 0x04", rows)
    assert len(rows) == len(runs)


# Registers the first sweep left out, from the manual's register table: 0x11 LocalChanMode (bit 0: digital and analog
# receive together), 0x21 / 0x22 the vocoder buffer controls (0x22: EncodeStart / EncodeStop / DecodeStart /
# DecodeStop, sel_I2S_mode, sel_I2StoI2S: the vocoder PCM interface), 0x30-0x34 the external codec clock and the FM
# audio processing, 0x37 the DAC data shift, 0x40 / 0x41 / 0x50-0x52 (what makes the chip send the TX RAM in DMR),
# 0x60 TransControl, 0xD0-0xDF
SWEEP2_REGS = [0x11, 0x21, 0x22, 0x30, 0x31, 0x32, 0x33, 0x34, 0x37, 0x40, 0x41, 0x50, 0x51, 0x52, 0x60] + \
    list(range(0xD0, 0xE0))


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_sweep2(probe, sdr_bench, pytestconfig):
    """As test_probe_fm_tx_sweep, over SWEEP2_REGS: each bit of the keyed value flipped for 0.5 s, tones on I2S
    (600 Hz) and in the TX RAM (1000 Hz, both FM buffers and 0x000-0x02F)."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    steps = [(f"4:0x{r:02X} bit {b}", PAGE, r, ("xor", 1 << b)) for r in SWEEP2_REGS for b in range(8)]
    hits, unmapped, missing, texts = marked_steps(probe, sdr_bench, pytestconfig, steps, "sweep2")
    print_steps("FM TX bit sweep 2", len(steps), hits, unmapped, missing, texts)
    assert not missing, f"key-ups without the marker tone: {missing}"


# Answers to the FM "sending data request" (0x82 bit 7), written at every request as DMR answers its slot interrupt
# (0x41 = 0x80: send in the next slot). (name, [(register, value)]): an absolute write per request
ACK_VARIANTS = [
    ("flag cleared only", []),
    ("0x41 = 0x80 (TxNxtSlotEn)", [(0x41, 0x80)]),
    ("0x41 = 0xC0", [(0x41, 0xC0)]),
    ("0x22 = 0x80 (EncodeStart)", [(0x22, 0x80)]),
    ("0x22 = 0x20 (DecodeStart)", [(0x22, 0x20)]),
    ("0x60 = 0xC0 (voice + data request)", [(0x60, 0xC0)]),
    ("0x60 = 0x40 (data request)", [(0x60, 0x40)]),
    ("0x50 = 0x10, 0x41 = 0x80 (DMR data send)", [(0x50, 0x10), (0x41, 0x80)]),
]
ACK_S = 3.0


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_ram_ack(probe, sdr_bench, pytestconfig):
    """Keyed with the microphone muted, the TX RAM holding a 1 kHz tone (big endian) and the FM flags unmasked, the
    host polls 0x82 and answers each "sending data request" (clear it, then the variant's writes)."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    saved = regs_saved(L, {0x81, 0x22, 0x41, 0x50, 0x60})

    def restore():
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)

    def run(writes):
        def before():
            restore()
            L.probe_ram(RAM_PAGE, 0, data=tx_ram_tone())

        def during():
            apply(L, 0x81, FM_FLAG_MASK, 0)
            t0, reqs, polls = time.monotonic(), 0, 0
            while time.monotonic() - t0 < ACK_S:
                f = L.probe_reg(PAGE, FM_FLAGS)
                polls += 1
                if f & 0x80:
                    L.probe_reg(PAGE, 0x83, 0x80)
                    for r, v in writes:
                        L.probe_reg(PAGE, r, v)
                    reqs += 1
            return f"{reqs / ACK_S:.0f} requests/s answered, {polls / ACK_S:.0f} polls/s"
        return before, during

    runs = [(name, *run(w)) for name, w in ACK_VARIANTS]
    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "ack")
    finally:
        restore()
    print_tx_rows("FM TX from the TX RAM, each data request answered", rows)
    assert len(rows) == len(runs)


# The manual's U_SPI command (5.2, the Chinese edition): OPMode 010 writes the TX RAM and reads the RX RAM (page 2),
# 110 writes the RX RAM and reads the TX RAM (page 6). Reading the TX RAM back shows whether the chip fills the FM
# buffers itself (the RAM table: "externally written FM voice data, or internally sent voice data from the Codec")
TX_RAM_READ_PAGE = 6


def tx_ram_activity(L, seconds=1.0):
    """Reads both TX RAM FM buffers repeatedly for seconds: (reads, reads that differ from the previous one, the
    first 16 bytes of the last read)"""
    t0, n, changed, last = time.monotonic(), 0, 0, None
    while time.monotonic() - t0 < seconds:
        b = L.probe_ram(TX_RAM_READ_PAGE, RAM_FM, RAM_FM_LEN)
        n += 1
        if last is not None and b != last:
            changed += 1
        last = b
    return n, changed, last


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_ram_readback(probe, sdr_bench, pytestconfig):
    """Unkeyed: a pattern written through page 2 reads back through page 6 (the TX RAM). Keyed (microphone on, then
    muted), the FM buffers read through page 6: does the chip write the codec's samples there, and does what we
    wrote survive?"""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    pattern = bytes(range(64))
    notes = {}
    with probe.vfo_session():
        L.probe_ram(RAM_PAGE, RAM_FM, data=pattern)
        back = L.probe_ram(TX_RAM_READ_PAGE, RAM_FM, len(pattern))
        rx = L.probe_ram(RAM_PAGE, RAM_FM, len(pattern))
        notes["unkeyed"] = (f"page 6 reads back the pattern: {back == pattern} ({back[:8].hex()}); "
                            f"page 2 read (RX RAM) {rx[:8].hex()}; idle activity {tx_ram_activity(L, 0.5)[:2]}")

    def run(mic):
        def before():
            L.probe_ram(RAM_PAGE, 0, data=tx_ram_tone())

        def during():
            if mic:
                apply(L, 0xE0, 0x40, 0)
            n, changed, last = tx_ram_activity(L, 1.0)
            ours = last == tx_ram_tone()[RAM_FM:RAM_FM + RAM_FM_LEN]
            w = L.probe_watch(500, FM_FLAGS, ram_addr=RAM_FM)
            return f"{changed}/{n} reads changed, still our tone: {ours}, first bytes {last[:16].hex()}; {watch_text(w)}"
        return before, during

    runs = [("keyed, microphone on", *run(True)), ("keyed, microphone muted", *run(False))]
    rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "rback")
    print(f"\nTX RAM read back through page 6:\n  unkeyed: {notes['unkeyed']}")
    for name, _, note in rows:
        print(f"  {name}: {note}")
    assert len(rows) == len(runs)


def chase_halves(L, seconds, data_for_half, settle=0.0):
    """While keyed: watch which TX RAM FM half the codec is writing (its first word changes) and, when it moves on,
    write our samples into the half it just finished (data_for_half(h) -> 512 bytes), after settle seconds.
    Returns (half switches seen, writes, mean ms from the switch to the end of our write)"""
    addr = (RAM_FM, RAM_FM + 512)
    prev = [L.probe_ram(TX_RAM_READ_PAGE, a, 4) for a in addr]
    active, switches, writes, lag = None, 0, 0, 0.0
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        cur = [L.probe_ram(TX_RAM_READ_PAGE, a, 4) for a in addr]
        moving = [cur[h] != prev[h] for h in (0, 1)]
        prev = cur
        if moving[0] == moving[1]:
            continue
        now = 0 if moving[0] else 1
        if active is not None and now != active:
            switches += 1
            ts = time.monotonic()
            if settle:
                time.sleep(settle)
            L.probe_ram(RAM_PAGE, addr[active], data=data_for_half(active))
            writes += 1
            lag += time.monotonic() - ts
        active = now
    return switches, writes, 1000 * lag / max(writes, 1)


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_ram_chase(probe, sdr_bench, pytestconfig):
    """The codec fills the TX RAM FM halves while keyed (test_probe_fm_tx_ram_readback). Keyed with the microphone
    muted, overwrite each half with a 1 kHz tone (big endian, then little endian) as soon as the codec moves to the
    other one. If the modulator reads a half after the codec, the tone goes on air."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link

    def run(order, settle=0.0):
        half = tone_bytes(order)

        def during():
            s, w, lag = chase_halves(L, PTT_S - 0.3, lambda h: half, settle)
            after = L.probe_ram(TX_RAM_READ_PAGE, RAM_FM, 16)
            return f"{s} half switches ({s / (PTT_S - 0.3):.0f}/s), {w} writes, {lag:.1f} ms to write; RAM {after[:8].hex()}"
        return (lambda: None), during

    runs = [("mic muted, nothing written (baseline)", lambda: None, lambda: None),
            ("chase, tone big endian", *run(">")),
            ("chase, tone little endian", *run("<")),
            ("chase, big endian, 10 ms after the switch", *run(">", 0.010))]
    rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "chase")
    print_tx_rows("FM TX: TX RAM halves overwritten behind the codec", rows)
    assert len(rows) == len(runs)


# Registers whose bits might stop the codec writing the TX RAM FM buffers (then the modulator could take ours)
WRITER_REGS = [(PAGE, r) for r in [0x06, 0x0D, 0x0E, 0x11, 0x21, 0x22, 0x25, 0x26, 0x30, 0x31, 0x32, 0x33, 0x34,
                                  0x36, 0x37, 0x60, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5] + list(range(0xD0, 0xE0))]
WRITER_KEY_S = 8.0


def ram_moving(L, gap=0.045):
    """True when either TX RAM FM half's first 4 bytes change within gap (one half takes 32 ms)"""
    a = [L.probe_ram(TX_RAM_READ_PAGE, x, 4) for x in (RAM_FM, RAM_FM + 512)]
    time.sleep(gap)
    b = [L.probe_ram(TX_RAM_READ_PAGE, x, 4) for x in (RAM_FM, RAM_FM + 512)]
    return a != b


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_probe_fm_tx_ram_writer_sweep(probe, sdr_bench, pytestconfig):
    """Keyed with the microphone muted, each bit of WRITER_REGS flipped in turn: does the codec stop writing the TX RAM
    FM buffers (read back through page 6)? Prints the flips that stop it (no RF analysis; the SDR records anyway)."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    mhz = pytestconfig.getoption("--fm-mhz")
    hz = round(mhz * 1e6)
    flips = [(p, r, b) for p, r in WRITER_REGS for b in range(8)]
    stops, i = [], 0
    with probe.vfo_session():
        L.vfo_set(hz)
        probe.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        probe.lowest_power()
        try:
            while i < len(flips):
                with sdr_bench.lease():
                    L.ptt(True, int(WRITER_KEY_S * 1000) + 1500)
                    try:
                        probe.wait_for(lambda v: v.get("state") == "tx", timeout=2, what="keyed")
                        time.sleep(0.2)
                        apply(L, 0xE0, 0, 0x40)
                        assert ram_moving(L), "the codec does not write the TX RAM in this key-up"
                        t0 = time.monotonic()
                        while i < len(flips) and time.monotonic() - t0 < WRITER_KEY_S - 1.0:
                            p, r, b = flips[i]
                            v = L.probe_reg(p, r)
                            L.probe_reg(p, r, v ^ (1 << b))
                            time.sleep(0.03)
                            if not ram_moving(L) and not ram_moving(L):
                                stops.append((p, r, b, v))
                                print(f"  -> {p}:0x{r:02X} bit {b} (0x{v:02X} -> 0x{v ^ (1 << b):02X}) stops the "
                                      f"TX RAM writes", flush=True)
                            L.probe_reg(p, r, v)
                            time.sleep(0.03)
                            i += 1
                    finally:
                        L.ptt(False)
                    probe.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        finally:
            L.ptt(False)
    print(f"\nTX RAM writer sweep: {len(flips)} flips, {len(stops)} stop the codec's writes")
    for p, r, b, v in stops:
        print(f"  {p}:0x{r:02X} bit {b} (0x{v:02X} -> 0x{v ^ (1 << b):02X})")
    assert i == len(flips)


# --- The TX RAM kept full of our tone (firmware RAMFEED) while candidate settings are tried ---
#
# Every static TX RAM test above was moot: the codec overwrites both halves every 64 ms while keyed (readback test).
# RAMFEED rewrites each half about 4 ms after the codec finishes it, so if any setting makes the modulator read the
# RAM after the codec, the tone (or a 31 Hz chopped tone) goes on air during that setting.
FEED_HOLD_MS, FEED_GAP_MS, FEED_PER_KEY = 500, 250, 10
FEED_REGS = [0x01, 0x06, 0x0D, 0x0E, 0x11, 0x21, 0x22, 0x25, 0x26] + list(range(0x30, 0x38)) + \
    [0x40, 0x41, 0x50, 0x51, 0x52, 0x60, 0xA1] + list(range(0xD0, 0xE0)) + list(range(0xE0, 0xE6))


def feed_flips():
    """(label, page, register, xor mask): first nothing (the default settings, fed), then every FM value of 0x10,
    then each bit of FEED_REGS and of page 1 0x50 / 0x51; not 0x60 bit 7 (the analog transmit chain off)"""
    out = [("fed, nothing changed", PAGE, 0x60, 0x00)]
    out += [(f"4:0x10 ^ 0x{m:02X}", PAGE, 0x10, m) for m in range(1, 0x80)]
    out += [(f"4:0x{r:02X} bit {b}", PAGE, r, 1 << b) for r in FEED_REGS for b in range(8) if (r, b) != (0x60, 7)]
    out += [(f"1:0x{r:02X} bit {b}", 1, r, 1 << b) for r in (0x50, 0x51) for b in range(8)]
    return out


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_ramfeed_sweep(probe, sdr_bench, pytestconfig):
    """Keyed with the microphone muted, the firmware keeps a 1 kHz tone in the TX RAM FM halves and applies each flip
    for 0.5 s. A flip during which the voice band level jumps (or a 1 kHz tone shows) makes the modulator read the
    RAM. On the DM-1701 (2026-10-12): none of 511; 0x01 bit 4 and 0x25 bit 1 drop the carrier."""
    ramfeed_sweep(probe, sdr_bench, pytestconfig, feed_flips(), "feed")


# The flips the first run lost to a dropped carrier (0x01 bit 4, 0x25 bit 1 ended their key-ups); those two last
FEED_RERUN = [(f"4:0x{r:02X} bit {b}", PAGE, r, 1 << b) for r, bits in ((0x01, (5, 6, 7)), (0x06, (0, 1, 2, 3)),
                                                                       (0x25, (2, 3, 4, 5, 0))) for b in bits] + \
    [("4:0x01 bit 4", PAGE, 0x01, 0x10), ("4:0x25 bit 1", PAGE, 0x25, 0x02)]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_ramfeed_rerun(probe, sdr_bench, pytestconfig):
    """FEED_RERUN through the fed sweep, in key-ups of their own"""
    ramfeed_sweep(probe, sdr_bench, pytestconfig, FEED_RERUN, "rerun")


def ramfeed_sweep(probe, sdr_bench, pytestconfig, flips, tag):
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    mhz = pytestconfig.getoption("--fm-mhz")
    hz = round(mhz * 1e6)
    batches = [flips[i:i + FEED_PER_KEY] for i in range(0, len(flips), FEED_PER_KEY)]
    feed_s = (FEED_PER_KEY * (FEED_HOLD_MS + FEED_GAP_MS) + FEED_GAP_MS) / 1000
    key_s = feed_s + 0.8
    hits = []
    with probe.vfo_session():
        L.vfo_set(hz)
        probe.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO tuned")
        probe.lowest_power()
        try:
            for k, batch in enumerate(batches):
                rdir = sdr_bench.run_dir(f"hil_aprobe_{tag}{k}")
                with sdr_bench.lease():
                    for attempt in range(3):
                        try:
                            cap = sdr_bench.capture_start(rdir, mhz, seconds=key_s + 5)
                            break
                        except RuntimeError:
                            if attempt == 2:
                                raise
                            time.sleep(5)
                    t_ptt = time.monotonic()
                    L.ptt(True, int(key_s * 1000) + 1500)
                    try:
                        probe.wait_for(lambda v: v.get("state") == "tx", timeout=2, what="keyed")
                        time.sleep(0.2)
                        apply(L, 0xE0, 0, 0x40)
                        t_feed = time.monotonic() - t_ptt
                        f = L.probe_ramfeed(FEED_HOLD_MS, FEED_GAP_MS, TONE_HZ, TONE_AMP,
                                            [(p, r, m) for _, p, r, m in batch])
                    finally:
                        L.ptt(False)
                    probe.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
                    sdr_bench.capture_wait()
                rc, text = sdr_bench.analyze_fm(cap, bw_khz=12.5, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz,
                                                tones=True, levels=True)
                m = re.search(r"levels ([\d.]+) s, 0\.05 s steps: ([\d ]+)", text)
                if not m:
                    print(f"key-up {k}: no levels\n{text}")
                    continue
                t_seg, lv = float(m.group(1)), [float(v) for v in m.group(2).split()]
                # the segment starts when the carrier comes up, about when the PTT command went out
                base = sorted(lv)[len(lv) // 4]
                summary = []
                for j, (label, *_rest) in enumerate(batch):
                    a = t_feed + (FEED_GAP_MS + j * (FEED_HOLD_MS + FEED_GAP_MS)) / 1000 + 0.1
                    w = lv[int(a / 0.05):int((a + FEED_HOLD_MS / 1000 - 0.2) / 0.05)]
                    lvl = sum(w) / max(len(w), 1)
                    summary.append(f"{label} {lvl:.0f}")
                    if lvl > 3 * max(base, 30):
                        hits.append((label, lvl, base))
                        print(f"  -> {label}: voice band {lvl:.0f} Hz rms vs {base:.0f} Hz", flush=True)
                tones = re.findall(r"tone (\d+(?:\.\d)?) Hz ([\d.]+)-([\d.]+) s", text)
                print(f"key-up {k}/{len(batches)}: {f['switches']} half switches, {f['writes']} writes, max lag "
                      f"{f['max_lag_ms']} ms; base {base:.0f} Hz; tones {tones}; " + ", ".join(summary), flush=True)
        finally:
            L.ptt(False)
    print(f"\nTX RAM fed sweep: {len(flips)} settings, {len(hits)} raise the voice band level")
    for label, lvl, base in hits:
        print(f"  {label}: {lvl:.0f} Hz rms (base {base:.0f})")


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_mod_shaped(probe, sdr_bench, pytestconfig):
    """The 0x04 path's quality: a 1 kHz tone at 32 and 16 steps, plain and noise shaped; SINAD in the voice band."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link

    def run(steps, shaped):
        def during():
            m = L.probe_mod(MOD_MS, 1000, steps, shaped=shaped)
            return f"{m['writes'] * 1000 / max(m['elapsed_ms'], 1):.0f} writes/s"
        return (lambda: None), during

    runs = [(f"0x04 1 kHz, {s} steps, {'shaped' if sh else 'plain'}", *run(s, sh))
            for s in (32, 16, 4) for sh in (False, True)]
    rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "shaped", ptt_s=2.5, tone_snr=1000)
    print_tx_rows("FM TX through 0x04, plain and noise shaped", rows)
    assert len(rows) == len(runs)


# The manual's "send FM voice" bit is 0xD7 bit 1 (msk_voice_send_en: 0 MSK signaling, 1 FM voice), in the MSK block,
# whose transmit data comes from a ping-pong buffer the MCU fills (half full: 0x82 bit 6) before 0xD5 starts it
# (OperationType, TranOrRecFlag); 0xA1 bit 6 enables MSK mode (hrc6000-registers.md 7, 8). Applied together while
# the firmware keeps the tone in the TX RAM. (name, [(register, set mask, clear mask) or (register, None, value)])
FEED_COMBOS = [
    ("fed, nothing changed (baseline)", []),
    ("0xD7 |= 0x02 (FM voice)", [(0xD7, 0x02, 0)]),
    ("0xD7 |= 0x12 (FM voice, bit 4)", [(0xD7, 0x12, 0)]),
    ("0xA1 |= 0x40 (MSK mode), 0xD7 |= 0x02", [(0xA1, 0x40, 0), (0xD7, 0x02, 0)]),
    ("0xA1 |= 0x40, 0xD7 |= 0x02, 0xD5 = 0x48 (start TX)", [(0xA1, 0x40, 0), (0xD7, 0x02, 0), (0xD5, None, 0x48)]),
    ("0xA1 |= 0x40, 0xD7 |= 0x12, 0xD5 = 0x48", [(0xA1, 0x40, 0), (0xD7, 0x12, 0), (0xD5, None, 0x48)]),
    ("0xA1 |= 0x40, 0xD5 = 0x48 (MSK TX)", [(0xA1, 0x40, 0), (0xD5, None, 0x48)]),
    ("0x81 |= 0xC2, 0xD7 |= 0x02, 0xD5 = 0x48", [(0x81, 0xC2, 0), (0xD7, 0x02, 0), (0xD5, None, 0x48)]),
]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_ramfeed_combos(probe, sdr_bench, pytestconfig):
    """Keyed with the microphone muted: FEED_COMBOS applied together while the firmware keeps a 1 kHz tone in the TX
    RAM FM halves for 2 s; the voice band level and tones per combination."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    regs = {r for _, w in FEED_COMBOS for r, _, _ in w}
    saved = regs_saved(L, regs)

    def restore():
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)

    def run(writes):
        def during():
            for reg, s_, c in writes:
                if s_ is None:
                    L.probe_reg(PAGE, reg, c)
                else:
                    apply(L, reg, s_, c)
            f = L.probe_ramfeed(2000, 100, TONE_HZ, TONE_AMP, [(PAGE, 0x60, 0x00)])
            restore()
            return f"{f['switches']} half switches, {f['writes']} writes, max lag {f['max_lag_ms']} ms"
        return restore, during

    runs = [(name, *run(w)) for name, w in FEED_COMBOS]
    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "combo")
    finally:
        restore()
    print_tx_rows("FM TX from the fed TX RAM, MSK / FM voice settings", rows)
    assert len(rows) == len(runs)


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_mod_gain(probe, sdr_bench, pytestconfig):
    """Does 0x46 (two-point MOD1 amplitude, hrc6000-registers.md 5.1) scale the 0x04 offset? The same 16-step 1 kHz
    tone through 0x04 with 0x46 as set, halved and quartered: if the deviation follows, a small 0x46 gives finer
    steps (more resolution, less range)."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link

    def run(div):
        def during():
            v = L.probe_reg(PAGE, 0x46)
            L.probe_reg(PAGE, 0x46, max(1, v // div))
            m = L.probe_mod(MOD_MS, 1000, 16)
            L.probe_reg(PAGE, 0x46, v)
            return f"0x46 0x{v:02X} -> 0x{max(1, v // div):02X}"
        return (lambda: None), during

    runs = [(f"0x04 1 kHz 16 steps, 0x46 / {d}", *run(d)) for d in (1, 2, 4)]
    rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "gain", ptt_s=2.5)
    print_tx_rows("FM TX through 0x04 against the MOD1 amplitude 0x46", rows)
    assert len(rows) == len(runs)


def aux_write16(L, addr, value):
    """A 16-bit value low byte first into the auxiliary table (page 1) at an extended address (manual 5.2)"""
    L.probe_ram(1, addr, data=bytes([value & 0xFF, value >> 8]))


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_tone_gen(probe, sdr_bench, pytestconfig):
    """The chip's selcall tone generator (hrc6000-registers.md 7.8): eight 16-bit phase steps at aux 0x11A-0x129
    (f / 32000 x 2^16), deviation 0xA0. Keyed with the microphone muted: 2-tone mode, "long A" with tone A set to
    1000 Hz, retriggered by 0x60 0x00 -> 0x80; then tone A's step rewritten from the host every 100 ms between 1200
    and 2200 Hz (AFSK-style). Shows whether the generator is programmable and follows step writes while it plays."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    regs = (0xA0, 0xA1, 0xA3, 0xA4, 0xA5, 0xA6, 0xAE, 0xAF, 0x60)
    saved = regs_saved(L, set(regs))
    step = lambda f: round(f / 32000 * 65536)

    def restore():
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)

    def setup(a_hz):
        aux_write16(L, 0x11A, step(a_hz))
        for r, v in ((0xA1, 0x10), (0xA0, 0x39), (0xA6, 0x0A), (0xA3, 0x05), (0xA4, 0x05), (0xA5, 0x05)):
            L.probe_reg(PAGE, r, v)
        # TX code "long A": {0xAF bits 3-0, 0xAE bits 7-4}; the long-tone code is not stated, so try A in both nibbles
        L.probe_reg(PAGE, 0xAF, 0x0A)
        L.probe_reg(PAGE, 0xAE, 0xA0)

    def run_long():
        def during():
            setup(1000)
            L.probe_reg(PAGE, 0x60, 0x00)
            time.sleep(0.05)
            L.probe_reg(PAGE, 0x60, 0x80)
            time.sleep(1.5)
            return "2-tone long A = 1000 Hz, retriggered"
        return restore, during

    def run_afsk():
        def during():
            setup(1200)
            L.probe_reg(PAGE, 0x60, 0x00)
            time.sleep(0.05)
            L.probe_reg(PAGE, 0x60, 0x80)
            t0, n = time.monotonic(), 0
            while time.monotonic() - t0 < 1.5:
                aux_write16(L, 0x11A, step(2200 if n % 2 else 1200))
                n += 1
                time.sleep(0.1)
            return f"tone A step rewritten {n} times (1200 / 2200 Hz, 100 ms each)"
        return restore, during

    def run_before_key():
        def before():
            setup(1000)
        return before, (lambda: "set up before keying (the TX set-up may rewrite 0xA0 / 0xA1)")

    runs = [("2-tone set up before keying", *run_before_key()), ("2-tone long A, retriggered", *run_long()),
            ("2-tone, step rewritten 1200 / 2200", *run_afsk())]
    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "tgen")
    finally:
        restore()
    print_tx_rows("FM TX from the chip's tone generator", rows)
    assert len(rows) == len(runs)


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_ext_codec_switch(probe, sdr_bench, pytestconfig):
    """Does 0x06 bit 2 (CodeCMode external) move the FM transmit chain's input to the external codec port (pins 16-20,
    chip block diagram figure 1.1)? LINEIN2's own 2.5 kHz oscillation is a known source on the built-in codec: keyed
    with LINEIN2 on for 1.5 s, then 0x06 bit 2 set for 1.5 s. If the tone stops, the chain reads the external port,
    and a wire from the MCU's PCM line to ADCDAT (pin 19) would be full quality FM transmit audio."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    saved = regs_saved(L, {0x06, 0x36})

    def during():
        e0 = L.probe_reg(PAGE, 0xE0)
        L.probe_reg(PAGE, 0xE0, e0 | 0x20)
        time.sleep(1.5)
        apply(L, 0x06, 0x04, 0)
        time.sleep(1.5)
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)
        L.probe_reg(PAGE, 0xE0, e0)
        return "LINEIN2 on 0-1.5 s, then 0x06 |= 0x04 1.5-3 s"

    runs = [("LINEIN2 marker, then external codec mode", lambda: None, during)]
    rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "ext", ptt_s=3.5, levels=True)
    print_tx_rows("FM TX source switch 0x06 bit 2", rows)
    assert len(rows) == 1


# 0x06 bit 2 moves the FM transmit input off the built-in codec (test_probe_fm_tx_ext_codec_switch, 2026-10-12: the
# LINEIN2 tone stops at once). Does external-codec mode read the vocoder I2S port the MCU drives? The probe's I2S
# tone (1 kHz) runs throughout. (name, [(register, set mask, clear mask)])
EXT_I2S_VARIANTS = [
    ("I2S tone, built-in codec (baseline)", []),
    ("I2S tone, 0x06 |= 0x04 (external codec)", [(0x06, 0x04, 0)]),
    ("I2S tone, external, 0x36 &= ~0x80 (external clocks on)", [(0x06, 0x04, 0), (0x36, 0, 0x80)]),
    ("I2S tone, external, 0x22 |= 0x02 (vocoder PCM over I2S)", [(0x06, 0x04, 0), (0x22, 0x02, 0)]),
    ("I2S tone, external, 0x22 |= 0x06 (I2S master)", [(0x06, 0x04, 0), (0x22, 0x06, 0)]),
    ("I2S tone, external, clocks on, 0x22 |= 0x02", [(0x06, 0x04, 0), (0x36, 0, 0x80), (0x22, 0x02, 0)]),
]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_ext_i2s(probe, sdr_bench, pytestconfig):
    """Keyed with the microphone muted and the probe sending a 1 kHz tone on the I2S stream, the external-codec
    settings of EXT_I2S_VARIANTS: a 1 kHz tone on air means the FM chain reads the MCU's I2S samples."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    saved = regs_saved(L, {0x06, 0x36, 0x22})

    def restore():
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)

    def run(writes):
        def before():
            restore()
            L.probe_start(TONE_HZ, TONE_AMP)

        def during():
            for reg, s_, c in writes:
                apply(L, reg, s_, c)
            st = L.probe_status()
            return f"stream {'running' if st['running'] else 'stopped'}, probe owns it: {st['ours']}, I2S in rms L {st['left']['rms']} R {st['right']['rms']}"
        return before, during

    runs = [(name, *run(w)) for name, w in EXT_I2S_VARIANTS]
    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "exti2s")
    finally:
        restore()
    print_tx_rows("FM TX in external codec mode, MCU tone on I2S", rows)
    assert len(rows) == len(runs)


# LINEIN2 (pin 8) puts a steady 2.5-2.6 kHz tone on air when enabled while keyed: an oscillation, so a loop. If it is
# LINEOUT2 (on in TX, 0xE0 bit 3) -> the board's audio net -> LINEIN2 -> ADC, then MCU audio has a full-quality path:
# I2S -> codec DAC -> LINEOUT2 -> LINEIN2 -> the FM chain, once the loop gain is below oscillation. The probe sends a
# 1 kHz tone on I2S throughout. (name, [(register, set mask, clear mask) or (register, None, value)])
LOOP_VARIANTS = [
    ("LINEIN2 on (the oscillation)", [(0xE0, 0x20, 0)]),
    ("LINEIN2 on, LINEOUT2 off", [(0xE0, 0x20, 0x08)]),
    ("LINEIN2 on, LINEOUT1 on, LINEOUT2 off", [(0xE0, 0x30, 0x08)]),
    ("LINEIN2 on, mic gain lowest (0xE4 = 0x20)", [(0xE0, 0x20, 0), (0xE4, None, 0x20)]),
    ("LINEIN2 on, mic -12 dB (0xE4 = 0x20), DAC -15 dB (0x37 = 0x8A)", [(0xE0, 0x20, 0), (0xE4, None, 0x20),
                                                                       (0x37, None, 0x8A)]),
    ("LINEIN2 on, mic lowest, DAC -30 dB (0x37 = 0x94)", [(0xE0, 0x20, 0), (0xE4, None, 0x20), (0x37, None, 0x94)]),
]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_linein2_loop(probe, sdr_bench, pytestconfig):
    """Keyed with the microphone (LINEIN1) muted and a 1 kHz tone on the I2S stream: does LINEIN2 hear LINEOUT2, and
    with the loop gain lowered, does the I2S tone reach the air through DAC -> LINEOUT2 -> LINEIN2?"""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    saved = regs_saved(L, {0xE4, 0x37})

    def restore():
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)

    def run(writes):
        def before():
            restore()
            L.probe_start(TONE_HZ, TONE_AMP)

        def during():
            e0 = L.probe_reg(PAGE, 0xE0)
            for reg, s_, c in writes:
                if s_ is None:
                    L.probe_reg(PAGE, reg, c)
                else:
                    apply(L, reg, s_, c)
            time.sleep(2.5)
            L.probe_reg(PAGE, 0xE0, e0)
            restore()
            return None
        return before, during

    runs = [(name, *run(w)) for name, w in LOOP_VARIANTS]
    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "loop", ptt_s=3.0, tone_snr=TONE_HZ)
    finally:
        restore()
    print_tx_rows("FM TX through LINEIN2 with a 1 kHz tone on I2S", rows)
    assert len(rows) == len(runs)


def skip_without_pins(link):
    from odr_debug import DebugLinkError
    try:
        link.probe_pin_scan(10)
    except DebugLinkError:
        skip("no_hw", "the board has no pin tone / scan (probe ops 12, 13)")


def scan_text(scan):
    return " ".join(f"{p}:{m:04X}" for p, m in scan.items())


def test_probe_pin_scan_ext_codec(probe):
    """Which MCU pins toggle, with the built-in codec and with 0x06 bit 2 (external codec)? The I2S clocks the MCU
    receives (PA15 WS, PC10 CK) come from the chip; a pin that starts toggling in external mode would be wired to the
    external codec port (pins 16-20, the chip is its master)."""
    L = probe.link
    skip_without_pins(L)
    r06 = L.probe_reg(PAGE, 0x06)
    try:
        internal = [L.probe_pin_scan(1000) for _ in range(2)]
        L.probe_reg(PAGE, 0x06, r06 | 0x04)
        time.sleep(0.2)
        external = [L.probe_pin_scan(1000) for _ in range(2)]
    finally:
        L.probe_reg(PAGE, 0x06, r06)
    for name, scans in (("built-in codec", internal), ("external codec", external)):
        for s_ in scans:
            print(f"{name:16s} changed: {scan_text(s_)}")
    base = {p: internal[0][p] | internal[1][p] for p in internal[0]}
    new = {p: external[0][p] & external[1][p] & ~base[p] for p in base}
    stopped = {p: base[p] & ~(external[0][p] | external[1][p]) for p in base}
    print("toggle only in external mode:", scan_text(new))
    print("stop in external mode:", scan_text(stopped))
    assert base["A"] & 0x8000 and base["C"] & 0x0400, "the I2S clocks (PA15, PC10) should run with the built-in codec"


# PC8 is the stock firmware's beep PWM: if its net reaches the HR_C6000's audio inputs, filtered PWM is MCU audio into
# the FM chain without a mod. A 1 kHz square wave on PC8 while keyed, with each codec input on. The last run toggles
# pins 16-20 as the chip's timed GPIO outputs (0xC7-0xD0) and scans the MCU's pins for one that follows.
PIN_TONE_VARIANTS = [
    ("no tone (baseline)", 0, []),
    ("PC8 1 kHz, microphone off", 1000, []),
    ("PC8 1 kHz, LINEIN1 (microphone) on", 1000, [(0xE0, 0x40, 0)]),
    ("PC8 1 kHz, LINEIN2 on", 1000, [(0xE0, 0x20, 0)]),
    ("PC8 1 kHz, MIC_P / MIC_N on", 1000, [(0xE0, 0x06, 0)]),
]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_pin_tone(probe, sdr_bench, pytestconfig):
    """Keyed FM with a 1 kHz square wave on PC8 (the stock beep PWM) and each codec input on in turn: a 1 kHz tone on
    air means the beep net reaches the transmit audio. Then, keyed, pins 16-20 as timed GPIO outputs while the MCU's
    pins are scanned."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    skip_without_pins(L)
    timed = list(range(0xC7, 0xD1))
    saved = regs_saved(L, set(timed))

    def run(hz, writes):
        def during():
            e0 = L.probe_reg(PAGE, 0xE0)
            for reg, s_, c in writes:
                apply(L, reg, s_, c)
            L.probe_pin_tone(hz)
            time.sleep(2.5)
            L.probe_pin_tone(0)
            L.probe_reg(PAGE, 0xE0, e0)
            return None
        return lambda: None, during

    def scan_timed():
        before = L.probe_pin_scan(1000)
        for on in (0xC7, 0xC9, 0xCB, 0xCD, 0xCF):
            L.probe_reg(PAGE, on, 0xC1)      # high 100 us after the 30 ms boundary
            L.probe_reg(PAGE, on + 1, 0x4A)  # low 1 ms after it
        after = L.probe_pin_scan(1000)
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)
        new = {p: after[p] & ~before[p] for p in before}
        return f"scan {scan_text(before)}; timed outputs {scan_text(after)}; new {scan_text(new)}"

    runs = [(name, *run(hz, w)) for name, hz, w in PIN_TONE_VARIANTS]
    runs.append(("pins 16-20 as timed GPIO, MCU pin scan", lambda: None, scan_timed))
    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "pin", ptt_s=3.0, tone_snr=TONE_HZ)
    finally:
        L.probe_pin_tone(0)
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)
    print_tx_rows("FM TX with a 1 kHz square wave on PC8", rows)
    assert len(rows) == len(runs)


# PWM audio on PC8 into MIC_P / MIC_N (test_probe_fm_tx_pin_tone, 2026-10-12: the beep net reaches the differential
# mic input). Whatever RC the board has on that net, plus the codec ADC's own decimation filter, has to remove the
# carrier. (name, timer top, tone Hz, amplitude per mille); the carrier is 72 MHz / (top + 1)
PWM_VARIANTS = [
    ("70 kHz carrier, 1 kHz, 10 %", 1023, 1000, 100),
    ("70 kHz carrier, 1 kHz, 30 %", 1023, 1000, 300),
    ("140 kHz carrier, 1 kHz, 30 %", 511, 1000, 300),
    ("35 kHz carrier, 1 kHz, 30 %", 2047, 1000, 300),
    ("17.6 kHz carrier, 1 kHz, 30 %", 4095, 1000, 300),
    ("70 kHz carrier, 400 Hz, 30 %", 1023, 400, 300),
    ("70 kHz carrier, 2 kHz, 30 %", 1023, 2000, 300),
]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_pin_pwm(probe, sdr_bench, pytestconfig):
    """Keyed FM with MIC_P / MIC_N on and a PWM sine on PC8: the quality (SINAD) of PWM audio through the board's own
    filtering and the chip's FM chain, by carrier and level."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    from odr_debug import DebugLinkError
    try:
        L.probe_pin_pwm(1023, 0, 0)
    except DebugLinkError:
        skip("no_hw", "the board has no PWM audio output (probe op 14)")

    rows = pwm_runs(probe, sdr_bench, pytestconfig, [(n, t, h, a, []) for n, t, h, a in PWM_VARIANTS])
    print_tx_rows("FM TX with PWM audio on PC8 into MIC_P / MIC_N", rows)
    assert len(rows) == len(PWM_VARIANTS)


def pwm_runs(probe, sdr_bench, pytestconfig, variants):
    """Key once per (name, top, tone Hz, amplitude per mille, [(register, set, clear)]) with MIC_P / MIC_N on and the
    PWM sine on PC8; the registers written while keyed are restored after each run."""
    L = probe.link
    from odr_debug import DebugLinkError
    try:
        L.probe_pin_pwm(1023, 0, 0)
    except DebugLinkError:
        skip("no_hw", "the board has no PWM audio output (probe op 14)")

    def run(top, hz, amp, writes):
        def during():
            saved = {r: L.probe_reg(PAGE, r) for r in {0xE0} | {w[0] for w in writes}}
            apply(L, 0xE0, 0x06, 0)          # MIC_P, MIC_N on
            for reg, s_, c in writes:
                apply(L, reg, s_, c)
            fs = L.probe_pin_pwm(top, hz, amp)
            time.sleep(2.5)
            L.probe_pin_pwm(top, 0, 0)
            for r, v in saved.items():
                L.probe_reg(PAGE, r, v)
            return f"carrier {fs} Hz" if fs else "PWM DMA not running"
        return lambda: None, during

    rows = []
    try:
        for name, top, hz, amp, writes in variants:
            rows += keyed_runs(probe, sdr_bench, pytestconfig, [(name, *run(top, hz, amp, writes))], "pwm",
                               ptt_s=3.0, tone_snr=hz)
    finally:
        L.probe_pin_pwm(1023, 0, 0)
    return rows


# test_probe_fm_tx_pin_pwm (2026-10-12): 10 % already drives the chip's limiter (2.6 kHz rms). Lower levels, the mic
# first stage at -12 dB (0xE4 bits 5-4), then flat (0x34 bits 7-5 off: band-pass, compressor, pre-emphasis) across
# the band: how wide is this path for data (4800 / 9600 baud FSK needs flat to 2.4 / 4.8 kHz)?
FLAT = (0x34, 0, 0xE0)
PWM_SWEEP = [
    ("1 kHz, 1 %", 1023, 1000, 10, []),
    ("1 kHz, 2 %", 1023, 1000, 20, []),
    ("1 kHz, 4 %", 1023, 1000, 40, []),
    ("1 kHz, 10 %, mic -12 dB", 1023, 1000, 100, [(0xE4, 0x20, 0x10)]),
    ("1 kHz, 30 %, mic -12 dB", 1023, 1000, 300, [(0xE4, 0x20, 0x10)]),
] + [(f"flat, {hz} Hz, 3 %", 1023, hz, 30, [FLAT]) for hz in (300, 1000, 2000, 2500, 3000, 3500, 3800, 4500, 5500)]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_pin_pwm_sweep(probe, sdr_bench, pytestconfig):
    """PWM audio on PC8 below the limiter, and the flat path's frequency response."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    rows = pwm_runs(probe, sdr_bench, pytestconfig, PWM_SWEEP)
    print_tx_rows("FM TX with PWM audio on PC8: level and response", rows)
    assert len(rows) == len(PWM_SWEEP)


# What sets the PWM path's floor (about 120 Hz rms in the voice band with MIC_P / MIC_N on, 50 Hz with them off), and
# does the 0x04 path, which skips the 8 kHz codec, reach the 4.8 kHz that 9600 baud FSK needs?
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_wideband(probe, sdr_bench, pytestconfig):
    """The PWM path's noise with no audio, and 0x04 tones up to 6 kHz."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    rows = pwm_runs(probe, sdr_bench, pytestconfig, [
        ("PC8 low, MIC_P / MIC_N on, flat", 1023, 0, 0, [FLAT]),
        ("PWM 50 % carrier, no audio, flat", 1023, 1000, 0, [FLAT]),
    ])
    L = probe.link

    def mod(hz):
        def during():
            r = L.probe_mod(2000, hz, 16)
            return f"{r['writes'] * 1000 // max(r['elapsed_ms'], 1)} writes/s"
        return lambda: None, during

    for hz in (1000, 3000, 3800, 4800, 6000):
        rows += keyed_runs(probe, sdr_bench, pytestconfig, [(f"0x04, {hz} Hz, 16 steps", *mod(hz))], "wb",
                           ptt_s=2.5, tone_snr=hz)
    print_tx_rows("FM TX noise floor and the 0x04 path's response", rows)
    assert len(rows) == 7


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_probe_fm_tx_sinad_reference(probe, sdr_bench, pytestconfig):
    """The measurement's own ceiling: the chip's selcall tone generator (a digital sine inside the FM chain, as in
    test_probe_fm_tx_tone_gen) playing 1 kHz, measured like the PWM and 0x04 paths."""
    if "VFO_FM" not in probe.features():
        skip("not_built", "built without the FM VFO")
    L = probe.link
    saved = regs_saved(L, {0xA0, 0xA1, 0xA3, 0xA4, 0xA5, 0xA6, 0xAE, 0xAF, 0x60})

    def restore():
        for r, v in saved.items():
            L.probe_reg(PAGE, r, v)

    def during():
        aux_write16(L, 0x11A, round(1000 / 32000 * 65536))
        for r, v in ((0xA1, 0x10), (0xA0, 0x39), (0xA6, 0x0A), (0xA3, 0x05), (0xA4, 0x05), (0xA5, 0x05),
                     (0xAF, 0x0A), (0xAE, 0xA0)):
            L.probe_reg(PAGE, r, v)
        L.probe_reg(PAGE, 0x60, 0x00)
        time.sleep(0.05)
        L.probe_reg(PAGE, 0x60, 0x80)
        time.sleep(2.5)
        return "tone generator, 1 kHz"

    try:
        rows = keyed_runs(probe, sdr_bench, pytestconfig, [("tone generator 1 kHz", restore, during)], "ref",
                          ptt_s=3.0, tone_snr=1000)
    finally:
        restore()
    print_tx_rows("The SINAD measurement's reference", rows)
    assert len(rows) == 1
