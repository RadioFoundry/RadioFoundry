<!-- SPDX-License-Identifier: MIT -->

# USB network personality and the IP gateway

The radio can be a USB network adapter instead of a serial port, and route IPv4 between the host and other radios
over DMR packet data. Switches: `CONFIG_USB_NETWORK` (the adapter), `CONFIG_DMR_IP_GATEWAY` (the gateway on top of
it) and `CONFIG_DMR_CONTROL_STATION` (section 5), on wherever the radio has USB and DMR (`docs/build-config.md`). On
the link the radio also runs its own services: mDNS (`mdns.md`), a TFTP server for the codeplug (`tftp.md`) and the
TCP services (`tcp-services.md`). The same gateway and services also run over PPP on the serial host port, for radios
without USB (`ppp.md`).

## 1. One personality at a time

CDC-ACM needs two IN endpoints (notification, bulk IN) and CDC-NCM needs two. The USB cores in the radios with USB
have three IN endpoints besides EP0 (the STM32F405's OTG_FS), so a
composite ACM + NCM device does not fit: the radio is either a serial port (`board_usb_ids.pid_serial`) or a network
adapter (`pid_network`), with their own descriptors. Switching drops the D+ pull-up for
100 ms, so the host sees an unplug and enumerates the other personality.

- Boot is serial unless the stored setting `usb_mode` is 1 (Settings > Connections > USB network at start, debug link
  radio op 27, view key `usb.boot_network`): then the personality is chosen before the USB stack starts, and the radio
  enumerates as the adapter straight away. At run time the switch is a request: debug link radio op 25
  (`rfoundry_debug.py radio usb network`) or Tools > USB network, carried out after its reply has gone out; it doesn't
  change the stored setting.
- In network mode the serial protocols stay reachable over UDP: datagrams to the radio's own address, port 3334,
  carry the same byte stream as the serial port (debug link, MMDVM, CPS), and the replies go back to the address and
  port that last sent there, in datagrams of at most 512 bytes. `rfoundry_debug.py --port udp:<radio IP>` uses it, and so
  can MMDVMHost (`Protocol=udp`, `ModemAddress=<radio IP>`, `ModemPort=3334`). The MMDVM session's DTR rule does not
  apply (there is no DTR); the 20 s idle timeout does.
- Windows 10 binds NCM through Microsoft OS 1.0 descriptors (string 0xEE "MSFT100", vendor code 0x47, extended compat
  ID "WINNCM" for interface 0); Windows 11, Linux and macOS bind it by class.
- The link is up while the host has the adapter configured and the bus isn't suspended (`hal_netif_t.up`, view key
  `usb.net_up`). The STM32 radios run without VBUS sensing, so a pulled cable reads only as the bus going idle
  (suspend): the link goes down at once, and comes back with the configuration after the cable is plugged in again.
- macOS sometimes never activates the link after a cold boot in network mode: with the device configured and no frame
  from the host within 5 s, the radio re-enumerates once.
- A host that never configures the network personality at all (seen on macOS 26, Apple Silicon, 2026-10-10: the
  device shows unmatched, no interfaces) would leave no serial port and no debug link to switch back. After a switch
  to network mode, at run time or by the power-on setting, the radio goes back to serial when the host hasn't
  configured it within 10 s of its bus reset (`usb_ncm.c`, `switch_fallbacks`). The time counts only while a host is
  on the bus (`tud_connected`), so a radio switched on away from a computer still comes up as the adapter when it is
  plugged in later. The fallback lasts for that boot only: the setting stays, and the UI (Settings > Connections) or
  the debug link over the serial port it fell back to turns it off.

## 2. Frames

