# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR data services (docs/spec/dmr-data.md): packet data and text messages received, CSBK call alert and radio check
answered, and the radio's own call alert, radio check and text message on air.

Opt-in RF. The bench-to-radio tests (rf + bench_service: --allow-rf --allow-service-stop) send from the SDR bench's
modem with bench_tx.py data / csbk, then listen (--listen-after) for the radio's answer. The radio-to-bench tests
(radio_tx + bench_service) have the radio send through the debug link's DMR_DATA command while the bench modem listens.
The radio's own counters (the dmr_data view section) are the pass criteria; an answer the bench modem did not hear is
reported as a warning only (its TX to RX turnaround can swallow an answer sent within a few frames). Pass criteria
are in README.md.

The bench modem sits a meter from the radio and is overloaded by it even at the radio's lowest power: it misses some
bursts and hears others with bit errors. So what it heard is judged like this (heard_tries, heard_header): only
bursts whose CRC checks after the FEC count (bench_tx.py listen's "crc_ok"); every one of those must carry exactly the
expected content (opcode, FID, addresses: a wrong one fails); when the radio's counters say it sent N tries, at least
one and at most N must be heard, fewer than N is a warning. A single packet (a text message) the bench didn't hear is
sent again, up to 3 times, each send checked on the radio's counters.

LRRP and ARS need a dispatcher's packets and answers; they are covered on the host (dmrDataServiceTest,
dmrDataChainTest), not here."""
import time
import warnings

import pytest

from rfoundry_hil import skip

BENCH_SRC = 9990301
CSBK_CALL_ALERT, CSBK_CALL_ALERT_ACK, CSBK_RADIO_CHECK = 0x1F, 0x20, 0x24
FID_MOTOROLA = 0x10
DT_CSBK, DT_DATA_HEADER = 3, 6


def _setup(radio, pytestconfig, tx=False):
    feats = radio.features()
    if "DMR" not in feats:
        skip("not_built", "built without DMR")
    if radio.data_view().get("data.built") != 1:
        skip("not_built", "built without the DMR data features")
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    if tx:
        radio.lowest_power()
    own = int(radio.view(["dmr"])["dmr.own_id"])
    if own == 0:
        skip("state", "the radio has no DMR ID (dmr.own_id=0)")
    time.sleep(1.0)
    return freq, cc, own, feats


def _need(feats, name):
    if name not in feats:
        skip("not_built", f"built without CONFIG_{name}")


def _deltas(before, after, keys):
    return {k: after.get(k, 0) - before.get(k, 0) for k in keys}


def _ids(info_hex):
    b = bytes.fromhex(info_hex)
    return b[0] & 0x3F, b[1], b[3], int.from_bytes(b[4:7], "big"), int.from_bytes(b[7:10], "big")


def _heard_csbks(lines, opcode):
    """(octet 3, octets 4-6, octets 7-9) of every CSBK with this opcode the bench modem heard."""
    out = []
    for r in lines:
        if r.get("dt") == DT_CSBK and r.get("info") and r.get("crc_ok", True):
            op, _fid, o3, a, b = _ids(r["info"])
            if op == opcode:
                out.append((o3, a, b))
    return out


def _heard_headers(lines):
    """(destination, source) of every data header heard (ETSI header octets 2-4, 5-7)."""
    out = []
    for r in lines:
        if r.get("dt") == DT_DATA_HEADER and r.get("info") and r.get("crc_ok", True):
            b = bytes.fromhex(r["info"])
            out.append((int.from_bytes(b[2:5], "big"), int.from_bytes(b[5:8], "big")))
    return out


def _soft(cond, what):
    if not cond:
        warnings.warn(f"not heard by the bench modem: {what}")


def _crc_good(r):
    """A burst whose CRC checks after the FEC (older bench tools without "crc_ok": taken as heard)."""
    return r.get("crc_ok", True)


def heard_tries(heard, opcode, sent, match, what):
    """The CSBKs with this opcode the bench modem heard with a good CRC. Each must satisfy match((opcode, fid, octet 3,
    octets 4-6, octets 7-9)); the radio counted `sent` tries, and 1 to `sent` of them must be heard (fewer than `sent`
    only warns: the bench modem, overloaded by the radio next to it, misses bursts). Returns the records heard."""
    csbks = [r for r in heard if r.get("dt") == DT_CSBK and r.get("info") and _ids(r["info"])[0] == opcode]
    good = [r for r in csbks if _crc_good(r)]
    bad_crc = len(csbks) - len(good)
    wrong = [r for r in good if not match(_ids(r["info"]))]
    print(f"{what}: radio sent {sent}, bench heard {len(good)} with a good CRC ({bad_crc} more with a bad one)")
    assert not wrong, f"{what}: wrong content heard: {[r['info'] for r in wrong]}"
    assert 1 <= len(good) <= sent, f"{what}: {len(good)} of {sent} tries heard ({bad_crc} with a bad CRC): {heard}"
    if len(good) < sent:
        warnings.warn(f"{what}: the bench modem heard {len(good)} of the {sent} tries the radio sent "
                      f"({bad_crc} more with CRC errors)")
    return good


def heard_headers(heard, own):
    """Data headers from us (octets 5-7) the bench heard with a good CRC: the raw 12 octets of each."""
    out = []
    for r in heard:
        if r.get("dt") == DT_DATA_HEADER and r.get("info") and _crc_good(r):
            b = bytes.fromhex(r["info"])
            if int.from_bytes(b[5:8], "big") == own:
                out.append(b)
    return out


def send_until_heard(radio, sdr_bench, freq, cc, send, done, check, match, what, seconds=10, tries=3):
    """A single-packet send (no retries of its own): send(), wait for done(view), check(view) asserts on the radio's
    counters; repeated up to `tries` times until the bench hears a data header from us with a good CRC. Every such
    header must satisfy match(octets) (a wrong one fails at once). Returns the matching headers heard."""
    own = int(radio.view(["dmr"])["dmr.own_id"])
    for attempt in range(1, tries + 1):
        before = radio.data_view()
        heard, _ = _radio_sends(radio, sdr_bench, freq, cc, send, seconds, done)
        check(before, radio.data_view())
        hdrs = heard_headers(heard, own)
        wrong = [h.hex() for h in hdrs if not match(h)]
        assert not wrong, f"{what}: a data header from us with the wrong content: {wrong}"
        if hdrs:
            if attempt > 1:
                warnings.warn(f"{what}: heard by the bench modem on send {attempt} of {tries}")
            return hdrs
        print(f"{what}: send {attempt}: the bench modem heard no data header from us with a good CRC")
    raise AssertionError(f"{what}: the bench modem heard none of {tries} sends (the radio counted each as sent)")


@pytest.fixture
def data_settings(radio):
    """Puts the data service's RAM settings back after the test."""
    v = radio.data_view()
    x = radio.view(["dmr_ext"])  # before the test: read after it, these would be the test's own values
    yield
    if v.get("data.built") == 1:
        radio.link.data_settings(v["data.ars_server"], v["data.radio_check_reply"] == 1, v["data.call_alert_ack"] == 1,
                                 v["data.lrrp_reply"] == 1, radio_check_etsi=x.get("data.radio_check_etsi") == "1",
                                 sms_format=x.get("data.sms_format", "etsi_ip"))


