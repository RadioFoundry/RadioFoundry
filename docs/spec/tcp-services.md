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
| `CONFIG_NET_WEBCTL` | = NET_HTTP | web control: the front panel page and its WebSocket `/ws` (`core/net/webctl.c`, `core/net/ws.c`, section 5.1) |
| `CONFIG_NET_RIGCTL` | = NET_TCP | Hamlib's network rig control, port 4532 (`core/net/rigctld.c`, section 6) |
| `CONFIG_NET_APRSIS` | = NET_TCP and APRS | the radio's own APRS-IS client (`core/net/aprsis.c`, section 7) |

A board without USB runs them over PPP on its host port (`ppp.md`), with 3 TCP connections instead of 4
(`TCP_CONNS` in `platform/hrc7000/config.mk`). On a flash-tight board they fit only with link time optimization (its
default; [feature-matrix.md](../feature-matrix.md), "Whole images"). Which radios send and receive AFSK: `aprs.md` 1.

## 2. TCP

`core/net/tcp.c`, no RTOS calls: the host link task feeds it segments from the gateway (`ipGatewaySetTCP`) and calls
`tcp_poll` on every pass in network mode (5 ms); segments go out through `ipGatewaySendLocal` to the host's MAC
address, the host being the only neighbor and the router.

| Item | Value |
|---|---|
| Connections | `TCP_CONNS` (4 by default) in all, TIME_WAIT included; 4 listening ports. A SYN takes a free slot, else the oldest TIME_WAIT, else the oldest FIN_WAIT_2 (reset) |
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
- On a radio without an APRS receive path (`aprs.md` 1) only transmit works; a client sees no received packets.
- View keys (`net` section): `kiss.client kiss.conns kiss.frames_in kiss.frames_out kiss.dropped`.

## 5. HTTP status server (port 80)

HTTP/1.0 (RFC 1945): GET and HEAD, one request per connection, `Connection: close`, `Content-Length` always, no
keep-alive or chunking (HTTP/1.1 clients accept a 1.0 answer). The request line may be 95 bytes long (else 414);
header lines are skipped, except `Sec-WebSocket-Key` (any case) for `/ws`; a request not complete within 10 s is reset. Responses carry `Cache-Control: no-store` and
`Access-Control-Allow-Origin: *` (a web page on the host may poll the radio).