TinyUSB's NCM class (NTB16): OUT NTBs up to 2048 bytes, IN NTBs one datagram each. Frames from the host are copied in
the USB task into a ring of 4 frames of at most 600 bytes (the gateway's limit); longer frames and frames arriving
while the ring is full are dropped and counted (`usb_ncm_stats_t`), never held, so the class driver's buffer is free
at once. One frame to the host is in flight at a time; a second send reports busy and the caller retries. The class
driver is touched only from the USB task (`usbd_defer_func`). The interface reaches the core as `board_t.netif`
(`hal/hal_netif.h`).

MAC addresses are locally administered and derived from the chip's unique ID: the host adapter's (the descriptor's
iMACAddress) ends in an even byte, the radio's side of the link is the same address with the low bit set.

## 3. The gateway (core/dmr/ipGateway.c, core/hostlink/hostlink_net.c)

Addressing, ARP, DHCP, ping and the raw and monitor ports are as `core/dmr/ipGateway.h` describes: the host gets
12.<DMR ID>/32 by DHCP, the radio is that address xor 1, radios are 12.x.y.z and talkgroups 225.x.y.z. Control station
mode can replace the link pair with a fixed one (5.3).

- Host to air: IP for a radio ID or talkgroup, with a gateway ID any other unicast address (3.1), and datagrams to
  the raw port, go to the air ops registered with `hostlink_net_set_air()` (radio ID, IP to air, raw data as SAP 10).
  They are called from the host link task and must not block. The default ones (`core/hostlink/hostlink_net_air.c`, registered by the radio core's data service glue in
  `radio_sm.c`) copy the packet into a one-packet mailbox and return false while it is full. At the radio task's data
  tick an IP packet goes to the data service (`dmrDataServiceSendIP`), which builds it (rate 1/2, 2 preamble CSBKs):
  UDP with the compressed UDP/IPv4 header (SAP 3) when `udpCompress` allows it (default auto: ports outside
  4000-4099), else the full IPv4 header (SAP 4); confirmed data with retries when a confirmed rule names the
  destination radio or a UDP port, else unconfirmed (`dmr-data.md` 11, 12). The mailbox stays taken until the service
  is done with the packet (on air; confirmed: ACKed or failed after its tries). Raw data is built here as an unconfirmed
  SAP 10 packet for `dmrDataServiceSendBursts`. A service that can't take the packet (another one going, not a DMR
  channel, receive only) is tried again on every tick for up to 2 s, then the packet is dropped; one it took and
  couldn't deliver counts as `net.air_failed`. The host's address follows the radio's DMR ID. Refusals are counted
  (`net.to_air_refused`, `net.air_busy`, `net.air_dropped`); only confirmed packets are retried over the air.
- Air to host: the data service hands every packet it receives to `hostlink_net_packet()` (its `dmrDataEnv_t.packet`
  hook). Two packets wait for the host link task, which delivers SAP 4 as IP (ICMP, UDP, SCTP) with the addresses of
  its header (3.1), SAP 3 as UDP, and anything else as raw data on port 40078; a packet that finds the IN endpoint
  busy goes on the next pass, and packets are dropped while the link is down. Confirmed data responses are the data service's and don't go up.
- The radio's own services (`core/net`, `core/snmp`; `ipGateway.h` "The radio's own services"). The `core/net` ones run
  in the host link task, started and polled by `core/net/net.c` (`net_start` / `net_poll` from `hostlink_net.c`); the
  SNMP agent is started by `hostlink.c` (it answers over the air in either USB personality). Receive: TCP segments to
  the radio's address go to the one TCP handler (`ipGatewaySetTCP`, [tcp-services.md](tcp-services.md)); UDP
  datagrams to the radio's address, or to the link-local block 224.0.0.0/24, on a port in the gateway's one table
  (`ipGatewayUdpBind`, at most `IPGW_UDP_PORTS` = 8) go to that service, from any source address on the link.
  224.0.0.0/24 never goes over the air, bound or not. A port bound with `ipGatewayUdpBindAir` also takes UDP received
  over the air for our own DMR ID and its address (SAP 4 UDP or SAP 3, handed to `ipGatewayUdpFromAir` by `hostlink_net_packet` in the
  radio task, link up or not) instead of the host, with the sender's DMR ID and `fromAir` set; the service answers
  over the air itself. Bound now: mDNS on 5353 (`mdns.md`), TFTP on 69 and its transfer's own port (`tftp.md`), SNMP on
  161, from the air too ([snmp.md](snmp.md)). Send to the link: one buffer and one path for TCP and UDP, layer 4 built
  at `ipGatewayLocalPayload()` and sent with `ipGatewaySendLocal` (`ipGatewayUdpSend` adds the UDP header first): from
  the radio's address, IP TTL 255, to the host's MAC, the broadcast MAC or a group's multicast MAC.
