<!-- SPDX-License-Identifier: MIT -->

# SNMP agent

An SNMPv2c agent on the radio, reachable over the USB network link and over the air (SNMP over IP over DMR), with
notifications that double as the radio's event stream. Switch: `CONFIG_SNMP` (on with `CONFIG_DMR_IP_GATEWAY`,
[build-config.md](../build-config.md)). Status: built and host-tested (`tests/host/snmpTest.c`, the loopback check
against independent managers); the USB HIL test is written, not yet run on the radio; RF untested.

Sources: RFC 3416 (operations and PDUs), RFC 3417 (transport over UDP), RFC 1901 (the community-based message),
RFC 3418 (system and snmp groups), RFC 2578 / 2579 / 2580 (SMIv2), ITU-T X.690 (BER).

## 1. Files

| File | What |
|---|---|
| `core/snmp/snmpBer.c` | the BER subset: definite lengths up to 0xFFFF, INTEGER, OCTET STRING, NULL, OID, SEQUENCE and SNMP's application types; OID validation and ordering |
| `core/snmp/snmp.c` | message processing: Get, GetNext, GetBulk, Set, the error rules of RFC 3416 4.2, the community check, SNMPv2-Trap encoding. Pure, no state of its own |
| `core/snmp/snmpMib.c` | the MIB table (sorted instance OIDs, types, access) over a snapshot (`snmpData_t`); SET test and commit into `snmpConfig_t`; event detection, the notification queue, the RF rate limit, trap encoding. Pure |
| `core/snmp/snmp_agent.c` | the firmware glue: fills the snapshot from the radio, the transports, settings, the host link task's poll |
| `core/debuglink/debuglink_snmp.c` | the `snmp` view section and the SNMP debug link command |
| `docs/spec/ODR-MIB.txt` | the enterprise MIB (SMIv2) |
| `tools/odr_snmp.py` | a small manager (get, walk, set, trap listener), standard library only; a second, independent BER implementation |

## 2. Transports

Both use UDP port 161 at an address of the radio; the IP gateway's local port table (`ipGatewayBindUDP`,
[usb-network.md](usb-network.md) 3) hands the datagrams to the agent.

| | USB network | RF |
|---|---|---|
| Address | the radio's link address (the host's 12.<ID> xor 1) | the radio's own 12.<DMR ID> (in another radio's or a gateway's IP over DMR view) |
| Arrives | in the host link task (`hostlink_net_poll` → `ipGatewayEthernetIn`), only from the host's address | in the radio task: `hostlink_net_packet` takes individual SAP 4 (IPv4/UDP) or SAP 3 (compressed UDP) packets for our ID and port 161 before they would go to the host, whether or not the network link is up; copied into a one-request mailbox (at most 256 bytes) |
| Answered | at once, up to 548 bytes, waiting up to 50 ms for the USB IN endpoint | by `snmp_agent_poll`, up to 300 bytes, as an IPv4/UDP packet (SAP 4, rate 1/2, 2 preamble CSBKs) through the gateway's air side, which does the listen before talk; given up after 2 s |
| Limits | GetBulk max-repetitions capped at 32 | capped at 8; at most one answer per second (requests in between are dropped and counted); a second request while one waits is dropped |
| SET | with the write community | refused (noAccess) unless `odrAgentRfWrite` is true |

The RF path transmits only when the data service may (a DMR channel we may transmit on, not receive only, not a
hotspot, the channel clear; [dmr-data.md](dmr-data.md) 8). Answers go to the requester's ID and source port.
Because the radio consumes UDP 161 for its own ID, a host behind the radio can't run its own agent reachable over
the air on that port.

## 3. Messages

- SNMPv2c only (msgVersion 1). Other versions are dropped (`snmpInBadVersions`); SNMPv3 is not built (section 10).
- The community decides the access: the write community reads and writes, the read community reads. Unknown
  communities are dropped (`snmpInBadCommunityNames`) and, with `snmpEnableAuthenTraps` enabled, raise
  authenticationFailure to the host. A SET with the read community answers noAccess (`snmpInBadCommunityUses`).
