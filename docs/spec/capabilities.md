<!-- SPDX-License-Identifier: MIT -->

# Capabilities

What a user can expect a RadioFoundry radio to do, by area, and the build switch and spec behind each. Which radios have
a capability, what it costs in flash and what has been checked on each radio are in
[feature-matrix.md](../feature-matrix.md); the switches are in [build-config.md](../build-config.md). This page names the
areas and says nothing about menus, screens or keys: the UI is designed in `docs/ux/`.

## Analog FM

| Capability | Switch | Spec |
|---|---|---|
| FM voice, 12.5 and 25 kHz, squelch, monitor, repeater reverse | `FM` | [radio-features.md](radio-features.md) |
| CTCSS and DCS encode and decode, tone scan | `SUBTONE` | [radio-features.md](radio-features.md) |
| 1750 Hz burst, roger beep; DTMF digits from the keys | `FM_TX_TONES`, `DTMF` | [radio-features.md](radio-features.md) |
| VOX | `VOX` | [radio-features.md](radio-features.md) |
| Air band AM receive, broadcast FM receive | `AM_RX`, `BCAST_FM` | [airband-and-broadcast.md](airband-and-broadcast.md) |
| APRS: AFSK positions, status, messages, receive, digipeater, iGate, KISS TNC | `APRS`, `APRS_RX`, `APRS_IGATE` | [aprs.md](aprs.md) |

## DMR

| Capability | Switch | Spec |
|---|---|---|
| Tier II voice, direct mode and through repeaters, color code and slot per channel, talk groups, private calls, late entry, talker alias | `DMR`, `DMR_TALKER_ALIAS` | [dmr-voice.md](dmr-voice.md) |
| Basic Privacy | `DMR_PRIVACY` | [dmr-voice.md](dmr-voice.md) |
| Text messages, call alert, radio check, ARS, LRRP, confirmed data, compressed headers | `DMR_SMS`, `DMR_SIGNALING`, `DMR_ARS`, `DMR_LRRP`, `DMR_CONFIRMED`, `DMR_UDP_COMPRESS` | [dmr-data.md](dmr-data.md) |
| Emergency alarm, emergency call, hot mic | `DMR_EMERGENCY` | [dmr-emergency.md](dmr-emergency.md) |
| Repeater roaming by position | `DMR_ROAMING` | [dmr-roaming.md](dmr-roaming.md) |
| ETSI GPS information | `DMR_GPS` | [dmr-data.md](dmr-data.md) |
| IP over DMR, USB network adapter, TCP services, mDNS, TFTP, SNMP, PPP, USRP | `DMR_IP_GATEWAY`, `USB_NETWORK`, `NET_*`, `MDNS`, `TFTP`, `SNMP` | [usb-network.md](usb-network.md) and the pages it links |

## Hotspot and repeater

| Capability | Switch | Spec |
|---|---|---|
| MMDVM hotspot, simplex and MMDVM single frequency repeater | `HOTSPOT`, `SFR` | [mmdvm-protocol.md](mmdvm-protocol.md), [sfr.md](sfr.md) |
| Split duplex repeater from two radios | `HOTSPOT_SPLIT` | [split-duplex.md](split-duplex.md) |

## Radio, power and UI

| Capability | Switch | Spec |
|---|---|---|
| Zones and channels, VFO, scan, dual watch, channel and zone editors, side key assignment | `VFO`, `SCAN`, `DUAL_WATCH`, `CHANNEL_LIST`, `CHANNEL_EDIT`, `ZONE_EDIT`, `KEY_ASSIGN` | [ui.md](ui.md) |
| Power levels, TX time-out, battery saver, battery display, calibration | `BATTERY_SAVE` | [radio-features.md](radio-features.md) |
| GPS receiver, fixed position | `GPS` | [radio-features.md](radio-features.md) |
| Settings menu, color themes, fonts | `UI_SETTINGS`, `UI_COLOR`, `FONT_*` | [ui.md](ui.md) |

## Programming, update and diagnostics

| Capability | Switch | Spec |
|---|---|---|
| Codeplug programming with qdmr (open-firmware CPS) and the Codeplug Editor | `CPS_OPENFW`, `CPS_FAST` | [cps-protocol.md](cps-protocol.md), [cps-fast.md](cps-fast.md) |
| Firmware update and backup from the web installer or `rfoundry` | `STUB_ENTRY` | [../user/02-installing.md](../user/02-installing.md) |
| Debug link, crash log and report, watchdog | `DEBUGLINK`, `CRASH_LOG`, `WATCHDOG` | [debuglink.md](debuglink.md), [crash-log.md](crash-log.md), [watchdog.md](watchdog.md) |

## Not built

- Trunking (Capacity Plus, Connect Plus, Tier III): planning only ([dmr-tier3-plan.md](dmr-tier3-plan.md)).
- Encryption beyond Basic Privacy (ARC4, AES).
- DTMF decode and stored DTMF codes ([radio-features.md](radio-features.md)).
