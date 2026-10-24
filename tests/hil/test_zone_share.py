# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Zone sharing (docs/spec/zone-share.md): the `zs.*` keys of the air view section, and one channel sent from the UI
heard on air as a zone share fragment.

The send test drives the UI with injected keys: Zones, # on the current zone (its channels), # on the focused channel,
2 (Send channel to...), OK on the first contact. A talkgroup contact gets an unconfirmed packet, a private one a
confirmed packet the bench doesn't answer; either way the first try is on air. The pass criteria are the radio's
counters (one send taken and ended) and the bench hearing a UDP datagram from the radio to port 4100 whose payload is
a fragment of a valid record header.

Opt-in RF: radio_tx + bench_service + bench_modem (--allow-rf), a DMR channel on --dmr-freq in the current zone and at
least one contact in the codeplug. Import is not tested on air: the bench doesn't send zones."""
import pytest

from rfoundry_hil import skip
import dmr_packet
from test_dmr_ip import _setup

ZS_PORT = 4100


def _zs(radio):
    v = radio.view(["air"])
    if v.get("zs.built") != "1":
        skip("not_built", "built without CONFIG_ZONE_SHARE (zs.built=0) or before the zs keys")
    return v


def _screen(radio, name):
    radio.wait_for(lambda v: v.get("ui.screen") == name, sections=("link",), what=f"screen {name}")


@pytest.mark.feature("zone_share")
def test_zone_share_view(radio):
    """The keys are there and consistent: offers Off or Ask, a send state, counters that add up."""
    v = _zs(radio)
    assert v["zs.offers"] in ("0", "1")
    assert v["zs.send_state"] in ("0", "1", "2", "3")
    assert int(v["zs.sent"]) + int(v["zs.send_failed"]) <= int(v["zs.sends"])
    assert int(v["zs.offered"]) + int(v["zs.rx_bad"]) <= int(v["zs.rx_messages"])


@pytest.mark.feature("zone_share")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_send_channel_heard(radio, sdr_bench, pytestconfig):
    before = _zs(radio)
    freq, cc, own, _ = _setup(radio, pytestconfig)
    with sdr_bench.lease():
        lis = sdr_bench.listen_start(freq, cc, seconds=25)
        radio.press("ok")
        radio.press("4")
        _screen(radio, "zones")
        radio.press("hash")
        _screen(radio, "zone")
        radio.press("hash")
        _screen(radio, "zone_share")
        radio.press("2")
        _screen(radio, "zone_share_to")
        radio.press("ok")
        v = radio.wait_for(lambda x: int(x.get("zs.sends", 0)) > int(before["zs.sends"]) and
                           x.get("zs.send_state") in ("2", "3"), timeout=20, sections=("air",),
                           what="the send ended")
        heard = sdr_bench.listen_result(lis)
    radio.go_home()
    print("zs after:", {k: v[k] for k in v if k.startswith("zs.")})
    ours = [p for p in dmr_packet.packets(heard) if p.get("src") == own and p.get("dst_port") == ZS_PORT]
    print("bench heard:", [{k: p.get(k) for k in ("dpf", "group", "dst", "complete", "crc_ok")} for p in ours])
    if not ours:
        pytest.fail("the bench heard no datagram to port 4100 from the radio")
    payload = bytes.fromhex(ours[0]["payload"])
    # Fragment 1 of 1 (one channel fits one message): version 1, type 1, index 0 count 1, offset 0, then the record,
    # which starts with version 1 and its length (the rest of the payload)
    assert payload[0] == 1 and payload[1] == 1 and payload[3] == 0x01 and payload[4:6] == b"\0\0", payload.hex()
    record = payload[6:]
    assert record[0] == 1 and int.from_bytes(record[1:3], "big") == len(record), record.hex()
    assert record[7] & 0x01, "a one-channel record has the channel flag"