- Debug link view, section `hotspot`: `usb.network usb.net_up usb.boot_network net.passthrough net.passthrough_active
  net.fixed_link net.link_ip net.link_in_force net.host_ip net.radio_ip net.frames_in net.to_air net.to_air_refused
  net.packets_air net.packets_up net.packets_dropped net.air_sent net.air_busy net.air_dropped net.air_failed`; the
  compression and confirmed delivery counters are in the `dmr_ip` section (`dmr-data.md` 12.5), the radio's own
  services' in the `net` section.
- Test hook: the debug link's DMR_DATA IP_SEND hands a UDP datagram to `ipGatewayIPToAir`, the same path as a datagram
  from the host, without switching USB (`tests/hil/test_dmr_ip.py`).

The radio's own TCP services on its address (status page, KISS TNC, rigctld, APRS-IS) are in
[tcp-services.md](tcp-services.md); TCP to other radios is not carried.

Voice over the network personality (USRP audio, host PTT, COS) is designed in [usb-audio.md](usb-audio.md) 4.2;
not built yet.

### 3.1 Routing and IP addresses

A data packet carries DMR IDs, and with the full IPv4 header (SAP 4) also IPv4 addresses. The two need not agree:
a packet to a gateway radio's DMR ID may carry the address of a host behind that radio. The compressed header (SAP 3)
has no addresses, so a receiver rebuilds them from the IDs as 12.x.y.z (radio) and 225.x.y.z (talkgroup). A packet
goes compressed only when both of its addresses are these ([dmr-data.md](dmr-data.md) 11.1).

Host to air, by the IPv4 destination, first match:

| Destination | DMR destination | IP destination on the air | TTL on the air |
|---|---|---|---|
| the all call, 255.255.255.255, the link broadcast (UDP to 4000-4099 or the raw port only) | all call 16777215, group | 225.255.255.255 | 1 |
| the group range, the multicast range | the talkgroup | 225.x.y.z | 1 |
| the individual range | the radio | 12.x.y.z | the host's |
| any other unicast address, with a gateway ID set | the gateway ID, individual | the address as sent | the host's less 1; dropped at 1 or 0 |
| anything else | not sent | | |

- The IP source on the air is always our own ID's address (12.x.y.z of the channel's DMR ID): the host's link
  address means nothing on the air, and an answer must come back to this radio.
- "Any other unicast address" excludes 0.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16, 224.0.0.0 and up, the gateway's own
  ranges, the radio's and the host's addresses, and on a fixed link the link's /24. Datagrams to the raw port never
  take the off-net route: raw data has no header to carry the address.
