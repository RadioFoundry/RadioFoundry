# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The radio's transmit carrier in each mode against its FM carrier, on one frequency (opt-in: --allow-rf
--allow-service-stop --fm-mhz MHz; the radio transmits, the bench only receives).

FM goes out through the modem's FM path; DMR, P25 and NXDN through its digital modulator, P25 and NXDN by the air
interface provider with the deviation pair scaled to the profile (docs/spec/air-interface.md 3). A frequency correction
or bias set on one path and not the other, or a DC term that scales with the deviation pair, puts one mode's carrier
off the others, and a receiver tuned for one mode then needs another offset for the next.

test_tx_carrier_mode[dmr|p25|nxdn4800|nxdn9600]: the VFO on --fm-mhz at the lowest power. FM with no tone keys for
PTT_S, then the mode keys for PTT_S on the same VFO and frequency (tx_hz checked before each key-up). Each over's
carrier comes from the estimate the mode's own TX test uses:
- FM: the bench's FM receive (SdrBench.fm_over_start / fm_over_result), the mean of the discriminator over the
  keyed segment: an unmodulated carrier has no other mean;
- DMR: the bench board's decoded bursts (dmr_watch_start / dmr_watch_result), the median of the bursts' carrier
  offsets, as test_dmr_tx judges against --max-foff;
- P25: the FM receive's raw discriminator (the rx op's disc stream) through the host soft modem (p25tool.py), the
  median of the offsets it fits at the syncs, as test_p25_tx_off_air prints;
- NXDN: the bench's 4FSK receiver (fsk4_rx_start, the FSW as its sync), the median of the carrier offsets it fits at
  the syncs.
