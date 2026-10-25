# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Two radios as one split duplex repeater (docs/spec/split-duplex.md 10.2): the --radio-port radio is the TX half,
the --split-rx-port radio the RX half, each a CONFIG_HOTSPOT_SPLIT=y build. split_duplex_host.Pair runs the host
side (time sync, epoch relay, AT relay) and repeats every burst heard into the TX half, as a duplex host does.

- test_split_pair_repeat (radio_tx, bench_service: --allow-rf, --split-rx-port, --split-out-mhz): the TX half keyed on
  --split-out-mhz at its lowest power; the bench sends one group call (three superframes) on --dmr-freq, the repeater
  input. The SDR bench captures both frequencies. Asserted: the RX half sends the call up whole under one label
  (header, voice A-F three times in order, terminator); every burst went into the TX half; the outbound carries the
  call on one slot, whole and in order, with the bench call's LC in its headers and terminator, BS sync, the CACH
  right (TC alternating) and no underrun in STATUS. The repeat delay (the first header on the input to the first on
  the output) and the RX half's residual are printed.

- test_split_pair_handheld (radio_tx, codeplug_write: also --split-ms-port): a RadioFoundry radio as the subscriber, the one
  thing the bench can't be. Its current DMR channel is edited for the run (codeplug_edited puts it back) to receive
  the outbound and transmit on the input, on TS1 and then TS2 (--split-ms-ts). It hears the outbound, takes its slot
  from the CACH and keys 4 s. Asserted per slot: the RX half sends the call up under that slot's label only, every
  burst is repeated, and the outbound carries the call on the same slot (the CACH TC of its bursts). The RX half's
  residual, the fixed delay from the TX half's slot boundary to the RX half's (split-duplex.md 11 item 4), is printed
  and must stay under 5 ms.

