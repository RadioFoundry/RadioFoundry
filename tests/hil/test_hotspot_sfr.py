# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The MMDVM hotspot's SFR mode without transmitting (a CONFIG_SFR=y build): GET_VERSION announces the SFR, the
duplex flag with RX = TX turns it on (GET_STATUS grows to 121 bytes, both space bytes carry the outbound slot's
space, the debug link shows sfr.on=1), the simplex flag turns it off again (docs/spec/mmdvm-protocol.md 6,
docs/spec/sfr.md). The radio keys only for something heard or sent: this test sends nothing, but a call heard on
the channel would be repeated, so it is marked radio_tx (needs --allow-rf) and runs on the radio's current channel
frequency (the bench's). It expects a quiet channel (sfr.tx_starts stays put). The MMDVM client keeps DTR clear (see
test_hotspot.py).

test_sfr_repeats_bench_call (rf, radio_tx, bench_service): the radio in SFR mode on the DMR test channel
(tools/bench/mmdvm_hotspot.py's MmdvmLink as the host, RF level 12: the lowest power); the bench modem sends a 3
superframe group call with TS1 direct mode sync (the default inbound slot) while the RTL-SDR records. Pass: the call's
header goes up the USB link as DMR_DATA1; on air a TS2 direct mode sync burst (the outbound slot) carries the bench
call's LC (source ID and talkgroup); the session's sfr.repeated and sfr.tx_starts (counted from 0 per session) are at
least 1; the whole call is heard while keyed (docs/spec/sfr.md 3 item 1): at least 15 voice frames and the
terminator up, at least 15 voice bursts and the terminator repeated on the outbound slot.

Before the SFR session the radio keys up once on its own timing (1 s, nothing heard), so a TX slot phase left from
that transmission is in place, the case the repeater work found (modem-hrc7000.md 2.5): the repeat must still take
the received slot timing. On an HR_C7000 the debug link's txh line gives the repeat's first TX tick against the last
receive tick (u, 0.1 ms): a whole number of slots (300) is on the received grid. Pass also needs u within 1 ms of
one there. An SFR's start reads 600: after the SLOTON b5 strobe the first TX-vector interrupt comes two slots after
the last receive one (DM-32UV and DM-1801 Pro, 2026-10-18). The SDR's offset of the
repeat from the inbound bursts (30 ms) is printed. A bench station that keeps its carrier up through the call hides
our slot from the SDR until it drops: with no burst of ours seen inside the inbound call, the repeat is judged by the
radio's sfr.repeated (at least 19: the 18 voice bursts and a header) instead of the SDR's voice count."""
import re
import statistics
import struct
import time

import pytest

GET_VERSION, GET_STATUS, SET_CONFIG, SET_FREQ = 0x00, 0x01, 0x02, 0x04
SFR_DEBUG = 0xD5


def config(simplex, cc=1):
    return bytes([0x80 if simplex else 0x00, 0x02, 10, 2, 128, 128, cc, 0, 128, 0, 128, 0, 0, 128, 128] + [0] * 8)


def freq(rx, tx):
    return bytes([0]) + struct.pack("<II", rx, tx) + bytes([0]) + struct.pack("<I", tx)


@pytest.mark.radio_tx
def test_hotspot_sfr_mode(radio):
    feats = radio.features()
    if "SFR" not in feats or "HOTSPOT" not in feats:
        pytest.skip("built without the SFR (make CONFIG_SFR=y)")
    import mmdvm_probe
    hz = int(radio.core()["rx_hz"])
    tx0 = None
    with radio.released() as port:
        link = mmdvm_probe.Link(port, dtr=False)
        try:
            kind, f = link.request(GET_VERSION, timeout=2.0)
            assert kind == "reply" and b" SFR TS1>TS2 " in f, f
            kind, f = link.request(SET_CONFIG, config(simplex=False))
            assert kind == "ack", f
            kind, f = link.request(SET_FREQ, freq(hz, hz))
            assert kind == "ack", f
            st = None
            for _ in range(8):
                kind, st = link.request(GET_STATUS)
                assert kind == "reply", st
                if len(st) == 121:
                    break
                time.sleep(0.25)
            assert len(st) == 121, f"SFR status bytes missing: {st.hex()}"
            assert st[7] == st[8] == 16, st.hex()
            assert st[5] & 0x80 == 0, "inbound busy on a quiet channel"
            kind, f = link.request(SFR_DEBUG, bytes([0, 0]))
            assert kind == "ack", f
            kind, f = link.request(SFR_DEBUG, bytes([0]))
            assert kind == "nak" and f[4] == 4, f
        finally:
            link.s.close()
    v = radio.wait_for(lambda x: x.get("hostlink") == "hotspot" and x.get("sfr.on") == "1", 3,
                       sections=("hotspot", "sfr"), what="SFR mode on")
    tx0 = v.get("sfr.tx_starts")
    with radio.released() as port:
        link = mmdvm_probe.Link(port, dtr=False)
        try:
            kind, f = link.request(SET_CONFIG, config(simplex=True))
            assert kind == "ack", f
            kind, st = link.request(GET_STATUS)
            assert kind == "reply" and len(st) == 13, st
        finally:
            link.s.close()
    v = radio.wait_for(lambda x: x.get("sfr.on") == "0", 3, sections=("hotspot", "sfr"), what="SFR mode off")
    assert v.get("sfr.tx_starts") == tx0, "the SFR keyed on a quiet channel"
    radio.link.hotspot_exit()
    radio.wait_for(lambda x: x.get("hostlink") == "idle", 5, sections=("hotspot", "sfr"), what="hotspot left")


@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.sdr_rx
def test_sfr_repeats_bench_call(radio, sdr_bench, pytestconfig):
    feats = radio.features()
    if "SFR" not in feats or "HOTSPOT" not in feats:
        pytest.skip("built without the SFR (make CONFIG_SFR=y)")
    import mmdvm_hotspot as M
    freq, cc, tg = (pytestconfig.getoption(o) for o in ("--dmr-freq", "--dmr-cc", "--dmr-tg"))
    src = 9990301
    hz = round(freq * 1e6)
    radio.go_home()
    radio.find_channel(hz, "dmr")
    # A transmission on our own timing first (see the docstring)
    radio.ptt_for(1.0)
    radio.wait_for(lambda x: x.get("state") != "tx", timeout=5, what="TX ended")
    v0 = radio.view(["hotspot", "sfr"])
    rdir = sdr_bench.run_dir("hil_sfr_repeat")
    with sdr_bench.lease():
        watch = sdr_bench.dmr_watch_start(rdir, freq, 12, cc)     # bursts decoded on the bench when it can
        with radio.released() as port:
            t = M.Transport(port)
            hs = M.MmdvmLink(t, hz, cc, 12, simplex=False)
            try:
                mark = hs.mark()
                sdr_bench.transmit(f"voice --src {src} --dst {tg} --superframes 3 --family ts1", freq, cc)
                time.sleep(2.0)
                frames, naks = hs.since(mark)
            finally:
                hs.stop = True
                time.sleep(0.3)
                t.close()
        sdr = sdr_bench.dmr_watch_result(watch)
    v1 = radio.view(["hotspot", "sfr"])
    diag = [x for x in radio.link.diag()[1] if x.startswith(("txb ", "txh "))]
    up = [d for _, cmd, d in frames if cmd == M.DMR_DATA1]
    ctl = [d[3] for d in up if len(d) > 3]
    voice = [c for c in ctl if not c & 0x40]
    bursts = sdr.get("bursts", [])
    fams = {}
    for b in bursts:
        fams[b.get("family")] = fams.get(b.get("family"), 0) + 1
    ts2 = [b for b in bursts if b.get("family") == "ts2"]
    ts2_lc = [b["lc"] for b in ts2 if (b.get("lc") or {}).get("src") is not None and "flco" in b["lc"]]
    cmds = {}
    for _, cmd, d in frames:
        cmds[M.NAMES.get(cmd, hex(cmd))] = cmds.get(M.NAMES.get(cmd, hex(cmd)), 0) + 1
    print(f"frames up: {cmds}; sfr view after: " + " ".join(f"{k}={v1[k]}" for k in sorted(v1) if k.startswith("sfr.")))
    print(f"uplink: {len(up)} DMR_DATA1 (controls {sorted(set(ctl))}), NAKs {naks}; SDR families {fams}; "
          f"TS2 LCs {ts2_lc[:3]}; sfr.repeated {v0.get('sfr.repeated')} -> {v1.get('sfr.repeated')}, "
          f"sfr.tx_starts {v0.get('sfr.tx_starts')} -> {v1.get('sfr.tx_starts')}")
    print(f"capture on the bench: ~/{sdr_bench.dir}/{rdir}/" if "cap" in watch else "bursts decoded on the bench")
    # The repeat's slot timing: each TS2 burst against the last inbound TS1 one before it (30 ms on the grid)
    t_in = sorted(b["t_ms"] for b in bursts if b.get("family") == "ts1")
    offs = []
    for b in ts2:
        prior = [t for t in t_in if t < b["t_ms"]]
        if prior:
            offs.append(round((b["t_ms"] - prior[-1]) % 60.0, 1))
    print(f"repeat timing: {len(offs)} TS2 bursts, ms after the last inbound burst: "
          f"median {statistics.median(offs) if offs else None}, {offs[:12]}; diag {diag}")
    m = next((re.search(r"\bu(\d+)", x) for x in diag if x.startswith("txh ")), None)
    if m:
        u = int(m.group(1))
        off = min(u % 300, 300 - u % 300)
        assert u > 0 and off <= 10, \
            f"the repeat's first TX tick came {u / 10:.1f} ms after the last receive tick, off the slot grid"
    # What works with today's driver: the call's start heard, sent up and repeated on the outbound slot
    assert 0x41 in ctl, f"the call's header didn't go up as DMR_DATA1: {ctl}"
    assert any(x["src"] == src and x["dst"] == tg for x in ts2_lc), f"no repeat of the bench call's LC on TS2: {ts2_lc}"
    # the sfr.* counters start from 0 with each SFR session and stay readable after it: this session's
    assert int(v1.get("sfr.repeated", 0)) >= 1 and int(v1.get("sfr.tx_starts", 0)) >= 1, v1
    # The whole call, heard while keyed (sfr.md 3 item 1): sent up and repeated. Voice B-F carry an EMB, not a sync, so
    # the repeat's voice is counted by the SDR's slot letter of our TS2-synced bursts
    out_slot = {b.get("slot") for b in ts2}
    rep_voice = [b for b in bursts if b.get("voice") and b.get("slot") in out_slot]
    print(f"repeat on slot {sorted(out_slot)}: {len(rep_voice)} voice bursts, "
          f"{''.join(b['kind'][-1] for b in rep_voice)}")
    assert 0x42 in ctl, f"the terminator didn't go up: {ctl}"
    assert len(voice) >= 15, f"only {len(voice)} of 18 voice frames went up"
    # A bench station that keeps its carrier up through the call hides our slot from the SDR: no burst of ours inside
    # the inbound call's span (DM-32UV and DM-1801 Pro, 2026-10-18; the repeat after it was seen, 30 ms after the
    # inbound slot). Then the radio's count of repeated bursts decides: the 18 voice bursts and at least one header
    t_rep = [b["t_ms"] for b in bursts if b.get("slot") in out_slot]
    if t_in and not any(t_in[0] < t < t_in[-1] for t in t_rep):
        print(f"the SDR doesn't see our slot during the inbound call; sfr.repeated {v1.get('sfr.repeated')}")
        assert len(out_slot) == 1 and int(v1.get("sfr.repeated", 0)) >= 19, \
            f"only {v1.get('sfr.repeated')} bursts repeated (18 voice and a header expected)"
    else:
        assert len(out_slot) == 1 and len(rep_voice) >= 15, f"only {len(rep_voice)} of 18 voice bursts repeated"
    assert any(b.get("kind") == "TERM_LC" for b in ts2), "no repeated terminator on TS2"
