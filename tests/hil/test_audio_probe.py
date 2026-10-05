# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""USB audio feasibility on the DM-1701: which HR_C6000 settings put FM audio on the I2S link between the chip and the
MCU (docs/spec/usb-audio.md 2.3). Needs firmware built with CONFIG_AUDIO_PROBE=y (skipped otherwise): the debug
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

The HR_C6000's own FM sample path (usb-audio.md 2.3, candidate C): the manual's RAM tables give two 512-byte "FM data"
buffers in each of the TX and RX RAMs (page 2, 0x030-0x22F and 0x230-0x42F; the TX ones "can be written externally
for FM send"), and 0x82 has FM-mode flags bit 7 "sending data request" and bit 1 "receiving data". Neither the sample
format nor the control bit that picks the RAM over the codec is documented, so these tests try the likely ones:

- test_probe_fm_ram_rx (radio only, NOAA): with the FM flags unmasked, how often each 0x82 flag comes and whether the
  RX RAM buffers change, then the buffers' level read as 16-bit samples (both byte orders) and as 8-bit, with the
  NOAA signal and on a quiet frequency.
- test_probe_fm_ram_tx_variants (--allow-rf --allow-service-stop): both TX RAM buffers hold a whole number of cycles
  of a 1 kHz tone (8 samples per cycle at 8 kHz), so a chip that replays them sends a steady tone without the MCU
  refilling them. Keyed with the microphone muted, per candidate control setting: the 0x82 flag rate and the voice
  band deviation the SDR bench measures.
"""
import math
import time

import pytest

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

# FM transmit candidates: applied while keyed (the TX set-up writes 0xE0 = 0xC9), microphone (LINEIN1) muted in all
TX_VARIANTS = [
    ("mic muted, no tone", [], 0),
    ("mic muted, tone", [], TONE_AMP),
    ("mic muted, tone, codec pass-through (0xE3 |= 0x01)", [(0xE3, 0x01, 0)], TONE_AMP),
    ("mic muted, tone, LINEIN2 on (0xE0 |= 0x20)", [(0xE0, 0x20, 0)], TONE_AMP),
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
]
RAM_TX_SAVED = {0x81, 0xDB, 0x60, 0x06, 0xE0}


def tone_bytes(order, amp=TONE_AMP, n=RAM_FM_LEN // 2):
    """n samples of a TONE_HZ sine at 8 kHz, 16-bit in byte order order ('<' or '>')"""
    import struct
    x = [round(amp * math.sin(2 * math.pi * TONE_HZ * i / 8000)) for i in range(n)]
    return struct.pack(f"{order}{n}h", *x)


def ram_levels(data):
    """RMS of the buffer read as 16-bit little / big endian and as signed 8-bit, and its distinct byte count"""
    import struct
    out = {}
    for tag, fmt in (("le16", "<"), ("be16", ">")):
        x = struct.unpack(f"{fmt}{len(data) // 2}h", data)
        m = sum(x) / len(x)
        out[tag] = math.sqrt(sum((v - m) ** 2 for v in x) / len(x))
    x8 = [b - 256 if b > 127 else b for b in data]
    m = sum(x8) / len(x8)
    out["s8"] = math.sqrt(sum((v - m) ** 2 for v in x8) / len(x8))
    out["distinct"] = len(set(data))
    return out


def watch_text(w):
    bits = ", ".join(f"bit {b} {c * 1000 / max(w['elapsed_ms'], 1):.1f}/s" for b, (c, _) in w["bits"].items() if c)
    return f"{bits or 'no flags'}; RAM word changed {w['ram_changes']}x in {w['elapsed_ms']} ms"


@pytest.fixture
def probe(radio):
    from odr_debug import DebugLinkError
    try:
        radio.link.probe_status()
    except DebugLinkError as e:
        pytest.skip(f"firmware without the audio probe (build with CONFIG_AUDIO_PROBE=y): {e}")
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
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


def test_probe_stream_in_fm(probe, pytestconfig):
    if "VFO_FM" not in probe.features():
        pytest.skip("built without the FM VFO")
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
        pytest.skip("built without the FM VFO")
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


def keyed_runs(probe, sdr_bench, pytestconfig, runs, tag):
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
                    cap = sdr_bench.capture_start(rdir, mhz, seconds=PTT_S + 5)
                    L.ptt(True, int(PTT_S * 1000) + 1500)
                    try:
                        probe.wait_for(lambda v: v.get("state") == "tx", timeout=2, what="keyed")
                        time.sleep(0.2)          # after the TX set-up has written its registers
                        apply(L, 0xE0, 0, 0x40)  # microphone (LINEIN1) off
                        t0 = time.monotonic()
                        note = during()
                        time.sleep(max(0.0, PTT_S - (time.monotonic() - t0)))
                    finally:
                        L.ptt(False)
                    probe.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
                    sdr_bench.capture_wait()
                L.probe_stop()
                rc, text = sdr_bench.analyze_fm(cap, bw_khz=12.5, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz)
                m = re.search(r"voice 300-3000 Hz: peak (\d+) Hz \(99\.5%\), (\d+) Hz \(99\.9%\), rms (\d+) Hz", text)
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
def test_probe_fm_tx_variants(probe, sdr_bench, pytestconfig):
    if "VFO_FM" not in probe.features():
        pytest.skip("built without the FM VFO")
    L = probe.link

    def run(regs, amp):
        def during():
            for reg, s_, c in regs:
                apply(L, reg, s_, c)
        return (lambda: L.probe_start(TONE_HZ, amp)), during

    runs = [(name, *run(regs, amp)) for name, regs, amp in TX_VARIANTS]
    rows = keyed_runs(probe, sdr_bench, pytestconfig, runs, "tx")
    print_tx_rows("FM TX from the I2S link", rows)
    assert len(rows) == len(TX_VARIANTS)


def test_probe_fm_ram_rx(probe, pytestconfig):
    if "VFO_FM" not in probe.features():
        pytest.skip("built without the FM VFO")
    L = probe.link
    noaa = round(pytestconfig.getoption("--noaa-mhz") * 1e6)
    quiet = noaa + 2_000_000
    res = {}
    with probe.vfo_session():
        saved = regs_saved(L, {0x81})
        try:
            for tag, hz in (("signal", noaa), ("quiet", quiet)):
                L.vfo_set(hz)
                probe.wait_for(lambda v: v.get("rx_hz") == str(hz), what=f"VFO on {hz}")
                if tag == "signal":
                    probe.wait_for(lambda v: v.get("state") == "rx", timeout=6, what="NOAA squelch open")
                for r, v in saved.items():
                    L.probe_reg(PAGE, r, v)
                masked = L.probe_watch(1000, FM_FLAGS, ram_addr=RAM_FM)
                apply(L, 0x81, FM_FLAG_MASK, 0)
                unmasked = L.probe_watch(1000, FM_FLAGS, ram_addr=RAM_FM)
                a = L.probe_ram(RAM_PAGE, RAM_FM, RAM_FM_LEN)
                time.sleep(0.05)
                b = L.probe_ram(RAM_PAGE, RAM_FM, RAM_FM_LEN)
                res[tag] = (masked, unmasked, ram_levels(a), sum(x != y for x, y in zip(a, b)), a)
        finally:
            for r, v in saved.items():
                L.probe_reg(PAGE, r, v)
    print("\nFM RX through the RX RAM FM buffers (page 2 0x030-0x42F):")
    for tag in ("signal", "quiet"):
        masked, unmasked, lv, diff, a = res[tag]
        print(f"{tag:<7} 0x81 as set: {watch_text(masked)}")
        print(f"{'':<7} FM flags unmasked: {watch_text(unmasked)}")
        print(f"{'':<7} RMS le16 {lv['le16']:7.0f}  be16 {lv['be16']:7.0f}  s8 {lv['s8']:5.1f}  "
              f"{lv['distinct']} distinct bytes, {diff} bytes changed in 50 ms")
        print(f"{'':<7} first bytes: {a[:32].hex(' ')}")
    for fmt in ("le16", "be16", "s8"):
        s_, q = res["signal"][2][fmt], res["quiet"][2][fmt]
        if s_ > 4 * max(q, 1) and res["signal"][3] > 0:
            print(f"  -> the RX RAM carries receive audio read as {fmt} (RMS {s_:.0f} vs {q:.0f})")
    assert set(res) == {"signal", "quiet"}


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_probe_fm_ram_tx_variants(probe, sdr_bench, pytestconfig):
    if "VFO_FM" not in probe.features():
        pytest.skip("built without the FM VFO")
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
