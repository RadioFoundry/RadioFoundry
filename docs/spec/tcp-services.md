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
| `CONFIG_NET_KISS` | = NET_TCP and APRS | KISS over TCP, port 8001 (`core/net/kiss_tcp.c`, section 4) |
| `CONFIG_NET_HTTP` | = NET_TCP | the HTTP status server, port 80 (`core/net/http.c`, section 5) |

## 2. TCP

`core/net/tcp.c`, no RTOS calls: the host link task feeds it segments from the gateway (`ipGatewaySetTCP`) and calls
`tcp_poll` on every pass in network mode (5 ms); segments go out through `ipGatewaySendLocal` to the host's MAC
address, the host being the only neighbor and the router.

| Item | Value |
|---|---|
| Connections | 4 in all, TIME_WAIT included; 4 listening ports. A SYN takes a free slot, else the oldest TIME_WAIT, else the oldest FIN_WAIT_2 (reset) |
| MSS | 536 (14 + 20 + 20 + 536 = 590 bytes, within the gateway's 600 byte frames); a smaller MSS option from the peer is honored |
| Options | MSS only (sent on SYN); no window scaling, SACK or timestamps |
| Transmit | one buffer per connection, 1072 bytes (two segments) from SND.UNA on; data is sent as soon as it is written (no Nagle: the services write whole messages) |
| Receive | in order only; a segment that doesn't start at RCV.NXT is dropped and answered with an ACK for RCV.NXT; old data overlapping new data is trimmed. The window offered is what the service can take |
| Retransmission | RFC 6298 RTO, initial 1 s, minimum 200 ms (a USB round trip is about 1 ms), maximum 16 s, doubled per timeout; Karn's rule; after a timeout SND.NXT goes back to SND.UNA and one segment goes at a time until new data is acknowledged; an ACK up to the highest sequence number sent (SND.MAX) is accepted, so a peer that had it all doesn't get it twice; 7 retransmissions, then RST |
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

## 3. The TNC pump

The APRS engine has one output for the host: KISS frames in a ring (`aprs_host_output`: received packets on RX
builds, SetHardware 'G' lines for APRS-IS), which the host link sends to the serial or UDP port. While a TCP service
takes it (`net_tnc_taken`: a KISS client is connected and the APRS KISS setting is on, or the APRS-IS client is
logged in), `core/net/net.c` drains the ring instead, decodes it frame by frame and routes each frame: 'G' lines to
APRS-IS when it is logged in, everything else to the KISS client. It pulls only while the receivers have room for a
whole frame (every byte escaped), so nothing is dropped for want of TCP buffer; the ring holds the rest. The host link
leaves the output alone meanwhile (`hostlink.c`).

## 4. KISS over TCP (port 8001)

The de facto network TNC port of APRS programs (APRSdroid, Xastir, YAAC, Dire Wolf clients). The stream carries KISS
frames as on the serial port (Chepponis and Karn, "The KISS TNC", 1987; [aprs.md](aprs.md) 9): FEND-delimited, FESC
escapes, a type byte. Frames from the client go to the same handler as the serial port's (`aprs_host_kiss_frame`):
port 0 data frames are checked by the APRS engine (our callsign as the source, the amateur frequency, the rate limit)
and queued for RF; SetHardware 'G' and 'N' lines as on the serial port; TXDELAY and the other parameters are ignored.
The client receives the engine's output (section 3) exactly as the serial port would.

- One client at a time: a new connection replaces the old one (reset), so a client that vanished without closing
  doesn't hold the port.
- Needs the APRS KISS setting (Tools > APRS > KISS on USB), as on the serial port: without it the engine neither
  takes frames nor produces output.
- The DM-1701 has no APRS receive path: only transmit works there; a client sees no received packets.
- View keys (`net` section): `kiss.client kiss.conns kiss.frames_in kiss.frames_out kiss.dropped`.

## 5. HTTP status server (port 80)

HTTP/1.0 (RFC 1945): GET and HEAD, one request per connection, `Connection: close`, `Content-Length` always, no
keep-alive or chunking (HTTP/1.1 clients accept a 1.0 answer). The request line may be 95 bytes long (else 414);
header lines are skipped; a request not complete within 10 s is reset. Responses carry `Cache-Control: no-store` and
`Access-Control-Allow-Origin: *` (a web page on the host may poll the radio).

| Path | Response |
|---|---|
| `/`, `/index.html` | a page of about 700 bytes from flash: the status as a table and the screen, refreshed every 3 s |
| `/status`, `/api/status` | JSON: `radio version uptime_s state` (idle / rx / tx / tx_denied) `vfo zone channel_number channel rx_hz tx_hz mode` (fm / dmr) `bw` ("12.5" / "25") `power rssi_dbm squelch battery_pct battery_mv hotspot`, on a DMR channel `dmr: {own_id cc ts target private heard}`, `tcp_open` |
| `/api/view` | JSON `{"sections": [...]}`: the debug link's view section names (needs `CONFIG_DEBUGLINK`) |
| `/api/view?section=<name or number>` | the section's key=value lines as one flat JSON object; values that are decimal integers are numbers, the rest strings (hex values such as `net.host_ip` stay strings) |
| `/api/screen.bmp`, `/api/screen` | the frame buffer as a BMP: top-down (negative height), 16 bit BI_BITFIELDS (RGB565 masks), about 40 KB on a 160 x 128 screen; UI redraws are held while it goes out (as the debug link's screenshot does), so it is one frame. 404 on a 1 bpp display |
| `/api/aprsis` | the APRS-IS client's settings and state (section 7) |
| anything else | 404; another method 405; a malformed request line 400 |

Bodies stream into TCP as its buffer frees up: the page from flash and the screen straight from the frame buffer;
JSON is rendered once into a shared 1.5 KB buffer (a view section's text into its upper part, the JSON written from
the start never overtakes it), and a second JSON request waits until the first response is all in TCP's buffer.

View keys: `http.requests http.ok http.not_found http.bad http.busy_waits`.

## 6. Changes in the IP gateway

`core/dmr/ipGateway.{c,h}`: TCP (protocol 6) to the radio's own address goes to the function set with
`ipGatewaySetTCP` (NULL by default: dropped, as before), from any source and before the "only what the host itself
sends" check; any other TCP is dropped and never goes over the air. `ipGatewayLocalPayload` / `ipGatewaySendLocal`
send a packet from the radio's address to any destination through the host's MAC address. The TCP's glue
(`core/net/net.c`) waits up to 20 ms for a busy USB IN endpoint before giving a segment up to the retransmission timer.

## 9. Tests

- Host: `tests/host/tcpTest.c`, the TCP against a scripted peer: handshakes both ways, data within the window and the
  MSS, retransmission with backoff, a zero window probed, in-order receive with trimming, RST for unknown ports,
  RFC 5961 challenge ACKs, closes from either side (simultaneous too), TIME_WAIT reuse, slot exhaustion, keepalive, a
  lost SYN-ACK, a busy IP layer. Every segment goes to `tcpTest.pcap`, which tshark checks for malformed segments and
  bad checksums. `tests/host/hostlinkNetTest.c`: TCP through the gateway (RST from the radio's address, a source
  beyond the host, no TCP to the air, the link going down). `tests/host/netSvcTest.c` (+ `netSvcCases.c`): the
  services through the real gateway and TCP against a scripted TCP peer, with fakes for the APRS engine and the radio
  core; KISS: frames both ways with escapes and split segments, bulk output paced by the client's ACKs, a second
  client replacing the first, the KISS setting off, the client closing. HTTP: the page, HEAD, the status JSON with an
  escaped name, view sections as JSON (numbers, strings, escapes, by name and number, the list, unknown), the screen
  BMP against the frame buffer, 405 / 404 / 400 / 414, a client sending a byte at a time, two JSON requests at once
  (the second waits for the buffer), the 10 s request timeout. `netSvcTest.pcap` is checked by tshark.
- The host OS stack is the other end in the HIL tests (curl, nc); a tap / utun test on the host would need root and
  is not done.