- No ICMP time exceeded goes back for a packet dropped at TTL 1. A packet with TTL 0 is dropped on every route.
- Group packets always go with TTL 1: the air is the group's one hop, so no gateway that hears one routes it on.
- The host routes the off-net prefixes through the radio itself (for example a route to 192.168.10.0/24 via the
  radio's address). DHCP option 121 routes only the gateway's ranges.
- The gateway ID is the data setting `ipGatewayId` ([dmr-data.md](dmr-data.md) 8), 0 by default: off-net packets are
  dropped, as before it existed.

Air to host: the IP addresses come from the header (SAP 4) or from the IDs (SAP 3).

| IP destination | Delivered to |
|---|---|
| the CAI address of the DMR destination (exactly 12.x.y.z / 225.x.y.z) | as before: our ID's to the host's address and MAC, a talkgroup's to its multicast group, the all call to the broadcast, another radio's to its address in the individual range at the monitor MAC |
| our own ID's address in another form (12.x.y.z or the individual range) | the host's address and MAC |
| a multicast group, 255.255.255.255 | its multicast MAC, the broadcast |
| another unicast address, in a packet to our DMR ID | the host's MAC, the address kept: we are the gateway, the host routes it on |
| another unicast address, between other radios | the monitor MAC, the address kept |
| 0.0.0.0/8, 127.0.0.0/8 | dropped |

The source is the sender's address in the individual range when it is the sender ID's CAI address, else kept as it is.
A kept source must be an off-net address: one in the individual range or 12.0.0.0/8 would pass for another radio, and
the host's, the radio's, the link's or a group's isn't from the air. The packet is dropped otherwise.

The packet reaches the host with the TTL it had on the air (TTL 1 for a talkgroup, 64 for SAP 3 to a radio), so a
packet that goes round a loop of gateway radios runs out. One with TTL 0 is dropped.

A packet to our DMR ID whose IP destination is not our ID's address is never taken by the radio's own services (TMS,
ARS, LRRP, text, SNMP): it goes to the host only.

## 4. Tests

- Host: `tests/host/hostlinkNetTest.c` (the gateway with a fake interface and fake air ops: unicast, multicast and raw
  to air, refusals, UDP serial both ways with chunking and a busy endpoint, IP and short data from the air, link down;
  then the default air side against a fake data service: the mailbox, the bursts decoded back by the codec's receiver
  into the IP / SAP 10 packet, SAP 3 for ports outside 4000-4099 and SAP 4 for 4007, the mailbox held while the
  service has the packet, a busy service retried, the 2 s give-up, the packet hook, the off-net destination and TTL
  through the mailbox, a packet to our ID for another address passed to the host),
  and the gateway's own `ipGatewayTest*` with pcaps checked by tshark (`testOffNet`: the routing and addresses of
  3.1). Control station mode: section 5.6.
- HIL: `tests/hil/test_usb_network.py` (`--allow-usb-switch`). `tests/hil/test_usb_network_services.py` runs the host
  port's protocols over UDP 3334: the split duplex extension (protocol, and the RX half's labels against a bench call
  with `--allow-rf --allow-service-stop`) and the KISS TNC (TNC mode, a host position from SetHardware 'N'). All pass
  on 4a9c002. One protocol at a time: the radio answers the sender of the last frame, so a tool closes its socket
  before another protocol's tool opens one. `tests/hil/test_dmr_ip.py` (on air, the bench modem decoding; its last
  test sends from the host over the USB network). The radio's own services: `tests/hil/test_tcp_services.py`,
  `tests/hil/test_net_services.py` (`mdns.md` 5, `tftp.md` 6).

## 5. Control station mode

Dispatch software on a PC (TRBOnet, SmartPTT and the like) uses a radio on USB as its control station: the
PC sends and receives the fleet's data through it, and it answers for the radio's own DMR ID itself (text messages,
registration, location). Switch: `CONFIG_DMR_CONTROL_STATION` (= `CONFIG_DMR_IP_GATEWAY`). The pieces:

### 5.1 Boot as the adapter

The stored `usb_mode` (section 1): the radio comes up as the network adapter at power-on, so the PC finds it
without anyone switching it, with the 10 s fallback to serial for a host that doesn't configure it.

### 5.2 The PC is the endpoint (passthrough)

Stored setting `net_flags` bit `SETTINGS_NET_PASSTHROUGH` (Settings > Connections > PC answers data). In force while
it is set **and** the network link is up (host configured: `hostlink_net_passthrough_active()`, view key
`net.passthrough_active`); with the PC unplugged the radio's own services answer again, so a radio left in this mode
is never deaf.

What passthrough changes is defined by one rule: everything with an IP header (SAP 4, or SAP 3 compressed UDP/IP)
addressed to the radio's ID or one of its groups belongs to the PC; the link layer stays with the radio, because the
PC can't send it.

