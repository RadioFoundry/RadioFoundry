<!-- SPDX-License-Identifier: MIT -->

# Introduction

OpenDigitalRadio (ODR) is free, MIT-licensed firmware for inexpensive DMR radios. It replaces the radio's stock
firmware. One radio-independent core provides the DMR stack, the user interface, the codeplug and the host tools.
Each radio adds a thin board layer underneath.

This manual describes what ODR does on every radio. What differs between radios (the keys, power levels, battery,
power switch, display, missing hardware and test status) is in the [Radio reference](23-radios.md). The UI simulator
renders the screenshots at the DM-1701's screen size. Other radios show the same screens, laid out for their display.

<img src="../ux/screens/160x120i5/home_dmr.png" width="200" alt="Home screen on a DMR channel">

## Radios

| Radio | Status |
|---|---|
| Baofeng DM-1701 | Supported and in the releases. On the test bench now |
| TYT MD-380 / MD-390, Retevis RT3 | Builds and passes its host tests. Not run on a radio yet, not in the releases. You build the image yourself and move to it with `md380_migrate.py` ([Moving from stock MD-380 firmware](18-moving-from-stock-md380.md)) |
| Baofeng DM-32UV | Builds and passes its host tests. Not run on a radio yet, not in the releases. Testers move to it with `dm32_migrate.py` ([Moving from stock DM-32UV firmware](19-moving-from-stock-dm32.md)) |
| Radioddity DB25-D PRO | Early bring-up. Builds and receives FM only. Not run on a radio yet. Not for use |
| TYT MD-UV380, MD-9600, Radioddity GD-77, GD-88, Baofeng DM-1801 Pro | Planned. [The radio survey](../spec/radios.md) lists the rebadged models each covers |

The [Radio reference](23-radios.md) has a section for each radio.

## Project status

ODR is early software. Each feature is in one of three groups.

| Group | Meaning | Features |
|---|---|---|
| Verified on the radio | Tested on a radio, on air against the SDR bench where RF is involved | Only the DM-1701 has run ODR so far. Its list is in [Verified on the DM-1701](23-radios.md#verified-on-the-dm-1701) |
| Built, host-tested | Works against simulated chips in the host tests, or only partly checked on a radio | Everything on the radios that haven't run ODR yet. On the DM-1701: the split duplex repeater, IP over DMR and the services in USB network mode, control station mode, roaming between repeaters, ARS and LRRP, VOX |
| Not built yet | Shown in the menus, dimmed, with a message when selected | Voice prompts, typed text messages, speed dial, saving contacts on the radio |

Some features aren't on every radio. Either the hardware lacks something, or the radio's board code isn't written
yet. Examples are APRS receive on the DM-1701 and tones on the MD-380. The [Radio reference](23-radios.md) lists them for each
radio.

## What ODR does

- DMR voice: group and private calls, talker alias, basic privacy, positions in calls (ETSI GPS Info).
- DMR data: text messages, call alert, radio check, ARS registration, LRRP location answers.
- DMR emergency: alarm, emergency call with hot mic, alerts when someone else declares an emergency.
- DMR repeater roaming per zone.
- FM: CTCSS and DCS tones, squelch, monitor, 12.5 and 25 kHz bandwidth, VOX.
- Scan of a zone or of the VFO band.
- APRS: position, status and messages on FM, positions over DMR, a KISS TNC.
- Hotspot: the radio is an MMDVM modem for MMDVMHost and Pi-Star style hosts. It has a single frequency repeater mode
  (MMDVM SFR).
- USB network mode: the radio is a USB network adapter and routes IP over DMR. On that link the radio serves a status
  page, rig control, KISS over TCP, TFTP codeplug access, SNMP and mDNS. It can also act as a control station for
  dispatch software.
- Programming with qdmr.

## The AMBE codec

DMR voice needs DVSI's proprietary AMBE+2 vocoder. ODR never ships it. Each radio runs the codec from its own stock
firmware. odrflash keeps the codec already in your radio, or takes it from the stock firmware file on your computer
and writes it to the radio ([Installing](02-installing.md)). Without the codec the radio works on FM, shows DMR calls
and handles DMR data. It plays and sends no DMR voice.

## Conventions in this manual

Keys are named by the job they do. Each radio puts these jobs on its own keys. [Key roles](23-radios.md#key-roles)
has the table for every radio.

| Name in this manual | Job | On the DM-1701 |
|---|---|---|
| **OK** | Open or choose. Hold: save | The green Menu key |
| **Back** | Back one level. Hold: Home | The red Back key |
| **Up**, **Down** | Move the focus | The arrow keys |
| **\*** | Quick settings | The star key |
| **#** | Actions | The hash key |
| **0** to **9** | The number keys | The number keys |
| **Monitor** | Monitor while held | SK1, the middle side key |
| **Talk target** | Talk target picker. Hold: scan | SK2, the lowest side key |
| **Power level** | Next power level. Hold: emergency | The top side key |
| **Previous zone**, **Next zone** | Step the zone | P1, P2, the two front function keys |
| **PTT** | Transmit | The push-to-talk key |

On a radio without one of these keys, the menus reach the job ([Key roles](23-radios.md#key-roles)).

"Hold" means press and keep the key down for at least 0.6 s. This is the long-press time, adjustable in
[Settings](12-settings.md). "Press" means a short press. Menu paths are written with `>`. **Settings > Display >
Brightness** means open Settings, then Display, then Brightness.

On-screen text is quoted as it appears, for example "Saved to channel".
