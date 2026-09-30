<!-- SPDX-License-Identifier: MIT -->

# USB network mode

In USB network mode the radio is a USB network adapter for the computer. It routes IPv4 packets to other radios over
DMR packet data, and serves a status page, rig control, KISS, TFTP and SNMP to the computer. It is in the images for
the DM-1701 and the MD-380 (the DM-32UV has no USB). The switch, the address from the computer and ping are tested on the DM-1701
with macOS; IP over DMR to another radio and the services are host-tested only. A build made without
it (`CONFIG_DMR_IP_GATEWAY=n`) says "USB network isn't in this build" in **Tools > USB network**.

## Switching the radio to network mode

The radio starts as a USB serial port unless **USB network at start** is on (below). To switch it, open **Tools > USB
network** (**OK**, **5**, **4**). The page says which mode USB is in and what changes. Focus "Switch USB to network" and press **OK**. The radio says "USB
is a network adapter now" and returns to Home. While USB is a network adapter, qdmr, hotspot hosts and the debug link
over serial don't reach the radio.

To get back to serial, open **Tools > USB network** again and choose "Switch USB to serial" ("USB is a serial port
again"), or restart the radio. The Tools row shows the current mode (serial or network).

Developers can also switch over the debug link:

```
tools/odr_debug.py radio usb network
```

The radio drops off USB for a moment and comes back as a network adapter (USB ID 1FC9:0096). Linux, macOS, Windows 10
and 11 use their built-in drivers. If the computer doesn't set the adapter up within 10 s, the radio goes back to
serial.

## Starting in network mode

**Settings > Connections > USB network at start** makes the radio start as a network adapter. If the computer doesn't
set the adapter up within 10 s of plugging in, the radio becomes a serial port again until it is restarted, so you can
always reach it to turn the setting off. The radio waits for a computer: switched on away from one, it is still a
network adapter when you plug it in later.

## Addresses

The computer gets its address from the radio by DHCP:

| Who | Address |
|---|---|
| Your computer | 12.x.y.z, from your DMR ID (for example ID 3101234 is 12.47.82.50) |
| The radio | Your computer's address with the last bit flipped (12.47.82.51) |
| Another radio | 12.x.y.z from its DMR ID |
| A talkgroup | 225.x.y.z from the talkgroup number |

Packets to another radio's or a talkgroup's address go out over the air on the current DMR channel. Packets from other
radios arrive on the network adapter. DMR packet data is much slower than any wired network: a packet that can't
go out within 2 s is dropped. The fixed addresses of [control station mode](#control-station-for-dispatch-software)
replace the first two rows.

## Serial tools in network mode

The debug link, the MMDVM hotspot protocol and the CPS protocol stay reachable over UDP port 3334 at the radio's
address:

```
tools/odr_debug.py --port udp:12.47.82.51 version
```

MMDVMHost connects the same way (`Protocol=udp`, `ModemAddress=<radio address>`, `ModemPort=3334`). qdmr finds radios
only by their serial USB ID, so program the radio in serial mode.

## Finding the radio by name

The radio answers Multicast DNS (Bonjour, Avahi) on the network adapter: it is `odr-<DMR ID>.local`, for example
`odr-3101234.local`, and it advertises its services, so a browser for Bonjour services lists it as
"N0CALL DM-1701 (3101234)" (your call sign, the radio, your DMR ID):

```
ping odr-3101234.local
dns-sd -B _odr-debug._udp            # macOS; avahi-browse -r _odr-debug._udp on Linux
tools/odr_debug.py --port udp:odr-3101234.local version
```

| Service | Port | What |
|---|---|---|
| `_odr-debug._udp` | 3334 | the debug link (and the other serial protocols) over UDP |
| `_mmdvm._udp` | 3334 | the MMDVM modem, for MMDVMHost |
| `_tftp._udp` | 69 | the codeplug over TFTP (below) |
| `_snmp._udp` | 161 | the SNMP agent (below) |
| `_http._tcp` | 80 | the status page |
| `_kiss-tnc._tcp` | 8001 | KISS over TCP |
| `_rigctld._tcp` | 4532 | Hamlib rig control |

Each one carries the board, the DMR ID, the call sign and the firmware version in its TXT record.

## Services on the radio

The radio answers at its own address on these TCP ports, over the USB cable only, never over the air.

| Port | Service |
|---|---|
| 80 | A status page: open `http://<radio address>/` in a browser. It shows the channel, frequency, signal and battery, and the radio's screen. Programs can read `/status` (JSON) and the debug link's sections at `/api/view?section=core` |
| 8001 | KISS over TCP for APRS programs (APRSdroid, Xastir, YAAC): set the program's TNC to "KISS over TCP", the radio's address, port 8001. Turn on **KISS on USB** in Tools > APRS first. The radio sends only packets from your own callsign. The DM-1701 can't receive APRS, so programs see only what they send |
| 4532 | Hamlib rig control for loggers, SDR and satellite programs: choose "Hamlib NET rigctl" with the radio's address and port 4532, or `rigctl -m 2 -r <radio address>:4532 f`. Programs can read and set the frequency (a channel switches to the VFO), read the mode and the S meter, and key the transmitter. Transmit works only from your computer, at most 3 minutes per keying |

### APRS-IS from the radio

The radio can connect to an APRS-IS server itself and send your position there. Your computer must pass the radio's
traffic on to the Internet:

```
# macOS (en0: your Internet connection)
sudo sysctl -w net.inet.ip.forwarding=1
echo "nat on en0 from 12.47.82.51 to any -> (en0)" | sudo pfctl -ef -
# Linux (eth0: your Internet connection, usb0: the radio)
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 12.47.82.51/32 -o eth0 -j MASQUERADE
sudo iptables -A FORWARD -i usb0 -o eth0 -j ACCEPT
sudo iptables -A FORWARD -i eth0 -o usb0 -m state --state RELATED,ESTABLISHED -j ACCEPT
```

Use your radio's address instead of 12.47.82.51. Then give the radio a server's IP address (it can't look up names;
`dig +short rotate.aprs2.net` gives some) and, if you like, a range in km for the messages and packets it receives:

```
curl "http://12.47.82.51/api/aprsis?server=44.31.12.34:14580&range=50"
curl "http://12.47.82.51/api/aprsis"              # the state
curl "http://12.47.82.51/api/aprsis?server=off"   # stop
```

The radio logs in with its APRS callsign and the passcode computed from it. Your beacons go to APRS-IS when **KISS on
USB** and "to APRS-IS" are on (`tools/odr_debug.py aprs set to_is 1`).

## Programming over the network

The radio runs a TFTP server at its address. Any TFTP client works:

```
curl tftp://odr-3101234.local/codeplug.bin -o backup.bin       # read the codeplug
curl -T edited.bin tftp://odr-3101234.local/codeplug.bin        # write it
curl tftp://odr-3101234.local/info.txt                          # board, firmware, IDs, addresses
```

`codeplug.bin` is the same 716,384-byte image `tools/cps_client.py read` makes, so a file from either one can be
written with either one. A write is checked before anything is stored: the size, then that the file is a codeplug.
A wrong file, a transfer that stops half way, or a cable pulled during the transfer leaves the codeplug as it was; a
power cut while the new codeplug is being stored is finished at the next power on. The radio keeps its own quick
keys, as with the CPS, and re-reads the codeplug right after the write.

To stop writes over the network, turn off **Settings > Connections > TFTP codeplug write**. Reading stays possible.

## Monitoring the radio with SNMP

The radio answers SNMP version 2c requests at its address, port 161. Any SNMP manager works; the repository has a small one:

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
what didn't arrive, 4 tries in all by default. Name up to two destinations (DMR IDs) or UDP ports:

```
tools/odr_debug.py data ip-settings auto rules id=3101234 port=161 tries=4
```

Talkgroup packets are never confirmed. A packet to a radio that doesn't answer is sent every try before it is dropped,
so confirm only where it helps. The settings are saved in the radio.
