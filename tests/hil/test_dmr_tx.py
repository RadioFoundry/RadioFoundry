# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Radio DMR transmit checked off air by the SDR bench (opt-in: --allow-rf --allow-service-stop; the radio and the
bench's receivers only, the bench never transmits here).

The radio, on low power on the DMR test channel, keys for 3 s through the debug link's PTT. On the bench, a raw
RTL-SDR capture (sdrmon.py capture, analyzed by sdrmon.py analyze) and the bench modem's DMR listener (bench_tx.py
listen) run during the over. The checks are dmr_bench.py's case T1 (docs/testing/dmr-bench.md), on the radio's own
transmission (from its first voice LC header, by source ID):

- SDR: MS sync only; the channel's color code only; at least one voice LC header (3 expected); header and terminator
  LC with source = the radio's own ID (dmr.own_id), destination = the channel's talk target (dmr.target_id), FLCO 0
  (group) or 3 (private); embedded LC decoded and equal to the header's; a terminator after the last voice burst;
  voice A-F in whole superframes; same-slot spacing 60.00 +- 0.05 ms; |carrier offset| < --max-foff Hz (median over
  the bursts, default 500, corrected by --sdr-ppm when given); burst on-time 27-31.5 ms; ramps < 1.5 ms; no
  overlaps, power steps or unexplained carrier; capture clipping < 1 %.
- bench modem: exactly one call, started by a header (not late entry), ended by a terminator, source = the radio's
  ID, voice bursts in whole superframes, AMBE FEC corrections at most 0.5 % of the AMBE bits heard (the bench modem
  is overloaded by the radio a meter away, even at 50 mW: a few corrected bits are its own; more than any are a
  warning). Run with --sdr-gain 0 on a bench where the RTL-SDR clips. When the capture still clips (1 % or more of
  the samples at the ADC rails) the waveform checks (sync family, A-F order, spacing, on-time, ramps, overlaps, power
  steps, stray carrier) are reported as not judged, with a warning; the decoded content stays strict.
- radio: the call stack counted one TX call (call.tx_calls + 1), no TX underruns.
- talker alias: when the channel sends one (dmr.tx_alias not empty), the alias assembled from the embedded talker
  alias LCs (FLCO 4-7) on air equals it.

test_dmr_tx_talker_alias (also --allow-codeplug-write): the test channel's talker alias flag (TS1 text) is turned on
over CPS (backed up first, restored and verified afterwards, test_cps.codeplug_edited); dmr.alias_tx=1 and
dmr.tx_alias = the radio name; a 4 s over: the alias the SDR decodes from the embedded LCs and the one the bench
modem's listener assembles equal dmr.tx_alias (at least one of the two must have heard it complete; both, when they
did, must agree), call.tx_alias_lcs grew.