@pytest.mark.feature("dmr_data_rx")
def test_data_view_and_settings(radio, pytestconfig, data_settings):
    """Radio only: the dmr_data section is there, and the settings request changes what it shows."""
    _setup(radio, pytestconfig)
    radio.link.data_settings(0, radio_check_reply=True, call_alert_ack=False, lrrp_reply=False)
    # Wait for every value the asserts read: an earlier test can leave the reply flags as they are asked for here, and
    # then they alone say nothing about whether the request has been applied (ARS reads "off" only once it has)
    radio.wait_for(lambda v: v.get("data.radio_check_reply") == "1" and v.get("data.call_alert_ack") == "0" and
                   v.get("data.lrrp_reply") == "0" and v.get("data.ars_server") == "0" and
                   v.get("data.ars_state") == "off", sections=("dmr_data",), what="settings applied")
    v = radio.data_view()
    assert v["data.lrrp_reply"] == 0 and v["data.ars_server"] == 0 and v["data.ars_state"] == "off", v


@pytest.mark.feature("dmr_data_rx")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_unconfirmed_data_rx(radio, sdr_bench, pytestconfig):
    """40 octets of unconfirmed data to our ID: one header, the blocks, one packet for us, no CRC errors."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.reset_counters()
    before = radio.data_view()
    with sdr_bench.lease():
        sdr_bench.data(freq, cc, own, octets=40, src=BENCH_SRC)
    time.sleep(1.0)
    d = _deltas(before, radio.data_view(), ["data.rx_headers", "data.rx_blocks", "data.rx_packets",
                                            "data.rx_for_us", "data.rx_crc_errors", "data.rx_blocks_stale"])
    print("radio deltas:", d)
    assert d["data.rx_headers"] == 1 and d["data.rx_packets"] == 1 and d["data.rx_for_us"] == 1, d
    assert d["data.rx_blocks"] >= 4 and d["data.rx_crc_errors"] == 0 and d["data.rx_blocks_stale"] == 0, d


@pytest.mark.feature("dmr_confirmed")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_confirmed_data_response(radio, sdr_bench, pytestconfig):
    """Confirmed data to our ID: an ETSI response (ACK) goes back to the sender."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    # A sender ID of its own per run: the bench sends the same N(S) every time, and the same packet from one sender within
    # 20 s is a retry the radio ACKs again without counting it (ip.rx_duplicates)
    src = BENCH_SRC + 100 + int(time.time()) % 100
    before = radio.data_view()
    with sdr_bench.lease():
        heard = sdr_bench.data(freq, cc, own, octets=40, confirmed=True, src=src, listen_after=3)
    time.sleep(0.5)
    after = radio.data_view()
    d = _deltas(before, after, ["data.rx_packets", "data.rx_confirmed", "data.responses_sent", "data.tx_ok",
                                "data.tx_failed"])
    print("radio deltas:", d)
    assert d["data.rx_confirmed"] == 1 and d["data.responses_sent"] == 1, d
    assert d["data.tx_ok"] >= 1 and d["data.tx_failed"] == 0, d
    _soft((src, own) in _heard_headers(heard), "the response header to the bench")