The bench is a simplex station: its slots have no fixed phase against the outbound, so in test_split_pair_repeat the
label (and the outbound slot the call lands on) is whichever half of the outbound frame the call falls in, and the
residual is that phase. Only a subscriber synced to the outbound, as in test_split_pair_handheld, checks the labels.
"""
import json
import os
import sys
import threading
import time

import pytest

from rfoundry_hil import skip

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

SRC = 9990301


def analyze(sdr_bench, cap, freq, cc):
    """sdrmon.py analyze into its own file (FILE.json is the capture's metadata) and the capture's start time"""
    out = f"{cap}.an.json"
    txt = sdr_bench.ssh(f"cd ~/{sdr_bench.dir} && {sdr_bench.py} sdrmon.py analyze {cap} --freq {freq} --cc {cc} "
                        f"--cach --json {out} > {cap}.txt 2>&1; cat {out}", timeout=600)
    meta = sdr_bench.capture_meta(cap)
    return json.loads(txt), meta.get("t0")


def call_bursts(bursts):
    return [b for b in bursts if b["family"] in ("bs", "ms", "emb") and b["kind"] != "IDLE"]


@pytest.mark.feature("split_duplex", owner="connect a second radio as the RX half (--split-rx-port)")
@pytest.mark.second_radio
@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_split_pair_repeat(radio, sdr_bench, pytestconfig):
    rx_port = pytestconfig.getoption("--split-rx-port")
    fout = pytestconfig.getoption("--split-out-mhz")
    if not rx_port or not fout:
        skip("opt_in", "needs a second radio as the RX half (--split-rx-port) and the repeater output "
                       "(--split-out-mhz)")
    if "HOTSPOT_SPLIT" not in radio.features():
        skip("not_built", "built without the split duplex extension (make CONFIG_HOTSPOT_SPLIT=y)")
    import split_duplex_host as sdh
    fin, cc, tg = (pytestconfig.getoption(o) for o in ("--dmr-freq", "--dmr-cc", "--dmr-tg"))
    rdir = sdr_bench.run_dir("hil_split_pair")
    log = []
    with sdr_bench.lease():
        cap_in = sdr_bench.capture_start(rdir, fin, seconds=30, name="in")
        cap_out = sdr_bench.capture_start(rdir, fout, seconds=30, name="out")
        with radio.released() as port:
            p = sdh.Pair(port, rx_port, repeat=True, log=log.append)
            try:
                for r, role in ((p.tx, sdh.ROLE_TX), (p.rx, sdh.ROLE_RX)):
                    _, info = sdh.require_split(r)
                    if not info["offered"] & (1 << (role - 1)):
                        skip("no_hw", f"{r.name}: the radio doesn't offer split duplex role {role}")
                p.start(round(fin * 1e6), round(fout * 1e6), 0, cc, power_cap=1)
                p.run_background()
                time.sleep(2.0)
                p.keyed = True
                assert p.tx.command(sdh.DMR_START, b"\x01") == 0
                time.sleep(4.0)         # epochs relayed every 2 s; the outbound idle with its CACH
                with p.rx.lock:
                    p.uplink.clear()
                sdr_bench.voice_call(fin, cc, tg, superframes=3, src=SRC)
                time.sleep(2.0)
                st_tx, st_rx = sdh.get_status(p.tx), sdh.get_status(p.rx)
            finally:
                p.close()
        sdr_bench.capture_wait()
    print("\n".join(log))
    print("TX half STATUS:", st_tx)
    print("RX half STATUS:", st_rx)

    # RX half: the call went up whole under one label
    ups = [f for _, f in p.uplink if f[2] in (sdh.DMR_DATA1, sdh.DMR_DATA2)]
    labels = {f[2] for f in ups}
    ctl = [f[3] for f in ups]
    hdr, term = ctl.count(0x41), ctl.count(0x42)
    voice = [c for c in ctl if c & 0x40 == 0]
    print(f"uplink: {len(ups)} bursts, labels {sorted(labels)}, headers {hdr}, voice {len(voice)}, terminators {term}, "
          f"repeated {p.repeated}, residual {st_rx['residual_us']} us")
    assert len(labels) == 1, f"the call went up under both labels: {sorted(labels)}"
    assert hdr >= 1 and term >= 1 and len(voice) == 18, ctl
    assert sum(p.repeated.values()) == len(ups)
    ts = 1 if labels == {sdh.DMR_DATA1} else 2

    # Input and output off air
    sdr_in, t0_in = analyze(sdr_bench, cap_in, fin, cc)
    sdr_out, t0_out = analyze(sdr_bench, cap_out, fout, cc)
    inb = call_bursts(sdr_in["bursts"])
    bs = [b for b in sdr_out["bursts"] if b["family"] in ("bs", "emb")]
    ours = [b for b in bs if b["kind"] != "IDLE"]
    kinds = [b["kind"] for b in ours]
    print("input:", [b["kind"] for b in inb][:6], "...", len(inb), "bursts")
    print("output:", kinds)
    assert not [b for b in sdr_out["bursts"] if b["family"] == "ms"], "MS sync on the outbound"
    assert len({b["slot"] for b in ours}) == 1, "the repeat must stay on one outbound slot"
    v = [k[-1] for k in kinds if k.startswith("VOICE_")]
    assert "".join(v) == "ABCDEF" * 3, v
    h = [b for b in ours if b["kind"] == "VLC_HDR"]
    t = [b for b in ours if b["kind"] == "TERM_LC"]
    assert h and all(b["lc"] and b["lc"]["src"] == SRC and b["lc"]["dst"] == tg for b in h), h
    assert t and t[0]["lc"]["src"] == SRC, t
    # A bit or two of the 48-bit sync may go when the TX half clips the SDR's ADC (a radio on an antenna next to it);
    # the LCs and the voice order above are the real check
    assert all(b["sync_errors"] <= 2 for b in ours), [(b["kind"], b["sync_errors"]) for b in ours if b["sync_errors"]]
    cach = [b["cach"] for b in sdr_out["bursts"] if b.get("cach")]
    ok = [c for c in cach if c.get("ok")]
    assert len(ok) >= 0.98 * len(cach), f"CACH ok {len(ok)}/{len(cach)}"
    assert st_tx[f"underruns{ts}"] == 0, st_tx

    hin = [b for b in inb if b["kind"] == "VLC_HDR"]
    if hin and h and t0_in and t0_out:
        delay = (t0_out + h[0]["t_ms"] / 1e3) - (t0_in + hin[0]["t_ms"] / 1e3)
        print(f"repeat delay: {delay * 1e3:.0f} ms (first header in to first header out)")
        assert 0 < delay < 1.0, delay


def ts_of_outbound(bursts):
    """The outbound slot (1 or 2) of each 30 ms grid position ("A" / "B"), from the CACH TC of the BS bursts"""
    votes = {}
    for b in bursts:
        if b.get("cach_ts") in (1, 2):
            votes.setdefault(b["slot"], []).append(b["cach_ts"])
    return {k: max(set(v), key=v.count) for k, v in votes.items()}


def set_ms_channel(number, rx_hz, tx_hz, ts, cc):
    """A codeplug edit: the channel receives the repeater output, transmits on its input, on slot ts"""
    import cps_client

    def edit(img):
        _, a = cps_client.channel_addr(number)
        img[a + 0x10:a + 0x14] = cps_client._bcd_freq(rx_hz)
        img[a + 0x14:a + 0x18] = cps_client._bcd_freq(tx_hz)
        img[a + 0x2C] = cc
        img[a + 0x31] = (img[a + 0x31] & ~0x40) | (0x40 if ts == 2 else 0)
    return edit


@pytest.mark.feature("split_duplex", optional=True, owner="connect a third radio as the subscriber (--split-ms-port)")
@pytest.mark.second_radio
@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.codeplug_write
def test_split_pair_handheld(radio, sdr_bench, pytestconfig, backup_dir):
    rx_port, ms_port = pytestconfig.getoption("--split-rx-port"), pytestconfig.getoption("--split-ms-port")
    fout = pytestconfig.getoption("--split-out-mhz")
    if not (rx_port and ms_port and fout):
        skip("opt_in", "needs the RX half (--split-rx-port), a subscriber radio (--split-ms-port) and the repeater "
                       "output (--split-out-mhz)")
    if "HOTSPOT_SPLIT" not in radio.features():
        skip("not_built", "built without the split duplex extension (make CONFIG_HOTSPOT_SPLIT=y)")
    import split_duplex_host as sdh
    from rfoundry_hil import Radio
    from test_cps import codeplug_edited
    fin, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    slots = [int(x) for x in pytestconfig.getoption("--split-ms-ts").split(",")]
    ms = Radio(ms_port)
    try:
        if ms.view().get("set.tx_inhibit") == "1":
            skip("state", "the subscriber's TX inhibit is on (set.tx_inhibit=1)")
        own = int(ms.view(["dmr"])["dmr.own_id"])
        ms.go_home()
        ms.find_channel(round(fin * 1e6), "dmr")
        number = int(ms.core()["channel_number"])
        failed = []
        for ts in slots:
            edit = set_ms_channel(number, round(fout * 1e6), round(fin * 1e6), ts, cc)
            with codeplug_edited(ms, backup_dir, edit, f"split_ms_ts{ts}"):
                ms.wait_for(lambda x: x.get("rx_hz") == str(round(fout * 1e6)), timeout=5, what="the edited channel")
                try:
                    handheld_call(radio, ms, sdr_bench, rx_port, fin, fout, cc, ts, own)
                    print(f"TS{ts}: ok")
                except AssertionError as e:
                    failed.append(f"TS{ts}: {str(e).splitlines()[0][:300]}")
                    print(failed[-1])
        assert not failed, failed
    finally:
        ms.close()


def handheld_call(radio, ms, sdr_bench, rx_port, fin, fout, cc, ts, own):
    import split_duplex_host as sdh
    rdir = sdr_bench.run_dir(f"hil_split_ms_ts{ts}")
    log = []
    heard = []
    errs = set()
    with sdr_bench.lease():
        cap_in = sdr_bench.capture_start(rdir, fin, seconds=24, name="in")
        cap_out = sdr_bench.capture_start(rdir, fout, seconds=24, name="out")
        with radio.released() as port:
            p = sdh.Pair(port, rx_port, repeat=True, log=log.append)
            try:
                p.start(round(fin * 1e6), round(fout * 1e6), 0, cc, power_cap=1)
                p.run_background()
                p.keyed = True
                assert p.tx.command(sdh.DMR_START, b"\x01") == 0
                # The subscriber must hear the outbound before it keys (else it sends a wake-up and waits)
                v = ms.wait_for(lambda x: x.get("dmr.repeater_up") == "1", timeout=10, sections=("dmr",),
                                what="the subscriber hearing the outbound")
                time.sleep(3.0)         # epochs relayed every 2 s
                p.uplink.clear()
                _, before = ms.counters()
                ms.link.ptt(True, 6000)
                t0 = time.monotonic()
                try:
                    while time.monotonic() - t0 < 4.0:
                        x = ms.view(["dmr"])
                        heard.append(x.get("dmr.phase"))
                        errs.add(x.get("dmr.tx_error"))
                        time.sleep(0.2)
                finally:
                    ms.link.ptt(False)
                ms.wait_for(lambda x: x.get("state") != "tx", timeout=5, what="TX ended")
                time.sleep(1.5)
                st_tx, st_rx = sdh.get_status(p.tx), sdh.get_status(p.rx)
                _, after = ms.counters()
            finally:
                p.close()
        sdr_bench.capture_wait()
    print(f"--- TS{ts}")
    print("\n".join(log[-3:]))
    print("TX half STATUS:", st_tx)
    print("RX half STATUS:", st_rx)
    print("subscriber phases while keyed:", heard, "tx_error", sorted(e for e in errs if e))
    tx_calls = after.get("call.tx_calls", 0) - before.get("call.tx_calls", 0)
    denied = after.get("call.tx_denied", 0) - before.get("call.tx_denied", 0)

    ups = [f for _, f in p.uplink if f[2] in (sdh.DMR_DATA1, sdh.DMR_DATA2)]
    by_label = {1: 0, 2: 0}
    for f in ups:
        by_label[1 if f[2] == sdh.DMR_DATA1 else 2] += 1
    ctl = [f[3] for f in ups]
    voice = [c for c in ctl if c & 0x40 == 0]
    print(f"uplink: {len(ups)} bursts, by label {by_label}, headers {ctl.count(0x41)}, voice {len(voice)}, "
          f"terminators {ctl.count(0x42)}, repeated {p.repeated}, residual {st_rx['residual_us']} us; "
          f"subscriber call.tx_calls +{tx_calls}, call.tx_denied +{denied}")

    sdr_in, t0_in = analyze(sdr_bench, cap_in, fin, cc)
    sdr_out, t0_out = analyze(sdr_bench, cap_out, fout, cc)
    grid = ts_of_outbound(sdr_out["bursts"])
    ours = [b for b in sdr_out["bursts"] if b["family"] in ("bs", "emb") and b["kind"] != "IDLE"]
    out_ts = {}
    for b in ours:
        t = grid.get(b["slot"])
        out_ts[t] = out_ts.get(t, 0) + 1
    hdr_out = [b for b in ours if b["kind"] == "VLC_HDR"]
    hdr_in = [b for b in sdr_in["bursts"] if b["kind"] == "VLC_HDR" and (b.get("lc") or {}).get("src") == own]
    print(f"input: {len([b for b in sdr_in['bursts'] if b['family'] in ('ms', 'emb')])} subscriber bursts; outbound: "
          f"{len(ours)} call bursts by slot {out_ts} (grid {grid}), headers {len(hdr_out)}; captures ~/{sdr_bench.dir}/{rdir}/")
    if hdr_in and hdr_out and t0_in and t0_out:
        print(f"repeat delay: {((t0_out + hdr_out[0]['t_ms'] / 1e3) - (t0_in + hdr_in[0]['t_ms'] / 1e3)) * 1e3:.0f} ms")

    assert denied == 0 and tx_calls == 1, f"subscriber call.tx_calls +{tx_calls}, call.tx_denied +{denied}"
    assert "tx_voice" in heard, f"the subscriber never reached tx_voice: {heard}, tx_error {sorted(e for e in errs if e)}"
    assert len(voice) >= 30, f"only {len(voice)} voice bursts went up (4 s is about 66)"
    assert by_label[3 - ts] == 0, f"the call on TS{ts} went up under both labels: {by_label}"
    assert sum(p.repeated.values()) == len(ups)
    assert out_ts.get(ts, 0) >= 30 and not out_ts.get(3 - ts), f"the repeat isn't on outbound TS{ts}: {out_ts}"
    assert hdr_out and all((b.get("lc") or {}).get("src") == own for b in hdr_out), hdr_out[:2]
    assert st_tx[f"underruns{ts}"] == 0, st_tx
    assert st_rx["unlabeled"] == 0 and abs(st_rx["residual_us"]) < 5000, st_rx
