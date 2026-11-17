# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The radio's transmit carrier in each mode against its FM carrier, on one frequency (opt-in: --allow-rf
--allow-service-stop --fm-mhz MHz; the radio transmits, the bench only receives).

FM goes out through the modem's FM path; DMR, P25 and NXDN through its digital modulator, P25 and NXDN by the air
interface provider with the deviation pair scaled to the profile (docs/spec/air-interface.md 3). A frequency correction
or bias set on one path and not the other, or a DC term that scales with the deviation pair, puts one mode's carrier
off the others, and a receiver tuned for one mode then needs another offset for the next.

test_tx_carrier_mode[dmr|p25|nxdn4800|nxdn9600]: the VFO on --fm-mhz at the lowest power. FM with no tone keys for
PTT_S, then the mode keys for PTT_S on the same VFO and frequency (tx_hz checked before each key-up). Both overs are
measured by the same analyzer, the bench's FM receive (SdrBench.fm_over_start / fm_over_result: the board's mean carrier
offset, its own crystal taken out). Every mode keys on --fm-mhz, so that one frequency must be in tx_mhz; --dmr-freq,
--p25-mhz and --nxdn-mhz aren't used. Pass criteria:
- each over one keyed segment or more with a carrier;
- the mode's carrier within MODE_TOL_PPB of the reference carrier keyed just before it (about 150 Hz at UHF). The
  reference is FM; a build without the FM VFO uses DMR instead (P25 and NXDN against DMR; DMR itself is then judged
  on the absolute limit only);
- each carrier within --max-foff Hz of the frequency (default 500), plus the bench board's own tolerance at the
  frequency (freq_tol_hz), as test_fm_tx and test_dmr_tx judge it.
Every carrier and the difference are printed. Skipped for a mode the build lacks (DMR or the DMR VFO, P25, NXDN), a
rate the radio's air interface can't transmit (nxdn.fit4800 / nxdn.fit9600), a radio with no DMR ID (DMR), and in
hotspot mode. The VFO's mode and the NXDN settings are put back afterwards.
"""
import time

import pytest

from rfoundry_hil import as_int, skip
from hil_config import rig

PTT_S = 3.0
# A mode change can restart the air interface and retune: time for it and the RF supply before the key-up
MODE_SETTLE_S = 1.0
# How far a mode's carrier may sit from the FM carrier keyed just before it, in ppb so a higher band gets the same
# share: 325 ppb is about 150 Hz at 461 MHz. It holds the board's carrier estimate and the reference's drift between
# two overs back to back (test_ref_trim allows 260 ppb for that and a trim's scale), plus the transmitted symbols'
# own mean, which this analyzer (a mean of the discriminator) reads as carrier in a digital mode: syncs, status
# symbols and a quiet room's voice frames, tens of Hz. 150 Hz is under half of the 350 Hz decision margin of NXDN at
# 4800 bit/s (levels 700 Hz apart), the mode most hurt by an offset
MODE_TOL_PPB = 325
AIR_FIT_TX = 0x2
BURST_DB = 1.0          # a bursted over's full blocks: within this of the strongest
BURST_MIN_BLOCKS = 20   # about a third of a 3 s DMR over's full 10 ms blocks


def burst_carrier(job):
    """The mean carrier of a bursted over (DMR simplex: 27.5 ms on, 32.5 ms off), which fm_measure doesn't count as a
    segment: the board's carrier blocks within BURST_DB of the strongest ones (a block that straddles a burst's edge is
    weaker and carries the off part's noise), None without enough of them"""
    import fm_measure
    blocks = [b for b in fm_measure.blocks_of(job.records) if b.get("carrier") and b.get("cfo_hz") is not None
              and b.get("power_dbfs") is not None]
    print(f"bursted over: {len(blocks)} carrier blocks of {len(job.records)} records")
    if not blocks:
        return None
    top = sorted(b["power_dbfs"] for b in blocks)[len(blocks) * 9 // 10]
    full = [b["cfo_hz"] for b in blocks if b["power_dbfs"] >= top - BURST_DB]
    return round(sum(full) / len(full)) if len(full) >= BURST_MIN_BLOCKS else None


def over(radio, sdr_bench, mhz, tag):
    """One PTT_S over measured by the bench's FM receive: fm_measure's report (a bursted over's carrier from its
    full blocks)"""
    with sdr_bench.lease():
        job = sdr_bench.fm_over_start(mhz, PTT_S + 5, bw_khz=12.5)
        radio.ptt_for(PTT_S)
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        r = sdr_bench.fm_over_result(job)
    if r["segments"] == 0 and r.get("carrier_s"):
        r["carrier_hz"] = burst_carrier(job)
        r["segments"] = 1 if r["carrier_hz"] is not None else 0
        r["track"] = r.get("track") or []
    assert r["segments"] >= 1, f"{tag}: no keyed segment, the board heard {r['carrier_s']} s of carrier: {r}"
    assert r["carrier_hz"] is not None, f"{tag}: no carrier: {r}"
    return r


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
    if not sdr_bench.can_fm_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no FM receive")
    carriers = {}
    with radio.vfo_session():
        L = radio.link
        try:
            L.vfo_set(hz)
            radio.wait_for(lambda v: v.get("rx_hz") == str(hz), what="VFO tuned")
            time.sleep(MODE_SETTLE_S)
            for m in ([ref] if ref else []) + [mode]:
                v = enter(radio, m) if m != "fm" else to_mode(radio, "fm", lambda x: True, "the FM VFO")
                assert v.get("tx_hz") == str(hz), f"{m}: the VFO transmits on {v.get('tx_hz')}, not {hz}"
                radio.lowest_power()
                r = over(radio, sdr_bench, mhz, m)
                print(f"\n{m}: carrier {r['carrier_hz']:+d} Hz, first 50 ms steps {r['track'][:8]} (bench "
                      f"+-{r.get('freq_tol_hz')} Hz, {r.get('ppm_source')})")
                carriers[m] = r
        finally:
            if nxdn_old is not None:
                L.nxdn_settings(**nxdn_old)
    dig = carriers[mode]["carrier_hz"]
    tol = round(MODE_TOL_PPB * mhz / 1000)
    if ref:
        base = carriers[ref]["carrier_hz"]
        print(f"{mhz} MHz: {ref} {base:+d} Hz, {mode} {dig:+d} Hz, {mode} - {ref} {dig - base:+d} Hz (limit +-{tol})")
    max_foff = pytestconfig.getoption("--max-foff")
    for m, r in carriers.items():
        lim = max_foff + (r.get("freq_tol_hz") or 0.0)
        assert abs(r["carrier_hz"]) < lim, f"{m}: carrier offset {r['carrier_hz']:+d} Hz (limit {lim:.0f} Hz)"
    if ref:
        assert abs(dig - base) <= tol, f"{mode}'s carrier is {dig - base:+d} Hz from {ref}'s (limit +-{tol} Hz)"