@pytest.mark.feature("dmr_tms")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_sms_rx(radio, sdr_bench, pytestconfig):
    """A TMS text message to our ID with an ACK requested: kept in the inbox (unread, the bench as sender) and the
    TMS ACK sent back."""
    freq, cc, own, feats = _setup(radio, pytestconfig)
    _need(feats, "DMR_SMS")
    text = f"RadioFoundry HIL {int(time.time()) % 10000}"
    before = radio.data_view()
    with sdr_bench.lease():
        heard = sdr_bench.data(freq, cc, own, tms=text, tms_seq=(int(time.time()) % 31) + 1, src=BENCH_SRC,
                               listen_after=3)
    time.sleep(0.5)
    after = radio.data_view()
    d = _deltas(before, after, ["data.sms_received", "data.tms_acks_sent", "data.tx_ok", "data.tx_failed"])
    print("radio deltas:", d, "event:", after["data.event"], after["data.event_id"])
    assert d["data.sms_received"] == 1 and d["data.tms_acks_sent"] == 1 and d["data.tx_failed"] == 0, d
    # The ACK was the radio's own transmission: named in the data view, and not one that lights the screen
    if "data.tx_last_kind" in after:
        assert after["data.tx_last_kind"] == "reply" and after["data.tx_last_ms"] != before["data.tx_last_ms"], after
        assert radio.view(["core"]).get("tx_auto") == "1"
    m = radio.link.inbox(0)
    assert m is not None and m["text"] == text and m["src"] == BENCH_SRC and m["unread"], m
    radio.link.mark_read(0)
    assert radio.link.inbox(0)["unread"] is False
    _soft((BENCH_SRC, own) in _heard_headers(heard), "the TMS ACK packet's header to the bench")


