#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""A small SNMPv2c manager for the radio's agent (docs/spec/snmp.md), standard library only: get, walk (GetBulk),
set, and a notification listener. Written from RFC 3416 / RFC 1901 and ITU-T X.690, independently of the firmware's
encoder, so the host tests use it as a second implementation.

  odr_snmp.py get 12.0.39.20 sysName.0 odrDmrId.0
  odr_snmp.py walk 12.0.39.20                      # the whole MIB
  odr_snmp.py walk 12.0.39.20 odr                  # the enterprise subtree
  odr_snmp.py -c private set 12.0.39.20 sysName.0 s:N0CALL odrAgentRfManager.0 u:3100001
  odr_snmp.py traps                                # print notifications sent to UDP 162 (or --port)

Names: the few below (sysDescr.0, odrBatteryVoltage.0, ...) or dotted OIDs. Set values: i:INTEGER, u:Unsigned32
(Gauge32), s:string, x:hex octets, o:OID.
"""
import argparse
import random
import socket
import sys

PEN = 32473     # ODR_SNMP_PEN (core/snmp/snmpMib.h): the documentation number until ours is assigned
ODR = f"1.3.6.1.4.1.{PEN}.2.1"

NAMES = {
    "system": "1.3.6.1.2.1.1", "sysDescr": "1.3.6.1.2.1.1.1", "sysObjectID": "1.3.6.1.2.1.1.2",
    "sysUpTime": "1.3.6.1.2.1.1.3", "sysContact": "1.3.6.1.2.1.1.4", "sysName": "1.3.6.1.2.1.1.5",
    "sysLocation": "1.3.6.1.2.1.1.6", "sysServices": "1.3.6.1.2.1.1.7", "interfaces": "1.3.6.1.2.1.2",
    "snmp": "1.3.6.1.2.1.11", "snmpInPkts": "1.3.6.1.2.1.11.1", "snmpInBadCommunityNames": "1.3.6.1.2.1.11.4",
    "snmpInASNParseErrs": "1.3.6.1.2.1.11.6",
    "snmpEnableAuthenTraps": "1.3.6.1.2.1.11.30", "snmpTrapOID": "1.3.6.1.6.3.1.1.4.1",
    "coldStart": "1.3.6.1.6.3.1.1.5.1", "authenticationFailure": "1.3.6.1.6.3.1.1.5.5",
    "odr": f"1.3.6.1.4.1.{PEN}",
    "odrDmrId": f"{ODR}.1.1", "odrCallsign": f"{ODR}.1.2", "odrBoard": f"{ODR}.1.3", "odrFirmware": f"{ODR}.1.4",
    "odrBatteryVoltage": f"{ODR}.2.1", "odrBatteryPercent": f"{ODR}.2.2", "odrBatteryState": f"{ODR}.2.3",
    "odrRadioState": f"{ODR}.3.1", "odrRssi": f"{ODR}.3.2", "odrMode": f"{ODR}.3.3", "odrZoneName": f"{ODR}.3.4",
    "odrChannelNumber": f"{ODR}.3.5", "odrChannelName": f"{ODR}.3.6", "odrRxFrequency": f"{ODR}.3.7",
    "odrTxFrequency": f"{ODR}.3.8", "odrTxPowerLevel": f"{ODR}.3.9", "odrTxPowerName": f"{ODR}.3.10",
    "odrColorCode": f"{ODR}.3.11", "odrTimeslot": f"{ODR}.3.12", "odrTalkTarget": f"{ODR}.3.13",
    "odrTalkTargetType": f"{ODR}.3.14", "odrRole": f"{ODR}.3.15",
    "odrCallState": f"{ODR}.4.1", "odrCallSource": f"{ODR}.4.2", "odrCallDestination": f"{ODR}.4.3",
    "odrLastSource": f"{ODR}.4.4", "odrLastDestination": f"{ODR}.4.5", "odrLastGroup": f"{ODR}.4.6",
    "odrLastAge": f"{ODR}.4.7",
    "odrDataRxPackets": f"{ODR}.5.1", "odrDataRxForUs": f"{ODR}.5.2", "odrSmsReceived": f"{ODR}.5.3",
    "odrSmsSent": f"{ODR}.5.4", "odrSmsFailed": f"{ODR}.5.5", "odrSmsInbox": f"{ODR}.5.6",
    "odrSmsUnread": f"{ODR}.5.7", "odrArsState": f"{ODR}.5.8",
    "odrUsbNetwork": f"{ODR}.6.1", "odrNetToAir": f"{ODR}.6.3", "odrNetPacketsUp": f"{ODR}.6.6",
    "odrAgentRfManager": f"{ODR}.7.1", "odrAgentRfTrapInterval": f"{ODR}.7.2", "odrAgentRfAnswers": f"{ODR}.7.3",
    "odrAgentRfWrite": f"{ODR}.7.4", "odrAgentUsbTraps": f"{ODR}.7.5", "odrAgentRfCallTraps": f"{ODR}.7.6",
    "odrAgentTrapsSent": f"{ODR}.7.7", "odrAgentTrapsDropped": f"{ODR}.7.8",
    "odrEventSource": f"{ODR}.8.1", "odrEventDestination": f"{ODR}.8.2", "odrEventGroup": f"{ODR}.8.3",
    "odrEventSlot": f"{ODR}.8.4", "odrEventDuration": f"{ODR}.8.5", "odrEventText": f"{ODR}.8.6",
    "odrEventValue": f"{ODR}.8.7",
    "odrCallStart": f"1.3.6.1.4.1.{PEN}.2.0.1", "odrCallEnd": f"1.3.6.1.4.1.{PEN}.2.0.2",
    "odrSmsReceivedNotification": f"1.3.6.1.4.1.{PEN}.2.0.3", "odrEmergency": f"1.3.6.1.4.1.{PEN}.2.0.4",
    "odrArsChange": f"1.3.6.1.4.1.{PEN}.2.0.5", "odrLowBattery": f"1.3.6.1.4.1.{PEN}.2.0.6",
}

INTEGER, OCTETS, NULL, OID, SEQUENCE = 0x02, 0x04, 0x05, 0x06, 0x30
IPADDRESS, COUNTER32, GAUGE32, TIMETICKS, COUNTER64 = 0x40, 0x41, 0x42, 0x43, 0x46
NO_SUCH_OBJECT, NO_SUCH_INSTANCE, END_OF_MIB_VIEW = 0x80, 0x81, 0x82
GET, GETNEXT, RESPONSE, SET, GETBULK, TRAP2 = 0xA0, 0xA1, 0xA2, 0xA3, 0xA5, 0xA7
ERRORS = ["noError", "tooBig", "noSuchName", "badValue", "readOnly", "genErr", "noAccess", "wrongType",
          "wrongLength", "wrongEncoding", "wrongValue", "noCreation", "inconsistentValue", "resourceUnavailable",
          "commitFailed", "undoFailed", "authorizationError", "notWritable", "inconsistentName"]


class SnmpError(Exception):
    pass


# ---- BER (X.690) ----

def enc_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def tlv(tag, body):
    return bytes([tag]) + enc_len(len(body)) + body


def enc_int(v, tag=INTEGER):
    n = 1
    while not -(1 << (8 * n - 1)) <= v < (1 << (8 * n - 1)):
        n += 1
    return tlv(tag, v.to_bytes(n, "big", signed=True))


def enc_unsigned(v, tag):
    return tlv(tag, v.to_bytes(max(1, (v.bit_length() + 8) // 8), "big"))


def oid_tuple(name):
    base, _, rest = name.partition(".")
    if base in NAMES:
        name = NAMES[base] + ("." + rest if rest else "")
    return tuple(int(x) for x in name.strip(".").split("."))


def enc_oid_body(oid):
    out = bytearray([40 * oid[0] + oid[1]])
    for sub in oid[2:]:
        chunk = [sub & 0x7F]
        sub >>= 7
        while sub:
            chunk.append(0x80 | (sub & 0x7F))
            sub >>= 7
        out += bytes(reversed(chunk))
    return bytes(out)


def dec_oid_body(b):
    first = b[0]
    oid = [first // 40 if first < 80 else 2, first % 40 if first < 80 else first - 80]
    v = 0
    for x in b[1:]:
        v = (v << 7) | (x & 0x7F)
        if not x & 0x80:
            oid.append(v)
            v = 0
    return tuple(oid)


def dec_tlv(b, i):
    tag = b[i]
    n = b[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7F
        if k == 0:
            raise SnmpError("indefinite length")
        n = int.from_bytes(b[i:i + k], "big")
        i += k
    if i + n > len(b):
        raise SnmpError("truncated")
    return tag, b[i:i + n], i + n


def dec_seq(b):
    out, i = [], 0
    while i < len(b):
        tag, body, i = dec_tlv(b, i)
        out.append((tag, body))
    return out


def dec_value(tag, body):
    if tag == INTEGER:
        return int.from_bytes(body, "big", signed=True)
    if tag in (COUNTER32, GAUGE32, TIMETICKS, COUNTER64):
        return int.from_bytes(body, "big")
    if tag == OID:
        return ".".join(map(str, dec_oid_body(body)))
    if tag == IPADDRESS:
        return ".".join(map(str, body))
    if tag == OCTETS:
        return bytes(body)
    return {NULL: None, NO_SUCH_OBJECT: "noSuchObject", NO_SUCH_INSTANCE: "noSuchInstance",
            END_OF_MIB_VIEW: "endOfMibView"}.get(tag, bytes(body))


def message(community, pdu_tag, request_id, f2, f3, bindings):
    vbl = b"".join(tlv(SEQUENCE, tlv(OID, enc_oid_body(o)) + v) for o, v in bindings)
    pdu = tlv(pdu_tag, enc_int(request_id) + enc_int(f2) + enc_int(f3) + tlv(SEQUENCE, vbl))
    return tlv(SEQUENCE, enc_int(1) + tlv(OCTETS, community.encode()) + pdu)


def parse_message(data):
    """(pdu tag, request id, error status, error index, [(oid tuple, tag, value)]) of a v2c message."""
    tag, body, _ = dec_tlv(data, 0)
    if tag != SEQUENCE:
        raise SnmpError("not a sequence")
    parts = dec_seq(body)
    if len(parts) != 3 or dec_value(*parts[0]) != 1:
        raise SnmpError("not SNMPv2c")
    pdu_tag, pdu = parts[2]
    fields = dec_seq(pdu)
    bindings = []
    for _, vb in dec_seq(fields[3][1]):
        (otag, obody), (vtag, vbody) = dec_seq(vb)
        bindings.append((dec_oid_body(obody), vtag, dec_value(vtag, vbody)))
    return pdu_tag, dec_value(*fields[0]), dec_value(*fields[1]), dec_value(*fields[2]), bindings


class Manager:
    def __init__(self, host, community="public", port=161, timeout=2.0, retries=2):
        self.addr = (host, port)
        self.community = community
        self.timeout = timeout
        self.retries = retries
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)

    def close(self):
        self.sock.close()

    def request(self, pdu_tag, bindings, f2=0, f3=0):
        rid = random.randint(1, 0x7FFFFFFF)
        msg = message(self.community, pdu_tag, rid, f2, f3, bindings)
        for _ in range(self.retries + 1):
            self.sock.sendto(msg, self.addr)
            try:
                while True:
                    data, _ = self.sock.recvfrom(2048)
                    tag, got, status, index, vbs = parse_message(data)
                    if tag == RESPONSE and got == rid:
                        if status:
                            name = ERRORS[status] if status < len(ERRORS) else str(status)
                            raise SnmpError(f"{name} (index {index})")
                        return vbs
            except socket.timeout:
                continue
        raise SnmpError(f"no answer from {self.addr[0]}:{self.addr[1]}")

    def get(self, *names):
        return self.request(GET, [(oid_tuple(n), tlv(NULL, b"")) for n in names])

    def walk(self, root="1.3", max_rep=10):
        root_t = oid_tuple(root)
        cur = root_t
        out = []
        while True:
            vbs = self.request(GETBULK, [(cur, tlv(NULL, b""))], 0, max_rep)
            if not vbs:
                return out
            for oid, tag, value in vbs:
                if tag == END_OF_MIB_VIEW or oid[:len(root_t)] != root_t:
                    return out
                if oid <= cur:
                    raise SnmpError(f"not increasing: {oid} after {cur}")
                out.append((oid, tag, value))
                cur = oid

    def set(self, *pairs):
        """pairs: (name, 'i:5' / 'u:5' / 's:text' / 'x:0a0b' / 'o:1.3.6')."""
        return self.request(SET, [(oid_tuple(n), encode_setting(v)) for n, v in pairs])


def encode_setting(v):
    kind, _, text = v.partition(":")
    if kind == "i":
        return enc_int(int(text, 0))
    if kind == "u":
        return enc_unsigned(int(text, 0), GAUGE32)
    if kind == "s":
        return tlv(OCTETS, text.encode())
    if kind == "x":
        return tlv(OCTETS, bytes.fromhex(text))
    if kind == "o":
        return tlv(OID, enc_oid_body(oid_tuple(text)))
    raise SnmpError(f"value {v!r}: i:, u:, s:, x: or o:")


def name_of(oid):
    best = None
    for name, value in NAMES.items():
        t = oid_tuple(value)
        if oid[:len(t)] == t and (best is None or len(t) > len(best[1])):
            best = (name, t)
    if best is None:
        return ".".join(map(str, oid))
    rest = oid[len(best[1]):]
    return best[0] + ("." + ".".join(map(str, rest)) if rest else "")


def show(oid, tag, value):
    if isinstance(value, bytes):
        value = value.decode("latin-1") if all(32 <= c < 127 for c in value) else value.hex(":")
    return f"{name_of(oid)} = {value}"


def listen(port, count=0, timeout=None):
    """Notifications to UDP port: an iterator of (source address, [(oid, tag, value)]). The port is bound before
    this returns, so nothing sent afterwards is missed; socket.timeout when none comes within timeout."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", port))
    s.settimeout(timeout)
    return _notifications(s, count)


