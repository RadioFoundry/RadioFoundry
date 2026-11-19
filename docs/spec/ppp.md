<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# PPP on the serial host link

PPP carries IPv4 on the radio's serial host port, into the same IP gateway and services as the USB network adapter
([usb-network.md](usb-network.md)). It's the IP link of radios whose only computer link is a USB-serial cable on a UART
at 115200 baud. On radios with USB, it can also run on the USB serial port, as a fallback when a host doesn't bring up
the network adapter.

The switch is `CONFIG_NET_PPP` ([build-config.md](../build-config.md)). It's on for boards without USB (their only IP
link, with the gateway and every service) and off on boards with USB; see the [feature matrix](../feature-matrix.md).
It needs `CONFIG_HOSTLINK` and `CONFIG_DMR_IP_GATEWAY`. The gateway needs a link to the host, `CONFIG_USB_NETWORK` or
`CONFIG_NET_PPP`, and follows PPP by default.

The implementation follows RFC 1661 (PPP, LCP), RFC 1662 (HDLC-like framing), and RFC 1332 (IPCP) only.

| File | What |
|---|---|
| `core/net/ppp.c`, `ppp.h` | framing, LCP and IPCP; no RTOS calls, no hardware |
| `core/hostlink/hostlink_ppp.c` | the netif (`board_t.netif`) between PPP and the gateway, the engine's I/O |
| `core/hostlink/hostlink.c` | PPP as a personality of the host port: entering, feeding, leaving |
| `tests/host/pppTest.c` | host test and fuzz loop; `tests/host/pppPeer.c` the same code on a pseudo-terminal |
| `tools/rfoundry_ppp.sh` | host side: pppd on the radio's serial port, routes |
| `tools/rfoundry_ppp_interop.sh` | interop test against pppd (Linux, root) |

A board with PPP sets `board.netif = &hostlink_ppp_netif`. Without it, the link still negotiates, but the gateway's
answers go nowhere. The view key `net.netif` shows whether the board has a netif.

## 1. Framing (RFC 1662)

- Flag `0x7E` before and after every frame. On receive, one flag between back-to-back frames is enough; the radio
  always sends a leading flag. Control escape `0x7D`: the next byte is XORed with `0x20`. `0x7D 0x7E` aborts a frame.
- ACCM both ways. The radio sends every control character escaped until the peer's ACCM is known, and always in LCP
  (RFC 1662 7.1). It asks the peer for ACCM 0. Until LCP is open, and when the peer rejects the option, a bare control
  character from the peer is line noise and is dropped.
- FCS-16, computed bit by bit: 3,204 B of code for the whole engine on the Cortex-M4, against 3,704 B with the 512 B
  table. At 115200 baud, the bitwise loop costs well under 1% of the CPU.
- Address and Control are always `FF 03`, and the Protocol field is always two bytes: the radio rejects PFC and ACFC,
  so a frame without `FF 03` is an error.
- A frame longer than MRU + 4 + 2 is dropped. A frame that stops for 500 ms is dropped, and the next byte starts a new
  one. Bad FCS, aborted, too long, and bad header all count in `ppp.rx_errors`.
- Receive works in place in a buffer of `PPP_RX_BUFFER` (602) bytes, with 14 bytes of headroom in front of the
  information field so the netif can put an Ethernet header there. The host link lends PPP the USB network adapter's
  receive buffer (`hostlink_net_frame()`), which is unused while PPP runs, so PPP's own RAM is about 90 bytes.

## 2. LCP (RFC 1661)

