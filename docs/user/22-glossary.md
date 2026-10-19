<!-- SPDX-License-Identifier: MIT -->

# Glossary

| Term | Meaning |
|---|---|
| All call | DMR talkgroup 16777215, heard by every radio on the slot |
| AMBE+2 | DVSI's voice codec used by DMR. Not part of ODR. Taken from each radio's stock firmware |
| APRS | Automatic Packet Reporting System: positions and short messages shared between amateur stations |
| ARS | Automatic Registration Service: a radio tells a presence server it is on the air |
| Bandwidth | The width of an FM channel: 12.5 kHz (narrow) or 25 kHz (wide) |
| Basic privacy | A fixed voice scrambling from Motorola and Anytone radios. Not encryption |
| Call alert | A DMR page that makes another radio ring |
| Codec | See AMBE+2 |
| Codeplug | The radio's programming: channels, zones, contacts, talkgroup lists, DMR ID |
| Color code (CC) | A number from 0 to 15 a DMR channel uses. Radios only hear calls with their color code |
| Confirmed data | DMR packet data the receiving radio acknowledges. What didn't arrive is sent again |
| Contact | A person in the codeplug: name and DMR ID |
| Control station | A radio a dispatch program on a computer uses to reach the fleet over the air |
| CPS | Customer programming software: the program that writes the codeplug. ODR uses qdmr |
| CTCSS, DCS | Sub-audible tones and codes on FM that open the squelch only for the right signal |
| DFU | Device Firmware Upgrade: the USB protocol of the radio's bootloader |
| DMR | Digital Mobile Radio, the ETSI digital radio standard |
| DMR ID | The number that identifies your radio on DMR |
| Dual watch | Alternating between two channels to hear both |
| Hang time | How long after a call a repeater or radio keeps the call's talkgroup ready for a reply |
| Digital monitor | Playing every talkgroup on the timeslot (promiscuous mode; "hear all" in older ODR builds) |
| Hotspot | A small DMR modem that connects radios nearby to a DMR network over the internet |
| iGate | A station that passes APRS packets between the air and the APRS-IS internet network |
| KISS | A simple protocol between a computer's APRS program and a radio modem (TNC) |
| Listen list | The talkgroups a channel listens to. The codeplug calls it the RX group list |
| LRRP | Location Request and Response Protocol: a dispatcher asks a radio for its position |
| mDNS | Multicast DNS: finding a device on a local network by a `.local` name |
| MMDVM | Multi-Mode Digital Voice Modem: the modem protocol MMDVMHost speaks |
| Monitor | Opening the squelch (FM) or hearing all talkgroups (DMR) while a key is held |
| One-shot | A talk target used for one call and its reply window |
| PI header | A DMR header that announces encryption with a changing key. ODR never plays such calls |
| Private call | A DMR call to one radio |
| qdmr | A free, open source codeplug editor for DMR radios |
| Radio check | A DMR request another radio answers automatically, to show it is on and in range |
| Recent | ODR's list of stations heard and calls made |
| Reply window | The time after a call during which **PTT** answers it |
| rigctld | Hamlib's network rig control protocol, used by loggers and SDR programs |
| SFR | Single frequency repeater. It repeats a call from one timeslot on the other timeslot of the same frequency. In MMDVM SFR the hotspot repeats, turned on by its host. In standalone SFR a channel set up in the codeplug repeats on its own, or talks through one |
| SNMP | Simple Network Management Protocol: reading a device's state over the network |
| Squelch | The level a received signal must exceed before the radio plays it |
| Talk target | Where **PTT** goes: a talkgroup, a person or all call |
| Talker alias | A short text, usually a callsign and name, sent along with DMR voice |
| Talkgroup (TG) | A DMR group number. Everyone listening to it hears calls to it |
| Temporary change | A change that lasts until the channel changes or you press **Back** on Home. Hold **OK** to keep it |
| TFTP | Trivial File Transfer Protocol: a minimal way to copy files to and from a device |
| Timeslot (TS) | One of the two time slots of a DMR channel |
| TMS | Motorola's text message format on DMR |
| TOT | Transmit timeout: the longest transmission the radio allows |
| VFO | Variable frequency oscillator: tuning any frequency by hand instead of a stored channel |
| VOX | Voice-operated transmit |
| Zone | A named list of channels |
