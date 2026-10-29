# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""How our back-to-back data packets fare on air (docs/spec/dmr-data.md 12.6): several UDP datagrams to one
destination sent as one chained transmission, as separate transmissions with no gap, and as separate transmissions
with the default 250 ms guard. The bench modem counts which datagrams it heard whole.

A measurement for the open question of #154: receivers lost a packet that came in a new transmission right after
ours ended, and the cause (our re-key, or the receiver's end-of-call state) is not known. Each mode's delivery is
printed so runs can be compared; the pass criteria are the radio's own counters (every datagram sent, chained or
not as asked), since the bench modem next to the radio misses some bursts either way.

Opt-in RF: radio_tx + bench_service + bench_modem (--allow-rf), firmware with CONFIG_DMR_IP_GATEWAY and the debug
link's TX_SPACING operation. The datagrams go in through the debug link's IP_SEND, as the host's would."""
import time
import warnings

import pytest

from rfoundry_hil import skip
import dmr_packet
from test_dmr_ip import BENCH_ID, _ip_view, _ours, _setup, ip_settings  # noqa: F401 (ip_settings: the fixture)

COUNT = 6
SRC_PORT, DST_PORT = 50000, 5000
DEFAULT_GUARD_MS = 250
MODES = {  # id: (guard ms, chain)
    "chained": (DEFAULT_GUARD_MS, True),
    "separate_0ms": (0, False),
    "separate_250ms": (DEFAULT_GUARD_MS, False),
}


def _air(radio):
    return radio.view(["air"])


@pytest.fixture
def tx_spacing(radio):
    """Puts the spacing back as it was (RAM only: a restart does too)."""
    v = _air(radio)
    if "data.tx_guard_ms" not in v:
        skip("not_built", "firmware without the TX spacing keys in the air view section")
    yield
    radio.link.tx_spacing(int(v["data.tx_guard_ms"]), v.get("data.tx_chain") == "1")


@pytest.mark.feature("dmr_tx_spacing")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.parametrize("mode", list(MODES))
def test_back_to_back_delivery(radio, sdr_bench, pytestconfig, ip_settings, tx_spacing, mode):
    """COUNT datagrams queued at once to the bench's ID, unconfirmed (compressed headers, ports outside 4000-4099)."""
    freq, cc, own, _ = _setup(radio, pytestconfig)
    radio.link.ip_settings("auto", "off", 0, [])
    guard, chain = MODES[mode]
    radio.link.tx_spacing(guard, chain)
    radio.wait_for(lambda x: x.get("data.tx_guard_ms") == str(guard) and x.get("data.tx_chain") == str(int(chain)),
                   sections=("air",), what="the spacing applied")
    payloads = [bytes([0x40 + i]) * 24 for i in range(COUNT)]
    before, air0 = _ip_view(radio), _air(radio)
    with sdr_bench.lease():
        lis = sdr_bench.listen_start(freq, cc, seconds=25)
        for p in payloads:
            radio.link.ip_send(BENCH_ID, SRC_PORT, DST_PORT, p)
        after = radio.wait_for(lambda x: int(x.get("ip.sent", 0)) - int(before["ip.sent"]) >= COUNT and
                               x.get("ip.busy") == "0", timeout=20, sections=("dmr_ip",), what="all sent")
        heard = sdr_bench.listen_result(lis)
    air1 = _air(radio)
    chained = int(air1["ip.chained"]) - int(air0["ip.chained"])
    chains = int(air1["ip.chains"]) - int(air0["ip.chains"])
    got = {bytes.fromhex(p["payload"])[:1] for p in _ours(dmr_packet.packets(heard), own)
           if p.get("complete") and p.get("crc_ok") and p.get("dst_port") == DST_PORT}
    delivered = sum(1 for p in payloads if p[:1] in got)
    print(f"tx spacing {mode}: guard {guard} ms, chain {chain}: the bench heard {delivered} of {COUNT} whole; "
          f"{chains} chained transmission(s), {chained} packet(s) chained")
    assert int(after["ip.sent"]) - int(before["ip.sent"]) == COUNT, (before, after)
    if chain:
        # The first goes as soon as it is queued; those queued while it is on air go together after it
        assert chained >= 1, (air0, air1)
    else:
        assert chained == 0 and chains == 0, (air0, air1)
    if delivered < COUNT:
        warnings.warn(f"tx spacing {mode}: the bench heard {delivered} of {COUNT} datagrams whole")
    time.sleep(1.0)
