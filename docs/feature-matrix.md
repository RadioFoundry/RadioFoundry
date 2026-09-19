<!-- SPDX-License-Identifier: MIT -->

# Feature matrix

Every feature, the build switch it sits behind (`config/features.mk`, [build-config.md](build-config.md)), what it
costs in flash on the ARM radios, and where it's built by default.

- **Size** = default image minus the image with that switch off (`tools/ci.sh` builds each switch off), in bytes of
  flash. A parent switch's size includes its children (turning `CONFIG_FM` off also drops VOX, APRS, ...). Switches
  that are off by default were measured by turning them on in the DM-1701 build. Figures from main at c0022b4
  (2026-10-11); DM-1701 default image 298,572 B of 344,064 B, MD-380 270,684 B of 281,668 B.
- **Radios:** ● on by default · ○ optional (supported, off by default) · ✕ not compatible (the hardware lacks it, or
  the board code isn't written: the note says which) · — in a size column: not built on that radio, so not measured.
- The DM-32UV builds with csky-elf GCC; its sizes aren't listed yet (ARM only for now).

## Radio and voice

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Analog FM receive and transmit | `CONFIG_FM` | 25,736 | 23,704 | ● | ● | ● | Includes its children (tones, VOX, APRS, FM VFO) |
| CTCSS / DCS encode and decode | `CONFIG_SUBTONE` | 888 | — | ● | ✕ | ● | MD-380: software decode (ADC) and PWM encode not written |
| FM TX tones: roger beep, 1750 Hz burst, CW ID | `CONFIG_FM_TX_TONES` | 544 | — | ● | ✕ | ● | MD-380: TX tone path not written |
| FM VOX | `CONFIG_VOX` | 480 | 504 | ● | ● | ● | |
| Receiver battery saver | `CONFIG_BATTERY_SAVE` | 288 | 320 | ● | ● | ● | |
| Air band AM receive (108-137 MHz) | `CONFIG_AM_RX` | 408 | — | ✕ | ✕ | ● | Needs a receiver that does AM; DM-32 board glue not written yet, so nothing runs |
| Broadcast FM receiver | `CONFIG_BCAST_FM` | ~0 | — | ✕ | ✕ | ● | Needs a second receiver chip (DM-32's RDA5807M); glue not written yet |
| DMR Tier II voice (modem, call stack, AMBE FEC, vocoder) | `CONFIG_DMR` | 135,252 | 115,424 | ● | ● | ● | Includes every DMR feature below |
| Basic privacy (Motorola / Anytone BP) | `CONFIG_DMR_PRIVACY` | 1,936 | 1,936 | ● | ● | ● | |
| Talker alias receive and send | `CONFIG_DMR_TALKER_ALIAS` | 984 | 984 | ● | ● | ● | |
| ETSI GPS Info in the embedded LC | `CONFIG_DMR_GPS` | 2,208 | 2,200 | ● | ● | ● | |
| DMR emergency (alarm, emergency call, hot mic) | `CONFIG_DMR_EMERGENCY` | 3,280 | 3,280 | ● | ● | ● | |
| Repeater roaming | `CONFIG_DMR_ROAMING` | 11,944 | 11,912 | ● | ● | ● | |
| CSBK signaling: call alert, radio check | `CONFIG_DMR_SIGNALING` | 592 | 600 | ● | ● | ● | |
| VFO (hand-tuned frequency) | `CONFIG_VFO` | 504 | 504 | ● | ● | ● | |
| FM VFO | `CONFIG_VFO_FM` | 496 | 496 | ● | ● | ● | |
| DMR VFO (ID, color code, slot, talk target typed in) | `CONFIG_VFO_DMR` | 0 | 16 | ● | ● | ● | Switch exists; the feature isn't built yet |
| Scan, band scan, dual watch, nuisance delete | `CONFIG_SCAN` | 2,560 | 2,560 | ● | ● | ● | |
| Beeps and alert tones | `CONFIG_BEEP` | 1,528 | 1,552 | ● | ● | ● | |

## DMR data

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Text messages (ETSI and Motorola formats) | `CONFIG_DMR_SMS` | 1,920 | 4,200 | ● | ● | ● | |
| ARS registration | `CONFIG_DMR_ARS` | 1,024 | 1,044 | ● | ● | ● | |
| LRRP location | `CONFIG_DMR_LRRP` | 8,560 | 8,580 | ● | ● | ● | |
| Compressed UDP/IP headers on transmit (SAP 3) | `CONFIG_DMR_UDP_COMPRESS` | 408 | 412 | ● | ● | ● | Auto: not for ports 4000-4099 |
| Confirmed packet data | `CONFIG_DMR_CONFIRMED` | 2,816 | 2,824 | ● | ● | ● | Built in; off until rules are set |
| Single frequency repeater (hotspot SFR mode) | `CONFIG_SFR` | 6,852 | — | ● | ✕ | ● | MD-380: HR_C5000 lacks what the SFR glue uses |
| APRS: AFSK positions, status, messages; DMR positions; KISS TNC | `CONFIG_APRS` | 23,016 | 22,408 | ● | ● | ● | Includes APRS receive where built. MD-380: AFSK TX needs the tone path (not written), DMR positions work |
| APRS receive: demodulator, station list, digipeater | `CONFIG_APRS_RX` | 8,464 | — | ✕ | ✕ | ● | DM-1701: no ADC sees receive audio. MD-380: not written |
| APRS iGate rules (with tools/aprs_gate.py) | `CONFIG_APRS_IGATE` | 1,888 | — | ✕ | ✕ | ● | Needs APRS receive |

## Host link, USB and network

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| USB device | `CONFIG_USB` | 87,220 | 66,204 | ● | ● | ✕ | DM-32 has no USB (host link on the programming cable's UART). Includes everything on USB |
| Host link: hotspot, CPS, debug link on the host port | `CONFIG_HOSTLINK` | 75,576 | 54,560 | ● | ● | ● | Includes its children |
| MMDVM hotspot | `CONFIG_HOTSPOT` | 34,116 | 14,540 | ● | ● | ● | Includes split duplex and the SFR hotspot mode where built |
| Split duplex repeater (two radios, one duplex modem) | `CONFIG_HOTSPOT_SPLIT` | 12,536 | — | ● | ✕ | ● | MD-380: HR_C6000-only glue. Slot numbering (CACH) still open |
| Debug link | `CONFIG_DEBUGLINK` | 30,016 | 28,152 | ● | ● | ● | |
| Legacy CPS protocol (qdmr) | `CONFIG_CPS_LEGACY` | 3,096 | 3,056 | ● | ● | ● | |
| USB network adapter (CDC-NCM) | `CONFIG_USB_NETWORK` | 8,624 | 8,656 | ● | ● | ✕ | Includes the IP gateway |
| IP over DMR gateway | `CONFIG_DMR_IP_GATEWAY` | 4,736 | 4,760 | ● | ● | ✕ | Needs the USB network |

## User interface

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Settings menu on the radio | `CONFIG_UI_SETTINGS` | 4,864 | 4,896 | ● | ● | ● | |
| Diagnostics page | `CONFIG_DIAG` | 3,736 | 3,896 | ● | ● | ● | |
| 16x32 font | `CONFIG_FONT_XL` | 6,352 | — | ○ | ○ | ○ | Only useful at 240 px and wider; DM-32 runs at half resolution, so nothing gained yet |

## Diagnostics

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Audio path probe (debug link 0x10) | `CONFIG_AUDIO_PROBE` | 1,176 | — | ○ | ○ | ○ | Bench only, never in a release |

## Pending merge (2026-10-11)

Built and tested on their branches, being merged and fitted on the integration branch. Sizes are from the branch
agents, DM-1701, before the merge (the HTTP page will shrink with build-time gzip; the MD-380 column is being worked
out, with LTO under evaluation).

| Feature | Switch | Bytes DM-1701 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|:-:|:-:|:-:|---|
| Minimal TCP (USB network only) | `CONFIG_NET_TCP` | ~4,400 (+4.5 KB RAM) | ● | ? | ✕ | |
| KISS over TCP, port 8001 | `CONFIG_NET_KISS` | ~1,100 | ● | ? | ✕ | |
| HTTP status page and JSON, port 80 | `CONFIG_NET_HTTP` | ~4,300 | ● | ? | ✕ | Static assets gzipped at build time (switch) |
| rigctld, port 4532 | `CONFIG_NET_RIGCTL` | ~4,000 | ● | ? | ✕ | |
| APRS-IS client | `CONFIG_NET_APRSIS` | ~2,800 | ● | ? | ✕ | Needs NAT on the host |
| mDNS / DNS-SD (`odr-<id>.local`) | `CONFIG_MDNS` | ~6,950 | ● | ? | ✕ | |
| TFTP codeplug read / write | `CONFIG_TFTP` | ~6,700 | ● | ? | ✕ | |
| SNMP v2c agent and traps (USB and over the air) | `CONFIG_SNMP` | ~8,790 | ● | ? | ✕ | MD-380 measured 8,748 on its branch |
| Motorola TMS text format | `CONFIG_DMR_TMS` | ~860 | ● | ● | ● | ETSI IP text is the default format |
| Control station mode (boot as network, PC answers data, fixed link) | `CONFIG_DMR_CONTROL_STATION` | ~350 | ● | ● | ✕ | |
| ETSI IP text (UDP 5016) | part of `CONFIG_DMR_SMS` | ~800 | ● | ● | ● | |
