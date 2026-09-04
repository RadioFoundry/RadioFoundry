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
"""
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


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_probe_fm_tx_variants(probe, sdr_bench, pytestconfig):
    if "VFO_FM" not in probe.features():
        pytest.skip("built without the FM VFO")
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
            for i, (name, regs, amp) in enumerate(TX_VARIANTS):
                L.probe_start(TONE_HZ, amp)
                rdir = sdr_bench.run_dir(f"hil_aprobe_tx{i}")
                with sdr_bench.lease():
                    cap = sdr_bench.capture_start(rdir, mhz, seconds=PTT_S + 5)
                    L.ptt(True, int(PTT_S * 1000) + 1500)
                    try:
                        probe.wait_for(lambda v: v.get("state") == "tx", timeout=2, what="keyed")
                        time.sleep(0.2)          # after the TX set-up has written its registers
                        apply(L, 0xE0, 0, 0x40)  # microphone (LINEIN1) off
                        for reg, s, c in regs:
                            apply(L, reg, s, c)
                        time.sleep(PTT_S)
                    finally:
                        L.ptt(False)
                    probe.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
                    sdr_bench.capture_wait()
                L.probe_stop()
                rc, text = sdr_bench.analyze_fm(cap, bw_khz=12.5, ppm=pytestconfig.getoption("--sdr-ppm"), freq=mhz)
                m = re.search(r"voice 300-3000 Hz: peak (\d+) Hz \(99\.5%\), (\d+) Hz \(99\.9%\), rms (\d+) Hz", text)
                rows.append((name, tuple(int(x) for x in m.groups()) if m else None))
                if not m:
                    print(text)
        finally:
            L.ptt(False)
            L.probe_stop()
    print("\nFM TX deviation, voice band (peak 99.5 % / rms, Hz):")
    base = rows[0][1][2] if rows and rows[0][1] else None
    for name, r in rows:
        print(f"{name:<52} {'-' if r is None else f'{r[0]:5d} / {r[2]:5d}'}")
        if r and base is not None and r[2] > 4 * max(base, 20):
            print(f"  -> {name}: MCU audio reaches the modulator (rms {r[2]} Hz vs {base} Hz muted)")
    assert len(rows) == len(TX_VARIANTS)
