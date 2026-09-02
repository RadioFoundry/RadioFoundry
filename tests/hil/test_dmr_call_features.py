# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""DMR voice call features: private calls, emergency, talker alias, late entry, privacy marking (opt-in RF:
--allow-rf --allow-service-stop; the SDR bench's transmitter is the other station), and the talk target / privacy key
requests over the debug link (radio only, nothing transmits).

Each RF test sends one short call from the bench (bench_tx.py voice) on the DMR test channel and checks the call stack's
counters (dmr_stats) and the call it shows (dmr_call). Pass criteria are in README.md."""
import time

import pytest

from odr_hil import counter_deltas

BENCH_SRC = 9990301
STATS = ["call.rx_calls", "call.rx_calls_played", "call.rx_late_entries", "call.rx_voice_bursts",
         "call.rx_enc_dropped", "call.rx_private_calls", "call.rx_emergency_calls", "call.rx_encrypted_calls",
         "call.rx_alias_complete", "call.rx_pi_headers"]


def _on_dmr_channel(radio, pytestconfig):
    if "DMR" not in radio.features():
        pytest.skip("built without DMR")
    freq = pytestconfig.getoption("--dmr-freq")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    time.sleep(1.0)


def _bench_call(radio, sdr_bench, pytestconfig, **kw):
    """One bench call with the given bench_tx.py voice options; returns (counter deltas, dmr + dmr_call view)."""
    freq, cc, tg = (pytestconfig.getoption(o) for o in ("--dmr-freq", "--dmr-cc", "--dmr-tg"))
    kw.setdefault("dst", tg)
    kw.setdefault("src", BENCH_SRC)
    kw.setdefault("superframes", 4)
    radio.reset_counters()
    _, before = radio.counters()
    with sdr_bench.lease():
        sdr_bench.voice_call(freq, cc, **kw)
    time.sleep(1.0)                      # the call ends (terminator or fade) and the view settles
    _, after = radio.counters()
    d = counter_deltas(before, after, STATS)
    v = radio.view(["dmr", "dmr_call"])
    print("radio deltas:", d)
    print("view:", {k: v[k] for k in v if k.startswith(("call.", "last.", "dmr.target", "dmr.reply"))})
    return d, v


def _print_lc_trace(radio):
    """The HR_C6000 sticky LC trace of the last reception (DM-1701 and the other HR_C6000 radios; nothing elsewhere):
    the dle lines of the debug link log (capture, then every look at the LC page until the call's LC was taken) and
    the lc / lc2 / lt diagnostics lines."""
    _, text = radio.log()
    for line in text.splitlines():
        if " dle " in f" {line}":
            print("  " + line)
    _, lines = radio.link.diag()
    for line in lines:
        if line.split()[:1] in (["lc"], ["lc2"], ["lt"]):
            print("  " + line)


@pytest.mark.rf
@pytest.mark.bench_service
def test_private_call_to_us(radio, sdr_bench, pytestconfig):
    """A private call to our ID: played, shown as to us, and the reply window points back at the caller."""
    _on_dmr_channel(radio, pytestconfig)
    own = int(radio.view(["dmr"])["dmr.own_id"])
    if own == 0:
        pytest.skip("no DMR ID in the codeplug")
    d, v = _bench_call(radio, sdr_bench, pytestconfig, dst=own, private=True)
    assert d["call.rx_calls"] == 1 and d["call.rx_private_calls"] == 1 and d["call.rx_calls_played"] == 1, d
    assert v["last.src"] == str(BENCH_SRC) and v["last.to_us"] == "1" and v["last.group"] == "0", v
    # The reply window (codeplug private hang, else 10 s) is likely still open one second later
    if v.get("dmr.reply_active") == "1":
        assert v["dmr.target_id"] == str(BENCH_SRC) and v["dmr.target_private"] == "1", v


@pytest.mark.rf
@pytest.mark.bench_service
def test_private_call_to_other(radio, sdr_bench, pytestconfig):
    """A private call between two other radios: counted (and shown while it lasts) but never played."""
    _on_dmr_channel(radio, pytestconfig)
    d, _ = _bench_call(radio, sdr_bench, pytestconfig, dst=1234567, private=True)
    assert d["call.rx_calls"] == 1 and d["call.rx_private_calls"] == 1 and d["call.rx_calls_played"] == 0, d
    assert d["call.rx_voice_bursts"] == 0, d


@pytest.mark.rf
@pytest.mark.bench_service
def test_emergency_call(radio, sdr_bench, pytestconfig):
    """The emergency service option is flagged on the call and counted."""
    _on_dmr_channel(radio, pytestconfig)
    d, v = _bench_call(radio, sdr_bench, pytestconfig, emergency=True)
    assert d["call.rx_calls"] == 1 and d["call.rx_emergency_calls"] == 1, d
    assert v["last.emergency"] == "1", v


@pytest.mark.rf
@pytest.mark.bench_service
def test_talker_alias(radio, sdr_bench, pytestconfig):
    """A talker alias in the odd superframes (ISO 8-bit) is assembled and shown."""
    _on_dmr_channel(radio, pytestconfig)
    if "DMR_TALKER_ALIAS" not in radio.features():
        pytest.skip("built without CONFIG_DMR_TALKER_ALIAS")
    alias = "ODR HIL N0CALL"
    d, v = _bench_call(radio, sdr_bench, pytestconfig, talker_alias=alias, superframes=10)
    assert d["call.rx_calls"] == 1 and d["call.rx_alias_complete"] == 1, d
    assert v["call.alias"] == alias and v["call.alias_complete"] == "1", v


@pytest.mark.rf
@pytest.mark.bench_service
def test_late_entry(radio, sdr_bench, pytestconfig):
    """No header and the first 8 of 36 bursts left out: the call is taken from the embedded LC (one call, marked late).
    The radio hears from burst C of the second superframe, but takes voice only from a burst A on, so the first
    embedded LC it can assemble is the third superframe's (bursts 14-17): played from its F (burst 18, 19 bursts) if
    the modem has it in its LC page by F's report, else from the next A (burst 19, 18 bursts). A call with the same LC
    as the one before (the same talker again) can't be told from the modem's sticky LC buffer at first: taken one
    superframe later (burst 24), 13 bursts. HIL 2026-10-10: 7 on aa79493 (the stack waited a superframe more on top of
    the driver's rule), 13 for the new LC on c8c9b23 (the page still held the old LC at F's report).
    The modem's LC trace (dle log lines, lc / lc2 / lt diagnostics) is printed for each late call."""
    _on_dmr_channel(radio, pytestconfig)
    _bench_call(radio, sdr_bench, pytestconfig, src=BENCH_SRC + 1)    # another LC first: the next one is new
    radio.log()                                                        # empty the log ring
    d, v = _bench_call(radio, sdr_bench, pytestconfig, late=8, superframes=6)
    _print_lc_trace(radio)
    assert d["call.rx_calls"] == 1 and d["call.rx_late_entries"] == 1 and d["call.rx_calls_played"] == 1, d
    assert 18 <= d["call.rx_voice_bursts"] <= 20, d
    assert v["call.late_entry"] == "1" and v["last.src"] == str(BENCH_SRC), v
    radio.log()
    d, v = _bench_call(radio, sdr_bench, pytestconfig, late=8, superframes=6)    # the same LC again
    _print_lc_trace(radio)
    assert d["call.rx_calls"] == 1 and d["call.rx_late_entries"] == 1 and d["call.rx_calls_played"] == 1, d
    assert 12 <= d["call.rx_voice_bursts"] <= 14, d
    assert v["last.src"] == str(BENCH_SRC), v


@pytest.mark.rf
@pytest.mark.bench_service
def test_privacy_service_option(radio, sdr_bench, pytestconfig):
    """A call with the privacy service option: without a key every burst is dropped (never played); with a basic
    privacy key loaded the bursts go through the privacy hook to the vocoder. The bench doesn't scramble, so the
    audio itself is noise either way: this checks the path, not the cipher (dmrCallTest does that)."""
    _on_dmr_channel(radio, pytestconfig)
    if "DMR_PRIVACY" not in radio.features():
        pytest.skip("built without CONFIG_DMR_PRIVACY")
    radio.link.dmr_privacy("off")
    d, v = _bench_call(radio, sdr_bench, pytestconfig, service_options=0x40)
    assert d["call.rx_calls"] == 1 and d["call.rx_encrypted_calls"] == 1, d
    assert d["call.rx_voice_bursts"] == 0 and d["call.rx_enc_dropped"] >= 20, d
    assert v["last.encrypted"] == "1", v
    try:
        radio.link.dmr_privacy("moto", 1)
        radio.wait_for(lambda x: x.get("dmr.privacy") == "1", sections=("dmr",), what="privacy key loaded")
        d, v = _bench_call(radio, sdr_bench, pytestconfig, service_options=0x40)
        assert d["call.rx_calls"] == 1 and d["call.rx_enc_dropped"] == 0 and d["call.rx_voice_bursts"] >= 22, d
    finally:
        radio.link.dmr_privacy("off")


@pytest.mark.rf
@pytest.mark.bench_service
def test_pi_bit(radio, sdr_bench, pytestconfig):
    """The EMB PI bit on B-F. The HR_C6000 reports PI unreliably for voice, so its driver clears it (gotcha 6.4):
    on the DM-1701 the call plays as clear. Recorded, not asserted beyond one call heard."""
    _on_dmr_channel(radio, pytestconfig)
    d, _ = _bench_call(radio, sdr_bench, pytestconfig, pi=True)
    assert d["call.rx_calls"] == 1, d


def test_talk_target_requests(radio, pytestconfig):
    """Radio only (nothing transmits): a typed private ID, a contact and back to the channel's talk target; the
    privacy key request. Every change shows in the dmr view."""
    _on_dmr_channel(radio, pytestconfig)
    L = radio.link
    base = radio.view(["dmr"])
    try:
        L.dmr_target(3105555, private=True)
        v = radio.wait_for(lambda x: x.get("dmr.target_manual") == "1", sections=("dmr",), what="manual target")
        assert v["dmr.target_id"] == "3105555" and v["dmr.target_private"] == "1", v
        L.dmr_target(91, private=False)
        v = radio.wait_for(lambda x: x.get("dmr.target_id") == "91", sections=("dmr",), what="TG 91")
        assert v["dmr.target_private"] == "0" and v["dmr.target_manual"] == "1", v
        L.dmr_target_clear()
        v = radio.wait_for(lambda x: x.get("dmr.target_manual") == "0", sections=("dmr",), what="target cleared")
        assert v["dmr.target_id"] == base["dmr.target_id"], v
        if "DMR_PRIVACY" in radio.features():
            L.dmr_privacy("anytone", 0x1234)
            radio.wait_for(lambda x: x.get("dmr.privacy") == "1", sections=("dmr",), what="privacy on")
            L.dmr_privacy("off")
            radio.wait_for(lambda x: x.get("dmr.privacy") == "0", sections=("dmr",), what="privacy off")
    finally:
        L.dmr_target_clear()
        if "DMR_PRIVACY" in radio.features():
            L.dmr_privacy("off")
