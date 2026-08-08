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
  ID, voice bursts in whole superframes, AMBE FEC corrections 0.
- radio: the call stack counted one TX call (call.tx_calls + 1), no TX underruns.
"""
import pytest

PTT_S = 3.0


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
    c.items = [i for i in c.items if not i["check"].startswith("|frequency offset|")]    # replaced below
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
    for i in c.items:
        print(f"{ {True: 'ok  ', False: 'FAIL', None: 'man '}[i['ok']]} {i['check']}  {i['detail']}")
    print(f"captures on the bench: ~/{sdr_bench.dir}/{rdir}/")
    failed = [i for i in c.items if i["ok"] is False]
    assert not failed, "; ".join(f"{i['check']} ({i['detail']})" for i in failed)
