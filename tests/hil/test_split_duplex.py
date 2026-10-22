# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The split duplex extension on one radio (a CONFIG_HOTSPOT_SPLIT=y build; docs/spec/split-duplex.md 10). Every
test skips on a build without it.

- test_split_protocol (no RF): GET_VERSION announces " SPLIT1"; INFO offers the TX and RX halves; SET_ROLE 2 (the RX
  half never transmits) and back to 0; STATUS; an unknown 0xD6 sub-command answers result 1; an unknown command is
  still NAKed 1; the debug link's SPLIT view answers.
- test_split_tx_half_off_air (radio_tx, bench_service: --allow-rf --allow-service-stop): the radio as the TX half on
  --dmr-freq at its lowest power for 10 s, idle bursts with a short LC, SET_AT toggled every 2 s, the framing variant
  from $ODR_SPLIT_VARIANT (default 0, ours; on the DM-1701 1 is the chip's, whose CACH is empty; on the HR_C7000
  radios a bit field of physical layer options, modem-hrc7000.md 2.10). The SDR bench captures and decodes it
  (sdrmon.py analyze --cach). Asserted: BS sync only, one burst every 30 ms on both slots (same-slot spacing 60 ms),
  one key-up edge for the whole outbound, the channel's color code, no driver underruns in STATUS; with our framing
  also the CACH: TC alternating, AT following SET_AT, the short LC decoding.
- test_split_tx_half_voice (radio_tx, bench_service): the same outbound (our framing) with a group call from the host
  on TS2: three LC headers, three superframes A-F with the embedded LC in each, the terminator, decoded off air on
  one slot without sync errors; the CACH stays right.
- test_split_rx_half_labels (rf, bench_service): the radio as the RX half, the bench transmitting voice calls. The
  bench is simplex, so its slot phase (and which slot it lands on) is new with every call: one long call, the epoch
  centered on its first burst, then moved by one slot half way; each part goes up under one label, the labels differ,
  the residual stays under 5 ms.