LCP and IPCP use the option negotiation automaton of RFC 1661 4: Closed, Stopped, Closing, Stopping, Req-Sent,
Ack-Rcvd, Ack-Sent, Opened. Initial and Starting collapse, because the layer below is the serial port, which is always
up. Restart timer 3 s, Max-Configure 10, Max-Terminate 2, Max-Failure 5 (after that, Nak'd options are rejected).

| Option | The radio asks for | The radio takes from the peer |
|---|---|---|
| 1 MRU | 584 (`PPP_MRU`: the gateway's 600-byte frames); a Nak'd value from 68 to 584 is asked for instead | any; the radio's packets longer than it are dropped |
| 2 ACCM | 0 (a Nak'd map is asked for instead) | any |
| 5 Magic-Number | a number from the start time and DMR ID | any but 0 and the radio's own (Nak'd: a looped-back line) |
| 3 Authentication, 4 Quality, 7 PFC, 8 ACFC, others | (none) | Configure-Reject |

- The MRU the radio announces is the link MTU the peer takes for its interface (pppd sets it from the MRU). The air's
  MTU (596, [dmr-data.md](dmr-data.md) 12.8) is above it, so every datagram the peer sends for the air goes in one
  transmission ([usb-network.md](usb-network.md) 3.2; the gateway's input is shared).
- A peer that Naks the radio's MRU with a value from 68 (`PPP_MIN_MRU`, the smallest IPv4 MTU, RFC 791) up to 584
  gets that value asked for in the next request (RFC 1661 5.3). A bigger or smaller one is ignored and 584 is asked
  for again, until Max-Failure. A Reject drops the option: the default 1500 then applies (RFC 1661 6.1), and the
  radio's receive buffer still takes only 584, so longer frames are dropped. The view keys (section `net_link`) are
  `ppp.mru` (what the radio receives: the MRU asked for, 1500 when rejected) and `ppp.peer_mru` (the peer's, 1500 when
  it gave none).
- While open, the radio answers Echo-Request with its magic number. Echo-Reply and Discard-Request are taken quietly.
- Terminate-Request: Terminate-Ack, IP down at once, the link ends one restart time later (the Stopping state, so
  retransmitted requests are answered).
- Code-Reject for an unknown code. Protocol-Reject (inside LCP) for an unknown protocol while LCP is open: IPv6CP,
  CCP and anything else. A Protocol-Reject of IPCP ends the link.
- Before LCP is open, every other protocol is discarded silently, as is IPv4 before IPCP is open.

## 3. IPCP (RFC 1332)

IPCP negotiates only option 3, IP-Address. The radio knows both ends, the same pair the USB network adapter's DHCP
hands out (`core/dmr/ipGateway.h`): the radio is `ipGatewayRadioIP()`, and the host is `ipGatewayHostIP()`
(12.<DMR ID>, or the control station's fixed link, `usb-network.md` 5.3). The gateway, the services, and the
addressing rules therefore behave exactly as on the adapter.

- The radio's request carries its own address. The peer's request is Ack'd when it carries the host's address and
  Nak'd to it otherwise (pppd's `noipdefault` asks for 0.0.0.0). With no address (no DMR ID), the option is rejected.
- VJ compression (option 2), the old IP-Addresses (1), and the DNS and NBNS options are rejected.
- The radio doesn't adopt a Nak of its own address, so don't give pppd a remote address.

pppd installs a host route to the radio only. The radio IDs (12.0.0.0/8) and talkgroups (225.0.0.0/8) need routes
through the PPP interface, as the adapter's DHCP option 121 gives; `tools/rfoundry_ppp.sh` adds them.

## 4. PPP as a personality of the host port

The host link (`core/hostlink/hostlink.h`) dispatches the port's bytes to one protocol at a time (idle, hotspot, CPS,
TNC, and the debug link beside them). PPP isn't one more of those modes: it's a personality of the serial port, as
the network adapter is of the USB port. While PPP has the port, the protocols run over UDP serial with their own
modes, exactly as on the adapter, so MMDVMHost over UDP gets hotspot mode while the port carries PPP.

**Entering.**

- Passive: a request that starts with `0x7E` in idle mode. pppd's first frame is an LCP Configure-Request,
  `7E FF 7D 23 C0 21 ...` (or `7E FF 03 C0 21 ...` with a narrower ACCM); no other protocol on the port starts a
  request with `0x7E` (MMDVM `0xE0`, debug link `0xA5`, CPS ASCII letters, KISS `0xC0`, NMEA `$`). LCP starts in the
  Stopped state and answers the peer's request.
- Active: debug link radio op 28 with 1 (`rfoundry_debug.py radio ppp on`), after its reply has gone out. The
  radio's Configure-Request goes out at once and every 3 s, ten times. pppd doesn't need this, because it starts PPP
  itself; a peer that waits for the other end does. The op is ignored while the USB network adapter is the
  personality.

**While in PPP.** Every byte from the port goes to PPP; the radio looks for no debug link frame on the port. The network
personality is in use (`hostlink_network_mode()`): the gateway, the radio's services, and the serial protocols over UDP
3334 (`usb-network.md` 1), so `rfoundry_debug.py --port udp:<radio IP>`, MMDVMHost over UDP and the HTTP / SNMP / TFTP /
mDNS services work as over the adapter. UDP serial bytes never reach PPP, whatever they start with. The view section
`hotspot` shows `usb.network=1` and `usb.net_up` (IPCP open), section `net_link` the `ppp.*` keys; `hostlink` is the
mode of the protocols over UDP.

**Leaving** gives the port back to the serial protocols in idle mode. A hotspot session over UDP ends with the link,
as when the USB personality switches. These events end PPP:

| Cause | Detail |
|---|---|
| LCP Terminate from the peer | pppd hanging up (`kill`, Ctrl-C): Terminate-Ack, then idle 3 s later |
| The radio's Terminate | debug link radio op 28 with 0 (over UDP while PPP runs): Terminate-Request, idle on the Terminate-Ack or after 2 tries |
| Negotiation timed out | after an active start, 10 Configure-Requests 3 s apart without an answer |
| Dead peer | 60 s without a good frame. An open link quiet for 15 s sends an LCP Echo-Request, again every 15 s; pppd always answers them, so only a peer that went away hits this |
| Another protocol | a frame that starts (the first byte after a flag) with a byte another protocol starts a request with: the debug link's `0xA5`, MMDVM's `0xE0` (hotspot allowed), the TNC's bytes, an ASCII letter (CPS). PPP frames always start with `FF`. The bytes from there on go to that protocol at once, so a CPS or the debug link on the serial port works right after pppd went away without hanging up |

A lone `0x7E` (line noise) enters passive PPP. The next request of any other protocol leaves it again, and with no
frames it ends after 60 s. Losing exactly the `FF 03 00 21` of an IPv4 frame starting with `45` ('E') would look like
a CPS request. That can happen on a lossy line and is harmless: the link ends, and pppd's echo or retry brings it back.

There's no "+++" escape: the first-byte rule covers it without a guard time.

## 5. The host side

`tools/rfoundry_ppp.sh PORT` runs pppd on the radio's serial port (root, for the interface) and adds the routes:

```
sudo tools/rfoundry_ppp.sh /dev/ttyUSB0                   # Linux
sudo tools/rfoundry_ppp.sh /dev/cu.usbserial-XXXX         # macOS (/usr/sbin/pppd)
```

It runs, in effect:

```
pppd PORT 115200 noauth local nocrtscts noipdefault nodefaultroute novj noccp \
     lcp-echo-interval 10 lcp-echo-failure 3 unit 7 updetach
route: 12.0.0.0/8 and 225.0.0.0/8 through ppp7
```

`local` and `nocrtscts`: the programming cable has no modem lines. `noccp`, `novj`: the radio refuses them anyway
(pppd copes with the rejects; the options save a round trip). Then, with the radio at 12.0.39.20 (DMR ID 10005), these
commands reach it:

```
ping 12.0.39.20
tools/rfoundry_debug.py --port udp:12.0.39.20 view
curl http://12.0.39.20/
python3 tools/rfoundry_snmp.py get 12.0.39.20 sysName.0
python3 tools/tftp_client.py ...                     # tftp.md
```

`sudo kill $(cat /var/run/ppp7.pid)` hangs up (LCP Terminate); the radio goes back to the serial protocols.

## 6. Throughput at 115200 baud

The UART carries 11,520 bytes/s each way. With ACCM 0, a frame costs 8 bytes (flags, `FF 03`, protocol, FCS) plus an
escape for each `0x7E` / `0x7D` (0.8% of random data; with the default ACCM every control character too, 12.5%).

| Traffic | On the wire | Rate |
|---|---|---|
| ping, 56 data bytes | 92 B each way | about 16 ms round trip on the line |
| TCP, MSS 536 | 584 B a segment | about 10.5 KB/s of payload one way (ACKs use the other direction) |
| TFTP, 512 B blocks in lock step | 548 B a block + 38 B ACK | about 9 KB/s: a 100 KB codeplug in about 11 s |
| HTTP screenshot (BMP, at the panel's depth) | 1 KB (128 x 64, 1 bpp), 10 KB (160 x 128, 4 bpp), 38 KB (240 x 320, 4 bpp) | about 0.1 s, 1 s, 4 s |
| Web control screen (rows as runs, 5.1) | about 2 KB (160 x 128, 4 bpp) and 5 KB (240 x 320) the first time; a redraw sends only the rows it changed, often under 200 B | about 0.2 s, 0.5 s |
| Web control receive audio (8 kHz PCM16) | 16 KB/s | doesn't fit: not offered over PPP ([tcp-services.md](tcp-services.md) 5.1) |

On a board whose UART sends by polling, the host link task is busy for the length of each frame it sends (51 ms for
a full one). The board's receive ring must hold one full frame during that time.

## 7. Cost

This table gives the flash and RAM of `CONFIG_NET_PPP=y` against the default image (built with `SOURCE_DATE_EPOCH`
fixed, so the default images compare byte for byte). Boards with USB already have the gateway and its services, so
their figure is PPP alone; on a board without USB, the gateway comes with it.

| Board, build | Flash | RAM |
|---|---:|---:|
| STM32 board with an HR_C6000, LTO off (default) | +4,368 | +88 |
| same board, `LTO=1 LTO_3P=1` | +3,948 | +104 |
| STM32 board with an HR_C5000 (its default, `LTO=1 LTO_3P=1`) | +3,816 | +88 |
| HR_C7000 board, PPP and the IP gateway, no services | +10,736 | +4,080 |

PPP itself is about 3.9 KB of flash (`ppp.o` 3,386, `hostlink_ppp.o` 480) and 90 B of RAM. The rest of the HR_C7000
row's figure is the gateway, its air side, and the network personality (`hostlink_net*.c`, 1.6 KB RAM each, the
gateway's 600 B frame). For per-service costs and each board's default network build, see the
[feature matrix](../feature-matrix.md).

## 8. Tests

- `tests/host/pppTest.c` (`make -C tests/host run-ppp`): the FCS check value; passive negotiation with pppd's options
  (PFC, ACFC, PAP rejected; the radio's ACCM Nak'd and its magic number rejected, the next request changed; a stale Ack
  ignored; the looped-back magic number Nak'd); IPCP with VJ and DNS rejected, 0.0.0.0 Nak'd to the host address;
  Max-Failure; Echo; Protocol-Reject of IPv6CP; Code-Reject; ping, UDP serial both ways, UDP to a DMR ID and back from
  the air through the real gateway; the ACCM both ways with line noise; byte-by-byte input, back-to-back flags, bad FCS,
  abort, no `FF 03`, too long, a stalled frame; Terminate both ways; the negotiation time-out, the dead peer, a live
  peer answering echoes, another protocol's request; renegotiation on an open link; the MRU Nak'd to a smaller value, a
  too big and a too small one ignored, Rejected (`test_mru`); the air MTU over PPP, the largest UDP datagram PPP takes,
  DF set, to the air whole (`test_air_mtu`). Then a seeded fuzz loop (`RFOUNDRY_FUZZ_ITERS`, default 3,000): mutated
  whole sessions in random pieces, and single packets with a good FCS mutated inside (LCP, IPCP, IPv4, IPv6CP), every
  frame the radio sends checked. Clean under `make -C tests/host SANITIZE=1` with 30,000 iterations.
- `tools/rfoundry_ppp_interop.sh`: `tests/host/pppPeer` (PPP, the gateway, TCP with a one-line HTTP server, the SNMP
  engine, the debug link's PING and VERSION on UDP 3334) on a pseudo-terminal, with pppd 2.4.9 on the other side (in a
  Linux container with `--cap-add NET_ADMIN --device /dev/ppp`). It runs passive and `--active`: ping, a 500 byte ping
  with `7E 7D 11 13` in it, HTTP, SNMP get (`tools/rfoundry_snmp.py`), `rfoundry_debug.py --port udp:` ping and version,
  IPv6CP and CCP Protocol-Rejected, pppd's hang-up ending the link cleanly.
- `test_gateway_has_netif` (`tests/hil/test_dmr_ip.py`, radio only): a build with the gateway reports `net.netif=1`.
  A board without `board.netif` still opens LCP and IPCP, but nothing the gateway sends reaches the host (no ping
  reply). This test catches that without root or pppd.
- `test_ppp_link` (`tests/hil/test_ppp_link.py`, radio only): a PPP peer in user space (`tests/hil/ppp_client.py`, no
  pppd, no root) on the debug link's serial port. LCP and IPCP open with the host at 12.<DMR ID>, a 56 and a 500 byte
  ping (every byte value, so the escapes both ways) come back, the debug link answers over UDP 3334 through the link
  with `ppp.lcp` and `ppp.ipcp` at 9, `usb.net_up=1` and no `ppp.rx_errors`, and after LCP Terminate the serial debug
  link answers again with `ppp.running=0`. Over the same link: HTTP `/status` and a 404 (the client's own minimal
  TCP), an SNMP get of `sysName.0` and `sysDescr.0`, a TFTP read of `info.txt` (`test_ppp_http`, `test_ppp_snmp`,
  `test_ppp_tftp_read`). The tests share one link and send nothing on the serial port while it runs: a debug link
  frame there ends PPP.