- The settings keep a 32-bit FNV-1a hash of each community, not its text (the settings record has no room for two
  strings; [settings-store.md](settings-store.md) 3). A community is clear text on the wire anyway; the hash only
  saves space. Read community 0 = "public"; write community 0 = none, SET refused for everyone.
- Get: a value; noSuchInstance when the instance exists but has no value now (odrColorCode on an FM channel,
  odrLastAge before any call); noSuchObject for any other name. Every object is a scalar, so a wrong instance number
  of a real object (sysDescr.1) also answers noSuchObject: the agent doesn't keep the object list apart from the
  instances (flash on the MD-380). Managers treat both the same.
- GetNext: the next instance with a value; endOfMibView named as the request's binding.
- GetBulk: the non-repeaters once, then rounds of the repeaters until max-repetitions, every column at the end of
  the MIB, or the answer is full (whole bindings only; RFC 3416 4.2.3 allows fewer). More than 16 repeaters: one
  round.
- Set: two phases. Every binding is checked first (in this order: access, noCreation for a name that isn't an
  instance (nothing can be created), notWritable, wrongType, wrongEncoding, then the object's own wrongLength / wrongValue); the first failure
  answers with its index and nothing changes. Then all are applied. The answer carries the request's bindings.
- An answer that doesn't fit is tooBig with no bindings; if even that doesn't fit, nothing (`snmpSilentDrops`).
- Malformed messages (bad lengths, indefinite length, a bad OID, trailing bytes) are dropped
  (`snmpInASNParseErrs`). Responses, traps and reports sent to the agent are ignored.

## 4. MIB

`docs/spec/ODR-MIB.txt` defines the enterprise part. The enterprise number is **32473, the IANA number reserved
for documentation (RFC 5612), a placeholder** until the project has its own Private Enterprise Number
(`ODR_SNMP_PEN` in `core/snmp/snmpMib.h`, `PEN` in `tools/odr_snmp.py`, the MIB file).

| Group | Instances |
|---|---|
| system (1.3.6.1.2.1.1) | sysDescr ("OpenDigitalRadio <board> <git>"), sysObjectID (odrRadioProduct, enterprises.32473.1.1), sysUpTime, sysContact and sysLocation (empty, read-only), sysName (the radio name, writable: stored as the radio-name setting, used for the talker alias from the next start), sysServices (68: internet and applications) |
| snmp (1.3.6.1.2.1.11) | snmpInPkts, snmpInBadVersions, snmpInBadCommunityNames, snmpInBadCommunityUses, snmpInASNParseErrs, snmpEnableAuthenTraps (writable), snmpSilentDrops, snmpProxyDrops (0) |
| odrIdentity (.2.1.1) | odrDmrId, odrCallsign, odrBoard, odrFirmware |
| odrPower (.2.1.2) | odrBatteryVoltage (mV), odrBatteryPercent, odrBatteryState |
| odrRadio (.2.1.3) | odrRadioState, odrRssi, odrMode, odrZoneName, odrChannelNumber, odrChannelName, odrRxFrequency, odrTxFrequency, odrTxPowerLevel, odrTxPowerName, odrColorCode, odrTimeslot, odrTalkTarget, odrTalkTargetType (the last four DMR only), odrRole (radio, hotspot, SFR, split TX / RX) |
| odrDmr (.2.1.4) | odrCallState, odrCallSource, odrCallDestination, odrLastSource, odrLastDestination, odrLastGroup, odrLastAge |
| odrData (.2.1.5) | odrDataRxPackets, odrDataRxForUs, odrSmsReceived, odrSmsSent, odrSmsFailed, odrSmsInbox, odrSmsUnread, odrArsState |
| odrNet (.2.1.6) | odrUsbNetwork, odrNetToAir (the gateway's packets to the air), odrNetPacketsUp (air packets to the host); the gateway's other counters are in the debug link's `net.*`. No interfaces group (RFC 2863): it didn't fit the MD-380 |
| odrAgent (.2.1.7) | the agent's settings (section 6, writable) and its notification counters |
| odrEvent (.2.1.8) | the notification objects (accessible-for-notify) |

Enumerations start at 1 as SMIv2 asks (odrBatteryState ok(1), low(2), critical(3), ...). The radio has no
temperature sensor the firmware reads, so there is no temperature object.

## 5. Notifications

SNMPv2-Trap messages (community "public"): sysUpTime.0, snmpTrapOID.0, then the objects.

| Notification | When | Objects | RF |
|---|---|---|---|
| coldStart (SNMPv2-MIB) | once after power-up, at the first check | none | yes |
| authenticationFailure | a request with an unknown community, when snmpEnableAuthenTraps is enabled | none | never |
| odrCallStart | a DMR call starts (received on our slot, played or not, or ours), or another follows straight on | source, destination, group, slot | only with odrAgentRfCallTraps |
| odrCallEnd | it ends | the same and the duration | only with odrAgentRfCallTraps |
| odrSmsReceivedNotification | a text message for us arrives | source, destination (us), text (64 characters at most) | yes |
| odrEmergency | an emergency alarm heard (destination 0), or a call with the emergency service option | source, destination, group | yes, exempt from the rate limit |
| odrArsChange | the ARS state changes | odrEventValue = the new odrArsState | yes |
| odrLowBattery | the battery level goes to low or to critical | odrEventValue = mV | yes |

- Detection: every 100 ms the agent compares a new snapshot with the last (calls by odrCallState and the call's
  IDs, the battery level, the ARS state, the data service's last event). Two data events within 100 ms show as the
  later one only.
- Targets: the host, while the USB network personality is up, at its address port 162 (unless odrAgentUsbTraps
  is false; the debug link can move the port for a test host); and the RF manager (odrAgentRfManager, off by
  default) at its 12.x.y.z address port 162. Events for the host while the link is down are not kept.
- Queue: 4 events; when full the oldest goes (counted in odrAgentTrapsDropped).
- RF rate limit: at most one notification per odrAgentRfTrapInterval (default 60 s); the others are dropped, not
  delayed. Emergency is exempt. One RF datagram (answer or notification) waits for the air side at a time.

## 6. Settings

In the settings store (`snmp_*`, all zero = the defaults). Set with SNMP SET (USB, or RF when allowed) or the debug
link (section 7). There is no menu on the radio yet.

| Setting | Default | MIB object | Debug link |
|---|---|---|---|
| read community | "public" | none (write-only through the debug link) | `snmp read TEXT` |
| write community | none: SET refused | none | `snmp write TEXT` |
| agent off | no | none | flags bit 0x01 |
| answers over RF | yes | odrAgentRfAnswers | flags bit 0x02 (set = off) |
| SET over RF | no | odrAgentRfWrite | flags bit 0x04 |
| notifications to the host | yes | odrAgentUsbTraps | flags bit 0x08 (set = off) |
| authenticationFailure | disabled | snmpEnableAuthenTraps | flags bit 0x10 |
| call notifications over RF | no | odrAgentRfCallTraps | flags bit 0x20 |
| RF manager ID | 0 = none | odrAgentRfManager | `snmp set rf_manager ID` |
| RF notification interval | 60 s (10-3600) | odrAgentRfTrapInterval | `snmp set rf_gap_s S` |

Defaults follow the amateur rules and [principles](../ux/principles.md): nothing transmits unless asked (no RF
manager), management over the air is read-only, no encryption anywhere.

## 7. Debug link

View section 15 `snmp`: `snmp.built snmp.flags snmp.write_set snmp.trap_port` and the transport counters
`snmp.rf_requests rf_answers rf_dropped traps_usb traps_rf traps_dropped`. The protocol counters are the snmp group
(snmpInPkts, snmpInBadCommunityNames, ...) and the other settings odrAgent, both read over SNMP. Command 0x19 SNMP ([debuglink.md](debuglink.md) 3.17); `odr_debug.py snmp status | set FIELD
VALUE | read [TEXT] | write [TEXT] | trap`.

## 8. Resources

Default builds: DM-1701 8.6 KB flash and 3.2 KB RAM (the radio view and settings copies the snapshot is filled from,
the answer buffer, the RF mailboxes); MD-380 about 8 KB with LTO. Per-radio figures and the free space left:
`docs/feature-matrix.md` (the MD-380's default image has about 3 KB free). Off, the image grows by about 0.35 KB flash and 0.1 KB RAM (the gateway's port table
and the air-side check).

## 9. Tests

- Host: `tests/host/snmpTest.c`: BER encodings worked out by hand from X.690 (integers at the sign boundaries,
  Counter32 with a leading zero, long-form lengths, OID subidentifiers to 2^32-1, ordering), a GetRequest built byte
  by byte and its answer checked byte by byte, missing objects and instances, community and version drops,
  malformed messages, a GetNext walk of the whole MIB (each instance once, increasing), GetBulk (rounds, the end of
  the MIB, truncation, the cap), tooBig, SET (access, RF, atomic failure, each error status, flags), the event
  detection, queue, RF rate limit and the encoded traps.
- Host, against independent peers (`snmp_peer_check.sh`): the agent with the real MIB on a loopback UDP socket,
  walked, read and written with `tools/odr_snmp.py`, and with the system's SNMP command line tools when installed
  (get, walk, bulkwalk, set must agree); the MIB file must name every enterprise instance the agent has; two traps
  decoded by `odr_snmp.py traps`.
- HIL (the lead runs it): `python3 -m pytest tests/hil/test_snmp.py -v --allow-usb-switch` with a radio on USB and
  firmware with CONFIG_SNMP. It switches USB to network mode (as `test_usb_network.py`), walks the MIB at the
  radio's link address, compares values with the debug link over UDP 3334, checks the SET rules with a temporary
  write community, sees a wrong community dropped, receives the test notification on UDP 16162, and switches back.
  Nothing is transmitted. By hand: `tools/odr_snmp.py walk <radio IP>`, `tools/odr_snmp.py traps`.

### 9.1 RF test plan (not run yet)

Needs a second station that can send IP over DMR to the radio: another ODR radio in USB network mode (the
"manager radio"), or the SDR bench as a DMR data source. All steps transmit: `--allow-rf`, a dummy load or the
bench's attenuated path, an amateur DMR simplex channel, both radios on the same color code and slot.

1. Read over the air: from the manager radio's host, `odr_snmp.py -t 10 get 12.<target ID> sysName.0 odrRssi.0`.
   Expect an answer within a few seconds; on the target `snmp.rf_requests` and `snmp.rf_answers` go up by one,
   `net.air_sent` by one. The SDR bench's capture shows one data header with SAP 4 and its rate 1/2 blocks.
2. Walk over the air: `odr_snmp.py -t 10 walk 12.<target ID> odr` (GetBulk, 8 repetitions, answers up to 300
   bytes). Every enterprise instance arrives; no answer comes sooner than 1 s after the last.
3. Rate limit: two requests 0.3 s apart: the second is dropped (`snmp.rf_dropped`), the manager's retry gets it.
4. SET refused: `odr_snmp.py -c <write community> set ... odrAgentRfTrapInterval.0 u:30` over the air answers
   noAccess; after `odr_debug.py snmp set flags rf_write` on the target it works; clear the flag again.
5. Answers off: `odr_debug.py snmp set flags rf_off`: requests are counted, nothing is transmitted.
6. RF notifications: `odr_debug.py snmp set rf_manager <manager ID>`; on the manager's host `odr_snmp.py traps`.
   Send the target an SMS: one odrSmsReceivedNotification arrives; a second event within 60 s is dropped
   (`snmp.traps_dropped`); an emergency alarm is not.
7. Busy channel: start a voice call on the slot, then a request: the answer waits for the channel (listen before
   talk) and is dropped after 2 s if it never clears.
8. Not for us: requests to another ID or a talkgroup's 225.x.y.z address are not answered (they go to the host as
   before).

## 10. Not built

- SNMPv3 (USM authNoPriv with HMAC-SHA-96 or the RFC 7860 SHA-2 variants). It needs an engine ID, a persistent
  engineBoots counter (no room left in the settings record), discovery with Report PDUs, timeliness checks and the
  HMAC code; the MD-380 has about 3 KB of code space left (`docs/feature-matrix.md`), so it doesn't fit there. Possible
  once the settings get a second record and flash allows. Privacy (encryption) is never offered over RF.
- A trap community setting (traps use "public"), informs, a configurable trap target other than the host.
- Writing channels, zones or the talk target through SNMP.
- A radio menu for the agent's settings.
- The interfaces group (RFC 2863) and the gateway's other counters as objects (they didn't fit the MD-380).
