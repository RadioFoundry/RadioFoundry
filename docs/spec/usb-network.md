<!-- SPDX-License-Identifier: MIT -->

# USB network personality and the IP gateway

The radio can be a USB network adapter instead of a serial port, and route IPv4 between the host and other radios
over DMR packet data. Switches: `CONFIG_USB_NETWORK` (the adapter) and `CONFIG_DMR_IP_GATEWAY` (the gateway on top of
it), both off by default (`docs/build-config.md`).

## 1. One personality at a time

The STM32F405's OTG_FS core has three IN endpoints besides EP0. CDC-ACM needs two (notification, bulk IN) and CDC-NCM
needs two, so a composite ACM + NCM device does not fit; the radio is either a serial port (PID 0x0094) or a network
adapter (PID 0x0096), with the descriptors of `docs/spec/hardware-dm1701.md` 16. Switching drops the D+ pull-up for
100 ms, so the host sees an unplug and enumerates the other personality.

- Boot is always serial. The switch is a run-time request: debug link radio op 15 (`odr_debug.py radio usb network`),
  carried out after its reply has gone out. A stored setting (and a menu item) to boot as an adapter is not built yet.
- In network mode the serial protocols stay reachable over UDP: datagrams to the radio's own address, port 3334,
  carry the same byte stream as the serial port (debug link, MMDVM, CPS), and the replies go back to the address and
  port that last sent there, in datagrams of at most 512 bytes. `odr_debug.py --port udp:<radio IP>` uses it, and so
  can MMDVMHost (`Protocol=udp`, `ModemAddress=<radio IP>`, `ModemPort=3334`). The MMDVM session's DTR rule does not
  apply (there is no DTR); the 20 s idle timeout does.
- Windows 10 binds NCM through Microsoft OS 1.0 descriptors (string 0xEE "MSFT100", vendor code 0x47, extended compat
  ID "WINNCM" for interface 0); Windows 11, Linux and macOS bind it by class.
- macOS sometimes never activates the link after a cold boot in network mode: with the device configured and no frame
  from the host within 5 s, the radio re-enumerates once.

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
12.<DMR ID>/32 by DHCP, the radio is that address xor 1, radios are 12.x.y.z and talkgroups 225.x.y.z.

- Host to air: IP for a radio ID or talkgroup, and datagrams to the raw port, go to the air ops the DMR data
  integration registers with `hostlink_net_set_air()` (radio ID, IP to air as SAP 4, raw data as SAP 10). They are
  called from the host link task and must not block: they queue the packet for the radio task and return false when
  it can't go now (busy, not a DMR channel, receive only). Refusals are counted (`net.to_air_refused`); nothing is
  retried, as with any lost IP packet.
- Air to host: the data service hands every packet it receives to `hostlink_net_packet()` (its `dmrDataEnv_t.packet`
  hook). Two packets wait for the host link task, which delivers SAP 4 as IP (ICMP, UDP, SCTP), SAP 3 as UDP, and
  anything else as raw data on port 40078; a packet that finds the IN endpoint busy goes on the next pass, and packets
  are dropped while the link is down.
- Debug link view, section `hotspot`: `usb.network usb.net_up net.host_ip net.radio_ip net.frames_in net.to_air
  net.to_air_refused net.packets_air net.packets_up net.packets_dropped`.

## 4. Tests

- Host: `tests/host/hostlinkNetTest.c` (the gateway with a fake interface and fake air ops: unicast, multicast and raw
  to air, refusals, UDP serial both ways with chunking and a busy endpoint, IP and short data from the air, link down),
  and the gateway's own `ipGatewayTest*` with pcaps checked by tshark.
- HIL: `tests/hil/test_usb_network.py` (`--allow-usb-switch`).
