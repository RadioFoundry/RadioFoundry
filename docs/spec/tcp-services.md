<!-- SPDX-License-Identifier: MIT -->

# TCP services on the USB network link

In the USB network personality ([usb-network.md](usb-network.md)) the radio has an address of its own on the link
(the host's address xor 1, e.g. 12.0.39.20 for DMR ID 10005). With `CONFIG_NET_TCP` it runs a small TCP there and a
few services on it. TCP never goes over the air: the IP gateway hands the TCP only segments addressed to the radio
itself, and TCP to other radios' addresses is dropped as before.

Sources: RFC 9293 (TCP), RFC 6298 (retransmission timer), RFC 5681 3.1 (one segment after a timeout), RFC 5961
(RST and SYN checks), RFC 1122 4.2.3.6 (keepalive), RFC 6528 (initial sequence numbers).

## 1. Switches

| Switch | Default | What |
|---|---|---|
| `CONFIG_NET_TCP` | = DMR_IP_GATEWAY | the TCP (`core/net/tcp.c`) and its glue to the gateway (`core/net/net.c`) |

## 2. TCP

`core/net/tcp.c`, no RTOS calls: the host link task feeds it segments from the gateway (`ipGatewaySetTCP`) and calls
`tcp_poll` on every pass in network mode (5 ms); segments go out through `ipGatewaySendLocal` to the host's MAC
address, the host being the only neighbor and the router.

| Item | Value |
|---|---|
| Connections | 3 in all, TIME_WAIT included; 4 listening ports |
| MSS | 536 (14 + 20 + 20 + 536 = 590 bytes, within the gateway's 600 byte frames); a smaller MSS option from the peer is honored |
| Options | MSS only (sent on SYN); no window scaling, SACK or timestamps |
| Transmit | one buffer per connection, 1072 bytes (two segments) from SND.UNA on; data is sent as soon as it is written (no Nagle: the services write whole messages) |
| Receive | in order only; a segment that doesn't start at RCV.NXT is dropped and answered with an ACK for RCV.NXT; old data overlapping new data is trimmed. The window offered is what the service can take |
| Retransmission | RFC 6298 RTO, initial 1 s, minimum 200 ms (a USB round trip is about 1 ms), maximum 16 s, doubled per timeout; Karn's rule; after a timeout one segment at a time until new data is acknowledged; 7 retransmissions, then RST |
| SYN | ours: 4 retries (about 31 s); a peer's repeated SYN gets the same SYN-ACK again |
| Zero window | the persist timer probes with one byte, for as long as the peer answers |
| Unknown port or connection | RST (RFC 9293 3.10.7.1); a SYN while every slot is busy gets nothing (the client tries again a second later) |
| RST / SYN on a connection | RST only at exactly RCV.NXT, else a challenge ACK; a SYN gets a challenge ACK (RFC 5961) |
| TIME_WAIT | 2 s; the oldest TIME_WAIT slot is reused for a new SYN; a new SYN on the same ports ends it |
| FIN_WAIT_2 | 10 s, then the slot is freed |
| Keepalive | after 60 s without a segment from the peer, a probe every 10 s (SND.NXT - 1); 4 unanswered and the connection is reset |
| Link down | every connection dropped without a segment |
| Initial sequence numbers | a salt from the radio's MAC address, the clock (250 per ms) and a counter |

Segments to the radio's address are taken from any source, not only the host: replies to the radio's own client
connections come back through the host when it routes. The checksum is checked; bad segments are counted and dropped.

Debug link view, section `net`: `tcp.open tcp.segs_in tcp.segs_out tcp.retransmits tcp.rst_out tcp.bad_checksum
tcp.accepted tcp.refused tcp.no_slot tcp.timeouts net.link_up`, and the services' keys below.

## 3. Changes in the IP gateway

`core/dmr/ipGateway.{c,h}`: TCP (protocol 6) to the radio's own address goes to the function set with
`ipGatewaySetTCP` (NULL by default: dropped, as before), from any source and before the "only what the host itself
sends" check; any other TCP is dropped and never goes over the air. `ipGatewayLocalPayload` / `ipGatewaySendLocal`
send a packet from the radio's address to any destination through the host's MAC address. The TCP's glue
(`core/net/net.c`) waits up to 20 ms for a busy USB IN endpoint before giving a segment up to the retransmission timer.

## 4. Tests

- Host: `tests/host/tcpTest.c`, the TCP against a scripted peer: handshakes both ways, data within the window and the
  MSS, retransmission with backoff, a zero window probed, in-order receive with trimming, RST for unknown ports,
  RFC 5961 challenge ACKs, closes from either side (simultaneous too), TIME_WAIT reuse, slot exhaustion, keepalive, a
  lost SYN-ACK, a busy IP layer. Every segment goes to `tcpTest.pcap`, which tshark checks for malformed segments and
  bad checksums. `tests/host/hostlinkNetTest.c`: TCP through the gateway (RST from the radio's address, a source
  beyond the host, no TCP to the air, the link going down).
- The host OS stack is the other end in the HIL tests (curl, nc); a tap / utun test on the host would need root and
  is not done.
