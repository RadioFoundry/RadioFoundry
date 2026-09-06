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
- test_split_rx_half_labels (rf, bench_service): the radio as the RX half with an epoch made from its own clock, the
  bench transmitting voice calls: every burst of a call goes up under one label, with header, voice and terminator;
  the epoch moved by one slot flips the label. The STATUS phase residual is printed.
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
    radio.link.split_variant(variant)
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
    sp = sdr["summary"]["same_slot_spacing_ms"]
    assert sp and abs(sp["mean"] - 60.0) < 0.05, sp
    assert len(sdr["edges"]) == 1, f"key-up edges: {len(sdr['edges'])} (the outbound must stay keyed)"
    cach = [b["cach"] for b in bs if b.get("cach")]
    ok = [c for c in cach if c["ok"]]
    print(f"BS framing experiment variant {variant}: tact ok {len(ok)}/{len(cach)}")


@pytest.mark.rf
@pytest.mark.bench_service
def test_split_rx_half_labels(radio, sdr_bench, pytestconfig):
    need_split(radio)
    import split_duplex_host as sdh
    import threading
    freq, cc, tg = (pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc"),
                    pytestconfig.getoption("--dmr-tg"))
    hz = round(freq * 1e6)
    results = []
    with sdr_bench.lease():
        with radio.released() as port:
            r = sdh.Radio(port, "rx", dtr=False)
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
                th = threading.Thread(target=keep_lease, daemon=True)
                th.start()
                # Call 1 measures where the bench's slots fall against the epoch; the epoch is then centered on them
                for shift in (None, 0, 30_000):
                    if shift is not None:
                        r.split(sdh.SUB_SET_EPOCH, sdh.epoch_payload(e + shift))
                    with r.lock:
                        r.uplink.clear()
                    sdr_bench.voice_call(freq, cc, tg, superframes=2)
                    time.sleep(1.0)
                    with r.lock:
                        ups = list(r.uplink)
                    st = sdh.get_status(r)
                    results.append((shift, ups, st))
                    if shift is None:
                        e = (e + st["residual_us"]) % sdh.WRAP
            finally:
                stop.set()
                try:
                    sdh.set_role(r, sdh.ROLE_NORMAL)
                except Exception:
                    pass
                r.close()
    leave_hotspot(radio)
    labels = []
    for shift, ups, st in results:
        data = [f for _, f in ups if f[2] in (sdh.DMR_DATA1, sdh.DMR_DATA2)]
        cmds = {f[2] for f in data}
        print(f"epoch shift {shift}: {len(data)} bursts on {sorted(hex(c) for c in cmds)}, residual "
              f"{st['residual_us']} us, labeled {st['labeled1']}/{st['labeled2']}")
        if shift is None:
            continue
        assert len(cmds) == 1, f"one call, one label: {sorted(hex(c) for c in cmds)}"
        controls = [f[3] for f in data]
        assert 0x41 in controls and 0x42 in controls, "header and terminator went up"
        assert sum(1 for c in controls if c & 0x40 == 0) >= 12, "voice bursts went up"
        assert abs(st["residual_us"]) < 5000, st
        labels.append(cmds.pop())
    assert labels[0] != labels[1], "moving the epoch by one slot flips the label"
