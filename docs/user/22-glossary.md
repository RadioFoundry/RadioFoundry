<!-- SPDX-License-Identifier: MIT -->
<!-- Generated from docs/glossary.json by tools/glossary.py. Edit the data file, not this page. -->
<!-- glossary: off -->

# Glossary

Terms the user guide uses. In the web version, hover over, focus or tap a dotted term in the text to see its meaning. The developer glossary ([dev/glossary.md](../dev/glossary.md)) has the protocol and hardware terms.

## DMR

| Term | Meaning |
|---|---|
| <a id="t-all-call"></a>All call | DMR talkgroup 16777215, heard by every radio on the slot. |
| <a id="t-ambe-2"></a>AMBE+2 (AMBE) | DVSI's voice codec used by DMR. Not part of RadioFoundry. Taken from each radio's stock firmware. [More](../dev/codec.md) |
| <a id="t-call-alert"></a>Call alert | A DMR page that makes another radio ring. |
| <a id="t-color-code"></a>Color code (CC) | A number from 0 to 15 a DMR channel uses. Radios only hear calls with their color code. |
| <a id="t-confirmed-data"></a>Confirmed data | DMR packet data the receiving radio acknowledges. What didn't arrive is sent again. |
| <a id="t-dmr"></a>DMR | Digital Mobile Radio, the ETSI digital radio standard (TS 102 361). |
| <a id="t-hang-time"></a>Hang time | How long after a call a repeater or radio keeps the call's talkgroup ready for a reply. |
| <a id="t-pi-header"></a>PI header (PI headers, PI) | A DMR header that announces encryption with a changing key. RadioFoundry never plays such calls. |
| <a id="t-private-call"></a>Private call | A DMR call to one radio. |
| <a id="t-radio-check"></a>Radio check | A DMR request another radio answers automatically, to show it is on and in range. |
| <a id="t-sms"></a>SMS | Short message service: text messages. On DMR, short data carries them. |
| <a id="t-talker-alias"></a>Talker alias | A short text, usually a callsign and name, sent along with DMR voice. |
| <a id="t-talkgroup"></a>Talkgroup (TG, TGs) | A DMR group number. Everyone listening to it hears calls to it. |
| <a id="t-timeslot"></a>Timeslot (TS, TS1, TS2) | One of the two time slots of a DMR channel, TS1 and TS2. |

## RF and hardware

| Term | Meaning |
|---|---|
| <a id="t-am"></a>AM | Amplitude modulation: speech varies the carrier's strength. Airband uses it. |
| <a id="t-ani"></a>ANI | Automatic number identification: the caller's ID sent as tones in an analog call. |
| <a id="t-bandwidth"></a>Bandwidth | The width of an FM channel: 12.5 kHz (narrow) or 25 kHz (wide). |
| <a id="t-ctcss"></a>CTCSS | Continuous tone coded squelch system: a sub-audible tone on FM that opens the squelch only for the right signal. |
| <a id="t-dcs"></a>DCS | Digital coded squelch: a sub-audible digital code on FM that opens the squelch only for the right signal. |
| <a id="t-dfu"></a>DFU | Device Firmware Upgrade: the USB protocol of the radio's bootloader. |
| <a id="t-dtmf"></a>DTMF | Dual tone multi frequency: the touch tones a keypad sends, in the audio. |
| <a id="t-dual-watch"></a>Dual watch | Listening on two channels, A and B, and talking on the selected one. With one receiver the radio alternates between them. |
| <a id="t-fm"></a>FM | Frequency modulation: the analog voice mode. Speech moves the carrier's frequency. |
| <a id="t-gps"></a>GPS | Global positioning system: the satellite receiver that gives the radio its position. |
| <a id="t-lcd"></a>LCD | Liquid crystal display: the radio's screen. |
| <a id="t-led"></a>LED | Light-emitting diode: the radio's status light. |
| <a id="t-nmea"></a>NMEA | The text format GPS receivers use to report position and time. |
| <a id="t-rf"></a>RF | Radio frequency: the signal in the air, as opposed to audio or data inside the radio. |
| <a id="t-rssi"></a>RSSI | Received signal strength indicator: how strong the received signal is, in dBm. |
| <a id="t-rx"></a>RX | Receive. |
| <a id="t-sfr"></a>SFR | Single frequency repeater. It repeats a call from one timeslot on the other timeslot of the same frequency. In MMDVM SFR the hotspot repeats, turned on by its host. In standalone SFR a channel set up in the codeplug repeats on its own, or talks through one. [More](../spec/sfr.md) |
| <a id="t-split-duplex"></a>Split duplex | Transmitting on one frequency and receiving on another at the same time. Repeaters do, and so does the duplex hotspot setup. [More](../spec/split-duplex.md) |
| <a id="t-squelch"></a>Squelch | The level a received signal must exceed before the radio plays it. |
| <a id="t-tot"></a>TOT | Transmit timeout: the longest transmission the radio allows. |
| <a id="t-tx"></a>TX | Transmit. |
| <a id="t-uhf"></a>UHF | Ultra high frequency: 300 MHz to 3 GHz. The amateur 70 cm band is UHF. |
| <a id="t-usb"></a>USB | Universal Serial Bus. The radio uses it for the debug link, the network mode and firmware updates. |
| <a id="t-vfo"></a>VFO | Variable frequency oscillator: tuning any frequency by hand instead of a stored channel. |
| <a id="t-vhf"></a>VHF | Very high frequency: 30 to 300 MHz. The amateur 2 m band is VHF. |
| <a id="t-vox"></a>VOX | Voice-operated transmit. |

