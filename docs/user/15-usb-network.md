<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# USB network mode

In USB network mode, the radio is a USB network adapter for the computer. It routes IPv4 packets to other radios over
DMR packet data, and serves a status page, rig control, KISS, TFTP, and SNMP to the computer. It's in the images for
radios with USB. For which radio has which service, see the [feature matrix](../feature-matrix.md). A build made
without it (`CONFIG_DMR_IP_GATEWAY=n`) shows "USB network: not in this build" in **Tools > USB network**.

For how the link, the addresses, and each service work, see [USB network](../spec/usb-network.md).

## Switching the radio to network mode

The radio starts as a USB serial port, unless **USB network at start** is on
([Starting in network mode](#starting-in-network-mode)).

1. Open **Tools > USB network** (**OK**, **6**, **4**). The page says which mode USB is in and what changes.
2. Focus "Switch USB to network" and press **OK**. The radio says "USB is a network adapter now" and returns to Home,
   where the status line shows IP. An up and a down arrow appear beside it once the computer sets up the adapter and
   IP can flow.

While USB is a network adapter, qdmr, hotspot hosts, and the debug link over serial don't reach the radio.

To get back to serial, open **Tools > USB network** again and choose "Switch USB to serial" ("USB is a serial port
again"), or restart the radio. The **Tools** row shows the current mode (serial or network).

Developers can also switch over the debug link:

```sh
tools/rfoundry_debug.py radio usb network
```

The radio drops off USB for a moment and comes back as a network adapter (USB ID 1FC9:0096). Linux, macOS, and
Windows 10 and 11 use their built-in drivers. If the computer doesn't set up the adapter within 10 seconds, the radio
goes back to serial.

## Starting in network mode

**Settings > Connections > USB network at start** makes the radio start as a network adapter. If the computer doesn't
set up the adapter within 10 seconds of plugging in, the radio becomes a serial port again until you restart it. This
way, you can always reach it to turn the setting off. The radio waits for a computer: a radio that you turn on away
from a computer is still a network adapter when you plug it in later.

## Addresses

The computer gets its address from the radio by DHCP:

| Who | Address |
|---|---|
| Your computer | 12.x.y.z, from your DMR ID (for example, ID 9991234 is 12.152.116.66) |
| The radio | Your computer's address with the last bit flipped (12.152.116.67) |
| Another radio | 12.x.y.z, from its DMR ID |
| A talkgroup | 225.x.y.z, from the talkgroup number |

Packets to another radio's or a talkgroup's address go out over the air on the current DMR channel. Packets from other
radios arrive on the network adapter. DMR packet data is much slower than any wired network. A packet that can't go
out within 2 seconds is dropped. The fixed addresses of
[control station mode](#control-station-for-dispatch-software) replace the first two rows.

## Serial tools in network mode

The debug link, the MMDVM hotspot protocol, and the CPS protocol stay reachable over UDP port 3334 at the radio's
address:

```sh
tools/rfoundry_debug.py --port udp:12.152.116.67 version
```

MMDVMHost connects the same way (`Protocol=udp`, `ModemAddress=<radio address>`, `ModemPort=3334`). qdmr finds radios
only by their serial USB ID, so program the radio in serial mode.

The project's own tools need no address and no change on the radio:

| Tool | In network mode |
|---|---|
| `tools/rfoundry_debug.py`, `tools/cps_client.py`, `rfoundry.py` | With no `--port` and no serial radio plugged in, they find the radio on the network link and use it |
| `stub/stubtool.py --rfoundry-enter --write-app ...` (no port) | Updates the firmware and restarts the radio, which comes back in network mode |
| The web installer and the Codeplug Editor | They ask the radio to become a serial port for the job. Some browsers do this without showing anything. Otherwise, press **Switch the radio to USB serial**: a tab opens on the radio's own page (`http://12.255.255.254/`, every radio's address on its own USB link) and closes again. Then connect. The radio returns to network mode when it restarts |

If the button can't reach the radio (the tab shows an error because the computer hasn't set up the adapter, or sends
12.x addresses elsewhere), switch on the radio: **Tools > USB network**.

## Finding the radio by name

The radio answers Multicast DNS (Bonjour, Avahi) on the network adapter. Its name is `rfoundry-<DMR ID>.local`, for
example `rfoundry-9991234.local`. A browser for Bonjour services lists the radio by your call sign, the radio model,
and your DMR ID, for example "Q0TST XR-100UV (9991234)":

```sh
ping rfoundry-9991234.local
dns-sd -B _rfoundry-debug._udp            # macOS; avahi-browse -r _rfoundry-debug._udp on Linux
tools/rfoundry_debug.py --port udp:rfoundry-9991234.local version
```

For the services the radio advertises, see [mDNS](../spec/mdns.md).

## Services on the radio

The radio answers at its own address on these TCP ports, over the USB cable only, never over the air. For details,
see [TCP services](../spec/tcp-services.md).

| Port | Service |
|---|---|
| 80 | The radio's front panel. Open `http://<radio address>/` in a browser. It shows the radio with its live screen and LEDs, and the channel, frequency, signal, and battery. On a mobile radio, the page also draws its hand microphone. Click a key or use the keyboard. Side and top keys also have a labeled button outside the radio's edge, and the arrows over the channel knob turn it one step for each click (hold to keep turning). Keyboard keys: digits, `*`, and `#`; the arrows; Enter for select (green key or MENU); Esc or Backspace for back (red key or BACK); `[` and `]` (or Page Up and Page Down) turn the channel knob; hold Space to talk. The page lists the keyboard keys your radio has beside the picture. **Listen** plays what the radio receives, on radios with network audio ([Linking audio](#linking-audio-allstarlink-and-dvswitch)). Over the programming cable ([PPP](#network-over-the-programming-cable-ppp)), the page has no **Listen**, because the cable is too slow for audio. Keys work only from the computer that the radio is plugged into. One page at a time controls the radio; others show the status only. Programs can read `/status` (JSON) and the debug link's sections at `/api/view?section=core` |
| 8001 | KISS over TCP for APRS programs (APRSdroid, Xastir, YAAC). Set the program's TNC to "KISS over TCP", the radio's address, and port 8001. Turn on **KISS on USB** in **Tools > APRS** first. The radio sends only packets from your own callsign. On a radio that can't receive APRS ([Radio reference](23-radios.md)), programs see only what they send |
| 4532 | Hamlib rig control for loggers, SDR, and satellite programs. Choose "Hamlib NET rigctl" with the radio's address and port 4532, or run `rigctl -m 2 -r <radio address>:4532 f`. Programs can read and set the frequency, read the mode and the S meter, and key the transmitter. Setting the frequency of a channel switches to the VFO. Transmit works only from your computer, for at most 3 minutes each time |

### APRS-IS from the radio

The radio can connect to an APRS-IS server itself and send your position there.

1. Let your computer pass the radio's traffic on to the internet:

   ```sh
   # macOS (en0: your internet connection)
   sudo sysctl -w net.inet.ip.forwarding=1
   echo "nat on en0 from 12.152.116.67 to any -> (en0)" | sudo pfctl -ef -
   # Linux (eth0: your internet connection, usb0: the radio)
   sudo sysctl -w net.ipv4.ip_forward=1
   sudo iptables -t nat -A POSTROUTING -s 12.152.116.67/32 -o eth0 -j MASQUERADE
   sudo iptables -A FORWARD -i usb0 -o eth0 -j ACCEPT
   sudo iptables -A FORWARD -i eth0 -o usb0 -m state --state RELATED,ESTABLISHED -j ACCEPT
   ```

   Use your radio's address instead of 12.152.116.67.
2. Give the radio a server's IP address. The radio can't look up names; `dig +short rotate.aprs2.net` gives some
   addresses. You can also give a range in km for the messages and packets it receives:

   ```sh
   curl "http://12.152.116.67/api/aprsis?server=44.31.12.34:14580&range=50"
   curl "http://12.152.116.67/api/aprsis"              # the state
   curl "http://12.152.116.67/api/aprsis?server=off"   # stop
   ```

The radio logs in with its APRS callsign and the passcode computed from it. Your beacons go to APRS-IS when **KISS on
USB** and "to APRS-IS" are on (`tools/rfoundry_debug.py aprs set to_is 1`).

## Linking audio: AllStarLink and DVSwitch

The radio speaks the USRP voice protocol on UDP port 34001, the protocol that AllStarLink's USRP channel driver and
DVSwitch Analog_Bridge use. A node on your computer hears what the radio receives and talks through it, as an RF link:

- AllStarLink: `rxchannel = usrp/<radio address>:34001:32001` in the node's `rpt.conf` section.
- Analog_Bridge: `txPort = 34001`, `rxPort = 32001`, and the radio's address as the destination.

While the radio receives a signal (the squelch open, or a DMR call playing), it sends the audio to your computer's UDP
port 32001, with the squelch as the keyup flag. When the program keys up, the radio transmits its audio on the current
channel: on FM through the radio's own transmit audio (its tone and deviation limit apply), on DMR as a call to the
channel's contact. The radio stops transmitting when the program unkeys, stops sending, or after 3 minutes. Received
audio follows the volume knob. `tools/usrp_client.py` is a small test program
(`tools/usrp_client.py <radio address> tone 1000 3` transmits a tone for 3 seconds).

For which radios can transmit FM this way, see the [feature matrix](../feature-matrix.md). On the others, USRP
carries DMR only.

## Programming over the network

The radio runs a TFTP server at its address. Any TFTP client works:

```sh
curl tftp://rfoundry-9991234.local/codeplug.bin -o backup.bin       # read the codeplug
curl -T edited.bin tftp://rfoundry-9991234.local/codeplug.bin        # write it
curl tftp://rfoundry-9991234.local/info.txt                          # board, firmware, IDs, addresses
```

`codeplug.bin` is the same image that `tools/cps_client.py read` makes, so a file from either one can be written with
either one. The radio checks a write before it stores anything: first the size, then that the file is a codeplug. A
wrong file, a transfer that stops partway, or a cable pulled during the transfer leaves the codeplug as it was. If the
power fails while the radio stores the new codeplug, the radio finishes at the next power-on. The radio keeps its own
quick keys, as with the CPS, and reads the codeplug again right after the write.

The DMR ID database (the names shown for IDs that aren't contacts) has its own file:

```sh
curl tftp://rfoundry-9991234.local/iddb.bin -o iddb.bin             # read the ID database
curl -T iddb.bin tftp://rfoundry-9991234.local/iddb.bin              # write it
```

`iddb.bin` holds the database alone. The radio drops its old database as the new one starts to arrive, and shows names
again once the whole file is in. A transfer that stops leaves no database until the next try, never a broken one.
This works without a staging area, so it also works on radios where `codeplug.bin` can't be written.

A `codeplug.bin` saved by the Codeplug Editor with **Save > For USB Network** also carries the ID database when the
codeplug has one. The radio takes the database from it only when the client sends the file's size, as `curl` does.
Otherwise, and with any older `codeplug.bin`, the radio keeps the database it has. The codeplug part is written as
described earlier.

To stop writes over the network (`codeplug.bin` and `iddb.bin`), turn off **Settings > Connections > TFTP codeplug
write**. You can still read.

For details, see [TFTP](../spec/tftp.md).

## Monitoring the radio with SNMP

The radio answers SNMP version 2c requests at its address, unless the build leaves SNMP out for flash room. Any SNMP
manager works. The repository has a small one:

```sh
tools/rfoundry_snmp.py walk 12.152.116.67               # everything the radio reports
tools/rfoundry_snmp.py get 12.152.116.67 rfoundryBatteryVoltage.0 rfoundryChannelName.0
tools/rfoundry_snmp.py traps                            # events as they happen (needs port 162: root on Linux)
```

The radio reports its identity, battery, channel, frequencies, power, calls, message and data counters, ARS state, and
the USB network counters. It also sends events to your computer, such as calls, messages, emergencies, and low
battery. To see names instead of numbers, load `docs/spec/RFOUNDRY-MIB.txt` into your manager.

The read community is `public`. Changing settings needs a write community, which is off until you set one:

```sh
tools/rfoundry_debug.py snmp write MySecret             # over the serial port, before switching to network mode
tools/rfoundry_snmp.py -c MySecret set 12.152.116.67 sysName.0 s:Q0TST
tools/rfoundry_debug.py snmp status                     # the agent's settings and counters
```

Nothing is encrypted: SNMP community names travel as plain text. For the objects, the events, and queries from other
radios over the air, see [SNMP](../spec/snmp.md).

## Network over the programming cable (PPP)

Radios without USB ([Radio reference](23-radios.md)) can get the same network over their programming cable with PPP.
A radio's image includes PPP when the radio has no USB and runs DMR. Other builds need `CONFIG_NET_PPP=y`. Run this on
the computer as root. pppd comes with macOS and most Linux systems.

```sh
sudo tools/rfoundry_ppp.sh /dev/ttyUSB0                 # Linux
sudo tools/rfoundry_ppp.sh /dev/cu.usbserial-XXXX       # macOS
```

The radio switches to PPP by itself when pppd starts, and gives the computer the same address as in USB network mode.
The services work the same, only slower (about 10 KB/s at 115200 baud). The front panel page works without its
**Listen** button, and its screen takes up to a few seconds to update on a large color display. To hang up, run
`sudo tools/rfoundry_ppp.sh --stop`; the cable goes back to programming and the debug link. For details, see
[PPP](../spec/ppp.md).

## Control station for dispatch software

Dispatch software (TRBOnet, SmartPTT, and similar) can use the radio as its control station. The computer then sends
and receives the fleet's messages, locations, and registrations through the radio.

1. Turn on **USB network at start** and restart the radio, or switch it in **Tools > USB network**.
2. Turn on **PC answers data**. While the computer is connected, text messages, location requests, and registration
   answers sent to your radio's DMR ID go to the computer only. The radio no longer answers them itself, doesn't put
   messages in its inbox, and doesn't register with a presence server. It still confirms data packets that ask for a
   confirmation, and still answers call alerts and radio checks. When you unplug the computer, the radio answers for
   itself again.
3. If the software expects fixed addresses, set **USB address** to 192.168.10.x. The radio is then 192.168.10.1, and
   the computer gets 192.168.10.2. Unplug the cable and plug it in again, so that the computer picks up the new
   address.

In the dispatch software, set these values:

| Item | Value |
|---|---|
| Control station address | The radio's address: 192.168.10.1, or 12.x.y.z from your DMR ID with the last bit flipped |
| Radio network | 12.0.0.0/8 |
| Talkgroup network | 225.0.0.0/8 |
| DMR ID | Your radio's DMR ID |

What the computer sends goes over the air from your radio's DMR ID. Only one radio for each computer can use the fixed
addresses.

## Confirmed delivery

Packets to a radio can go as confirmed data. The other radio acknowledges each packet, and the radio sends again what
didn't arrive. By default, it makes 4 tries in all. Name up to two destinations (DMR IDs) or UDP ports:

```sh
tools/rfoundry_debug.py data ip-settings auto rules id=9991234 port=161 tries=4
```

Talkgroup packets are never confirmed. A packet to a radio that doesn't answer is sent on every try before it's
dropped, so confirm only where it helps. The radio saves the settings.

The radio also shortens the header of UDP packets to other radios, and keeps each packet to one transmission on the
air. A full-size packet keeps the channel for about 3 seconds (about 4 when it's sent confirmed). Small packets are
quicker and more likely to get through on a weak channel, because a lost burst loses the whole packet. There's nothing
to set on the computer. For header compression and packet sizes, see [USB network](../spec/usb-network.md).

## Other networks through a gateway radio

By default, the radio carries only addresses that stand for a DMR ID: 12.x.y.z for a radio and 225.x.y.z for a
talkgroup. It drops a packet from the computer to any other address.

If another radio in your fleet is a gateway to a network (for example, a commercial control station on a dispatch PC),
give the radio that radio's DMR ID in one of these places:

- **Settings > Connections > Data services > IP gateway** (0 is off).
- The [Codeplug Editor](04-programming.md).
- The debug link:

  ```sh
  tools/rfoundry_debug.py data ip-settings auto rules gateway=9990500
  tools/rfoundry_debug.py data ip-settings auto rules gateway=0      # off again
  ```

  The command sets every IP setting at once, so repeat any `id=`, `port=`, or `tries=` you use. The radio saves the
  setting.

Then add a route on the computer for that network through the radio's address. For example, on Linux with the radio as
`usb0`:

```sh
sudo ip route add 10.20.0.0/16 via 12.152.116.67 dev usb0   # the radio's address (see Addresses)
```

Packets to that network go over the air to the gateway radio, with the real destination address in the packet.
Answers come back to your radio's DMR ID with the sender's real address, and the radio passes them to the computer.
For how the gateway handles headers and the time to live, see [USB network](../spec/usb-network.md).
