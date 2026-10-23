<!-- SPDX-License-Identifier: MIT -->

# USB Network Mode

In USB network mode the radio is a USB network adapter for the computer. It routes IPv4 packets to other radios over
DMR packet data. It serves a status page, rig control, KISS, TFTP and SNMP to the computer. It is in the images for
radios with USB. IP over DMR to another radio is host-tested only. Which services have been checked on each radio is
in the [Radio reference](23-radios.md). A build made without it (`CONFIG_DMR_IP_GATEWAY=n`) says "USB network: not in
this build" in **Tools > USB network**.

## Switching the radio to network mode

The radio starts as a USB serial port unless **USB network at start** is on (below). To switch it, open **Tools > USB
network** (**OK**, **6**, **4**). The page says which mode USB is in and what changes. Focus "Switch USB to network"
and press **OK**. The radio says "USB is a network adapter now" and returns to Home, where the status line shows IP.
An up and a down arrow appear beside it once the computer has set the adapter up and IP can flow. While USB is a network adapter,
qdmr, hotspot hosts and the debug link over serial don't reach the radio.

To get back to serial, open **Tools > USB network** again and choose "Switch USB to serial" ("USB is a serial port
again"), or restart the radio. The Tools row shows the current mode (serial or network).

Developers can also switch over the debug link:

```
tools/rfoundry_debug.py radio usb network
```

The radio drops off USB for a moment and comes back as a network adapter (USB ID 1FC9:0096). Linux, macOS, Windows 10
and 11 use their built-in drivers. If the computer doesn't set the adapter up within 10 s, the radio goes back to
serial.

## Starting in network mode

**Settings > Connections > USB network at start** makes the radio start as a network adapter. If the computer doesn't
set the adapter up within 10 s of plugging in, the radio becomes a serial port again until it is restarted. This way
you can always reach it to turn the setting off. The radio waits for a computer. A radio switched on away from a
computer is still a network adapter when you plug it in later.

## Addresses

The computer gets its address from the radio by DHCP:

| Who | Address |
|---|---|
| Your computer | 12.x.y.z, from your DMR ID (for example ID 3101234 is 12.47.82.50) |
| The radio | Your computer's address with the last bit flipped (12.47.82.51) |
| Another radio | 12.x.y.z from its DMR ID |
| A talkgroup | 225.x.y.z from the talkgroup number |

Packets to another radio's or a talkgroup's address go out over the air on the current DMR channel. Packets from other
radios arrive on the network adapter. DMR packet data is much slower than any wired network. A packet that can't
go out within 2 s is dropped. The fixed addresses of [control station mode](#control-station-for-dispatch-software)
replace the first two rows.

## Serial tools in network mode

The debug link, the MMDVM hotspot protocol and the CPS protocol stay reachable over UDP port 3334 at the radio's
address:

```
tools/rfoundry_debug.py --port udp:12.47.82.51 version
```

MMDVMHost connects the same way (`Protocol=udp`, `ModemAddress=<radio address>`, `ModemPort=3334`). qdmr finds radios
only by their serial USB ID, so program the radio in serial mode.

## Finding the radio by name

The radio answers Multicast DNS (Bonjour, Avahi) on the network adapter. Its name is `rfoundry-<DMR ID>.local`, for
example `rfoundry-3101234.local`. It advertises its services. A browser for Bonjour services lists it by your call sign,
the radio model and your DMR ID, for example "N0CALL XR-100UV (3101234)":

```
ping rfoundry-3101234.local
dns-sd -B _rfoundry-debug._udp            # macOS; avahi-browse -r _rfoundry-debug._udp on Linux
tools/rfoundry_debug.py --port udp:rfoundry-3101234.local version
```

| Service | Port | What |
|---|---|---|
| `_rfoundry-debug._udp` | 3334 | the debug link (and the other serial protocols) over UDP |
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
| 80 | A status page. Open `http://<radio address>/` in a browser. It shows the channel, frequency, signal and battery, and the radio's screen. Programs can read `/status` (JSON) and the debug link's sections at `/api/view?section=core` |
| 8001 | KISS over TCP for APRS programs (APRSdroid, Xastir, YAAC). Set the program's TNC to "KISS over TCP", the radio's address, port 8001. Turn on **KISS on USB** in Tools > APRS first. The radio sends only packets from your own callsign. On a radio that can't receive APRS ([Radio reference](23-radios.md)), programs see only what they send |
| 4532 | Hamlib rig control for loggers, SDR and satellite programs. Choose "Hamlib NET rigctl" with the radio's address and port 4532, or run `rigctl -m 2 -r <radio address>:4532 f`. Programs can read and set the frequency, read the mode and the S meter, and key the transmitter. Setting the frequency of a channel switches to the VFO. Transmit works only from your computer, at most 3 minutes per keying |

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

Use your radio's address instead of 12.47.82.51. Then give the radio a server's IP address. The radio can't look up
names. `dig +short rotate.aprs2.net` gives some addresses. You can also give a range in km for the messages and packets it
receives:

```
curl "http://12.47.82.51/api/aprsis?server=44.31.12.34:14580&range=50"
curl "http://12.47.82.51/api/aprsis"              # the state
curl "http://12.47.82.51/api/aprsis?server=off"   # stop
```

The radio logs in with its APRS callsign and the passcode computed from it. Your beacons go to APRS-IS when **KISS on
USB** and "to APRS-IS" are on (`tools/rfoundry_debug.py aprs set to_is 1`).

## Linking audio: AllStarLink and DVSwitch

The radio speaks the USRP voice protocol on UDP port 34001, the protocol AllStarLink's USRP channel driver and DVSwitch
Analog_Bridge use. A node on your computer hears what the radio receives and talks through it, as an RF link:

- AllStarLink: `rxchannel = usrp/<radio address>:34001:32001` in the node's `rpt.conf` section.
- Analog_Bridge: `txPort = 34001`, `rxPort = 32001`, the radio's address as the destination.

While the radio receives a signal (the squelch open, or a DMR call playing), it sends the audio to your computer's
UDP port 32001, with the squelch as the keyup flag. When the program keys up, the radio transmits its audio on the
current channel: on FM through the radio's own transmit audio (its tone and deviation limit apply), on DMR as a call
to the channel's contact. The radio stops transmitting when the program unkeys, stops sending, or after 3 minutes.
Received audio follows the volume knob. `tools/usrp_client.py` is a small test program
(`tools/usrp_client.py <radio address> tone 1000 3` transmits a tone for 3 seconds).

Which radios can transmit FM this way is in the [Radio reference](23-radios.md); on the others USRP carries DMR only.

## Programming over the network

The radio runs a TFTP server at its address. Any TFTP client works:

```
curl tftp://rfoundry-3101234.local/codeplug.bin -o backup.bin       # read the codeplug
curl -T edited.bin tftp://rfoundry-3101234.local/codeplug.bin        # write it
curl tftp://rfoundry-3101234.local/info.txt                          # board, firmware, IDs, addresses
```

`codeplug.bin` is the same 716,384-byte image `tools/cps_client.py read` makes. A file from either one can be
written with either one. A write is checked before anything is stored. The radio checks the size first, then that
the file is a codeplug.
A wrong file, a transfer that stops half way, or a cable pulled during the transfer leaves the codeplug as it was. A
power cut while the new codeplug is being stored is finished at the next power on. The radio keeps its own quick
keys, as with the CPS, and re-reads the codeplug right after the write.

The DMR ID database (the names shown for IDs that aren't contacts) has its own file:

```
curl tftp://rfoundry-3101234.local/iddb.bin -o iddb.bin             # read the ID database
curl -T iddb.bin tftp://rfoundry-3101234.local/iddb.bin              # write it
```

`iddb.bin` holds the database alone. The radio drops its old database as the new one starts arriving and shows names
again once the whole file is in. A transfer that stops leaves no database until the next try, never a broken one.
This works without a staging area, so also on radios where `codeplug.bin` can't be written.

A `codeplug.bin` saved by the Codeplug Editor with **Save > For USB Network** also carries the ID database when the
codeplug has one. The radio takes the database from it only when the client sends the file's size, as `curl` does.
Otherwise, and with any older `codeplug.bin`, the radio keeps the database it has. The codeplug part is written as
above.

To stop writes over the network (`codeplug.bin` and `iddb.bin`), turn off **Settings > Connections > TFTP codeplug
write**. Reading stays possible.

## Monitoring the radio with SNMP

The radio answers SNMP version 2c requests at its address, port 161 (not in builds that leave SNMP out for flash room). Any SNMP manager works. The repository has a
small one:

```
tools/rfoundry_snmp.py walk 12.47.82.51                 # everything the radio reports
tools/rfoundry_snmp.py get 12.47.82.51 rfoundryBatteryVoltage.0 rfoundryChannelName.0
tools/rfoundry_snmp.py traps                            # events as they happen (needs port 162: root on Linux)
```

The radio reports its identity (DMR ID, radio name, model, firmware), battery, channel, frequencies, power, the DMR
call in progress and the last one heard, message and data counters, ARS state and the USB network counters. The
objects are described in `docs/spec/RFOUNDRY-MIB.txt`. Load it into your manager to see names instead of numbers.

While USB is a network adapter, the radio also sends events to your computer (port 162): power-up, calls starting
and ending, text messages received, emergencies, ARS changes and low battery.

The read community is `public`. Changing settings needs a write community, which is off until you set one:

```
tools/rfoundry_debug.py snmp write MySecret             # over the serial port, before switching to network mode
tools/rfoundry_snmp.py -c MySecret set 12.47.82.51 sysName.0 s:N0CALL
tools/rfoundry_debug.py snmp status                     # the agent's settings and counters
```

Other radios can query this radio over the air at its own 12.x.y.z address (read only unless you allow writes).
Events over the air go only to a manager radio you name (`tools/rfoundry_debug.py snmp set rf_manager <DMR ID>`). Apart
from emergencies, they go at most once a minute. Nothing is encrypted. SNMP community names travel as plain text.

## Network over the programming cable (PPP)

Radios without USB ([Radio reference](23-radios.md)) can get the same network over their programming cable with PPP. A
radio's image includes PPP when the radio has no USB and runs DMR. Other builds need `CONFIG_NET_PPP=y`. Run this on the computer as root. pppd comes with macOS and most Linux systems.

```
sudo tools/rfoundry_ppp.sh /dev/ttyUSB0                 # Linux
sudo tools/rfoundry_ppp.sh /dev/cu.usbserial-XXXX       # macOS
```

The radio switches to PPP by itself when pppd starts. It gives the computer the same address as in USB network mode.
The services above work the same, only slower (about 10 KB/s at 115200 baud). `sudo tools/rfoundry_ppp.sh --stop` hangs
up, and the cable goes back to programming and the debug link. The details are in `docs/spec/ppp.md`.

## Control station for dispatch software

Dispatch software (TRBOnet, SmartPTT and similar) can use the radio as its control station. The computer then sends
and receives the fleet's messages, locations and registrations through the radio.

1. Turn on **USB network at start** and restart the radio, or switch it in **Tools > USB network**.
2. Turn on **PC answers data**. While the computer is connected, text messages, location requests and registration
   answers sent to your radio's DMR ID go to the computer only. The radio no longer answers them itself, doesn't put
   messages in its inbox and doesn't register with a presence server. It still confirms data packets that ask for a
   confirmation, and still answers call alerts and radio checks. Unplug the computer and the radio answers for itself
   again.
3. If the software expects fixed addresses, set **USB address** to 192.168.10.x. The radio is then 192.168.10.1 and
   the computer gets 192.168.10.2. Unplug and plug in the cable so the computer picks up the new address.

In the dispatch software, set:

| Item | Value |
|---|---|
| Control station address | The radio's address: 192.168.10.1, or 12.x.y.z from your DMR ID with the last bit flipped |
| Radio network | 12.0.0.0/8 |
| Talkgroup network | 225.0.0.0/8 |
| DMR ID | Your radio's DMR ID |

What the computer sends goes over the air from your radio's DMR ID. Only one radio per computer can use the fixed
addresses.

## Shorter packets and confirmed delivery

UDP packets to other radios go with a compressed header (ETSI's "UDP/IP header compression"). The header is 5 to 9
bytes instead of 28. Packets with a port in 4000-4099 keep the full header, because MOTOTRBO text, location and
presence servers listen there.
That saves one or two data blocks (60-120 ms on air) per packet. Radios that can't read it are rare. For them, or to
compress those ports too:

```
tools/rfoundry_debug.py data ip-settings off        # always the full header
tools/rfoundry_debug.py data ip-settings all        # compress every UDP packet
```

Packets to a radio can also go as confirmed data. The other radio acknowledges each packet, and the radio sends again
what didn't arrive. By default it makes 4 tries in all. Name up to two destinations (DMR IDs) or UDP ports:

```
tools/rfoundry_debug.py data ip-settings auto rules id=3101234 port=161 tries=4
```

Talkgroup packets are never confirmed. A packet to a radio that doesn't answer is sent every try before it is dropped,
so confirm only where it helps. The settings are saved in the radio.
