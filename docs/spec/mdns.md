<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# mDNS and DNS-SD on the USB network link

In USB network mode ([usb-network.md](usb-network.md)), the radio answers Multicast DNS for its own name and
advertises its services. A host finds it as `rfoundry-<DMR ID>.local` and browses its services (debug link, MMDVM
modem, TFTP, SNMP, HTTP, KISS, rigctld: section 2) with Bonjour or Avahi.

| Part | Where |
|---|---|
| Switch | `CONFIG_MDNS` (= `CONFIG_DMR_IP_GATEWAY`, the IP stack it runs on) |
| Responder (no RTOS) | `core/net/mdns.c` |
| Glue to the gateway | `core/net/net_services.c` |
| Standards | RFC 6762, RFC 6763. IPv4 only: the link has no IPv6 |

## 1. Names and records

| Name | Value |
|---|---|
| Host | `rfoundry-<DMR ID>.local`, for example `rfoundry-9991234.local`. The DMR ID is the radio's own (the one the gateway's addresses come from) |
| Instance | `<callsign> <board> (<DMR ID>)`, for example `Q0TST XR-100UV (9991234)`. `<board>` is `board.name`. The call sign is the settings' radio name, otherwise the codeplug's, first word only; left out when there is none |
| After a conflict | host `rfoundry-9991234-2`, instance `... (9991234) (2)`, counting up |

| Owner | Type | Data | TTL | Kind |
|---|---|---|---|---|
| host | A | the radio's address (`ipGatewayRadioIP`) | 120 s | unique |
| host | NSEC | next name = host, types: A | 120 s | unique |
| `<type>.local` | PTR | `<instance>.<type>.local` | 4500 s | shared |
| `<instance>.<type>.local` | SRV | priority 0, weight 0, the port, target host | 120 s | unique |
| `<instance>.<type>.local` | TXT | `board=<board.name>` `id=9991234` `call=Q0TST` `fw=<git>` | 4500 s | unique |
| `<instance>.<type>.local` | NSEC | next name = itself, types: TXT, SRV | 120 s | unique |
| `_services._dns-sd._udp.local` | PTR | `<type>.local` | 4500 s | shared |

Unique records go out with the cache-flush bit, except in legacy unicast answers and in probes. Names are compressed
in owner names and in PTR and SRV targets, not in NSEC (RFC 6762 18.14).

## 2. Services

`core/net/net_services.c` holds one table, one line per service. A service is built in when its feature is:

| Type | Port | When |
|---|---|---|
| `_rfoundry-debug._udp` | 3334 | `CONFIG_DEBUGLINK` (the serial protocols over UDP, `usb-network.md` 1) |
| `_mmdvm._udp` | 3334 | `CONFIG_HOTSPOT` |
| `_tftp._udp` | 69 | `CONFIG_TFTP` (`tftp.md`) |
| `_snmp._udp` | 161 | `CONFIG_SNMP` (`snmp.md`) |
| `_http._tcp` | 80 | `CONFIG_NET_HTTP` (`tcp-services.md` 5) |
| `_kiss-tnc._tcp` | 8001 | `CONFIG_NET_KISS` (`tcp-services.md` 4) |
| `_rigctld._tcp` | 4532 | `CONFIG_NET_RIGCTL` (`tcp-services.md` 6) |

A new service adds its line there. The table holds at most `MDNS_SERVICES_MAX` (8) services; 7 are in use with
everything on. Every instance has the same TXT record.

## 3. Behavior