| Packet to the radio | Without passthrough | With passthrough |
|---|---|---|
| any data packet | forwarded to the PC (as now) | forwarded to the PC |
| confirmed data (C_HEAD, A bit) | ETSI response from the radio | ETSI response from the radio (the PC can't send it) |
| ARS, UDP 4005 (answers to the radio's registration, queries) | the radio's ARS | ignored; the radio's own registration, refresh and deregistration stop (the PC registers) |
| Motorola TMS, UDP 4007 | inbox, TMS ACK from the radio | PC only: no ACK, no inbox entry |
| ETSI IP text, UDP 5016 | inbox | PC only |
| LRRP, UDP 4001 | answered by the radio | PC only; running triggers and queued requests are dropped when passthrough starts |
| other UDP, ICMP, SCTP | PC (no service of the radio's) | PC |
| ETSI defined short data, UDT text (no IP header) | inbox, ETSI response | unchanged (and forwarded raw on 40078) |
| call alert, radio check, emergency alarm (CSBK) | answered by the radio per its settings | unchanged |
| the TMS ACK of a message the radio's own user sent | completes it | still completes it (nothing is sent) |

The PC's replies (and anything else it sends to a radio ID or talkgroup) go out as now: built by the radio with its own
DMR ID as the source, so to the fleet they come from the control station. The data service counts what it left to the
PC in `data.pt_yielded` and shows `data.pt_active` (debug link section `dmr_ext`). Spec of the data side:
`dmr-data.md` 10.

### 5.3 Link addressing

Stored setting `net_flags` bit `SETTINGS_NET_FIXED_LINK` with `net_link_ip` (Settings > Connections > USB link
address: "By DMR ID" / "192.168.10.x"). It fits the gateway's design because the gateway's addressing has only two
link-local facts, the radio's address and the host's, and the rest (the 12/8 and 225/8 ranges, the air side) doesn't
depend on them:

| | By DMR ID (default) | Fixed link |
|---|---|---|
| radio | 12.<ID> xor 1 | `net_link_ip`, default 192.168.10.1 |
| host (DHCP) | 12.<ID>/32 | the next address, 192.168.10.2, netmask 255.255.255.0 |
| option 121 / 249 | the radio on the link, 12/8 and 225/8 via the radio | 12/8 and 225/8 (and the group range) via the radio |
| ARP answered | every address but the host's | the radio's address only |
| packets to the radio's own ID from the air | to 12.<ID> (the host's address) | rewritten to the host's fixed address |
| broadcasts to the host (all call, monitor) | 255.255.255.255 | 255.255.255.255 |
| host to air broadcast | 255.255.255.255, the all call address | also the link's 192.168.10.255 |
| radios per host | several (each has its own pair) | one (every radio has the same pair) |

`net_link_ip` must be a unicast address outside the 12/8, 225/8 (and group) ranges with a last octet of 1-253;
anything else (or 0) means the default 192.168.10.1 when set from the UI, or the DMR ID addressing when the debug link
stores an unusable address (`ipGatewaySetFixedLink` refuses it; `net.link_in_force` shows what is used). A change
applies at once on the radio's side; the PC picks it up at its next DHCP exchange (the radio refuses the renewal of
the old address, so at the latest when the 10 min lease runs out; unplugging the cable is quicker).

What the dispatch application must be told, in either mode: the control station's IP address (the radio's link
address: 192.168.10.1 or 12.<ID> xor 1), the radio network 12.0.0.0/8 for radio IDs and 225.0.0.0/8 for talkgroups
(already routed through the radio by DHCP), the radio's own DMR ID, the data ports (ARS 4005, TMS 4007, LRRP 4001,
ETSI text 5016). Headers on the air follow the radio's compression setting (`dmr-data.md` 11: by default UDP
outside 4000-4099, ETSI text 5016 among them, goes with the SAP 3 compressed header; set `ip.compress` off for
full IPv4 headers). An application that expects to talk to
the radio itself over a vendor control protocol (XNL / XCMP and the like) can't use it: the radio is a router, not a
vendor control head.