## Vendor terms

| Term | Meaning |
|---|---|
| <a id="t-ars"></a>ARS | Automatic Registration Service: a radio tells a presence server it is on the air. |
| <a id="t-basic-privacy"></a>Basic privacy (Basic Privacy, Enhanced privacy, Enhanced Privacy) | A fixed voice scrambling from Motorola and Anytone radios. Not encryption. |
| <a id="t-control-station"></a>Control station | A radio a dispatch program on a computer uses to reach the fleet over the air. |
| <a id="t-cps"></a>CPS | Customer programming software: the program that writes the codeplug. RadioFoundry uses qdmr. [More](04-programming.md) |
| <a id="t-lrrp"></a>LRRP (LIP) | Location Request and Response Protocol: a dispatcher asks a radio for its position. Motorola's position format; the standard has LIP. |
| <a id="t-mototrbo"></a>MOTOTRBO | Motorola's DMR product line. Its extensions (ARS, TMS, LRRP, Capacity Plus) are not in the ETSI standard. |
| <a id="t-tms"></a>TMS | Text message service: Motorola's text message format on DMR. The standard's own short data uses other formats. |
| <a id="t-tyt"></a>TYT | A Chinese maker of DMR radios. |

## Amateur and network

| Term | Meaning |
|---|---|
| <a id="t-aprs"></a>APRS | Automatic Packet Reporting System: positions and short messages shared between amateur stations. [More](13-aprs.md) |
| <a id="t-brandmeister"></a>BrandMeister | A worldwide amateur DMR network of linked servers and talkgroups. |
| <a id="t-cw-id"></a>CW ID | A station's callsign sent in Morse code. A standalone SFR repeater channel sends one in FM every few minutes after traffic (Settings > Talk & listen > SFR CW ID). [More](14-hotspot.md) |
| <a id="t-dmr-id"></a>DMR ID (RadioID, DMR IDs) | The number that identifies your radio on DMR. Amateur IDs come from RadioID.net. |
| <a id="t-hotspot"></a>Hotspot | A small DMR modem that connects radios nearby to a DMR network over the internet. [More](14-hotspot.md) |
| <a id="t-igate"></a>iGate | A station that passes APRS packets between the air and the APRS-IS internet network. |
| <a id="t-kiss"></a>KISS | A simple protocol between a computer's APRS program and a radio modem (TNC). |
| <a id="t-mdns"></a>mDNS | Multicast DNS: finding a device on a local network by a `.local` name. [More](../spec/mdns.md) |
| <a id="t-mmdvm"></a>MMDVM | Multi-Mode Digital Voice Modem: the modem protocol MMDVMHost speaks. [More](../spec/mmdvm-protocol.md) |
| <a id="t-rigctld"></a>rigctld | Hamlib's network rig control protocol, used by loggers and SDR programs. |
| <a id="t-snmp"></a>SNMP | Simple Network Management Protocol: reading a device's state over the network. [More](../spec/snmp.md) |
| <a id="t-tftp"></a>TFTP | Trivial File Transfer Protocol: a minimal way to copy files to and from a device. [More](../spec/tftp.md) |

## RadioFoundry and firmware

| Term | Meaning |
|---|---|
| <a id="t-codeplug"></a>Codeplug | The radio's programming: channels, zones, contacts, talkgroup lists, DMR ID. [More](../dev/cps-and-codeplug.md) |
| <a id="t-contact"></a>Contact | A person in the codeplug: name and DMR ID. |
| <a id="t-csv"></a>CSV | Comma separated values: a table as plain text, as programming software exports it. |
| <a id="t-digital-monitor"></a>Digital monitor | Playing every talkgroup on the timeslot (promiscuous mode; "hear all" in older RadioFoundry builds). |
| <a id="t-id"></a>ID | Identifier: a number or name that picks out one thing. |
| <a id="t-listen-list"></a>Listen list (RX group list) | The talkgroups a channel listens to. The codeplug calls it the RX group list. |
| <a id="t-monitor"></a>Monitor | Opening the squelch (FM) or hearing all talkgroups (DMR) while a key is held. |
| <a id="t-ok"></a>OK | The key that selects or confirms. |
| <a id="t-one-shot"></a>One-shot | A talk target used for one call and its reply window. |
| <a id="t-pc"></a>PC | Personal computer. |
| <a id="t-ptt"></a>PTT | Push to talk: the key that transmits. |
| <a id="t-qdmr"></a>qdmr | A free, open source codeplug editor for DMR radios. [More](04-programming.md) |
| <a id="t-radiofoundry"></a>RadioFoundry | RadioFoundry, this firmware. |
| <a id="t-recent"></a>Recent | RadioFoundry's list of stations heard and calls made. |
| <a id="t-reply-window"></a>Reply window | The time after a call during which PTT answers it. |
| <a id="t-stock-firmware"></a>Stock firmware | The firmware a radio shipped with, before RadioFoundry. Each RadioFoundry radio takes its voice codec from it. |
| <a id="t-talk-target"></a>Talk target | Where PTT goes: a talkgroup, a person or all call. |
| <a id="t-temporary-change"></a>Temporary change | A change that lasts until the channel changes or you press Back on Home. Hold OK to keep it. |
| <a id="t-ui"></a>UI | User interface: the screen, keys and menus. |
| <a id="t-yaml"></a>YAML | A text file format. qdmr stores its codeplug as YAML. |
| <a id="t-zone"></a>Zone | A named list of channels. |