| When | What |
|---|---|
| Link up with an address (a DMR ID) | Probe after 0-250 ms: three rounds 250 ms apart, one message per name (the host, each instance): question `ANY <name>` with the unicast-response bit, the proposed records (A; SRV and TXT) in the authority section |
| No objection 250 ms after the third round | Announce: every record, one message per name (the host's A and NSEC; a service's PTR, SRV, TXT, enumeration PTR and NSEC), twice, 1 s apart |
| The first mDNS message from the host after the link came up | Announce again (two rounds): the host's responder may have started after the first ones, typically before its DHCP lease |
| A query for one of the radio's names | Answer: A for the host; PTR, plus SRV, TXT and A as additional records, for a service type; SRV (plus A) and TXT for an instance; every service type for `_services._dns-sd._udp.local`. `ANY` gets all. A type the radio doesn't have (AAAA) gets no answer and the NSEC listing the types it has as an additional record |
| Known answers in the query | A PTR the querier holds with at least half its TTL left is left out (RFC 6762 7.1). With nothing left, no response |
| Unicast-response bit, or a source port other than 5353 | Answer by unicast to the querier's address and port. Source port not 5353 is legacy unicast: the query ID and question echoed, TTLs at most 10 s, no cache-flush bit |
| Otherwise | Multicast to 224.0.0.251:5353, ID 0, no questions; each record at most once a second (250 ms when the query is a probe for one of the radio's names) |
| A response from another host with one of the radio's unique records and other data (A, SRV, TXT) | Conflict: rename (section 1), probe again |
| While probing, another host's probe for one of the radio's names | Tie-break on the first record of each set (RFC 6762 8.2: class, type, data byte by byte): A for the host, TXT for an instance. The other host's greater: the radio waits 1 s and probes again. The radio's greater or equal: it continues |
| The identity changes (address, DMR ID, call sign) | Goodbye (every record but the NSECs with TTL 0) for the old names, then probe the new ones |
| Link down, or no address | Silent; probe again when it comes back |

The responder leaves out these parts of RFC 6762:

- The 20-120 ms delay for shared records (section 6): a point-to-point USB link has no other responder to collide with.
- The reverse-mapping PTR (`in-addr.arpa`).
- Continuation of truncated known-answer lists.
- The QU "multicast if not sent recently" refinement: QU answers always go unicast.

A message with an opcode or rcode other than 0 is ignored.

Messages are at most `IPGW_UDP_PAYLOAD_MAX` (558 bytes, from the gateway's 600-byte frames). A response that doesn't
fit continues in another message. Replies wait in record sets and go out from `mdnsTick`, so a busy USB IN endpoint only
delays them. The identity is read once a second (the call sign comes from storage).

## 4. The gateway and the USB link

- **Local ports** (`ipGateway.h`): datagrams to the radio's address, or to 224.0.0.0/24, on a port in the gateway's
  table (`ipGatewayUdpBind`) go to that service's handler. mDNS binds 5353. 224.0.0.0/24 is link-local
  (RFC 5771): never sent over the air, bound or not. Sources are not checked against the host's lease, so a host
  still on a link-local address is answered too.
- **Sending** (`ipGatewayUdpSend`): from the radio's address with IP TTL 255 (RFC 6762 11), to the host's MAC for
  unicast and to 01:00:5E plus the low 23 bits of the group for multicast.
- **Multicast on the USB link**: nothing filters in the device. The gateway takes every frame from the host whatever
  its destination MAC, so frames to 01:00:5E:00:00:FB reach it, and the NCM function advertises no multicast filters
  (`wNumberMCFilters` 0) and no packet filter capability, so the host has nothing to program in it (TinyUSB accepts
  and ignores a SET_ETHERNET_PACKET_FILTER anyway). What reaches the host depends on its stack: mDNSResponder and
  Avahi join 224.0.0.251 on every multicast-capable interface. `usb_ncm.c` needs no mDNS-specific code.

## 5. Tests

- Host: `tests/host/mdnsTest.c` with the real gateway: probing and announcing (counts, timing, flags, the records),
  queries (A, case-insensitive names, QU, legacy unicast, browse with additional records, known-answer suppression,
  enumeration, NSEC for AAAA, several questions, foreign names), the rate limit, a busy endpoint, a conflict and the
  rename, the tie-break both ways, goodbyes on an identity change, link down and no address, re-announcing when the
  host is first heard, malformed messages (short, pointer loop, label past the end, counts past the end), and the
  gateway's port table (any source, link-local never to the air, unbound ports still to the air, the table full,
  TTL 255 and checksums). `mdnsTest.pcap` is checked by tshark's DNS dissector.
- HIL: `tests/hil/test_net_services.py` (`--allow-usb-switch`): `dns-sd -G v4 rfoundry-<id>.local` resolves to the
  radio's address, `dns-sd -B _rfoundry-debug._udp` finds the instance, and `dns-sd -L` gives port 3334 and the TXT
  record. Where Python zeroconf is installed, the test uses it instead of `dns-sd`. Debug link view keys (section
  `net_svc`): `mdns.state` (0 off, 1 probing, 2 announcing, 3 running) `mdns.name mdns.queries mdns.answered mdns.sent
  mdns.busy mdns.conflicts mdns.bad`.
