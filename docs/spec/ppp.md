<!-- SPDX-License-Identifier: MIT -->

# PPP on the serial host link

IPv4 over PPP on the radio's serial host port, into the same IP gateway and services as the USB network adapter
(`usb-network.md`). It is the IP link of radios whose only computer link is a USB-serial cable on a UART at 115200 baud. On radios
with USB it can also run on the USB serial port, as a fallback when a host doesn't bring up the network adapter.

Switch `CONFIG_NET_PPP` (`docs/build-config.md`), on for boards without USB (their only IP link, with the gateway and
every service) and off on the USB radios ([feature-matrix.md](../feature-matrix.md)): new, host tested and interop tested with pppd, not yet tried on a radio. It needs `CONFIG_HOSTLINK` and `CONFIG_DMR_IP_GATEWAY`; the gateway needs a link to
the host, `CONFIG_USB_NETWORK` or `CONFIG_NET_PPP`, and follows PPP by default.

Written from RFC 1661 (PPP, LCP), RFC 1662 (HDLC-like framing) and RFC 1332 (IPCP) only.

| File | What |
|---|---|
| `core/net/ppp.c`, `ppp.h` | framing, LCP and IPCP; no RTOS calls, no hardware |
| `core/hostlink/hostlink_ppp.c` | the netif (`board_t.netif`) between PPP and the gateway, the engine's I/O |
| `core/hostlink/hostlink.c` | PPP as a personality of the host port: entering, feeding, leaving |
| `tests/host/pppTest.c` | host test and fuzz loop; `tests/host/pppPeer.c` the same code on a pseudo-terminal |
| `tools/odr_ppp.sh` | host side: pppd on the radio's serial port, routes |
| `tools/odr_ppp_interop.sh` | interop test against pppd (Linux, root) |

## 1. Framing (RFC 1662)

- Flag `0x7E` before and after every frame (one flag between back-to-back frames is enough on receive; we always send
  a leading flag). Control escape `0x7D`, the next byte XOR `0x20`. `0x7D 0x7E` aborts a frame.
- ACCM both ways. We send every control character escaped until the peer's ACCM is known, and LCP always so (RFC 1662
  7.1). We ask the peer for ACCM 0; until LCP is open, and when the peer rejects the option, a bare control character
  from the peer is line noise and is dropped.
- FCS-16, computed bit by bit: 3,204 B of code for the whole engine on the Cortex-M4 against 3,704 B with the 512 B
  table. At 115200 baud the bitwise loop costs well under 1% of the CPU.
- Address and Control are always `FF 03` and the Protocol field always two bytes: we reject PFC and ACFC, so a frame
  without `FF 03` is an error.
- A frame longer than MRU + 4 + 2 is dropped. A frame that stops for 500 ms is dropped: the next byte starts a new
  one. Bad FCS, aborted, too long and bad header all count in `ppp.rx_errors`.
- Receive is in place in a buffer of `PPP_RX_BUFFER` (602) bytes, 14 bytes of headroom in front of the information
  field so the netif can put an Ethernet header there. The host link lends PPP the USB network adapter's receive
  buffer (`hostlink_net_frame()`), unused while PPP runs: PPP's own RAM is about 90 bytes.

## 2. LCP (RFC 1661)