@pytest.mark.feature("dmr_signaling")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_call_alert_rx(radio, sdr_bench, pytestconfig, data_settings):
    """A call alert CSBK to our ID: the user is alerted (event call_alert from the bench) and, with the ack setting
    on, a call alert ack (opcode 0x20, octets 4-6 the caller, 7-9 us) goes back."""
    freq, cc, own, feats = _setup(radio, pytestconfig)
    _need(feats, "DMR_SIGNALING")
    radio.link.data_settings(0, radio_check_reply=False, call_alert_ack=True)
    before = radio.data_view()
    with sdr_bench.lease():
        heard = sdr_bench.csbk(freq, cc, own, opcode=CSBK_CALL_ALERT, fid=0, src=BENCH_SRC, listen_after=3)
    time.sleep(0.5)
    after = radio.data_view()
    d = _deltas(before, after, ["data.call_alerts_received", "data.csbk_answers_sent", "data.tx_ok"])
    print("radio deltas:", d)
    assert d["data.call_alerts_received"] == 1 and d["data.csbk_answers_sent"] == 1 and d["data.tx_ok"] >= 1, d
    assert after["data.event"] in ("call_alert",) and after["data.event_id"] == BENCH_SRC, after
    _soft(any(a == BENCH_SRC and b == own for _o3, a, b in _heard_csbks(heard, CSBK_CALL_ALERT_ACK)),
          "the call alert ack")


@pytest.mark.feature("dmr_signaling")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_radio_check_rx(radio, sdr_bench, pytestconfig, data_settings):
    """A radio check request (opcode 0x24, Motorola FID, octet 3 0x80) to our ID: answered when the setting allows
    (the request echoed with octet 3 cleared), counted but not answered when it doesn't (the default)."""
    freq, cc, own, feats = _setup(radio, pytestconfig)
    _need(feats, "DMR_SIGNALING")
    radio.link.data_settings(0, radio_check_reply=False)
    before = radio.data_view()
    with sdr_bench.lease():
        sdr_bench.csbk(freq, cc, own, opcode=CSBK_RADIO_CHECK, fid=FID_MOTOROLA, octet3=0x80, src=BENCH_SRC)
        time.sleep(0.5)
        mid = radio.data_view()
        d = _deltas(before, mid, ["data.radio_checks_received", "data.csbk_answers_sent"])
        print("reply off:", d)
        assert d["data.radio_checks_received"] == 1 and d["data.csbk_answers_sent"] == 0, d

        radio.link.data_settings(0, radio_check_reply=True)
        heard = sdr_bench.csbk(freq, cc, own, opcode=CSBK_RADIO_CHECK, fid=FID_MOTOROLA, octet3=0x80,
                               src=BENCH_SRC, listen_after=3)
    time.sleep(0.5)
    after = radio.data_view()
    d = _deltas(mid, after, ["data.radio_checks_received", "data.csbk_answers_sent", "data.tx_ok"])
    print("reply on:", d)
    assert d["data.radio_checks_received"] == 1 and d["data.csbk_answers_sent"] == 1 and d["data.tx_ok"] >= 1, d
    assert after["data.event"] == "radio_check_answered" and after["data.event_id"] == BENCH_SRC, after
    _soft(any(o3 == 0 and a == own and b == BENCH_SRC for o3, a, b in _heard_csbks(heard, CSBK_RADIO_CHECK)),
          "the radio check answer")


def _radio_sends(radio, sdr_bench, freq, cc, send, seconds, done):
    """The bench listens while the radio sends (send()), until done(view) or the time is up; returns the JSON lines
    heard and the last data view."""
    with sdr_bench.lease():
        lis = sdr_bench.listen_start(freq, cc, seconds=seconds)
        send()
        v = radio.wait_for(done, timeout=seconds - 1, sections=("dmr_data",), what="request over")
        heard = sdr_bench.listen_result(lis)
    for r in heard:                       # what the bench modem heard, for the failure report
        print("bench heard:", {k: r[k] for k in r if k != "t"})
    return heard, v


