<!-- SPDX-License-Identifier: MIT -->

# Introduction

OpenDigitalRadio (ODR) is free, MIT-licensed firmware for inexpensive DMR radios. It replaces the radio's stock
firmware. One radio-independent core provides the DMR stack, the user interface, the codeplug and the host tools;
each radio adds a thin board layer underneath.

This manual describes how to install ODR and how to use it on the radio. It describes the Baofeng DM-1701, the first
supported radio. The screenshots are DM-1701 screens rendered by the UI simulator.

<img src="../ux/screens/160x120i5/home_dmr.png" width="200" alt="Home screen on a DMR channel">

## Radios

| Radio | Status |
|---|---|
| Baofeng DM-1701 | Supported. On the test bench now. |
| Baofeng DM-32UV | Builds and passes its host tests. Not run on a radio yet, not released. |
| TYT MD-UV380, MD-9600, MD-380, Radioddity GD-77, GD-88 | Planned. See [the radio survey](../spec/radios.md) for the rebadged models each covers. |

## Project status

ODR is early software. There are no release binaries: you build the firmware yourself and supply the codec
(see [Installing](02-installing.md)).

Features fall into three groups. This manual marks each feature that isn't verified on the radio yet.

| Group | Meaning | Features on the DM-1701 |
|---|---|---|
| Verified on the radio | Tested on a DM-1701 | Boot, display, keys, calibration, FM receive and transmit, DMR voice receive, DMR transmit |
| Built, host-tested | Works against simulated chips in the host tests. Nobody has heard it on air yet. | DMR data (messages, call alert, radio check), emergency, privacy, hotspot, USB network, SFR, programming with qdmr, scan, battery saver, VOX |
| Not built yet | Shown in the menus, dimmed, with a message when selected | Voice prompts, GPS position, typed text messages, speed dial, saving contacts on the radio |

## What ODR does

- DMR voice: group and private calls, talker alias, basic privacy.
- DMR data: text messages, call alert, radio check, ARS registration, LRRP location answers.
- DMR emergency: alarm, emergency call with hot mic, alerts when someone else declares an emergency.
- FM: CTCSS and DCS tones, squelch, monitor, 12.5 and 25 kHz bandwidth, VOX (optional build).
- Scan of a zone or of the VFO band.
- Hotspot: the radio works as an MMDVM modem for MMDVMHost and Pi-Star style hosts over USB.
- Single frequency repeater (SFR) mode on the hotspot (optional build).
- USB network mode (optional build): the radio becomes a USB network adapter and routes IP over DMR.
- Programming with qdmr through a compatibility layer.

## The AMBE codec

DMR voice needs DVSI's proprietary AMBE+2 vocoder. ODR never ships it. Each radio runs the codec from its own stock
firmware: you download the stock firmware for your radio, and a tool pulls the codec out of it and merges it into
your ODR image on your own computer. Without the codec the radio works on FM, shows DMR calls and handles DMR data,
but plays and sends no DMR voice.

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