A digital mode's estimate is fitted to the sync's known symbols: the mean of a discriminator would also read the
mean of the symbols sent (syncs, status symbols, a quiet room's voice frames), a hundred Hz or more on P25.
Every mode keys on --fm-mhz, so that one frequency must be in tx_mhz; --dmr-freq, --p25-mhz and --nxdn-mhz aren't
used. Pass criteria:
- the FM over one keyed segment or more with a carrier; a digital over at least one sync or burst;
- the mode's carrier within MODE_TOL_PPB of the reference carrier keyed just before it (about 120 Hz at UHF). The
  reference is FM; a build without the FM VFO uses DMR instead (P25 and NXDN against DMR; DMR itself is then judged
  on the absolute limit only);
- each carrier within --max-foff Hz of the frequency (default 500), plus the bench board's own tolerance at the
  frequency (freq_tol_hz, a DMR watch's cfo_hz.tol), as test_fm_tx and test_dmr_tx judge it.
Every carrier and the difference are printed. Skipped for a mode the build lacks (DMR or the DMR VFO, P25, NXDN), a
rate the radio's air interface can't transmit (nxdn.fit4800 / nxdn.fit9600), a radio with no DMR ID (DMR), in
hotspot mode, and where the bench board lacks a receiver an over needs (bench): FM receive for FM and P25, decoded
bursts on --fm-mhz for DMR, 4FSK receive for NXDN. The VFO's mode and the NXDN settings are put back afterwards.

test_dmr_after_mode[p25|nxdn4800|nxdn9600]: DMR, then the mode, then DMR again on --dmr-freq, with no restart between
(its own docstring).

The bench board takes its own crystal out of all four the same way (the gateway's bench-api.md 14.7): the rx op (FM,
its disc stream, 4FSK) and the bursts op each mix the channel from where a carrier on the frequency shows with the
board's ppm, so every estimate is the carrier from the true frequency. What is left, ppm_tol at the frequency, is
common to the two overs of a pair (one board, seconds apart) and drops out of their difference.
"""
import statistics
import time

import pytest

from rfoundry_hil import as_int, skip
from hil_config import rig

PTT_S = 3.0
# A mode change can restart the air interface and retune: time for it and the RF supply before the key-up
MODE_SETTLE_S = 1.0
# How far a mode's carrier may sit from the reference carrier keyed just before it, in ppb so a higher band gets the
# same share: 260 ppb is about 120 Hz at 461 MHz. It holds the two estimates' scatter (medians over tens of syncs or
# bursts: a few Hz to tens) and the reference's drift between two overs back to back, test_ref_trim's figure for the
# same two terms. The symbols' own mean, which made a mean of the discriminator need 325 ppb, is out of the sync
# fits. 120 Hz is about a third of the 350 Hz decision margin of NXDN at 4800 bit/s (levels 700 Hz apart), the mode
# most hurt by an offset
MODE_TOL_PPB = 260
AIR_FIT_TX = 0x2


def fm_over(radio, sdr_bench, mhz):
    """One PTT_S FM over measured by the bench's FM receive: (carrier Hz, bench tolerance Hz, what else to print)"""
    with sdr_bench.lease():
        job = sdr_bench.fm_over_start(mhz, PTT_S + 5, bw_khz=12.5)
        radio.ptt_for(PTT_S)
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        r = sdr_bench.fm_over_result(job)
    assert r["segments"] >= 1, f"fm: no keyed segment, the board heard {r['carrier_s']} s of carrier: {r}"
    assert r["carrier_hz"] is not None, f"fm: no carrier: {r}"
    return r["carrier_hz"], r.get("freq_tol_hz"), f"discriminator mean, first 50 ms steps {r['track'][:8]}"


def dmr_over(radio, sdr_bench, mhz):
    """One PTT_S DMR over watched by the bench board's burst decoder (test_dmr_tx's carrier)"""
    from test_dmr_tx import burst_carrier
    with sdr_bench.lease():
        watch = sdr_bench.dmr_watch_start(None, mhz, PTT_S + 6)
        radio.ptt_for(PTT_S)
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        sdr = sdr_bench.dmr_watch_result(watch)
    hz = burst_carrier(sdr)
    assert hz is not None, f"dmr: the bench decoded no burst: {sdr.get('bursts_summary')}"
    cfo = sdr.get("bursts_summary", {}).get("cfo_hz")
    return hz, cfo.get("tol") if isinstance(cfo, dict) else None, f"median of {len(sdr['bursts'])} bursts"


def p25_over(radio, sdr_bench, mhz, v):
    """One PTT_S P25 over, its discriminator through the soft modem (test_p25_tx_off_air's carrier)"""
    from test_p25 import key_over, p25_decode, sync_offset
    job, _ = key_over(radio, sdr_bench, mhz, v)
    lines = p25_decode(job)
    hz = sync_offset(lines)
    assert hz is not None, "p25: the soft modem fitted no sync"
    n = sum(1 for d in lines if d["ev"] == "sync" and "outer_hz" in d)
    return round(hz), (job.header or {}).get("freq_tol_hz"), f"median of {n} syncs"


def nxdn_over(radio, sdr_bench, mhz, rate, v):
    """One PTT_S NXDN over sliced by the bench's 4FSK receiver (test_nxdn_tx_off_air's receive)"""
    from test_nxdn import key_over, sync_cfo
    job, done, _ = key_over(radio, sdr_bench, mhz, rate, v)
    cfo = sync_cfo(job.records)
    assert cfo, f"nxdn{rate}: the bench's 4FSK receiver found no sync: {done.get('summary')}"
    return round(statistics.median(cfo)), (job.header or {}).get("freq_tol_hz"), f"median of {len(cfo)} syncs"


def over(radio, sdr_bench, mhz, mode, v):
    """One over in mode, measured by its estimate (the module docstring): (carrier Hz, bench tolerance Hz, note)"""
    if mode == "fm":
        return fm_over(radio, sdr_bench, mhz)
    if mode == "dmr":
        return dmr_over(radio, sdr_bench, mhz)
    if mode == "p25":
        return p25_over(radio, sdr_bench, mhz, v)
    return nxdn_over(radio, sdr_bench, mhz, mode[4:], v)


def need_receiver(sdr_bench, mode, mhz):
    """Skip, before anything keys, when the bench board lacks the receiver an over in mode needs"""
    if mode in ("fm", "p25") and not sdr_bench.can_fm_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no FM receive")
    if mode == "dmr" and not sdr_bench.can_watch(mhz):
        skip("bench", f"the bench board {sdr_bench.api_rx} can't decode bursts on {mhz} MHz")
    if mode.startswith("nxdn") and not sdr_bench.can_fsk4_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no 4FSK receive")


def to_mode(radio, mode, ready, what, sections=("core",)):
    radio.link.dmr_fields(mode=mode)
    v = radio.wait_for(lambda x: x.get("mode") == mode and ready(x), 3.0, sections=sections, what=what)
    time.sleep(MODE_SETTLE_S)
    return v


def check_built(radio, mode):
    """Skip when the build or the radio lacks the mode; returns the NXDN settings to put back (None otherwise)"""
    feats = radio.features()
    if mode == "dmr":
        if "DMR" not in feats or "VFO_DMR" not in feats:
            skip("not_built", "built without DMR or the DMR VFO")
        if (as_int(radio.view(["dmr"]).get("dmr.own_id")) or 0) == 0:
            skip("state", "the radio has no DMR ID (dmr.own_id=0)")
    elif mode == "p25":
        if "P25" not in feats:
            skip("not_built", "built without CONFIG_P25")
    else:
        from test_nxdn import settings_of
        v = radio.view(["nxdn"])
        if v.get("nxdn.built") != "1":
            skip("not_built", "built without CONFIG_NXDN")
        rate = mode[4:]
        if (as_int(v.get(f"nxdn.fit{rate}")) or 0) & AIR_FIT_TX == 0:
            skip("no_hw", f"the radio's air interface can't transmit NXDN at {rate} bit/s")
        return settings_of(v)
    return None


def enter(radio, mode):
    """The VFO switched to mode, its engine running"""
    L = radio.link
    if mode == "fm":
        return to_mode(radio, "fm", lambda x: True, "the FM VFO")
    if mode == "dmr":
        return to_mode(radio, "dmr", lambda x: x.get("dmr.active") == "1", "the DMR VFO", ("core", "dmr"))
    if mode == "p25":
        return to_mode(radio, "p25", lambda x: x.get("p25.on") == "1", "P25 running on the VFO", ("core", "p25"))
    rate = mode[4:]
    L.nxdn_settings_update(rate9600=rate == "9600")
    return to_mode(radio, "nxdn", lambda x: x.get("nxdn.on") == "1" and x.get("nxdn.rate") == rate,
                   f"NXDN at {rate} bit/s on the VFO", ("core", "nxdn"))


MODES = [
    pytest.param("dmr", marks=[pytest.mark.feature("vfo_dmr"), pytest.mark.needs_switch("VFO_DMR")]),
    pytest.param("p25", marks=pytest.mark.feature("p25")),
    pytest.param("nxdn4800", marks=pytest.mark.feature("nxdn")),
    pytest.param("nxdn9600", marks=pytest.mark.feature("nxdn")),
]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.parametrize("mode", MODES)
def test_tx_carrier_mode(radio, sdr_bench, pytestconfig, mode):
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    feats = radio.features()
    if "VFO_FM" in feats:
        ref = "fm"
    elif mode != "dmr" and "DMR" in feats and "VFO_DMR" in feats:
        ref = "dmr"
    else:
        ref = None      # DMR without an FM VFO: no reference, the absolute limit only
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")
    nxdn_old = check_built(radio, mode)
    order = ([ref] if ref else []) + [mode]
    for m in order:
        need_receiver(sdr_bench, m, mhz)
    carriers = {}
    with radio.vfo_session():
        L = radio.link
        try:
            L.vfo_set(hz)
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz), what="VFO tuned")
            time.sleep(MODE_SETTLE_S)
            for m in order:
                v = enter(radio, m)
                assert v.get("tx_hz") == str(hz), f"{m}: the VFO transmits on {v.get('tx_hz')}, not {hz}"
                radio.lowest_power()
                carriers[m] = over(radio, sdr_bench, mhz, m, v)
                c, tol_hz, note = carriers[m]
                print(f"\n{m}: carrier {c:+d} Hz, {note} (bench +-{tol_hz} Hz)")
        finally:
            if nxdn_old is not None:
                L.nxdn_settings(**nxdn_old)
    dig = carriers[mode][0]
    tol = round(MODE_TOL_PPB * mhz / 1000)
    if ref:
        base = carriers[ref][0]
        print(f"{mhz} MHz: {ref} {base:+d} Hz, {mode} {dig:+d} Hz, {mode} - {ref} {dig - base:+d} Hz (limit +-{tol})")
    max_foff = pytestconfig.getoption("--max-foff")
    for m, (c, tol_hz, _) in carriers.items():
        lim = max_foff + (tol_hz or 0.0)
        assert abs(c) < lim, f"{m}: carrier offset {c:+d} Hz (limit {lim:.0f} Hz)"
    if ref:
        assert abs(dig - base) <= tol, f"{mode}'s carrier is {dig - base:+d} Hz from {ref}'s (limit +-{tol} Hz)"


