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

## Serial tools in network mode

The debug link, the MMDVM hotspot protocol and the CPS protocol stay reachable over UDP port 3334 at the radio's
address:

```
tools/odr_debug.py --port udp:12.47.82.51 version
```

MMDVMHost connects the same way (`Protocol=udp`, `ModemAddress=<radio address>`, `ModemPort=3334`). qdmr finds radios
only by their serial USB ID, so program the radio in serial mode.

## Monitoring the radio with SNMP

Builds with the SNMP agent (`CONFIG_SNMP`, on with the USB network) answer SNMP version 2c requests at the radio's
address, port 161. Any SNMP manager works; the repository has a small one:

```
tools/odr_snmp.py walk 12.47.82.51                 # everything the radio reports
tools/odr_snmp.py get 12.47.82.51 odrBatteryVoltage.0 odrChannelName.0
tools/odr_snmp.py traps                            # events as they happen (needs port 162: root on Linux)
```

The radio reports its identity (DMR ID, radio name, model, firmware), battery, channel, frequencies, power, the DMR
call in progress and the last one heard, message and data counters, ARS state and the USB network counters. The
objects are described in `docs/spec/ODR-MIB.txt`; load it into your manager to see names instead of numbers.

While USB is a network adapter, the radio also sends events to your computer (port 162): power-up, calls starting
and ending, text messages received, emergencies, ARS changes and low battery.

The read community is `public`. Changing settings needs a write community, which is off until you set one:

```
tools/odr_debug.py snmp write MySecret             # over the serial port, before switching to network mode
tools/odr_snmp.py -c MySecret set 12.47.82.51 sysName.0 s:N0CALL
tools/odr_debug.py snmp status                     # the agent's settings and counters
```

Other radios can query this radio over the air at its own 12.x.y.z address (read only unless you allow writes).
Events over the air go only to a manager radio you name (`tools/odr_debug.py snmp set rf_manager <DMR ID>`), at most
one a minute apart from emergencies. Nothing is encrypted; SNMP community names travel as plain text.