@pytest.mark.feature("dmr_signaling")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_call_alert_tx(radio, sdr_bench, pytestconfig):
    """Our call alert to the bench (which doesn't answer): three tries on air (opcode 0x1F, octets 4-6 the bench,
    7-9 us), then failed."""
    freq, cc, own, feats = _setup(radio, pytestconfig, tx=True)
    _need(feats, "DMR_SIGNALING")
    before = radio.data_view()
    heard, _ = _radio_sends(radio, sdr_bench, freq, cc, lambda: radio.link.call_alert(BENCH_SRC), 14,
                            lambda v: v.get("data.signal_state") in ("ok", "failed"))
    after = radio.data_view()
    d = _deltas(before, after, ["data.call_alerts_sent", "data.tx_ok", "data.tx_failed", "data.tx_gave_up"])
    print("radio deltas:", d)
    assert after["data.signal_state"] == "failed" and after["data.signal_kind"] == "call_alert", after
    # tx_ok may include an unrelated send in the window (a registration after the bench restarts): count the alerts
    assert d["data.call_alerts_sent"] == 3 and d["data.tx_ok"] >= 3 and d["data.tx_failed"] == 0, d
    heard_tries(heard, CSBK_CALL_ALERT, d["data.call_alerts_sent"],
                lambda x: x[1] == 0 and x[3] == BENCH_SRC and x[4] == own, "call alert")


@pytest.mark.feature("dmr_signaling")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_radio_check_tx(radio, sdr_bench, pytestconfig):
    """Our radio check of the bench: the request on air (opcode 0x24, Motorola FID, octet 3 0x80), three tries
    without an answer, then failed."""
    freq, cc, own, feats = _setup(radio, pytestconfig, tx=True)
    _need(feats, "DMR_SIGNALING")
    before = radio.data_view()
    heard, _ = _radio_sends(radio, sdr_bench, freq, cc, lambda: radio.link.radio_check(BENCH_SRC), 14,
                            lambda v: v.get("data.signal_state") in ("ok", "failed"))
    after = radio.data_view()
    d = _deltas(before, after, ["data.radio_checks_sent", "data.tx_ok", "data.tx_failed"])
    print("radio deltas:", d)
    assert after["data.signal_state"] == "failed" and after["data.signal_kind"] == "radio_check", after
    assert d["data.radio_checks_sent"] == 3 and d["data.tx_failed"] == 0, d
    heard_tries(heard, CSBK_RADIO_CHECK, d["data.radio_checks_sent"],
                lambda x: x[1] == FID_MOTOROLA and x[2] == 0x80 and x[3] == BENCH_SRC and x[4] == own, "radio check")


@pytest.mark.feature("dmr_tms")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_sms_tx(radio, sdr_bench, pytestconfig):
    """Our text message to the bench, no ACK asked: one packet on air (preambles, a data header to the bench from us,
    the blocks), then sent."""
    freq, cc, own, feats = _setup(radio, pytestconfig, tx=True)
    _need(feats, "DMR_SMS")

    def check(before, after):
        d = _deltas(before, after, ["data.sms_sent", "data.ars_sent", "data.tx_ok", "data.tx_failed"])
        print("radio deltas:", d)
        # an ARS registration the radio retries on its own (no ARS server answers on the bench) is a packet too
        assert after["data.sms_state"] == "ok" and d["data.sms_sent"] == 1 and \
            d["data.tx_ok"] == 1 + d["data.ars_sent"], (after, d)

    send_until_heard(radio, sdr_bench, freq, cc, lambda: radio.link.sms(BENCH_SRC, "RadioFoundry HIL TX", ack=False),
                     lambda v: v.get("data.sms_state") in ("ok", "failed"), check,
                     lambda b: int.from_bytes(b[2:5], "big") == BENCH_SRC, "text message")


# --- ETSI short data and the ETSI radio check form (docs/spec/dmr-data.md 5.2, 6) ---

def _ext(radio):
    out = {}
    for k, v in radio.view(["dmr_ext"]).items():
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