# The second DMR over must decode at least this share of the first's bursts: a mode that leaves the modulator or the
# DMR engine set up its way makes the bench (and a hotspot) decode few or none
SWITCH_BURSTS_MIN = 0.8

SWITCH_MODES = [
    pytest.param("p25", marks=pytest.mark.feature("p25")),
    pytest.param("nxdn4800", marks=pytest.mark.feature("nxdn")),
    pytest.param("nxdn9600", marks=pytest.mark.feature("nxdn")),
]


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.feature("vfo_dmr")
@pytest.mark.needs_switch("VFO_DMR")
@pytest.mark.parametrize("mode", SWITCH_MODES)
def test_dmr_after_mode(radio, sdr_bench, pytestconfig, mode):
    """DMR, then mode, then DMR again on one VFO frequency (--dmr-freq) without a restart: the second DMR over must
    decode like the first, on the same carrier (MODE_TOL_PPB), and mode's carrier must match DMR's. A mode that leaves
    the modulator's deviation pair, the RF chip's set-up or the DMR engine its own way shows here, where a channel
    change from a P25 or NXDN channel to a DMR one would no longer reach a hotspot or repeater until a power cycle."""
    mhz = rig(pytestconfig, "dmr-freq")
    hz = round(mhz * 1e6)
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")
    check_built(radio, "dmr")
    nxdn_old = check_built(radio, mode)
    for m in ("dmr", mode):
        need_receiver(sdr_bench, m, mhz)
    res = []
    with radio.vfo_session():
        L = radio.link
        try:
            L.vfo_set(hz)
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz), what="VFO tuned")
            time.sleep(MODE_SETTLE_S)
            for m in ("dmr", mode, "dmr"):
                v = enter(radio, m)
                assert v.get("tx_hz") == str(hz), f"{m}: the VFO transmits on {v.get('tx_hz')}, not {hz}"
                radio.lowest_power()
                if m == "dmr":
                    from test_dmr_tx import burst_carrier
                    with sdr_bench.lease():
                        watch = sdr_bench.dmr_watch_start(None, mhz, PTT_S + 6)
                        radio.ptt_for(PTT_S)
                        radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
                        sdr = sdr_bench.dmr_watch_result(watch)
                    n = len(sdr.get("bursts") or [])
                    c = burst_carrier(sdr)
                    res.append((m, c, n))
                    print(f"\n{m} over {len(res)}: {n} bursts, carrier {c} Hz")
                else:
                    c, _, note = over(radio, sdr_bench, mhz, m, v)
                    res.append((m, c, None))
                    print(f"\n{m}: carrier {c:+d} Hz, {note}")
        finally:
            if nxdn_old is not None:
                L.nxdn_settings(**nxdn_old)
    (_, c1, n1), (_, cm, _), (_, c2, n2) = res
    tol = round(MODE_TOL_PPB * mhz / 1000)
    assert n1 > 0 and c1 is not None, "the first DMR over: the bench decoded no burst"
    assert n2 >= SWITCH_BURSTS_MIN * n1, f"DMR after {mode}: {n2} bursts decoded, {n1} before it"
    assert c2 is not None, f"DMR after {mode}: no carrier"
    assert abs(c2 - c1) <= tol, f"DMR after {mode}: carrier {c2 - c1:+d} Hz from before it (limit +-{tol})"
    assert abs(cm - c1) <= tol, f"{mode}'s carrier is {cm - c1:+d} Hz from DMR's (limit +-{tol})"
