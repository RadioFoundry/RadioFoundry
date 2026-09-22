<!-- SPDX-License-Identifier: MIT -->

# USB network mode

In USB network mode the radio becomes a USB network adapter for the computer, and routes IPv4 packets to other radios
over DMR packet data. It is in the standard builds for radios with USB. Built and host-tested, not tested on air. A
build made without it (`CONFIG_DMR_IP_GATEWAY=n`) says "USB network isn't in this build" in **Tools > USB network**.

## Switching the radio to network mode

The radio starts as a USB serial port unless **USB network at start** is on (below). To switch it, open **Tools > USB network** (**OK**, **5**, **4**). The
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

## Serial tools in network mode

The debug link, the MMDVM hotspot protocol and the CPS protocol stay reachable over UDP port 3334 at the radio's
address:

```
tools/odr_debug.py --port udp:12.47.82.51 version
```

MMDVMHost connects the same way (`Protocol=udp`, `ModemAddress=<radio address>`, `ModemPort=3334`). qdmr finds radios
only by their serial USB ID, so program the radio in serial mode.

## Starting in network mode

**Settings > Connections > USB network at start** makes the radio start as a network adapter. If the computer doesn't
set the adapter up within 10 s of plugging in, the radio becomes a serial port again until it is restarted, so you can
always reach it to turn the setting off. The radio waits for a computer: switched on away from one, it is still a
network adapter when you plug it in later.

## Control station for dispatch software

Dispatch software (RadioDesk, TRBOnet, SmartPTT and similar) can use the radio as its control station: the computer
sends and receives the fleet's messages, locations and registrations through the radio.

1. Turn on **USB network at start** and restart the radio, or switch it in **Tools > USB network**.
2. Turn on **PC answers data**. While the computer is connected, text messages, location requests and registration
   answers sent to your radio's DMR ID go to the computer only. The radio no longer answers them itself, doesn't put
   messages in its inbox and doesn't register with a presence server. It still confirms data packets that ask for a
   confirmation, and still answers call alerts and radio checks. Unplug the computer and the radio answers for itself
   again.
3. If the software expects fixed addresses, set **USB link address** to 192.168.10.x: the radio is 192.168.10.1 and
   the computer gets 192.168.10.2. Unplug and plug in the cable so the computer picks up the new address.

In the dispatch software, set the control station's address to the radio's address (192.168.10.1, or 12.x.y.z from
your DMR ID with the last bit flipped), the radio network to 12.0.0.0/8 and the talkgroup network to 225.0.0.0/8, and
your radio's DMR ID. What the computer sends goes over the air from your radio's DMR ID. Only one radio per computer
can use the fixed addresses.
