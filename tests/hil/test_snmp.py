# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The SNMP agent over the USB network (opt-in: --allow-usb-switch; firmware built with CONFIG_SNMP): the radio
switches to the network adapter, then tools/odr_snmp.py walks the whole MIB at the radio's link address (UDP 161),
checks values against the debug link, sets an agent setting with a write community and back, sees a SET with the
read community refused and a wrong community ignored, and receives a notification (the debug link's test trap, sent
to a high port so no root is needed on Linux); then USB goes back to serial and the settings back to what they were.

Nothing is transmitted (no request goes over the air). The write community is set to a test value for the run and
cleared afterwards; on a radio that already has one (snmp.write_set=1) the SET checks are skipped, since its text
can't be read back to restore it. docs/spec/snmp.md 9."""
import ipaddress
import os
import sys
import time

import pytest

from odr_hil import DebugLink, DebugLinkError, find_ports

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import odr_snmp  # noqa: E402

INDIVIDUAL_NET = 0x0C000000      # 12.0.0.0/8, IPGW_INDIVIDUAL_NET
TEST_COMMUNITY = "odr-hil-rw"
TRAP_PORT = 16162


def _udp_link(ip, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            link = DebugLink(f"udp:{ip}", timeout=1.0)
            link.ping()
            return link
        except (DebugLinkError, OSError):
            time.sleep(0.5)
    return None


def _serial_back(timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if find_ports():
            return True
        time.sleep(0.5)
    return False


def _value(vbs, name):
    want = odr_snmp.oid_tuple(name)
    for oid, tag, value in vbs:
        if oid == want:
            return value
    raise AssertionError(f"{name} not in the answer")


@pytest.mark.usb_switch
def test_snmp_usb(radio):
    if "SNMP" not in radio.features():
        pytest.skip("built without CONFIG_SNMP")
    own_id = int(radio.view(["dmr"]).get("dmr.own_id", "0"))
    assert 0 < own_id < (1 << 24), "no DMR ID: the gateway can't make an address"
    before = radio.view(["snmp"])
    assert before.get("snmp.built") == "1", before
    if int(before.get("snmp.flags", "0"), 16) & 0x01:
        pytest.skip("the agent is switched off in this radio's settings (snmp.flags)")
    had_write = before.get("snmp.write_set") == "1"
    if not had_write:
        radio.link.snmp_community(True, TEST_COMMUNITY)
    radio.link.snmp_set("trap_port", TRAP_PORT)
    host_ip = ipaddress.IPv4Address(INDIVIDUAL_NET | own_id)
    radio_ip = str(ipaddress.IPv4Address(int(host_ip) ^ 1))
    radio.link.usb_network(True)
    radio.link.close()
    udp = None
    try:
        udp = _udp_link(radio_ip, 30)
        assert udp is not None, f"no debug link over UDP at {radio_ip} within 30 s (DHCP on the host's adapter?)"
        m = odr_snmp.Manager(radio_ip, "public", timeout=2.0)

        # The whole MIB, in order (Manager.walk refuses a non-increasing answer)
        walk = m.walk("1.3")
        names = {odr_snmp.name_of(oid) for oid, _, _ in walk}
        assert len(walk) >= 60, f"only {len(walk)} instances"
        for n in ("sysDescr.0", "sysObjectID.0", "sysUpTime.0", "sysName.0", "odrDmrId.0", "odrBoard.0", "odrBatteryVoltage.0", "odrRxFrequency.0", "odrAgentRfManager.0"):
            assert n in names, f"{n} missing from the walk"
        v = udp.view(["core", "dmr"])
        got = m.get("odrDmrId.0", "odrRxFrequency.0", "sysUpTime.0", "odrUsbNetwork.0")
        assert _value(got, "odrDmrId.0") == own_id
        assert _value(got, "odrRxFrequency.0") == int(v["rx_hz"])
        assert _value(got, "odrUsbNetwork.0") == 1      # TruthValue true
        t1 = _value(got, "sysUpTime.0")
        time.sleep(1.0)
        t2 = _value(m.get("sysUpTime.0"), "sysUpTime.0")
        assert 50 <= t2 - t1 <= 300, (t1, t2)            # hundredths of a second
        descr = _value(m.get("sysDescr.0"), "sysDescr.0").decode()
        assert descr.startswith("OpenDigitalRadio "), descr

        # SET: refused with the read community; with the write community the RF trap interval round trip
        with pytest.raises(odr_snmp.SnmpError, match="noAccess"):
            m.set(("odrAgentRfTrapInterval.0", "u:120"))
        if not had_write:
            w = odr_snmp.Manager(radio_ip, TEST_COMMUNITY, timeout=2.0)
            old = _value(w.get("odrAgentRfTrapInterval.0"), "odrAgentRfTrapInterval.0")
            new = 120 if old != 120 else 90
            w.set(("odrAgentRfTrapInterval.0", f"u:{new}"))
            assert _value(m.get("odrAgentRfTrapInterval.0"), "odrAgentRfTrapInterval.0") == new
            with pytest.raises(odr_snmp.SnmpError, match="wrongValue"):
                w.set(("odrAgentRfTrapInterval.0", "u:5"))
            with pytest.raises(odr_snmp.SnmpError, match="notWritable"):
                w.set(("sysDescr.0", "s:x"))
            w.set(("odrAgentRfTrapInterval.0", f"u:{old}"))
            w.close()

        # A wrong community: no answer, counted
        bad0 = _value(m.get("snmpInBadCommunityNames.0"), "snmpInBadCommunityNames.0")
        b = odr_snmp.Manager(radio_ip, "wrong", timeout=0.5, retries=0)
        with pytest.raises(odr_snmp.SnmpError, match="no answer"):
            b.get("sysName.0")
        b.close()
        assert _value(m.get("snmpInBadCommunityNames.0"), "snmpInBadCommunityNames.0") == bad0 + 1

        # A notification to the host (the test trap: coldStart), from the radio's address
        traps = odr_snmp.listen(TRAP_PORT, count=1, timeout=5.0)
        udp.snmp_test_trap()
        addr, vbs = next(traps)
        assert addr[0] == radio_ip, addr
        assert _value(vbs, "snmpTrapOID.0") == "1.3.6.1.6.3.1.1.5.1", vbs
        traps.close()
        s = udp.view(["snmp"])
        assert int(s["snmp.traps_usb"]) >= 1, s
        assert _value(m.get("snmpInASNParseErrs.0"), "snmpInASNParseErrs.0") == 0
        m.close()
        udp.snmp_set("trap_port", 0)
        if not had_write:
            udp.snmp_community(True, "")
        udp.usb_network(False)
    finally:
        if udp is not None:
            udp.close()
        if not _serial_back(20):
            again = _udp_link(radio_ip, 10)
            if again is not None:
                again.usb_network(False)
                again.close()
            assert _serial_back(20), "the radio didn't come back as a serial port"
        for _ in range(40):
            try:
                radio.link = DebugLink(radio.port, timeout=1.5)
                break
            except DebugLinkError:
                time.sleep(0.25)
    after = radio.view(["snmp"])
    if after.get("snmp.write_set") == "1" and not had_write:
        radio.link.snmp_community(True, "")        # the run ended early: don't leave the test community behind
    radio.link.snmp_set("trap_port", 0)
    assert radio.view(["hotspot"]).get("usb.network") == "0"