"""
import os
import sys
import time

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
TX_SECONDS = 10.0


def need_split(radio):
    feats = radio.features()
    if "HOTSPOT_SPLIT" not in feats:
        pytest.skip("built without the split duplex extension (make CONFIG_HOTSPOT_SPLIT=y)")


def need_role(r, sdh, role):
    """Skip unless the radio offers the split duplex role (INFO "offered": bit 0 TX half, bit 1 RX half)"""
    _, info = sdh.require_split(r)
    if not info["offered"] & (1 << (role - 1)):
        pytest.skip(f"the radio doesn't offer split duplex role {role} (offered {info['offered']}: its modem glue has "
                    f"no slot clock or base-station hooks)")


def leave_hotspot(radio):
    radio.link.hotspot_exit()
    radio.wait_for(lambda x: x.get("hostlink") == "idle", 5, sections=("hotspot",), what="hotspot left")


def test_split_protocol(radio):
    need_split(radio)
    import split_duplex_host as sdh
    hz = int(radio.core()["rx_hz"])
    with radio.released() as port:
        r = sdh.Radio(port, "radio", dtr=False)
        try:
            ver, info = sdh.require_split(r)
            assert ver == 1
            if info["offered"] == 0:
                pytest.skip("the radio doesn't offer split duplex (its modem glue has no slot clock)")
            assert info["offered"] & 3 == 3, info
            assert info["role"] == sdh.ROLE_NORMAL
            print(f"INFO {info} (cach method {sdh.CACH_METHODS.get(info['cach'])})")
            sdh.setup_half(r, hz, hz, 0, 1)
            info = sdh.set_role(r, sdh.ROLE_RX)
            assert info["role"] == sdh.ROLE_RX
            st = sdh.get_status(r)
            assert st["role"] == sdh.ROLE_RX and not st["flags"] & sdh.FLAG_KEYED, st
            _, _, res, _ = r.split(0x07)
            assert res == 1, "unknown sub-command"
            _, _, f = r.request(sdh.frame(0xD7), lambda x: x[2] in (sdh.NAK, sdh.ACK))
            assert f[2] == sdh.NAK and f[3] == 0xD7 and f[4] == 1, f"unknown command: {f.hex()}"
            assert sdh.set_role(r, sdh.ROLE_NORMAL)["role"] == sdh.ROLE_NORMAL
        finally:
            r.close()
    v = radio.link.split_view()
    assert v.get("split.built") == "1" and v.get("split.role") == "normal", v
    leave_hotspot(radio)


# A group call for the voice test as host frames (control byte, 33-byte burst), made with the core's encoders
# (dmr_lc_encode_full, dmrFecBptcEncode): LC 201 from 9990301; voice is AMBE silence, the middle 48 bits left zero
CALL_HEADER = bytes.fromhex("003c0a120b0c0ac870f090218870000000000008986815c01de0b8e0c2813b80c9")
CALL_VOICE = bytes.fromhex("b9e881526173002a6bb9e881526000000000000173002a6bb9e881526173002a6b")
CALL_TERM = bytes.fromhex("00530ac60bbc0ab07080908188b000000000000fcd7c16f81a80b4a0ca812280fa")
CALL = ([(0x41, CALL_HEADER)] * 3 + [(0x20 if n == 0 else n, CALL_VOICE) for _ in range(3) for n in range(6)]
        + [(0x42, CALL_TERM)])


def tx_half_run(radio, sdr_bench, pytestconfig, variant, regs=(), call=()):
    """The radio as the TX half for TX_SECONDS at its lowest power: idle bursts and a short LC, SET_AT toggled every
    2 s, the host frames of call on TS2 half a second in. Returns the SDR decode and STATUS keyed / after unkey."""
    import split_duplex_host as sdh
    dmr_cach = sdh.dmr_cach
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    hz = round(freq * 1e6)
    radio.link.split_variant(variant, regs)
    sdr_bench.sync_tool(os.path.join(ROOT, "tools", "bench", "sdrmon.py"))
    sdr_bench.sync_tool(os.path.join(ROOT, "tools", "bench", "dmr_cach.py"))
    rdir = sdr_bench.run_dir("hil_split_tx")
    call = list(call)
    # Raw I/Q on purpose: sdrmon analyze --cach decodes the CACH from the waveform, which the burst records lack
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, freq, seconds=TX_SECONDS + 6)
        with radio.released() as port:
            r = sdh.Radio(port, "tx", dtr=False)
            try:
                need_role(r, sdh, sdh.ROLE_TX)
                sdh.setup_half(r, hz, hz, 0, cc)        # RF level 0: the lowest step (the role's cap is 64)
                sdh.set_role(r, sdh.ROLE_TX, power_cap=1)
                slc = dmr_cach.slc_activity(8, 0, dmr_cach.slc_hash(9), 0)
                assert r.command(sdh.DMR_SHORTLC, dmr_cach.shortlc_frame_payload(slc)) == 0
                assert r.command(sdh.DMR_START, b"\x01") == 0
                t0, mask = time.monotonic(), 0
                while time.monotonic() - t0 < TX_SECONDS:
                    mask ^= 1
                    r.split(sdh.SUB_SET_AT, bytes([mask]))
                    if call:
                        time.sleep(0.5)
                        t1 = time.monotonic()
                        for i, (c, f) in enumerate(call):     # paced at the slot's 60 ms (no reply to data frames)
                            time.sleep(max(0.0, t1 + i * 0.06 - time.monotonic()))
                            r.send(sdh.frame(sdh.DMR_DATA2, bytes([c]) + f))
                        call = []
                    for _ in range(4):
                        time.sleep(0.5)
                        r.split(sdh.SUB_TIME, bytes(8))
                st = sdh.get_status(r)
                assert r.command(sdh.DMR_START, b"\x00") == 0
                time.sleep(1.5)
                end = sdh.get_status(r)
                sdh.set_role(r, sdh.ROLE_NORMAL)
            finally:
                r.close()
        sdr_bench.capture_wait()
    txt = sdr_bench.ssh(f"cd ~/{sdr_bench.dir} && {sdr_bench.py} sdrmon.py analyze {cap} --freq {freq} --cc {cc} "
                        f"--cach --json {cap}.json > {cap}.txt 2>&1; tail -40 {cap}.txt", timeout=600)
    print(txt)
    import json
    sdr = json.loads(sdr_bench.ssh(f"cat {cap}.json"))
    leave_hotspot(radio)
    return sdr, st, end


def tx_half_cach(bs):
    """The CACH decode of the BS bursts: (CACHs, tact ok, TC alternations, AT values, short LCs decoded, bad)"""
    cach = [b["cach"] for b in bs if b.get("cach")]
    ok = [c for c in cach if c["ok"]]
    alt = sum(1 for x, y in zip(ok, ok[1:]) if x["tc"] != y["tc"])
    slcs = [c["slc"] for c in cach if "slc" in c]
    good = sum(1 for _, g in slcs if g)
    return cach, ok, alt, {c["at"] for c in ok}, good, len(slcs) - good


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_split_tx_half_off_air(radio, sdr_bench, pytestconfig):
    need_split(radio)
    import split_duplex_host as sdh
    cc = pytestconfig.getoption("--dmr-cc")
    variant = int(os.environ.get("ODR_SPLIT_VARIANT", "0"), 0)
    regs = [tuple(int(x, 0) for x in kv.split("=")) for kv in os.environ.get("ODR_SPLIT_REGS", "").split()]
    sdr, st, end = tx_half_run(radio, sdr_bench, pytestconfig, variant, regs)   # $ODR_SPLIT_REGS: "REG=VAL ..."
    print(f"STATUS keyed: {st}\nSTATUS after unkey: {end}")
    assert st["flags"] & sdh.FLAG_KEYED and not end["flags"] & sdh.FLAG_KEYED
    assert st["underruns1"] == 0 and st["underruns2"] == 0, st
    assert st["slc_taken"] >= 1 and st["idle"] > 200, st
    bursts = [b for b in sdr["bursts"] if b["family"] != "emb"]
    bs = [b for b in bursts if b["family"] == "bs"]
    assert len(bs) >= 0.9 * TX_SECONDS / 0.03, f"BS bursts {len(bs)}"
    assert all(b["family"] == "bs" for b in bursts), {b["family"] for b in bursts}
    assert all(b["cc"] in (None, cc) for b in bs)
    # Both slots are filled, so neighbors are 30 ms apart and the same slot is two bursts on (sdrmon's
    # same_slot_spacing only pairs neighbors, which suits a one-slot capture)
    t = [b["t_ms"] for b in bs]
    nb = [b - a for a, b in zip(t, t[1:])]
    same = [b - a for a, b in zip(t, t[2:])]
    print(f"BS burst spacing: neighbors {min(nb):.2f}-{max(nb):.2f} ms, same slot mean "
          f"{sum(same) / len(same):.3f} ms; kinds {sdr['summary']['counts']}")
    assert all(abs(x - 30.0) < 0.5 for x in nb), f"a gap or overlap in the outbound: {sorted(set(round(x) for x in nb))}"
    assert abs(sum(same) / len(same) - 60.0) < 0.05
    # Key-ups of our carrier only: the SDR also reports sub-millisecond blips some 50 dB below it
    top = max(e["level_dbfs"] for e in sdr["edges"])
    ups = [e for e in sdr["edges"] if e["level_dbfs"] > top - 20]
    assert len(ups) == 1, f"key-up edges: {[(e['up_ms'], e['level_dbfs']) for e in ups]} (the outbound must stay keyed)"
    other = [b for b in bs if b["kind"] != "IDLE" and not b["kind"].startswith("slottype?")]   # an undecoded slot type
    assert not other, f"only idle bursts were queued: {sdr['summary']['counts']}"
    cach, ok, alt, at_seen, slc_good, slc_bad = tx_half_cach(bs)
    _, cnt = radio.counters()
    bs_cnt = {k: v for k, v in cnt.items() if k.startswith("bs.")}
    print(f"BS framing variant {variant}: tact ok {len(ok)}/{len(cach)}, TC alternating {alt} of "
          f"{max(len(ok) - 1, 0)}, AT values {sorted(at_seen)}, short LC {slc_good} ok / {slc_bad} "
          f"bad; radio {bs_cnt}")
    assert bs_cnt.get("bs.cach", 0) > 0.9 * TX_SECONDS / 0.03, bs_cnt
    # Our framing (the core's CACH on air) except the HR_C6000's variant 1, its chip's layer 2 framing (split-duplex.md
    # 10.1); every HR_C7000 variant is a physical layer option of ours (modem-hrc7000.md 2.10)
    chip_framing = variant == 1 and radio.version().get("board", "").startswith("DM-1701")
    if not chip_framing:
        assert len(ok) >= 0.98 * len(cach) and alt >= 0.98 * (len(ok) - 1), "TC must alternate"
        assert at_seen == {0, 1}, f"AT must follow SET_AT: {sorted(at_seen)}"
        assert slc_good >= 10 and slc_bad == 0, f"short LC {slc_good} ok, {slc_bad} bad"


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_split_tx_half_voice(radio, sdr_bench, pytestconfig):
    """A group call from the host on TS2 of the outbound, our framing: headers, three superframes with the embedded
    LC, the terminator, decoded off air; the CACH stays right around it"""
    need_split(radio)
    sdr, st, end = tx_half_run(radio, sdr_bench, pytestconfig, 0, call=CALL)
    assert st["underruns1"] == 0 and st["underruns2"] == 0, st
    bursts = [b for b in sdr["bursts"] if b["family"] in ("bs", "emb")]
    ours = [b for b in bursts if b["kind"] != "IDLE"]
    kinds = [b["kind"] for b in ours]
    print("call on air:", kinds)
    assert all(b["sync_errors"] == 0 for b in ours)
    assert len({b["slot"] for b in ours}) == 1, "the call must stay on one slot"
    hdr = [b for b in ours if b["kind"] == "VLC_HDR"]
    term = [b for b in ours if b["kind"] == "TERM_LC"]
    voice = [b["kind"][-1] for b in ours if b["kind"].startswith("VOICE_")]
    emb = [b["lc"] for b in ours if b["kind"].startswith("VOICE_") and b.get("lc")]
    assert len(hdr) == 3 and all(b["lc"] and b["lc"]["src"] == 9990301 and b["lc"]["dst"] == 201 for b in hdr), hdr
    assert "".join(voice) == "ABCDEF" * 3, voice
    assert len(emb) == 3 and all(x["src"] == 9990301 and x["dst"] == 201 for x in emb), emb
    assert len(term) == 1 and term[0]["lc"]["src"] == 9990301, term
    cach, ok, alt, _, slc_good, slc_bad = tx_half_cach([b for b in sdr["bursts"] if b.get("cach")])
    assert len(ok) >= 0.98 * len(cach) and alt >= 0.98 * (len(ok) - 1) and slc_bad == 0


def rx_half_labels(port, sdr_bench, freq, cc, tg, dtr=False):
    """The RX half against one long bench call on an MMDVM port (serial, or udp:<ip>): the epoch centered on the
    call's first burst, then moved by one slot half way through. Returns what check_rx_labels judges."""
    import split_duplex_host as sdh
    import threading
    hz = round(freq * 1e6)
    results, first, marks, ups = [], None, [], []
    r = sdh.Radio(port, "rx", dtr=dtr)
    stop = threading.Event()

    def keep_lease():
        while not stop.wait(1.0):
            try:
                r.split(sdh.SUB_TIME, bytes(8))
            except Exception:
                pass
    try:
        need_role(r, sdh, sdh.ROLE_RX)
        sdh.setup_half(r, hz, hz, 0, cc)
        sdh.set_role(r, sdh.ROLE_RX)
        sync = sdh.ClockSync()
        sdh.sync_burst(r, sync)
        e = sync.to_modem(sdh.Radio.host_us())
        r.split(sdh.SUB_SET_EPOCH, sdh.epoch_payload(e))
        threading.Thread(target=keep_lease, daemon=True).start()
        # The bench is a simplex modem: every call starts its slots where it keys up, so no epoch holds from one
        # call to the next, not even which slot it lands on. One long call: the epoch is centered on its first
        # burst, then moved by one slot half way through; the label must flip there.
        with r.lock:
            r.uplink.clear()
        before = sdh.get_status(r)
        n0 = before["labeled1"] + before["labeled2"]
        call = threading.Thread(target=sdr_bench.voice_call, args=(freq, cc, tg), kwargs={"superframes": 8},
                                daemon=True)
        call.start()
        t0 = time.monotonic()
        while time.monotonic() - t0 < 30 and call.is_alive():
            st = sdh.get_status(r)
            if st["labeled1"] + st["labeled2"] > n0:
                first = st["residual_us"]
                e = (e + first) % sdh.WRAP           # residuals are measured from the epoch set last
                r.split(sdh.SUB_SET_EPOCH, sdh.epoch_payload(e))
                with r.lock:
                    marks.append(len(r.uplink))
                time.sleep(1.2)
                results.append(("centered", sdh.get_status(r)))
                r.split(sdh.SUB_SET_EPOCH, sdh.epoch_payload((e + 30_000) % sdh.WRAP))
                with r.lock:
                    marks.append(len(r.uplink))
                break
            time.sleep(0.02)
        call.join(60)
        time.sleep(1.0)
        with r.lock:
            ups = list(r.uplink)
        results.append(("shifted", sdh.get_status(r)))
    finally:
        stop.set()
        try:
            sdh.set_role(r, sdh.ROLE_NORMAL)
        except Exception:
            pass
        r.close()
    return first, marks, ups, results


