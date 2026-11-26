# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The debug link over the air (docs/spec/debuglink.md 7), firmware built with CONFIG_DEBUGLINK_DMRIP.

Over the cable (radio only, nothing transmitted): the dmrip view section and DMRIP STATUS, a key set and back to the
chip UID's, a short detour that changes the squelch and the power, back at its deadline with nothing kept, and RETURN
before the deadline.

Over the air (opt-in: --allow-rf and --dmrip-relay): the radio on the rig's DMR channel (--dmr-freq, --dmr-cc) at its
lowest power, the key learned over the cable, then PING, VERSION and a paged view through an IP gateway on that
channel: a radio in USB network mode or a bench IP gateway (--dmrip-relay route: the host's own route to 12.0.0.0/8;
HOST[:PORT]: a relay that forwards each datagram to the DMR ID in its header). A command the air may not send is
refused, a datagram under another key gets no answer, and the radio's counters agree. Every reply the radio sends is a
short data transmission on that channel."""
import struct
import time

import pytest

from rfoundry_hil import DebugLink, DebugLinkError, skip
from rfoundry_debug import OP_TX_LIMIT, RADIO, ST_REFUSED
from hil_config import rig


def _setup(radio):
    # VERSION's features= lists only some switches: a build without the transport has no dmrip view section
    try:
        v = radio.view(["dmrip"])
    except DebugLinkError:
        v = {}
    if v.get("dmrip.built") != "1":
        skip("not_built", "built without CONFIG_DEBUGLINK_DMRIP")
    return v


def _wait_idle(link, timeout):
    end = time.monotonic() + timeout
    st = link.detour_status()
    while st["phase"] != "idle" and time.monotonic() < end:
        time.sleep(0.25)
        st = link.detour_status()
    return st


@pytest.mark.feature("debuglink_dmrip")
def test_dmrip_view_and_key(radio):
    v = _setup(radio)
    own = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert int(v["dmrip.id"]) == own
    st = radio.link.dmrip_status()
    assert st["port"] == 3335
    if st["key"] != "uid":
        skip("state", f"the radio's key is {st['key']} (set by someone else): left alone")
    radio.link.dmrip_set_key(bytes(range(1, 17)))
    try:
        assert radio.link.dmrip_status()["key"] == "set"
        assert radio.view(["dmrip"])["dmrip.key"] == "set"
    finally:
        radio.link.dmrip_set_key(bytes(16))
    assert radio.link.dmrip_status()["key"] == "uid"


@pytest.mark.feature("debuglink_dmrip")
def test_detour_comes_back(radio):
    _setup(radio)
    radio.go_home()
    before = radio.core()
    if int(before.get("power_count") or 1) < 2:
        skip("not_built", "one power level: nothing to change")
    step = -1 if int(before.get("power") or 0) > 0 else 1
    import rfoundry_debug
    items = rfoundry_debug.detour_steps([f"radio power {step:+d}", "radio squelch 1"])
    assert radio.link.detour_run(3, items) == 3
    time.sleep(1.0)
    during = radio.core()
    assert during["power"] != before["power"], "the detour's power change didn't land"
    st = _wait_idle(radio.link, 15)
    assert st["phase"] == "idle", st
    assert st["failed_item"] == 0 and st["result"] == 0, st
    assert st["last"]["restored"] and st["last"]["why"] == "asked", st
    after = radio.core()
    for k in ("power", "squelch", "temp", "rx_hz", "tx_hz", "zone_index", "zone_pos", "vfo"):
        assert after.get(k) == before.get(k), (k, before.get(k), after.get(k))

    # RETURN before the deadline
    radio.link.detour_run(60, items)
    time.sleep(1.0)
    radio.link.detour_return()
    st = _wait_idle(radio.link, 5)
    assert st["phase"] == "idle" and st["last"]["restored"], st
    assert radio.core().get("power") == before.get("power")


@pytest.mark.feature("debuglink_dmrip")
@pytest.mark.rf
@pytest.mark.radio_tx
def test_dmrip_over_the_air(radio, pytestconfig):
    _setup(radio)
    relay = pytestconfig.getoption("--dmrip-relay")
    if not relay:
        skip("opt_in", "needs --dmrip-relay route|HOST[:PORT]: an IP gateway on the rig's DMR channel")
    freq = rig(pytestconfig, "dmr-freq")
    radio.go_home()
    radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    assert int(radio.core()["tx_hz"]) == round(freq * 1e6)
    v = radio.view(["dmrip"])
    rid = int(v["dmrip.id"])
    if not rid:
        skip("state", "the radio has no DMR ID")
    if v["dmrip.key"] != "uid":
        skip("state", f"the radio's key is {v['dmrip.key']}")
    import rfoundry_debug
    m = rfoundry_debug.dmrip_module()
    key = m.kdf(b"".join(struct.pack("<I", u) for u in radio.link.chip()["uid"]))
    spec = f"dmrip:{rid}" + ("" if relay == "route" else f"@{relay}")
    before = radio.link.dmrip_status()

    with DebugLink(spec, dmrip_key=key.hex()) as air:
        assert air.ping(b"air") is not None
        assert air.version().get("features")
        page = air.view_prefix("core", "rx_hz")
        assert int(page["rx_hz"]) == int(radio.core()["rx_hz"])
        with pytest.raises(DebugLinkError) as e:
            air.request(RADIO, bytes([OP_TX_LIMIT, 0]))   # would stop the replies: the cable only
        assert e.value.status == ST_REFUSED
        with pytest.raises(DebugLinkError) as e:
            air.chip()          # the UID gives the default key: never sent on the air
        assert e.value.status == ST_REFUSED

    with DebugLink(spec, dmrip_key=bytes(range(16)).hex()) as wrong:
        wrong.s.timeout, wrong.s.tries = 3.0, 1
        with pytest.raises(DebugLinkError):
            wrong.ping(b"wrong")

    after = radio.link.dmrip_status()
    assert after["answered"] >= before["answered"] + 4
    assert after["bad_auth"] >= before["bad_auth"] + 1