test_dmr_privacy_tx: Motorola basic privacy key 1 loaded (RADIO op 19), a 3 s over: every voice LC header and
terminator from us that the SDR or the bench modem decoded carries the privacy service option (so & 0x40), every
embedded call LC FID 0x10, and a clear over just before carries no privacy option. The scrambling itself isn't
checked (the microphone's frames are unknown; the bench modem's frame counts are printed). The key is turned off
afterwards (the test skips when a key was set already).
"""
import warnings

import pytest

PTT_S = 3.0
# dmr_bench.check_tx items that judge the waveform, meaningless in a capture whose ADC clipped
SDR_WAVEFORM_CHECKS = ("capture not saturated", "sync family", "voice A-F in order", "same-slot spacing", "ramps",
                       "no overlapping bursts", "no power steps", "burst on-time", "no unexplained carrier")


def talker_alias_on_air(bursts):
    """The talker alias in the embedded LCs sdrmon decoded (FLCO 4 header, 5-7 blocks; TS 102 361-2 7.2.18-7.2.21),
    or None. Bit layout as core/dmr/call/dmr_lc.c: octet 2 = format (2) + length (5) + the first data bit, then 48 bits
    in the header and 56 per block from octet 2; 8-bit text starts after one pad bit."""
    lcs = {}
    for b in bursts:
        lc = b.get("lc") or {}
        if 4 <= lc.get("emb_flco", -1) <= 7 and "raw" in lc:
            lcs[lc["emb_flco"]] = bytes.fromhex(lc["raw"])
    if 4 not in lcs:
        return None
    bits = lambda data: [(byte >> (7 - i)) & 1 for byte in data for i in range(8)]
    h = lcs[4]
    fmt, length = h[2] >> 6, (h[2] >> 1) & 0x1F
    stream = [h[2] & 1] + bits(h[3:9])
    for flco in (5, 6, 7):
        if flco not in lcs:
            break
        stream += bits(lcs[flco][2:9])
    width, at = (7, 0) if fmt == 0 else (8, 1)
    chars = []
    for _ in range(length):
        if at + width > len(stream):
            break
        chars.append(int("".join(map(str, stream[at:at + width])), 2))
        at += width
    return bytes(chars).decode("latin-1")


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_dmr_tx_off_air(radio, sdr_bench, pytestconfig):
    if "DMR" not in radio.features():
        pytest.skip("built without DMR")
    import dmr_bench
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    d = radio.view(["dmr"])
    own, target, private = int(d["dmr.own_id"]), int(d["dmr.target_id"]), d.get("dmr.target_private") == "1"
    ch_cc = int(radio.view(["channel"])["ch.color_code"])
    if own == 0:
        pytest.skip("the radio has no DMR ID (dmr.own_id=0)")
    assert ch_cc == cc, f"the channel's color code {ch_cc} != --dmr-cc {cc}"
    radio.reset_counters()
    _, before = radio.counters()
    rdir = sdr_bench.run_dir("hil_dmr_tx")
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, freq, seconds=PTT_S + 8)
        lis = sdr_bench.listen_start(freq, cc, seconds=PTT_S + 6, ambe_out=f"~/{sdr_bench.dir}/{rdir}/heard")
        radio.ptt_for(PTT_S)
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        heard = sdr_bench.listen_result(lis)
        sdr_bench.capture_wait()
    sdr = sdr_bench.analyze_dmr(cap, freq, cc)      # with the gateway back up, as dmr_bench.py does
    _, after = radio.counters()

    c = dmr_bench.Checks()
    dmr_bench.check_tx(sdr, {"flco": 3 if private else 0, "dst": target}, own, cc, c)
    dmr_bench.check_modem_calls(heard, own, c)
    c.items = [i for i in c.items if not i["check"].startswith("|frequency offset|")    # replaced below
               and not i["check"].startswith("bench modem: AMBE FEC")]
    for k in [r for r in heard if "call" in r][:1]:
        fixed, frames = k.get("ambe_fec_bits_corrected", 0), k.get("ambe_frames", 0)
        c.add("bench modem: AMBE FEC corrections <= 0.5 % of the bits", fixed <= 0.005 * 72 * max(1, frames),
              f"{fixed} bits in {frames} frames")
        if fixed:
            warnings.warn(f"the bench modem corrected {fixed} AMBE bits in {frames} frames (overloaded?)")
    max_foff = pytestconfig.getoption("--max-foff")
    ppm = pytestconfig.getoption("--sdr-ppm")
    fo = sorted(b["foff_hz"] for b in sdr.get("bursts", []))
    if fo:
        med = fo[len(fo) // 2] - (freq * ppm if ppm else 0.0)     # MHz x ppm = Hz
        c.add(f"|carrier offset| < {max_foff} Hz{' (SDR ppm corrected)' if ppm else ''}", abs(med) < max_foff,
              f"median {med:.0f} Hz")
    tx_calls = after.get("call.tx_calls", 0) - before.get("call.tx_calls", 0)
    c.add("radio: one TX call", tx_calls == 1, f"{tx_calls}")
    under = after.get("call.tx_underruns", 0) - before.get("call.tx_underruns", 0)
    c.add("radio: no TX underruns", under == 0, f"{under}")
    alias = d.get("dmr.tx_alias", "")
    if alias:
        got = talker_alias_on_air(sdr.get("bursts", []))
        c.add(f"talker alias on air = {alias!r}", got == alias, repr(got))
    else:
        print("talker alias: the channel sends none (dmr.tx_alias empty, or firmware without the key)")
    clip = next((float(i["detail"].split()[0]) for i in c.items if i["check"].startswith("capture not saturated")),
                0.0)
    if clip >= 1.0:
        # A clipped capture distorts the waveform (false BS syncs, timing and power-step artifacts): those checks
        # aren't judged, with a warning. What the SDR decoded (CC, LCs, embedded LC, headers, terminator) and the
        # bench modem's checks stay strict.
        for i in c.items:
            if i["ok"] is False and i["check"].startswith(SDR_WAVEFORM_CHECKS):
                i["ok"], i["detail"] = None, f"{i['detail']} (not judged: the capture clipped {clip:.1f} %)"
        warnings.warn(f"the SDR capture clipped {clip:.1f} %: waveform checks not judged (the bench needs an "
                      f"attenuator in front of the RTL-SDR)")
    for i in c.items:
        print(f"{ {True: 'ok  ', False: 'FAIL', None: 'man '}[i['ok']]} {i['check']}  {i['detail']}")
    print(f"captures on the bench: ~/{sdr_bench.dir}/{rdir}/")
    failed = [i for i in c.items if i["ok"] is False]
    assert not failed, "; ".join(f"{i['check']} ({i['detail']})" for i in failed)


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_dmr_gps_tx(radio, sdr_bench, pytestconfig):
    """Our ETSI GPS Info (FLCO 8) in the alias superframes: a fixed APRS position and "position in DMR calls" set over
    the debug link, a 4 s over, the bench modem's listener decodes the GPS Info LC (bench_tx.py listen: the call's
    "gps"). The settings are put back afterwards."""
    feats = radio.features()
    if "DMR_GPS" not in feats or "APRS" not in feats:
        pytest.skip("needs CONFIG_DMR_GPS and CONFIG_APRS (the position source)")
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    if int(radio.view(["dmr"])["dmr.own_id"]) == 0:
        pytest.skip("the radio has no DMR ID (dmr.own_id=0)")
    before = radio.view(["aprs_set"])
    lat, lon = 33.7490, -84.3880
    try:
        radio.link.aprs_set("lat", lat)
        radio.link.aprs_set("lon", lon)
        radio.link.aprs_set("pos_src", 3)          # fixed
        radio.link.aprs_set("dmr_gps", 1)
        with sdr_bench.lease():
            lis = sdr_bench.listen_start(freq, cc, seconds=4.0 + 6)
            radio.ptt_for(4.0)
            radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
            heard = sdr_bench.listen_result(lis)
    finally:
        radio.link.aprs_set("dmr_gps", 0)
        radio.link.aprs_set("pos_src", int(before.get("set.aprs_pos_src", 0)))
    calls = [r for r in heard if "call" in r]
    print("bench heard:", calls)
    assert len(calls) == 1 and calls[0].get("gps"), calls
    glat, glon, err = calls[0]["gps"]
    assert abs(glat - lat) < 2e-5 and abs(glon - lon) < 2e-5, calls[0]["gps"]
    assert err == 3, err        # a typed position: 200 m estimate, error code 3 (< 2 km)


def _dmr_tx_setup(radio, pytestconfig):
    if "DMR" not in radio.features():
        pytest.skip("built without DMR")
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    radio.go_home()
    pos = radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    d = radio.view(["dmr"])
    if int(d["dmr.own_id"]) == 0:
        pytest.skip("the radio has no DMR ID (dmr.own_id=0)")
    return freq, cc, pos, d


def _over(radio, sdr_bench, freq, cc, seconds, tag):
    """A PTT over captured by the SDR and heard by the bench modem; returns (sdr analysis, bench lines)."""
    rdir = sdr_bench.run_dir(tag)
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, freq, seconds=seconds + 8)
        lis = sdr_bench.listen_start(freq, cc, seconds=seconds + 6)
        radio.ptt_for(seconds)
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=3, what="TX ended")
        heard = sdr_bench.listen_result(lis)
        sdr_bench.capture_wait()
    sdr = sdr_bench.analyze_dmr(cap, freq, cc)
    print(f"captures on the bench: ~/{sdr_bench.dir}/{rdir}/")
    return sdr, heard


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.codeplug_write
def test_dmr_tx_talker_alias(radio, sdr_bench, pytestconfig, backup_dir):
    if "DMR_TALKER_ALIAS" not in radio.features():
        pytest.skip("built without CONFIG_DMR_TALKER_ALIAS")
    import cps_client
    from test_cps import codeplug_edited
    freq, cc, _, d = _dmr_tx_setup(radio, pytestconfig)
    if d.get("dmr.ts") != "1":
        pytest.skip("the test channel is on TS2: the edit sets the TS1 flag")
    number = int(radio.core()["channel_number"])
    _, before = radio.counters()
    with codeplug_edited(radio, backup_dir, lambda img: cps_client.set_channel_alias(img, number, ts1=True),
                         "alias"):
        v = radio.wait_for(lambda x: x.get("dmr.alias_tx") == "1", timeout=5, sections=("dmr",),
                           what="talker alias TX on (dmr.alias_tx)")
        alias = v.get("dmr.tx_alias", "")
        assert alias, "dmr.alias_tx=1 but dmr.tx_alias is empty (no radio name in the codeplug?)"
        sdr, heard = _over(radio, sdr_bench, freq, cc, 4.0, "hil_dmr_alias")
    _, after = radio.counters()
    radio.wait_for(lambda x: x.get("dmr.alias_tx") == "0", timeout=5, sections=("dmr",), what="alias TX off again")
    lcs = after.get("call.tx_alias_lcs", 0) - before.get("call.tx_alias_lcs", 0)
    on_sdr = talker_alias_on_air(sdr.get("bursts", []))
    calls = [r for r in heard if "call" in r]
    on_bench = calls[0].get("talker_alias") if calls else None
    print(f"alias {alias!r}: SDR {on_sdr!r}, bench modem {on_bench!r}, call.tx_alias_lcs +{lcs}")
    assert lcs >= 1, f"call.tx_alias_lcs +{lcs}"
    assert on_sdr is not None or on_bench is not None, "neither receiver assembled the talker alias"
    for name, got in (("SDR", on_sdr), ("bench modem", on_bench)):
        assert got is None or got == alias, f"{name} heard alias {got!r}, the radio sends {alias!r}"


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_dmr_privacy_tx(radio, sdr_bench, pytestconfig):
    if "DMR_PRIVACY" not in radio.features():
        pytest.skip("built without CONFIG_DMR_PRIVACY")
    freq, cc, _, d = _dmr_tx_setup(radio, pytestconfig)
    if d.get("dmr.privacy") == "1":
        pytest.skip("a privacy key is set already: not overwriting it")
    own = int(d["dmr.own_id"])
    clear_sdr, clear_heard = _over(radio, sdr_bench, freq, cc, 3.0, "hil_dmr_clear")
    try:
        radio.link.dmr_privacy("moto", 1)
        radio.wait_for(lambda x: x.get("dmr.privacy") == "1", sections=("dmr",), what="privacy key loaded")
        sdr, heard = _over(radio, sdr_bench, freq, cc, 3.0, "hil_dmr_privacy")
    finally:
        radio.link.dmr_privacy("off")
        radio.wait_for(lambda x: x.get("dmr.privacy") == "0", sections=("dmr",), what="privacy off")
    # Voice LC headers and terminators from us (the SDR's and the bench modem's), embedded call LCs (the SDR's)
    lcs = [b["lc"] for b in sdr.get("bursts", []) if (b.get("lc") or {}).get("so") is not None
           and b["lc"].get("src") == own]
    lcs += [{"so": r["so"], "src": r.get("src")} for r in heard if r.get("so") is not None and r.get("src") == own]
    emb = [b["lc"] for b in sdr.get("bursts", []) if (b.get("lc") or {}).get("emb_flco") in (0, 3)
           and b["lc"].get("src") == own]
    emb += [r["emb_lc"] for r in heard if isinstance(r.get("emb_lc"), dict)    # a burst's (a call's is a count)
            and r["emb_lc"].get("flco") in (0, 3) and r["emb_lc"].get("src") == own]
    clear_lcs = [b["lc"] for b in clear_sdr.get("bursts", []) if (b.get("lc") or {}).get("so") is not None
                 and b["lc"].get("src") == own]
    print(f"privacy over: {len(lcs)} header / terminator LCs, service options {sorted({x['so'] for x in lcs})}; "
          f"{len(emb)} embedded LCs, FIDs {sorted({x.get('fid') for x in emb})}; clear over: service options "
          f"{sorted({x['so'] for x in clear_lcs})}")
    for name, recs in (("clear", clear_heard), ("privacy", heard)):
        for k in [r for r in recs if "call" in r]:
            print(f"{name} over, bench modem: {k.get('ambe_frames')} AMBE frames, "
                  f"{k.get('ambe_silence_frames')} silence, crc32 {k.get('ambe_crc32')}")
    assert lcs, "no voice LC header or terminator from us decoded by the SDR or the bench modem"
    assert all(x["so"] & 0x40 for x in lcs), f"an LC without the privacy service option: {lcs}"
    assert not any(x["so"] & 0x40 for x in clear_lcs), f"the clear over carries the privacy option: {clear_lcs}"
    assert emb, "no embedded call LC from us decoded"
    assert all(x.get("fid") == 0x10 for x in emb), f"an embedded LC without FID 0x10: {emb}"