| Path | Response |
|---|---|
| `/` and `/<file>` | the static files (below); `/` is `index.html`, the status as a table and the screen, refreshed every 3 s; with `CONFIG_NET_WEBCTL` the front panel page (5.1) |
| `/ws` | with `CONFIG_NET_WEBCTL`: the WebSocket upgrade (5.1); 400 without a valid key or for HEAD, 503 while another client has it |
| `/status` | JSON: `radio version uptime_s state` (idle / rx / tx / tx_denied) `vfo zone channel_number channel rx_hz tx_hz mode` (fm / dmr) `bw` ("12.5" / "25") `power rssi_dbm squelch battery_pct battery_mv` (both left out on a radio without a battery) `hotspot`, on a DMR channel `dmr: {own_id cc ts target private heard}`, `tcp_open`, `led_green led_red` (the radio's LEDs), `backlight` (percent), `draws` (counts screen redraws: fetch the screen when it changes) |
| `/api/view` | JSON `{"sections": [...]}`: the debug link's view section names (needs `CONFIG_DEBUGLINK`) |
| `/api/view?section=<name or number>` | the section's key=value lines as one flat JSON object; values that are decimal integers are numbers, the rest strings (hex values such as `net.host_ip` stay strings) |
| `/api/screen.bmp` | the frame buffer as a BMP: top-down (negative height), 16 bit BI_BITFIELDS (RGB565 masks), about 40 KB on a 160 x 128 screen; UI redraws are held while it goes out (as the debug link's screenshot does), so it is one frame; the hold ends with the last byte, so a page that polls it does not slow the UI. 404 on a 1 bpp display |
| `/api/aprsis` | the APRS-IS client's settings and state (section 7) |
| anything else | 404; another method 405; a malformed request line 400 (errors have an empty body) |

Static files: every file in `core/net/http_assets/` (html, css, js, svg, ...: add one there and it is served at
`/<name>`, the type from its extension) is put into a table at build time by `tools/gen_http_assets.py` (run by make
into the build's `gen/http_assets_gen.h`: path, Content-Type, bytes, length, gzip flag). With `CONFIG_NET_HTTP_GZIP`
(the default with `CONFIG_NET_HTTP`) the files are stored gzip compressed (level 9, mtime 0: the same bytes on every
build machine) and served with `Content-Encoding: gzip`; off, stored and served as they are. A gzip response goes
whatever the request's `Accept-Encoding` says: a request without the header accepts any coding (RFC 9110 12.5.3),
every browser takes gzip, and keeping a plain copy too would cost the flash the compression saves; `curl` needs
`--compressed`. `index.html` today: 498 bytes, 360 gzip compressed. Host test: `httpAssetsTest` (the table, gzip and
plain, decompresses to the sources), `netSvcTest` (the bytes and headers served).

Bodies stream into TCP as its buffer frees up: the files from flash and the screen straight from the frame buffer;
JSON is rendered once into a shared 1.5 KB buffer (a view section's text into its upper part, the JSON written from
the start never overtakes it), and a second JSON request waits until the first response is all in TCP's buffer.

View keys: `http.requests http.ok http.not_found http.bad http.busy_waits`.

### 5.1 Web control (`/ws`)

With `CONFIG_NET_WEBCTL` the page at `/` is the radio's front panel: its drawing (the site's
`site/static/img/radios/src/<radio>.svg`, served as `/radio.svg`) with the live screen in its display, the LEDs, and
every key clickable. Keys the drawing doesn't show, the knob (when the radio has an encoder) and PTT are buttons below
it. The page's files are in `core/net/webctl_assets/` and replace `core/net/http_assets/` files of the same name;
`tools/gen_webctl_assets.py` writes `/keys.json` (each key's name and bit, from the board's `KEY_*` enum) and
`/radio.svg`. A radio without a drawing gets its keys as buttons and the screen as an image.

`GET /ws` with `Upgrade: websocket` and a `Sec-WebSocket-Key` gets `HTTP/1.1 101` and the connection becomes a
WebSocket (RFC 6455; the accept value from SHA-1 and base64, `ws.c`). One client at a time; the next one gets 503
until it goes. The radio's rules:

| Rule | Value |
|---|---|
| Client frames | masked, unfragmented, payload up to 125 bytes; else close 1002 (protocol) or 1009 (too big) |
| Binary from the client | close 1003 (nothing takes it yet) |
| Close | the client's close code is sent back, then the connection closes |
| Ping | one every 5 s; nothing from the client for 15 s and it is dropped |
| Status | `/status`'s JSON as a text frame, rendered every 250 ms and sent when it changed |
| Audio | 20 ms binary frames, 160 samples of 8 kHz PCM16 little-endian, while the client asked and the squelch is open; needs `CONFIG_NET_USRP` (the frames USRP reads, `usb-audio.md`); a frame that doesn't fit in TCP's buffer is dropped |

The first frame is the hello: `{"hello":1,"control":true,"audio":true,"keys":[0,1,...],"ptt":21,"encoder":true}`
(the keymap's bits, the PTT key's bit or -1, whether the radio has an encoder). Commands are text frames:

| Command | Effect |
|---|---|
| `d<bit>` / `u<bit>` | the key down / up, through the input task as an injected key (`input_inject_keys`): one key at a time, released after 10 s if no `u` comes |
| `d<ptt bit>` | PTT for 4 s; the page repeats it every 2 s while held. Released after 3 minutes whatever the page sends, and not keyed again until a `u` |
| `e<n>` | n encoder detents, -127..127 (forward is positive) |
| `a1` / `a0` | receive audio on / off |

Keys, PTT and the encoder are taken only from the computer the radio is plugged into (`control:false` for a client
beyond it, as rigctld's PTT); audio and status go to any client. Everything the client holds is released when it
closes, its TCP ends, or it is dropped. Unknown commands are counted and ignored.

View keys: `webctl.client` (its TCP slot, -1 none) `webctl.control webctl.conns webctl.commands webctl.refused`
(control from beyond the host, an unknown key) `webctl.busy` (another injected key held) `webctl.errors
webctl.frames_out webctl.audio_out webctl.audio_dropped webctl.ptt webctl.ptt_downs`.

## 6. rigctld (port 4532)

Hamlib's network rig control protocol as the rigctld(1) and rigctl(1) manual pages describe it, so loggers, SDR
programs and satellite trackers can follow and set the radio (`rigctl -m 2 -r <radio>:4532`, or a program's "Hamlib
NET rigctl" rig). One command per line (CR ignored), the short form or the backslash long name, arguments separated
by spaces. A "get" answers its value lines; a "set" answers `RPRT 0`, an error `RPRT -<Hamlib error>` (1 invalid
argument, 4 not implemented, 9 refused, 11 not available). A leading `+` (or `;` `|` `,`) asks for the extended answer:
the command's long name with a colon, the values with their labels, then `RPRT`. A VFO argument right after the
command (clients started with `--vfo`) is skipped. A line longer than 63 characters is answered -1 and not run.

| Command | Answer / action |
|---|---|
| `f` `\get_freq` | the receive frequency in Hz; a frequency just set is reported until the radio shows it (at most 1 s) |
| `F` `\set_freq <Hz>` | the VFO to that frequency (simplex); on a channel the radio moves to the VFO first. Outside the board's bands -1, in hotspot mode -9. Requests go at most every 20 ms: a burst of updates (Doppler tracking) ends at its latest value |
| `m` `\get_mode` | `FM` and the passband, 12500 or 25000 (a DMR channel answers FM too: DMR is 4FSK on an FM carrier, and Hamlib has no DMR mode) |
| `M` `\set_mode FM\|FMN <passband>` | FM on the channel (a DMR channel turns FM for now) and the bandwidth: 0 / -1 unchanged, up to 12500 narrow, above wide; FMN narrow. Other modes -1 |
| `t` `\get_ptt`, `T` `\set_ptt 0-3` | PTT; keyed only from the computer the radio is plugged into (else -9), at most 3 minutes per keying (the radio's own TX time-out may end it sooner), again only 1 s after a release (else -9), released when the connection ends |
| `\get_dcd` | 1 while the squelch is open |
| `l STRENGTH` | dB over S9, S9 = -93 dBm (IARU VHF / UHF); `l RFPOWER` 0.00-1.00 from the power level; other levels -11 |
| `v`, `V VFOA` | `VFOA`; `V` takes VFOA, currVFO, VFO, Main |
| `s`, `S 0`, `i` | no split (`0`, `VFOA`); `i` the transmit frequency |
| `_` `\get_info` | `RadioFoundry <radio>` |
| `\chk_vfo` | `0` |
| `\get_powerstat` | `1` |
| `\dump_state` | a minimal protocol 0 state: model 2, ITU region 2, the board's bands as receive ranges (FM, 0x20) and its transmit bands, FM tuning steps and filters, no RIT / XIT / preamp / attenuator, `STRENGTH` as the only level |
| `q` `Q` `\quit` | closes the connection |
| anything else | `RPRT -4` |

`\dump_state` is written from the manual's description and the line layout clients are known to parse; the HIL test
checks it with Hamlib's own `rigctl -m 2` where it is installed.

View keys: `rigctl.conns rigctl.commands rigctl.errors rigctl.freq_sets rigctl.ptt rigctl.ptt_downs`.

## 7. APRS-IS client

The radio connects to an APRS-IS server itself, out through the host, instead of the host helper
`tools/aprs_gate.py` doing it over USB (aprs-is.net "Connecting to APRS-IS", "IGate Design"). It carries lines only;
the iGate rules stay in the APRS engine ([aprs.md](aprs.md) 9.1).

- Off until a server address is set (settings `aprsis_ip`, `aprsis_port` 0 = 14580, `aprsis_range_km`) and the APRS
  callsign is. IPv4 addresses only: no DNS client (pick a server's address, e.g. `dig +short rotate.aprs2.net`, or run
  a local aprsc / aprs_gate-like relay on the host).
- Set over HTTP from the host only (a request from beyond the host gets 403): `GET /api/aprsis?server=a.b.c.d[:port]`
  (`off` clears it), `&range=<km>`; `GET /api/aprsis` alone answers the state as JSON (`state server range_km call
  connects logins unverified lines_out lines_in`).
- Login: `user <CALL-SSID> pass <passcode> vers RadioFoundry <version>[ filter m/<km>]`, the passcode computed
  from the callsign by the published 15-bit hash (`aprsis_passcode`, the same as `tools/aprs_gate.py`). Online once
  the server's `# logresp ... verified` line comes; an "unverified" answer drops the connection and waits 5 minutes.
- Online, the TNC pump (section 3) sends it the engine's SetHardware 'G' lines with CR LF: our beacons, status and
  messages as `CALL>APZRDF,TCPIP*:...` when the APRS settings "KISS on USB" and "to APRS-IS" (`to_is`) are on, and on
  receive builds the RF packets the iGate passes. Server lines that aren't comments go to the engine
  (`aprs_host_is_line`), whose IS -> RF rules decide (iGate builds; elsewhere they are dropped).
- Reconnects 30 s after a failure, doubling up to 5 minutes; a server silent for 2 minutes (servers comment every
  20 s) or no logresp within 30 s is dropped; a settings or callsign change reconnects at once.
- View keys: `aprsis.state aprsis.server aprsis.connects aprsis.logins aprsis.unverified aprsis.lines_out
  aprsis.lines_in aprsis.drops`.

### 7.1 Routing through the host

The radio's address (12.x.y.z, the host's xor 1) is only known to the host, which must forward and NAT it. With
`RADIO=12.0.39.20` (the radio's address) and the host's Internet interface `en0` / `eth0`:

```
# macOS
sudo sysctl -w net.inet.ip.forwarding=1
echo "nat on en0 from $RADIO to any -> (en0)" | sudo pfctl -ef -

# Linux (usb0: the radio's interface)
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s $RADIO/32 -o eth0 -j MASQUERADE
sudo iptables -A FORWARD -i usb0 -o eth0 -j ACCEPT
sudo iptables -A FORWARD -i eth0 -o usb0 -m state --state RELATED,ESTABLISHED -j ACCEPT
```

The replies come back translated to the radio's address and reach it over the host route the radio's DHCP gave
(option 121). A server address inside 12.0.0.0/8 can't be used (the host routes that range to the radio).

## 8. Changes in the IP gateway

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
  (the second waits for the buffer), the 10 s request timeout. Web control: the handshake, the hello, status sent on change, 503 for a second
  client, 400 without a key, keys down and up (busy, refused from beyond the host), the encoder, PTT repeats and the 3
  minute release, pings, the 15 s drop with keys released, audio frames only when asked and the squelch is open, the
  close code echoed, an unmasked frame (1002); `wsTest.c` checks SHA-1, base64 and frame headers against FIPS 180 and
  RFC 6455. rigctld: get / set frequency (the VFO move, the read
  back before the radio shows it, the request gap, out of band), mode, PTT (the rate limit, the 3 minute limit, the
  release on disconnect, refused from beyond the host), levels, VFO, split, info, chk_vfo, dump_state, extended
  answers, unknown commands, quit. APRS-IS: the passcode, off without a server, settings over HTTP (403 from beyond
  the host, 400 for a bad address), the login line, logresp verified / unverified and the waits, the engine's 'G'
  lines to the server and nothing else, server lines to the engine without comments, a silent server dropped and
  tried again, a callsign change, turned off. `netSvcTest.pcap` is checked by tshark.
- HIL: `tests/hil/test_tcp_services.py` (opt-in, below).
- The host OS stack is the other end in the HIL tests (curl, nc); a tap / utun test on the host would need root and
  is not done.
