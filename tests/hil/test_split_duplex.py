# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The split duplex extension on one radio (a CONFIG_HOTSPOT_SPLIT=y build; docs/spec/split-duplex.md 10). Every
test skips on a build without it.

- test_split_protocol (no RF): GET_VERSION announces " SPLIT1"; INFO offers the TX and RX halves; SET_ROLE 2 (the RX
  half never transmits) and back to 0; STATUS; an unknown 0xD6 sub-command answers result 1; an unknown command is
  still NAKed 1; the debug link's SPLIT view answers.
- test_split_tx_half_off_air (radio_tx, bench_service: --allow-rf --allow-service-stop): the radio as the TX half on
  --dmr-freq at its lowest power for 10 s, idle bursts with a short LC, SET_AT toggled every 2 s, the BS framing
  experiment's variant from $ODR_SPLIT_VARIANT (default 0). The SDR bench captures and decodes it (sdrmon.py analyze
  --cach). Asserted: BS sync only, one burst every 30 ms on both slots (same-slot spacing 60 ms), one key-up edge
  for the whole outbound, the channel's colour code, no driver underruns in STATUS. Printed, not asserted (the
  experiment's result): the CACH decode (tact ok share, TC alternation, AT, short LCs).
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


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_split_tx_half_off_air(radio, sdr_bench, pytestconfig):
    need_split(radio)
    import split_duplex_host as sdh
    dmr_cach = sdh.dmr_cach
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    hz = round(freq * 1e6)
    variant = int(os.environ.get("ODR_SPLIT_VARIANT", "0"), 0)
    regs = [tuple(int(x, 0) for x in kv.split("=")) for kv in os.environ.get("ODR_SPLIT_REGS", "").split()]
    radio.link.split_variant(variant, regs)    # $ODR_SPLIT_REGS: "REG=VAL ..." for variant 255
    sdr_bench.sync_tool(os.path.join(ROOT, "tools", "bench", "sdrmon.py"))
    sdr_bench.sync_tool(os.path.join(ROOT, "tools", "bench", "dmr_cach.py"))
    rdir = sdr_bench.run_dir("hil_split_tx")
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, freq, seconds=TX_SECONDS + 6)
        with radio.released() as port:
            r = sdh.Radio(port, "tx", dtr=False)
            try:
                sdh.require_split(r)
                sdh.setup_half(r, hz, hz, 0, cc)        # RF level 0: the lowest step (the role's cap is 64)
                sdh.set_role(r, sdh.ROLE_TX, power_cap=1)
                slc = dmr_cach.slc_activity(8, 0, dmr_cach.slc_hash(9), 0)
                assert r.command(sdh.DMR_SHORTLC, dmr_cach.shortlc_frame_payload(slc)) == 0
                assert r.command(sdh.DMR_START, b"\x01") == 0
                t0, mask = time.monotonic(), 0
                while time.monotonic() - t0 < TX_SECONDS:
                    mask ^= 1
                    r.split(sdh.SUB_SET_AT, bytes([mask]))
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
    print(f"STATUS keyed: {st}\nSTATUS after unkey: {end}")
    assert st["flags"] & sdh.FLAG_KEYED and not end["flags"] & sdh.FLAG_KEYED
    assert st["underruns1"] == 0 and st["underruns2"] == 0, st
    assert st["slc_taken"] >= 1 and st["idle"] > 200, st
    bursts = [b for b in sdr["bursts"] if b["family"] != "emb"]
    bs = [b for b in bursts if b["family"] == "bs"]
    assert len(bs) >= 0.9 * TX_SECONDS / 0.03, f"BS bursts {len(bs)}"
    assert all(b["family"] == "bs" for b in bursts), {b["family"] for b in bursts}
    assert all(b["cc"] in (None, cc) for b in bs)
    # Both slots are filled, so neighbours are 30 ms apart and the same slot is two bursts on (sdrmon's
    # same_slot_spacing only pairs neighbours, which suits a one-slot capture)
    t = [b["t_ms"] for b in bs]
    nb = [b - a for a, b in zip(t, t[1:])]
    same = [b - a for a, b in zip(t, t[2:])]
    print(f"BS burst spacing: neighbours {min(nb):.2f}-{max(nb):.2f} ms, same slot mean "
          f"{sum(same) / len(same):.3f} ms; kinds {sdr['summary']['counts']}")
    assert all(abs(x - 30.0) < 0.5 for x in nb), f"a gap or overlap in the outbound: {sorted(set(round(x) for x in nb))}"
    assert abs(sum(same) / len(same) - 60.0) < 0.05
    # Key-ups of our carrier only: the SDR also reports sub-millisecond blips some 50 dB below it
    top = max(e["level_dbfs"] for e in sdr["edges"])
    ups = [e for e in sdr["edges"] if e["level_dbfs"] > top - 20]
    assert len(ups) == 1, f"key-up edges: {[(e['up_ms'], e['level_dbfs']) for e in ups]} (the outbound must stay keyed)"
    other = [b for b in bs if b["kind"] != "IDLE" and not b["kind"].startswith("slottype?")]   # an undecoded slot type
    assert not other, f"only idle bursts were queued: {sdr['summary']['counts']}"
    cach = [b["cach"] for b in bs if b.get("cach")]
    ok = [c for c in cach if c["ok"]]
    alt = sum(1 for x, y in zip(ok, ok[1:]) if x["tc"] != y["tc"])
    at_seen = {c["at"] for c in ok}
    slcs = [c["slc"] for c in cach if "slc" in c]
    slc_ok = [s for s, good in slcs if good]
    _, cnt = radio.counters()
    bs_cnt = {k: v for k, v in cnt.items() if k.startswith("bs.")}
    print(f"BS framing experiment variant {variant}: tact ok {len(ok)}/{len(cach)}, TC alternating {alt} of "
          f"{max(len(ok) - 1, 0)}, AT values {sorted(at_seen)}, short LC {len(slc_ok)} ok / {len(slcs) - len(slc_ok)} "
          f"bad; radio {bs_cnt}")
    # The CACH from the core reaches the chip (AT / TC in 0x52, the short LC in TX RAM; modem-hrc6000.md 9.1). Whether
    # it shows on air is the experiment's result above (split-duplex.md 10.1)
    assert bs_cnt.get("bs.cach", 0) > 0.9 * TX_SECONDS / 0.03, bs_cnt


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
        sdh.require_split(r)
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
def test_split_rx_half_labels(radio, sdr_bench, pytestconfig):
    need_split(radio)
    freq, cc, tg = (pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc"),
                    pytestconfig.getoption("--dmr-tg"))
    with sdr_bench.lease():
        with radio.released() as port:
            got = rx_half_labels(port, sdr_bench, freq, cc, tg)
    leave_hotspot(radio)
    check_rx_labels(*got)
