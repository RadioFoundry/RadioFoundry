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
  the bursts, default 500, corrected for the SDR's own error by the SDR bench's calibration); burst on-time 27-31.5 ms; ramps < 1.5 ms; no
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

test_dmr_tx_encode_options: per vocoder encode option set (0x3 = the default flags word 0x1840, 0x0 = 0x800, 0x2 =
0x1800 with Mic noise reduction off, 0x7 = 0x3840 with Mic AGC on; docs/spec/dmr-voice.md 6), set over the debug link
(SETTING 31, RAM only): voice.enc_options equals the set, a 3 s over passes the checks above, voice.enc_flags after the
over is the word (where the board reports it), and the options in force before are back afterwards. The AMBE
parameter statistics of what the bench modem heard (a quiet room, nobody talking) are printed, not judged.

test_dmr_tx_split_repeater (also --allow-codeplug-write and --split-tx-mhz): the test channel's TX frequency is set to
--split-tx-mhz over CPS (backed up first, restored and verified afterwards), so the channel is a repeater pair: RX
--dmr-freq, TX --split-tx-mhz. The bench modem is a duplex repeater on that pair (bench_repeater.py, input
--split-tx-mhz, output --dmr-freq), down at the key-up. A 4 s over: the radio sends its wake-up (BS_Dwn_Act, seen by
the SDR on --split-tx-mhz) and listens; the repeater keys its outbound 0.6 s after the key-up, inside the first listen
window; the radio must hear it on --dmr-freq and go on to voice: tx_voice reached, never tx_error no_repeater,
call.tx_calls + 1, call.tx_denied unchanged, and the SDR or the repeater's input hears a voice LC header from the
radio's ID and its voice bursts, on the test channel's TS (90% of them, by the repeater's input and by the outbound
capture's slot grid, the latter only when 90% of its labeled bursts fit that grid). A radio whose one synthesizer stays on the TX frequency after the wake-up is deaf to
the answer and gives up with no_repeater (issue 72). The repeater answers on a timer rather than on the BS_Dwn_Act
because its idle receiver didn't hear a radio on this bench (2026-10-18); keyed, it does. Whether the radio still hears
the outbound while it talks (dmr.repeater_up during tx_voice) is printed, not judged: the HR_C7000 radios don't
receive between their own bursts outside SFR (tx_listen).
"""
import collections
import time
import warnings

import pytest

from odr_hil import skip

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


@pytest.mark.feature("dmr_voice")
@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_dmr_tx_off_air(radio, sdr_bench, pytestconfig):
    if "DMR" not in radio.features():
        skip("not_built", "built without DMR")
    import dmr_bench
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    d = radio.view(["dmr"])
    own, target, private = int(d["dmr.own_id"]), int(d["dmr.target_id"]), d.get("dmr.target_private") == "1"
    ch_cc = int(radio.view(["channel"])["ch.color_code"])
    if own == 0:
        skip("state", "the radio has no DMR ID (dmr.own_id=0)")
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
    _report(_tx_checks(sdr, heard, own, target, private, cc, before, after, d.get("dmr.tx_alias", ""), pytestconfig),
            sdr_bench, rdir)


def _tx_checks(sdr, heard, own, target, private, cc, before, after, alias, pytestconfig):
    """dmr_bench.py case T1's checks on one over from us (the module docstring): the SDR analysis, the bench modem's
    calls, the radio's counters before and after the over. Returns the dmr_bench.Checks."""
    import dmr_bench
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
    fo = sorted(b["foff_hz"] for b in sdr.get("bursts", []))     # corrected by the ppm the capture recorded
    if fo:
        med = fo[len(fo) // 2]
        c.add(f"|carrier offset| < {max_foff} Hz", abs(med) < max_foff, f"median {med:.0f} Hz")
    tx_calls = after.get("call.tx_calls", 0) - before.get("call.tx_calls", 0)
    c.add("radio: one TX call", tx_calls == 1, f"{tx_calls}")
    under = after.get("call.tx_underruns", 0) - before.get("call.tx_underruns", 0)
    c.add("radio: no TX underruns", under == 0, f"{under}")
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
    return c


def _report(c, sdr_bench, rdir):
    for i in c.items:
        print(f"{ {True: 'ok  ', False: 'FAIL', None: 'man '}[i['ok']]} {i['check']}  {i['detail']}")
    print(f"captures on the bench: ~/{sdr_bench.dir}/{rdir}/")
    failed = [i for i in c.items if i["ok"] is False]
    assert not failed, "; ".join(f"{i['check']} ({i['detail']})" for i in failed)


@pytest.mark.feature("dmr_gps")
@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_dmr_gps_tx(radio, sdr_bench, pytestconfig):
    """Our ETSI GPS Info (FLCO 8) in the alias superframes: a fixed APRS position and "position in DMR calls" set over
    the debug link, a 4 s over, the bench modem's listener decodes the GPS Info LC (bench_tx.py listen: the call's
    "gps"). The settings are put back afterwards."""
    feats = radio.features()
    if "DMR_GPS" not in feats or "APRS" not in feats:
        skip("not_built", "needs CONFIG_DMR_GPS and CONFIG_APRS (the position source)")
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    if int(radio.view(["dmr"])["dmr.own_id"]) == 0:
        skip("state", "the radio has no DMR ID (dmr.own_id=0)")
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
        skip("not_built", "built without DMR")
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    radio.go_home()
    pos = radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    d = radio.view(["dmr"])
    if int(d["dmr.own_id"]) == 0:
        skip("state", "the radio has no DMR ID (dmr.own_id=0)")
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


@pytest.mark.feature("dmr_talker_alias")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.codeplug_write
def test_dmr_tx_talker_alias(radio, sdr_bench, pytestconfig, backup_dir):
    if "DMR_TALKER_ALIAS" not in radio.features():
        skip("not_built", "built without CONFIG_DMR_TALKER_ALIAS")
    import cps_client
    from test_cps import codeplug_edited
    freq, cc, _, d = _dmr_tx_setup(radio, pytestconfig)
    if d.get("dmr.ts") != "1":
        skip("state", "the test channel is on TS2: the edit sets the TS1 flag")
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


BEACON_PHASE_S = 6.0     # the bench repeater's beacon (its answer) this long after it started
WAKE_ANSWER_S = 0.6      # ... which is this long after the radio's key-up: inside its first 600 ms listen window


def _wait_line(p, text, timeout):
    """When (time.monotonic()) a background bench job on a shared bench (SdrBench._daemon_bg) printed a line containing
    text; waits for it."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for line, at in zip(list(getattr(p, "lines", [])), list(getattr(p, "stamps", []))):
            if text in line:
                return at
        time.sleep(0.05)
    raise AssertionError(f"the bench job never printed {text!r}: {''.join(getattr(p, 'lines', []))[-600:]}")


def _repeated_slots(outbound):
    """Which outbound slot the repeater repeated voice on, from the outbound capture alone: idle and data bursts carry
    their slot in the CACH (TC), and a repeated voice burst falls in the same 60 ms frame position as the slot's other
    bursts. The repeater puts an inbound TSx call on outbound TSx, so this is the slot the radio sent on. Returns
    ({1: n, 2: n} voice bursts, the share of labeled bursts within 5 ms of the slot grid), or None without at least 4
    labeled bursts."""
    import math
    starts = [b["t_ms"] / 1e3 - (0.030 if b.get("cach_ts") == 2 else 0.0) for b in outbound
              if b.get("family") == "bs" and not b.get("voice") and b.get("cach_ts") in (1, 2)]
    if len(starts) < 4:
        return None
    ang = [2 * math.pi * (t % 0.060) / 0.060 for t in starts]
    epoch = (math.atan2(sum(map(math.sin, ang)), sum(map(math.cos, ang))) % (2 * math.pi)) / (2 * math.pi) * 0.060
    agree = sum(abs(((t - epoch + 0.030) % 0.060) - 0.030) < 0.005 for t in starts) / len(starts)
    pos = [((b["t_ms"] / 1e3 - epoch) % 0.060) * 1e3 for b in outbound if b.get("voice")]
    return dict(collections.Counter(2 if 15.0 <= p < 45.0 else 1 for p in pos)), round(agree, 3)


@pytest.mark.feature("split_duplex", optional=True)
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.codeplug_write
def test_dmr_tx_split_repeater(radio, sdr_bench, pytestconfig, backup_dir):
    split = pytestconfig.getoption("--split-tx-mhz")
    if not split:
        skip("opt_in", "give the split channel's TX frequency: --split-tx-mhz MHz (one you may transmit on)")
    import cps_client
    from test_cps import codeplug_edited
    freq, cc, _, d = _dmr_tx_setup(radio, pytestconfig)
    if abs(split - freq) < 0.0125:
        skip("opt_in", "--split-tx-mhz must differ from --dmr-freq")
    if radio.view().get("set.tx_inhibit") == "1":
        skip("state", "the radio's TX inhibit is on (set.tx_inhibit=1)")
    own, ts = int(d["dmr.own_id"]), int(d["dmr.ts"])
    number = int(radio.core()["channel_number"])
    tx_hz = round(split * 1e6)
    samples = []        # (s since key-up, dmr.phase, dmr.repeater_up, dmr.tx_error) while PTT is held
    with codeplug_edited(radio, backup_dir, lambda img: cps_client.set_channel_tx(img, number, tx_hz), "split"):
        v = radio.wait_for(lambda x: x.get("tx_hz") == str(tx_hz), timeout=5, what="the channel's TX frequency")
        assert v.get("rx_hz") == str(round(freq * 1e6)), v.get("rx_hz")
        def one_call():
            nonlocal samples
            samples = []
            _, before = radio.counters()
            rdir = sdr_bench.run_dir("hil_dmr_split")
            with sdr_bench.lease():
                # The repeater answers the radio's wake-up: it keys its outbound (one long beacon) WAKE_ANSWER_S after
                # the key-up, inside the radio's first listen window (see the docstring for why not on the BS_Dwn_Act)
                # The captures first: starting them takes 6-8 s on the shared bench, longer than the beacon's delay, so
                # started after the repeater the beacon was already on at the check below (SDR bench, 2026-10-18)
                cap = sdr_bench.capture_start(rdir, split, seconds=22)     # the radio's side: what it sent, where
                cap_out = sdr_bench.capture_start(rdir, freq, seconds=22, name="cap1")   # the repeater's: slot timing
                rpt = sdr_bench.repeater_start(split, freq, cc, seconds=30,
                                               extra=f"--beacon 60 --beacon-len 16 --beacon-phase {BEACON_PHASE_S}")
                at = _wait_line(rpt, "beacon every", 20)       # the beacon timer starts as this is logged
                late = time.monotonic() - at
                assert late < BEACON_PHASE_S - WAKE_ANSWER_S - 1.0, f"the repeater's start line came {late:.1f} s late"
                assert radio.view(["dmr"]).get("dmr.repeater_up") == "0", \
                    f"the repeater is up before its beacon ({late:.1f} s after its start line)"
                time.sleep(max(0.0, at + BEACON_PHASE_S - WAKE_ANSWER_S - time.monotonic()))
                ints0 = [x for x in radio.link.diag()[1] if x.startswith("ts ")]
                radio.link.ptt(True, 6000)
                t0 = time.monotonic()
                try:
                    while time.monotonic() - t0 < 5.0:
                        x = radio.view(["dmr"])
                        samples.append((round(time.monotonic() - t0, 2), x.get("dmr.phase"), x.get("dmr.repeater_up"),
                                        x.get("dmr.tx_error")))
                        time.sleep(0.15)
                finally:
                    radio.link.ptt(False)
                radio.wait_for(lambda x: x.get("state") != "tx", timeout=5, what="TX ended")
                sdr_bench.capture_wait()
                events = sdr_bench.repeater_result(rpt, timeout=60)
                sdr = sdr_bench.analyze_dmr(cap, split, cc)
                out = sdr_bench.analyze_dmr(cap_out, freq, cc)
                _, after = radio.counters()
                # HR_C7000: the slot choice, and the timeslot interrupt counts after and before
                txb = [x for x in radio.link.diag()[1] if x.startswith(("txb ", "txh ", "ts "))] + ints0
                bursts = sdr.get("bursts", [])
                kinds = collections.Counter(f"{b.get('family')}:{b.get('kind')}" for b in bursts)
                headers = [b for b in bursts if b.get("kind") == "VLC_HDR" and (b.get("lc") or {}).get("src") == own]
                voice = [b for b in bursts if str(b.get("kind", "")).startswith("VOICE_")]
                rx = [e["rx"] for e in events if "rx" in e]
                # The repeater's input hears the radio once keyed (only its idle wake-up receiver doesn't, see above)
                rpt_headers = [r for r in rx if r.get("dt") == 1 and r.get("src") == own]
                rpt_voice = [r for r in rx if "voice" in r and r.get("slot") == ts]
                tx_calls = after.get("call.tx_calls", 0) - before.get("call.tx_calls", 0)
                denied = after.get("call.tx_denied", 0) - before.get("call.tx_denied", 0)
                talking = [x for x in samples if x[1] == "tx_voice"]
                wakes = [b for b in bursts if b.get("kind") == "CSBK"]
                slots = _repeated_slots(out.get("bursts", []))
                heard = [x[2] for x in talking]
                print(f"while talking dmr.repeater_up was 1 in {heard.count('1')} of {len(heard)} samples (not judged)")
                print(f"radio while keyed: {samples}")
                if txb:
                    print(f"radio TX slot choice: {txb}")
                if slots:
                    print(f"voice the repeater repeated, by outbound slot: {slots[0]} ({slots[1]:.1%} of the labeled "
                          f"bursts on the slot grid)")
                else:
                    print(f"no slot labels in the outbound capture on {freq} MHz ({len(out.get('bursts', []))} bursts)")
                print(f"radio: call.tx_calls +{tx_calls}, call.tx_denied +{denied}; SDR on {split} MHz (the radio's TX): "
                      f"{dict(kinds)}; captures: ~/{sdr_bench.dir}/{rdir}/")
                print(f"bench repeater: keyed {[e['key'] for e in events if 'key' in e]}, heard {len(rx)} bursts on its "
                      f"input: {len(rpt_headers)} voice LC headers from us, {len(rpt_voice)} voice bursts on TS{ts}; "
                      f"per slot {next((e['summary'] for e in events if 'summary' in e), None)}")
            assert wakes, f"no BS_Dwn_Act CSBK from the radio on {split} MHz (the wake-up path wasn't taken)"
            assert talking, f"the radio never reached tx_voice (tx_error {sorted({x[3] for x in samples})}: deaf to the " \
                            f"repeater's answer?)"
            assert not any(x[3] == "no_repeater" for x in samples), "tx_error no_repeater: the radio didn't hear the repeater"
            assert denied == 0 and tx_calls == 1, f"call.tx_calls +{tx_calls}, call.tx_denied +{denied}"
            # Either receiver will do: the SDR's capture can clip with the repeater keyed next to it
            assert headers or rpt_headers, f"neither the SDR nor the repeater heard a voice LC header from us on {split} MHz"
            n = max(len(voice), len(rpt_voice))
            assert n >= 30, f"only {n} voice bursts from us on {split} MHz (4 s is about 66)"
            # The slot: the repeater's input labels what it heard, its outbound shows what it repeated (either can miss it)
            by_rpt = collections.Counter(r.get("slot") for r in rx if "voice" in r)
            # The outbound counts only with a clean slot grid fit: at 21% of the labeled bursts on it, the capture put
            # a TS1 call on TS2 while the repeater's input had all 78 bursts on TS1 (DM-1801 Pro, SDR bench, 2026-10-18)
            by_out = slots[0] if slots and sum(slots[0].values()) >= 10 and slots[1] >= 0.9 else {}
            # Voice off the repeater's slot grid is neither labeled nor repeated: one of them must have the call
            assert by_out or sum(by_rpt.values()) >= 10, \
                f"the repeater neither heard nor repeated our voice (input {dict(by_rpt)}, outbound {slots[0] if slots else {}})"
            for name, got in (("the repeater's input", by_rpt), ("the repeated outbound", by_out)):
                if sum(got.values()) >= 10:
                    assert got.get(ts, 0) >= 0.9 * sum(got.values()), \
                        f"the radio's voice is not on TS{ts}: {name} has {dict(got)}"

        calls = pytestconfig.getoption("--split-calls")
        failed = []
        for k in range(calls):
            try:
                one_call()
                print(f"split call {k + 1} of {calls}: ok")
            except AssertionError as e:
                failed.append(str(e).split(chr(10))[0][:200])
                print(f"split call {k + 1} of {calls}: {failed[-1]}")
            if k + 1 < calls:
                time.sleep(8)      # the radio is back to idle and the bench jobs are over
        assert not failed, f"{len(failed)} of {calls} calls failed: {failed}"

@pytest.mark.feature("dmr_privacy")
@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_dmr_privacy_tx(radio, sdr_bench, pytestconfig):
    if "DMR_PRIVACY" not in radio.features():
        skip("not_built", "built without CONFIG_DMR_PRIVACY")
    freq, cc, _, d = _dmr_tx_setup(radio, pytestconfig)
    if d.get("dmr.privacy") == "1":
        skip("state", "a privacy key is set already: not overwriting it")
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


# The vocoder's encode options (VOCODER_OPT_*: 0x1 noise reduction, 0x2 tone frames, 0x4 AGC) and the flags word the
# AMBE+2 hosts pass the codec for each (docs/spec/codec-md380.md "Encode flags")
ENC_CASES = ((0x3, 0x1840),     # the default: Mic noise reduction on
             (0x0, 0x800),      # no options: what the hosts sent before encode options existed
             (0x2, 0x1800),     # Mic noise reduction off
             (0x7, 0x3840))     # Mic AGC on


def ambe_param_stats(amb):
    """What a DSD .amb file (bench_tx.py listen --ambe-out: ".amb", then per frame one error byte and the 49 parameter
    bits in 7 bytes) says about the frames: count, distinct parameter sets, repeats of the previous frame, and the
    mean number of parameter bits that change from one frame to the next. Reported only: the frames depend on the
    room."""
    frames = [amb[i + 1:i + 8] for i in range(4, len(amb) - 7, 8)]
    if not frames:
        return {"frames": 0}
    flips = [bin(int.from_bytes(a, "big") ^ int.from_bytes(b, "big")).count("1") for a, b in zip(frames, frames[1:])]
    return {"frames": len(frames), "distinct": len(set(frames)), "repeats": flips.count(0),
            "bits_changed": round(sum(flips) / max(1, len(flips)), 1)}


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.parametrize("opts,word", ENC_CASES, ids=[f"{w:#x}" for _, w in ENC_CASES])
def test_dmr_tx_encode_options(radio, sdr_bench, pytestconfig, opts, word):
    """One 3 s over per encode option set, given over the debug link (SETTING 31, RAM only; dmr-voice.md 6): the
    options in force (voice.enc_options) are the ones set, the over passes test_dmr_tx_off_air's checks (valid call,
    header and terminator, whole superframes, AMBE FEC corrections), and on a board that reports its codec word,
    voice.enc_flags after the over is the word for those options. The options in force before (the Mic noise
    reduction and Mic AGC settings') are put back afterwards. Prints ambe_param_stats() of what the bench modem heard
    (the room's noise, nobody talking) to compare the settings by hand; nothing is asserted on it."""
    freq, cc, _, d = _dmr_tx_setup(radio, pytestconfig)
    if d.get("dmr.codec") != "1":
        pytest.skip("no vocoder merged in this firmware (dmr.codec=0)")
    v = radio.view(["dmr_stats"])
    if "voice.enc_supported" not in v:
        pytest.skip("firmware without the voice.enc_* view keys")
    if int(v["voice.enc_supported"], 0) & opts != opts:
        pytest.skip(f"this board's vocoder doesn't take options {opts:#x} (voice.enc_supported "
                    f"{v['voice.enc_supported']})")
    prev = int(v["voice.enc_options"], 0)
    own, target, private = int(d["dmr.own_id"]), int(d["dmr.target_id"]), d.get("dmr.target_private") == "1"
    try:
        radio.link.setting(30, opts)
    except DebugLinkError as e:
        pytest.skip(f"firmware without the enc_options setting (SETTING 31): {e}")
    try:
        got = int(radio.view(["dmr_stats"])["voice.enc_options"], 0)
        assert got == opts, f"options in force {got:#x} after SETTING 31 {opts:#x}"
        radio.reset_counters()
        _, before = radio.counters()
        rdir = sdr_bench.run_dir(f"hil_dmr_enc_{word:x}")
        with sdr_bench.lease():
            cap = sdr_bench.capture_start(rdir, freq, seconds=PTT_S + 8)
            lis = sdr_bench.listen_start(freq, cc, seconds=PTT_S + 6, ambe_out=f"{rdir}/heard")
            radio.ptt_for(PTT_S)
            radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
            heard = sdr_bench.listen_result(lis)
            sdr_bench.capture_wait()
        sdr = sdr_bench.analyze_dmr(cap, freq, cc)
        _, after = radio.counters()
        flags = radio.view(["dmr_stats"]).get("voice.enc_flags")
    finally:
        radio.link.setting(30, prev)
    assert int(radio.view(["dmr_stats"])["voice.enc_options"], 0) == prev, "the options in force weren't put back"
    c = _tx_checks(sdr, heard, own, target, private, cc, before, after, d.get("dmr.tx_alias", ""), pytestconfig)
    if flags is not None:
        c.add(f"codec flags word {word:#x}", int(flags, 0) == word, flags)
    for k in [r for r in heard if "call" in r][:1]:
        amb = next((f for f in k.get("files", []) if f.endswith(".amb")), None)
        stats = {}
        if amb:
            hexdump = sdr_bench.ssh(f"cd ~/{sdr_bench.dir} && {sdr_bench.py} -c \"import sys; "
                                    f"print(open(sys.argv[1], 'rb').read().hex())\" {amb}", check=False)
            stats = ambe_param_stats(bytes.fromhex(hexdump.strip() or ""))
        print(f"encode options {opts:#x} (flags {flags}): {k.get('ambe_frames')} frames heard, "
              f"{k.get('ambe_silence_frames')} silence, parameters {stats}")
    _report(c, sdr_bench, rdir)
