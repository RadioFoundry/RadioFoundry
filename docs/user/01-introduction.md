<!-- SPDX-License-Identifier: MIT -->

# Introduction

OpenDigitalRadio (ODR) is free, MIT-licensed firmware for inexpensive DMR radios. It replaces the radio's stock
firmware. One radio-independent core provides the DMR stack, the user interface, the codeplug and the host tools;
each radio adds a thin board layer underneath.

This manual describes the Baofeng DM-1701, the first supported radio; [On the MD-380](#on-the-md-380) lists what
differs there. The screenshots are DM-1701 screens rendered by the UI simulator.

<img src="../ux/screens/160x120i5/home_dmr.png" width="200" alt="Home screen on a DMR channel">

## Radios

| Radio | Status |
|---|---|
| Baofeng DM-1701 | Supported and in the releases. On the test bench now |
| TYT MD-380 / MD-390, Retevis RT3 | Builds and passes its host tests. Not run on a radio yet, not in the releases. You build the image yourself and move to it with `md380_migrate.py` ([Moving from stock MD-380 firmware](18-moving-from-stock-md380.md)) |
| Baofeng DM-32UV | Builds and passes its host tests. Not run on a radio yet, not in the releases |
| TYT MD-UV380, MD-9600, Radioddity GD-77, GD-88 | Planned. [The radio survey](../spec/radios.md) lists the rebadged models each covers |

## Project status

ODR is early software. This manual marks each feature that isn't verified on the radio yet.

| Group | Meaning | Features on the DM-1701 |
|---|---|---|
| Verified on the radio | Tested on a DM-1701, on air against the SDR bench where RF is involved | Boot, display, keys, calibration, FM receive and transmit, CTCSS / DCS sent, 1750 Hz burst, roger beep, DMR group and private calls both ways (late entry, talker alias, positions in calls, the privacy signaling), text messages, call alert, radio check and emergency both ways, hotspot calls, APRS on FM and over DMR, battery saver, zone scan, VFO scan, dual watch, reading and writing the radio with qdmr's protocol, switching to USB network mode |
| Built, host-tested | Works against simulated chips in the host tests, or only partly checked on the radio | SFR, the split duplex repeater, IP over DMR and the services in USB network mode, control station mode, roaming between repeaters, ARS and LRRP, VOX |
| Not built yet | Shown in the menus, dimmed, with a message when selected | Voice prompts, APRS receive (the DM-1701 can't sample received audio), typed text messages, speed dial, saving contacts on the radio |

## What ODR does

- DMR voice: group and private calls, talker alias, basic privacy, positions in calls (ETSI GPS Info).
- DMR data: text messages, call alert, radio check, ARS registration, LRRP location answers.
- DMR emergency: alarm, emergency call with hot mic, alerts when someone else declares an emergency.
- DMR repeater roaming per zone.
- FM: CTCSS and DCS tones, squelch, monitor, 12.5 and 25 kHz bandwidth, VOX.
- Scan of a zone or of the VFO band.
- APRS: position, status and messages on FM, positions over DMR, a KISS TNC.
- Hotspot: the radio is an MMDVM modem for MMDVMHost and Pi-Star style hosts, with a single frequency repeater (SFR)
  mode.
- USB network mode: the radio is a USB network adapter and routes IP over DMR. On that link the radio serves a status
  page, rig control, KISS over TCP, TFTP codeplug access, SNMP and mDNS, and can act as a control station for
  dispatch software.
- Programming with qdmr.

## The AMBE codec

DMR voice needs DVSI's proprietary AMBE+2 vocoder. ODR never ships it. Each radio runs the codec from its own stock
firmware: odrflash keeps the codec already in your radio, or takes it from the stock firmware file on your computer
and writes it to the radio ([Installing](02-installing.md)). Without the codec the radio works on FM, shows DMR calls
and handles DMR data, but plays and sends no DMR voice.

## Conventions in this manual

Keys are named by their label or position on the DM-1701:

| Name in this manual | Key |
|---|---|
| **OK** | The green Menu key |
| **Back** | The red Back key |
| **Up**, **Down** | The arrow keys |
| **\*** | The star key: quick settings |
| **#** | The hash key: actions |
| **0** to **9** | The number keys |
| **P1**, **P2** | The two front function keys |
| **SK1**, **SK2** | Side keys 1 and 2 |
| **Top side key** | The third side key |
| **PTT** | The push-to-talk key |

"Hold" means press and keep the key down for at least 0.6 s (the long-press time, adjustable in
[Settings](12-settings.md)). "Press" means a short press. Menu paths are written with `>`: **Settings > Display >
Brightness** means open Settings, then Display, then Brightness.

On-screen text is quoted as it appears, for example "Saved to channel".

## On the MD-380

The MD-380 image runs the same firmware with these differences (the port is untested on a radio):

- One band per radio: UHF (400-480 MHz) or VHF (136-174 MHz), one image for each. Two power levels, 1 W and 5 W.
- No **P1**, **P2** or top side key. Change the zone with **OK**, **4**, the power with **\***, **1**. There is no
  emergency key.
- No CTCSS or DCS yet: a channel with a tone opens on any carrier and sends no tone. No roger beep, 1750 Hz burst or
  APRS on FM; APRS positions over DMR work.
- No hotspot mode, SFR or split duplex in the default image. USB network mode and its services are in.