The option negotiation automaton of RFC 1661 4 for LCP and IPCP: Closed, Stopped, Closing, Stopping, Req-Sent,
Ack-Rcvd, Ack-Sent, Opened (Initial and Starting collapse: the layer below is the serial port, always up). Restart
timer 3 s, Max-Configure 10, Max-Terminate 2, Max-Failure 5 (then Nak'd options are rejected).

| Option | We ask for | We take from the peer |
|---|---|---|
| 1 MRU | 584 (the gateway's 600 byte frames) | any; our packets longer than it are dropped |
| 2 ACCM | 0 (a Nak'd map is asked for instead) | any |
| 5 Magic-Number | a number from the start time and DMR ID | any but 0 and ours (Nak'd: a looped-back line) |
| 3 Authentication, 4 Quality, 7 PFC, 8 ACFC, others | (none) | Configure-Reject |

- Echo-Request answered with our magic number while open. Echo-Reply and Discard-Request are taken quietly.
- Terminate-Request: Terminate-Ack, IP down at once, the link ends one restart time later (the Stopping state, so
  retransmitted requests are answered).
- Code-Reject for an unknown code. Protocol-Reject (inside LCP) for an unknown protocol while LCP is open: IPv6CP,
  CCP and anything else. A Protocol-Reject of IPCP ends the link.
- Before LCP is open every other protocol is discarded silently, as is IPv4 before IPCP is open.

## 3. IPCP (RFC 1332)

Only option 3, IP-Address. The radio knows both ends, the same pair the USB network adapter's DHCP hands out
(`core/dmr/ipGateway.h`): the radio is `ipGatewayRadioIP()`, the host `ipGatewayHostIP()` (12.<DMR ID>, or the
control station's fixed link, `usb-network.md` 5.3). So the gateway, the services and the addressing rules behave
exactly as on the adapter.

- Our request carries our address. The peer's request is Ack'd when it carries the host's address and Nak'd to it
  otherwise (pppd's `noipdefault` asks for 0.0.0.0). With no address yet (no DMR ID) the option is rejected.
- VJ compression (option 2), the old IP-Addresses (1), the DNS and NBNS options are rejected.
- We don't adopt a Nak of our own address: don't give pppd a remote address.

pppd installs a host route to the radio only. The radio IDs (12.0.0.0/8) and talkgroups (225.0.0.0/8) need routes
through the PPP interface, as the adapter's DHCP option 121 gives: `tools/odr_ppp.sh` adds them.

## 4. PPP as a personality of the host port

The host link (`core/hostlink/hostlink.h`) dispatches the port's bytes to one protocol at a time (idle, hotspot, CPS,
TNC, and the debug link beside them). PPP isn't one more of those modes: it is a personality of the serial port, as
the network adapter is of the USB one. While PPP has the port, the protocols run over UDP serial with their own
modes, exactly as on the adapter, so MMDVMHost over UDP gets hotspot mode while the port carries PPP.

**Entering.**

- Passive: a request that starts with `0x7E` in idle mode. pppd's first frame is an LCP Configure-Request,
  `7E FF 7D 23 C0 21 ...` (or `7E FF 03 C0 21 ...` with a narrower ACCM); no other protocol on the port starts a
  request with `0x7E` (MMDVM `0xE0`, debug link `0xA5`, CPS ASCII letters, KISS `0xC0`, NMEA `$`). LCP starts in the
  Stopped state and answers the peer's request.
- Active: debug link radio op 28 with 1 (`odr_debug.py radio ppp on`), after its reply has gone out. Our
  Configure-Request goes out at once and every 3 s, ten times. Not needed with pppd, which starts PPP itself; useful
  with a peer that waits for the other end. Refused (ignored) while the USB network adapter is the personality.

**While in it.** Every byte from the port goes to PPP; no debug link frame is looked for on the port. The network
personality is in use (`hostlink_network_mode()`): the gateway, the radio's services, and the serial protocols over
UDP 3334 (`usb-network.md` 1), so `odr_debug.py --port udp:<radio IP>`, MMDVMHost over UDP and the HTTP / SNMP / TFTP
/ mDNS services work as over the adapter. UDP serial bytes never reach PPP, whatever they start with. The view
section `hotspot` shows `usb.network=1`, `usb.net_up` (IPCP open) and the `ppp.*` keys; `hostlink` is the mode of
the protocols over UDP.

**Leaving** gives the port back to the serial protocols in idle mode; a hotspot session over UDP ends with the link,
as when the USB personality switches.

| Cause | Detail |
|---|---|
| LCP Terminate from the peer | pppd hanging up (`kill`, Ctrl-C): Terminate-Ack, then idle 3 s later |
| Our Terminate | debug link radio op 28 with 0 (over UDP while PPP runs): Terminate-Request, idle on the Terminate-Ack or after 2 tries |
| Negotiation timed out | after an active start, 10 Configure-Requests 3 s apart without an answer |
| Dead peer | 60 s without a good frame. An open link quiet for 15 s sends an LCP Echo-Request, again every 15 s; pppd always answers them, so only a peer that went away hits this |
| Another protocol | a frame that starts (the first byte after a flag) with a byte another protocol starts a request with: the debug link's `0xA5`, MMDVM's `0xE0` (hotspot allowed), the TNC's bytes, an ASCII letter (CPS). PPP frames always start with `FF`. The bytes from there on go to that protocol at once, so a CPS or the debug link on the serial port works right after pppd went away without hanging up |

A lone `0x7E` (line noise) enters passive PPP; the next request of any other protocol leaves it again, and with no
frames it ends after 60 s. Losing exactly the `FF 03 00 21` of an IPv4 frame starting with `45` ('E') would look like
a CPS request: possible on a lossy line, harmless (the link ends; pppd's echo or retry brings it back).

No "+++" escape: the first-byte rule covers it without a guard time.

## 5. The host side

`tools/odr_ppp.sh PORT` runs pppd on the radio's serial port (root, for the interface) and adds the routes:

```
sudo tools/odr_ppp.sh /dev/ttyUSB0                   # Linux
sudo tools/odr_ppp.sh /dev/cu.usbserial-XXXX         # macOS (/usr/sbin/pppd)
```

It runs, in effect:

```
pppd PORT 115200 noauth local nocrtscts noipdefault nodefaultroute novj noccp \
     lcp-echo-interval 10 lcp-echo-failure 3 unit 7 updetach
route: 12.0.0.0/8 and 225.0.0.0/8 through ppp7
```

`local` and `nocrtscts`: the programming cable has no modem lines. `noccp`, `novj`: we refuse them anyway (pppd
copes with the rejects; the options save a round trip). Then, with the radio at 12.0.39.20 (DMR ID 10005):

```
ping 12.0.39.20
tools/odr_debug.py --port udp:12.0.39.20 view
curl http://12.0.39.20/
python3 tools/odr_snmp.py get 12.0.39.20 sysName.0
python3 tools/tftp_client.py ...                     # tftp.md
```

`sudo kill $(cat /var/run/ppp7.pid)` hangs up (LCP Terminate); the radio goes back to the serial protocols.

## 6. Throughput at 115200 baud

The UART carries 11,520 bytes/s each way. With ACCM 0 a frame costs 8 bytes (flags, `FF 03`, protocol, FCS) plus an
escape for each `0x7E` / `0x7D` (0.8% of random data; with the default ACCM every control character too, 12.5%).

| Traffic | On the wire | Rate |
|---|---|---|
| ping, 56 data bytes | 92 B each way | about 16 ms round trip on the line |
| TCP, MSS 536 | 584 B a segment | about 10.5 KB/s of payload one way (ACKs use the other direction) |
| TFTP, 512 B blocks in lock step | 548 B a block + 38 B ACK | about 9 KB/s: a 100 KB codeplug in about 11 s |
| HTTP screenshot (BMP) | about 40 KB | about 4 s |

On a board whose UART sends by polling, the host link task is busy for the length of each frame it sends (51 ms for
a full one). The board's receive ring must hold one full frame while that happens.

## 7. Cost

Flash and RAM of `CONFIG_NET_PPP=y` against the default image (agent/ppp at f52d6b9, `SOURCE_DATE_EPOCH` fixed so the
default images compare byte for byte with main). The USB radios already have the gateway and its services, so the
figure is PPP alone; on a radio without USB the gateway comes with it.

| Board, build | Flash | RAM | Fits |
|---|---:|---:|---|
| DM-1701, LTO off (default) | +4,368 | +88 | no: 3,680 B free, 688 B short |
| DM-1701, `LTO=1 LTO_3P=1` | +3,948 | +104 | yes: 20,424 B left |
| MD-380 (its default, `LTO=1 LTO_3P=1`) | +3,816 | +88 | no: 2,228 B free, 1,588 B short |
| DM-32UV, PPP and the IP gateway, no services | +10,736 | +4,080 | yes: RAM 95.3%, 7,792 B left |

PPP itself is about 3.9 KB flash (`ppp.o` 3,386, `hostlink_ppp.o` 480) and 90 B RAM; the rest of the DM-32UV's
figure is the gateway, its air side and the network personality (`hostlink_net*.c`, 1.6 KB RAM each, the gateway's
600 B frame). The DM-32UV's per-service costs and its default network build are in
[feature-matrix.md](../feature-matrix.md), "Whole images".

## 8. Tests

- `tests/host/pppTest.c` (`make -C tests/host run-ppp`): the FCS check value; passive negotiation with pppd's
  options (PFC, ACFC, PAP rejected; our ACCM Nak'd and our magic number rejected, the next request changed; a stale
  Ack ignored; the looped-back magic number Nak'd); IPCP with VJ and DNS rejected, 0.0.0.0 Nak'd to the host address;
  Max-Failure; Echo; Protocol-Reject of IPv6CP; Code-Reject; ping, UDP serial both ways, UDP to a DMR ID and back
  from the air through the real gateway; the ACCM both ways with line noise; byte-by-byte input, back-to-back flags,
  bad FCS, abort, no `FF 03`, too long, a stalled frame; Terminate both ways; the negotiation time-out, the dead
  peer, a live peer answering echoes, another protocol's request; renegotiation on an open link. Then a seeded fuzz
  loop (`ODR_FUZZ_ITERS`, default 3,000): mutated whole sessions in random pieces, and single packets with a good
  FCS mutated inside (LCP, IPCP, IPv4, IPv6CP), every frame the radio sends checked. Clean under
  `make -C tests/host SANITIZE=1` with 30,000 iterations.
- `tools/odr_ppp_interop.sh`: `tests/host/pppPeer` (PPP, the gateway, TCP with a one-line HTTP server, the SNMP
  engine, the debug link's PING and VERSION on UDP 3334) on a pseudo-terminal, pppd 2.4.9 on the other side
  (Ubuntu 24.04 container on the CI VM, `--cap-add NET_ADMIN --device /dev/ppp`). Passes passive and `--active`:
  ping, a 500 byte ping with `7E 7D 11 13` in it, HTTP, SNMP get (`tools/odr_snmp.py`), `odr_debug.py --port udp:`
  ping and version, IPv6CP and CCP Protocol-Rejected, pppd's hang-up ending the link cleanly.
- Not tried yet: a radio, macOS pppd (it needs root).