def _notifications(s, count):
    seen = 0
    try:
        while count == 0 or seen < count:
            data, addr = s.recvfrom(2048)
            try:
                tag, _, _, _, vbs = parse_message(data)
            except (SnmpError, IndexError, ValueError):
                continue
            if tag == TRAP2:
                seen += 1
                yield addr, vbs
    finally:
        s.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-c", "--community", default="public")
    p.add_argument("-p", "--port", type=int, default=None, help="161 for requests, 162 for traps")
    p.add_argument("-t", "--timeout", type=float, default=2.0)
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("get")
    g.add_argument("host")
    g.add_argument("names", nargs="+")
    w = sub.add_parser("walk")
    w.add_argument("host")
    w.add_argument("root", nargs="?", default="1.3")
    s = sub.add_parser("set")
    s.add_argument("host")
    s.add_argument("pairs", nargs="+", help="NAME VALUE ...")
    t = sub.add_parser("traps")
    t.add_argument("-n", "--count", type=int, default=0)
    a = p.parse_args()
    try:
        if a.cmd == "traps":
            for addr, vbs in listen(a.port or 162, a.count):
                print(f"from {addr[0]}: " + ", ".join(show(*vb) for vb in vbs), flush=True)
            return 0
        m = Manager(a.host, a.community, a.port or 161, a.timeout)
        if a.cmd == "get":
            vbs = m.get(*a.names)
        elif a.cmd == "walk":
            vbs = m.walk(a.root)
        else:
            if len(a.pairs) % 2:
                print("set: NAME VALUE pairs", file=sys.stderr)
                return 2
            vbs = m.set(*zip(a.pairs[::2], a.pairs[1::2]))
        for vb in vbs:
            print(show(*vb))
    except (SnmpError, OSError) as e:
        print(f"odr_snmp: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