@pytest.mark.feature("dmr_sms")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_etsi_sms_rx(radio, sdr_bench, pytestconfig):
    """An ETSI text message (defined short data, ISO 8859-1) to our ID with a response requested: kept in the inbox
    as ETSI, the ETSI response (ACK, SAP 10) goes back."""
    freq, cc, own, feats = _setup(radio, pytestconfig)
    _need(feats, "DMR_SMS")
    text = f"RadioFoundry ETSI {int(time.time()) % 10000}"
    before, xb = radio.data_view(), _ext(radio)
    with sdr_bench.lease():
        heard = sdr_bench.data(freq, cc, own, etsi=text, src=BENCH_SRC, listen_after=3)
    time.sleep(0.5)
    after, xa = radio.data_view(), _ext(radio)
    d = _deltas(before, after, ["data.sms_received", "data.responses_sent", "data.tms_acks_sent", "data.tx_failed"])
    e = _deltas(xb, xa, ["data.etsi_sms_received"])
    print("radio deltas:", d, e)
    assert d["data.sms_received"] == 1 and e["data.etsi_sms_received"] == 1, (d, e)
    assert d["data.responses_sent"] == 1 and d["data.tms_acks_sent"] == 0 and d["data.tx_failed"] == 0, d
    m = radio.link.inbox(0)
    assert m is not None and m["text"] == text and m["src"] == BENCH_SRC and m["format"] == "etsi", m
    radio.link.mark_read(0)
    _soft((BENCH_SRC, own) in _heard_headers(heard), "the ETSI response header to the bench")


@pytest.mark.feature("dmr_sms")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_etsi_sms_tx(radio, sdr_bench, pytestconfig):
    """Our ETSI text message to the bench, no response asked: a defined short data header (DPF 13, SAP 10) to the
    bench from us, then sent."""
    freq, cc, own, feats = _setup(radio, pytestconfig, tx=True)
    _need(feats, "DMR_SMS")
    xb = [_ext(radio)]

    def check(before, after):
        xa = _ext(radio)
        assert after["data.sms_state"] == "ok" and xa["data.etsi_sms_sent"] - xb[0]["data.etsi_sms_sent"] == 1, \
            (after, xa)
        xb[0] = xa

    send_until_heard(radio, sdr_bench, freq, cc,
                     lambda: radio.link.sms(BENCH_SRC, "RadioFoundry HIL ETSI", ack=False, fmt="etsi"),
                     lambda v: v.get("data.sms_state") in ("ok", "failed"), check,
                     lambda b: (b[0] & 0x0F) == 13 and (b[1] >> 4) == 10 and
                     int.from_bytes(b[2:5], "big") == BENCH_SRC, "ETSI text message")


@pytest.mark.feature("dmr_signaling")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_radio_check_tx_etsi(radio, sdr_bench, pytestconfig, data_settings):
    """Our radio check in the ETSI form: opcode 0x24 with FID 0x00 on air, three tries, then failed."""
    freq, cc, own, feats = _setup(radio, pytestconfig, tx=True)
    _need(feats, "DMR_SIGNALING")
    radio.link.data_settings(0, radio_check_etsi=True)
    radio.wait_for(lambda v: v.get("data.radio_check_etsi") == "1", sections=("dmr_ext",), what="ETSI form on")
    before = radio.data_view()
    heard, _ = _radio_sends(radio, sdr_bench, freq, cc, lambda: radio.link.radio_check(BENCH_SRC), 14,
                            lambda v: v.get("data.signal_state") in ("ok", "failed"))
    after = radio.data_view()
    d = _deltas(before, after, ["data.radio_checks_sent", "data.tx_failed"])
    print("radio deltas:", d)
    assert after["data.signal_state"] == "failed" and d["data.radio_checks_sent"] == 3 and d["data.tx_failed"] == 0, \
        (after, d)
    heard_tries(heard, CSBK_RADIO_CHECK, d["data.radio_checks_sent"],
                lambda x: x[1] == 0 and x[3] == BENCH_SRC and x[4] == own, "ETSI radio check")
