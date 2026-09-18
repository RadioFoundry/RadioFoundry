<!-- SPDX-License-Identifier: MIT -->

# USB network mode

USB network mode is an optional build. In it the radio becomes a USB network adapter for the computer, and routes IPv4
packets to other radios over DMR packet data. Built and host-tested, not tested on air.

## Building it

```
make BOARD=dm1701 CONFIG_DMR_IP_GATEWAY=y
```

`CONFIG_DMR_IP_GATEWAY` turns on `CONFIG_USB_NETWORK` with it. In a default build, **Tools > USB network** says "USB
network isn't in this build".

## Switching the radio to network mode

The radio always starts as a USB serial port. To switch it, open **Tools > USB network** (**OK**, **5**, **4**). The
page says which mode USB is in and what changes. Focus "Switch USB to network" and press **OK**. The radio says "USB
is a network adapter now" and returns to Home. While USB is a network adapter, qdmr, hotspot hosts and the debug link
over serial don't reach the radio.

To get back to serial, open **Tools > USB network** again and choose "Switch USB to serial" ("USB is a serial port
again"), or restart the radio. The Tools row shows the current mode (serial or network).

Developers can also switch over the debug link:

```
tools/odr_debug.py radio usb network
```

The radio drops off USB for a moment and comes back as a network adapter (USB ID 1FC9:0096). Linux, macOS and Windows
11 use their built-in drivers; Windows 10 binds it automatically too. Restarting the radio, or the switch in Tools, returns it to serial mode.

## Addresses

The computer gets its address from the radio by DHCP:

| Who | Address |
|---|---|
| Your computer | 12.x.y.z, from your DMR ID (for example ID 3101234 is 12.47.82.50) |
| The radio | Your computer's address with the last bit flipped (12.47.82.51) |
| Another radio | 12.x.y.z from its DMR ID |
| A talkgroup | 225.x.y.z from the talkgroup number |

Packets to another radio's or a talkgroup's address go out over the air on the current DMR channel. Packets from other
radios arrive on the network adapter. DMR packet data is much slower than any wired network. A packet
that can't go out within 2 s is dropped.

## Shorter packets and confirmed delivery

UDP packets to other radios go with a compressed header (5 to 9 bytes instead of 28, ETSI's "UDP/IP header
compression"), unless one of their ports is in 4000-4099, where MOTOTRBO text, location and presence servers listen.
That saves one or two data blocks (60-120 ms on air) per packet. Radios that can't read it are rare; for them, or to
compress those ports too:

```
tools/odr_debug.py data ip-settings off        # always the full header
tools/odr_debug.py data ip-settings all        # compress every UDP packet
```

Packets to a radio can also go as confirmed data: the other radio acknowledges each one, and the radio sends again
what didn't arrive (up to 4 times by default). Name up to two destinations (DMR IDs) or UDP ports:

```
tools/odr_debug.py data ip-settings auto rules id=3101234 port=161 tries=4
```

Talkgroup packets are never confirmed. A packet to a radio that doesn't answer is sent every try before it is dropped,
so confirm only where it helps. The settings are saved in the radio.

## Serial tools in network mode

The debug link, the MMDVM hotspot protocol and the CPS protocol stay reachable over UDP port 3334 at the radio's
address:

```
tools/odr_debug.py --port udp:12.47.82.51 version
```

MMDVMHost connects the same way (`Protocol=udp`, `ModemAddress=<radio address>`, `ModemPort=3334`). qdmr finds radios
only by their serial USB ID, so program the radio in serial mode.