def check_rx_labels(first, marks, ups, results):
    import split_duplex_host as sdh
    assert first is not None and len(marks) == 2, "the radio labeled nothing of the bench's call"
    a = [f for _, f in ups[marks[0]:marks[1]] if f[2] in (sdh.DMR_DATA1, sdh.DMR_DATA2)]
    b = [f for _, f in ups[marks[1]:] if f[2] in (sdh.DMR_DATA1, sdh.DMR_DATA2)]
    la, lb = {f[2] for f in a}, {f[2] for f in b}
    for name, st in results:
        print(f"{name}: residual {st['residual_us']} us, labeled {st['labeled1']}/{st['labeled2']}")
    print(f"first burst residual {first} us; centered {len(a)} bursts on {sorted(hex(c) for c in la)}, "
          f"shifted {len(b)} bursts on {sorted(hex(c) for c in lb)}")
    assert len(la) == 1 and len(lb) == 1, "one label each side of the shift"
    assert la != lb, "moving the epoch by one slot flips the label"
    assert sum(1 for f in a if f[3] & 0x40 == 0) >= 12, "voice bursts went up"
    assert 0x42 in [f[3] for f in b], "the terminator went up"
    assert all(abs(st["residual_us"]) < 5000 for _, st in results), results


@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_split_rx_half_labels(radio, sdr_bench, pytestconfig):
    need_split(radio)
    freq, cc, tg = (pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc"),
                    pytestconfig.getoption("--dmr-tg"))
    with sdr_bench.lease():
        with radio.released() as port:
            got = rx_half_labels(port, sdr_bench, freq, cc, tg)
    leave_hotspot(radio)
    check_rx_labels(*got)
