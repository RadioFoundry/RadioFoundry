<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Capabilities

This page lists what a RadioFoundry radio can do, by area, with the build switch and the spec behind each capability.
It doesn't cover menus, screens, or keys; the user interface design is in [docs/ux](../ux/principles.md).

- For which radios have a capability, its flash cost, and what was checked on each radio, see the
  [feature matrix](../feature-matrix.md).
- For each switch's default and dependencies, see [Build configuration](../build-config.md). A switch named `X` here
  is `CONFIG_X` there.

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
| Tier II voice, direct mode and through repeaters, color code and slot per channel, talkgroups, private calls, late entry, talker alias | `DMR`, `DMR_TALKER_ALIAS` | [dmr-voice.md](dmr-voice.md) |
| Basic Privacy | `DMR_PRIVACY` | [dmr-voice.md](dmr-voice.md) |
| Text messages, call alert, radio check, ARS, LRRP, confirmed data, compressed headers | `DMR_SMS`, `DMR_SIGNALING`, `DMR_ARS`, `DMR_LRRP`, `DMR_UDP_COMPRESS` | [dmr-data.md](dmr-data.md) |
| Emergency alarm, emergency call, hot mic | `DMR_EMERGENCY` | [dmr-emergency.md](dmr-emergency.md) |
| Repeater roaming by position | `DMR_ROAMING` | [dmr-roaming.md](dmr-roaming.md) |
| Tier III trunking, mobile station | `DMR_TRUNK`, `DMR_TRUNK_TX` | [dmr-trunking.md](dmr-trunking.md) |
| ETSI GPS information | `DMR_GPS` | [dmr-data.md](dmr-data.md) |
| Zone and channel sharing | `ZONE_SHARE` | [zone-share.md](zone-share.md) |
| IP over DMR, USB network adapter, TCP services, mDNS, TFTP, SNMP, PPP | `DMR_IP_GATEWAY`, `USB_NETWORK`, `NET_*`, `MDNS`, `TFTP`, `SNMP` | [usb-network.md](usb-network.md) and the pages it links |
| USRP voice over the network link | `NET_USRP` | [usb-audio.md](usb-audio.md) |

## Other digital modes

| Capability | Switch | Spec |
|---|---|---|
| P25 conventional, P25 trunking | `P25`, `P25_TRUNK` | [p25.md](p25.md), [p25-trunking.md](p25-trunking.md) |
| NXDN conventional | `NXDN` | [nxdn.md](nxdn.md) |

## Hotspot and repeater

| Capability | Switch | Spec |
|---|---|---|
| MMDVM hotspot, simplex and MMDVM single-frequency repeater | `HOTSPOT`, `SFR` | [mmdvm-protocol.md](mmdvm-protocol.md), [sfr.md](sfr.md) |
| Split duplex repeater from two radios | `HOTSPOT_SPLIT` | [split-duplex.md](split-duplex.md) |

## Radio, power, and UI

| Capability | Switch | Spec |
|---|---|---|
| Zones and channels, VFO, scan, dual watch, channel, zone, and contact editors, key assignment | `VFO`, `SCAN`, `DUAL_WATCH`, `CHANNEL_LIST`, `CHANNEL_EDIT`, `ZONE_EDIT`, `CONTACT_EDIT`, `KEY_ASSIGN` | [ui.md](ui.md) |
| Power levels, TX time-out, battery saver, battery display, calibration | `BATTERY_SAVE` | [radio-features.md](radio-features.md) |
| GPS receiver, fixed position | `GPS` | [position.md](position.md) |
| Settings menu, color themes, fonts, languages | `UI_SETTINGS`, `UI_COLOR`, `FONT_*`, `LANG_*` | [ui.md](ui.md) |

## Programming, update, and diagnostics

| Capability | Switch | Spec |
|---|---|---|
| Codeplug programming with qdmr (open-firmware CPS) and the Codeplug Editor | `CPS_OPENFW`, `CPS_FAST` | [cps-protocol.md](cps-protocol.md), [cps-fast.md](cps-fast.md) |
| Firmware update and backup from the web installer or `rfoundry` | `STUB_ENTRY` | [stub-entry.md](stub-entry.md) |
| Debug link, crash log and report, watchdog | `DEBUGLINK`, `CRASH_LOG`, `WATCHDOG` | [debuglink.md](debuglink.md), [crash-log.md](crash-log.md), [watchdog.md](watchdog.md) |