### 5.4 Settings and the debug link

| Setting (`settings_t`) | Values | UI (Settings > Connections) | Debug link |
|---|---|---|---|
| `usb_mode` | 0 serial (default), 1 network at power-on | USB network at start | radio op 27 bit 0, `usb.boot_network` |
| `net_flags` & `SETTINGS_NET_PASSTHROUGH` | off (default) / on | PC answers data | op 27 bit 1, `net.passthrough`, `net.passthrough_active` |
| `net_flags` & `SETTINGS_NET_FIXED_LINK` | off (default: by DMR ID) / on | USB address | op 27 bit 2, `net.fixed_link` |
| `net_link_ip` | 0 = 192.168.10.1 | (the default only) | op 27 IP, `net.link_ip`, `net.link_in_force` |

Radio op 27 (`debuglink.md` 3.8): flags u8, link IP u32 little endian; stored, and applied at once
(`hostlink_settings_changed`). `rfoundry_debug.py radio usb-settings [boot] [passthrough] [fixed[=IP]]`; no words: all off.

### 5.5 Not done

- Confirmed data from the PC per packet: the PC can't ask for it itself; the data service confirms the gateway's
  packets by its rules (per destination ID or UDP port, `dmr-data.md` 12), the same for the PC's packets as for any
  host's.
- Voice: the control station passes data only (voice over the network personality is `usb-audio.md` 4.2).

### 5.6 Tests

- Host: `ipGatewayTest*` `testFixedLink` (refused addresses, DHCP offer and routes, the renewal NAK, ARP, to air from
  the fixed address, the link broadcast, UDP serial, delivery to our ID rewritten to the host, all call and monitor
  broadcasts, ping, back to the DMR ID; written to the pcaps tshark checks); `hostlinkNetTest` (passthrough follows
  the link, the fixed link end to end: an ETSI IP text from the air reaches 192.168.10.2, the PC's answer goes over
  the air from the radio's ID); `dmrDataServiceTest` `testPassthrough` (the table in 5.2); `radioCoreTest` (the
  settings records).
- HIL: `tests/hil/test_control_station.py` (opt-in, below). The lead runs it against a dispatch application.

HIL plan (`tests/hil/README.md`):

1. `test_usb_settings_stored` (radio, `--allow-usb-switch`): op 27 stores each bit; the view keys follow; all off at
   the end.
2. `test_boot_network` (`--allow-usb-switch --allow-reboot`): with `usb_mode` 1, a reboot comes back as 1FC9:0096 and
   the debug link answers over `udp:<radio IP>`; the setting is then cleared over UDP and the radio rebooted to serial.
   Manually on macOS 26: a host that doesn't configure the adapter gets the serial port back 10 s after plugging in.
3. `test_fixed_link` (network mode): op 27 fixed, the host renews DHCP, gets 192.168.10.2, pings 192.168.10.1, the
   debug link answers at `udp:192.168.10.1`.
4. `test_passthrough_text` (`--allow-rf --allow-service-stop`, SDR bench): passthrough on, the bench sends an ETSI IP
   text and a TMS with an ACK request to the radio's ID; the PC's socket on UDP 5016 / 4007 gets them, the radio's
   inbox count doesn't change, `data.tms_acks_sent` doesn't move, `data.pt_yielded` does; a confirmed packet still
   gets the radio's ETSI response (the bench hears it).
5. `test_passthrough_reply` (same): the PC sends UDP 4007 to the bench's ID; the bench hears the packet with the
   radio's DMR ID as the source.
6. A dispatch application (manual): its control station set to the radio's link address, its own ARS / TMS / LRRP on.
   With passthrough on, a text from a fleet radio appears in the application and only there, its reply reaches the
   fleet radio from the control station's ID, a location request from it is answered by the fleet radio (the control
   station doesn't answer for itself), and the radio's own ARS registration does not appear in its log.
