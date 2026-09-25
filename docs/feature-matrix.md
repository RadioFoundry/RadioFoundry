<!-- SPDX-License-Identifier: MIT -->

# Feature matrix

Every feature, the build switch it sits behind (`CONFIG_<switch>` in `config/features.mk`,
[build-config.md](build-config.md)), what it costs in flash on the ARM radios, and where it's built by default.

- **Bytes** = default image minus the image with that switch off (`tools/ci.sh` builds each switch off), flash bytes.
  A parent switch's figure includes its children (turning `FM` off also drops VOX, APRS, ...).
- Figures from main at 4d56f20 (2026-10-11).
  - DM-1701: plain build (LTO off), default image 339,528 B of 344,064 B (4,536 free).
  - MD-380: link time optimization on (`LTO=1 LTO_3P=1`, its default and the only way everything fits), default
    image 278,600 B of 281,668 B (3,068 free). With LTO a switch also changes what gets inlined elsewhere, so small MD-380 figures are approximate.
  - `*` = off by default; measured by turning it on in the DM-1701 build (main c0022b4).
- **Radios:** ● on by default · ○ optional (supported, off by default) · ✕ not compatible (the hardware lacks it, or
  the board code isn't written: the note says which) · — in a bytes column: not built there, so not measured.
- The DM-32UV builds with csky-elf GCC; its sizes aren't listed (ARM only for now).


## Radio and voice

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Analog FM receive and transmit | `FM` | 29,304 | 28,188 | ● | ● | ● | Includes its children (tones, VOX, APRS, FM VFO) |
| CTCSS / DCS encode and decode | `SUBTONE` | 912 | — | ● | ✕ | ● | MD-380: software decode (ADC) and PWM encode not written |
| FM TX tones: roger beep, 1750 Hz burst, CW ID | `FM_TX_TONES` | 568 | — | ● | ✕ | ● | MD-380: TX tone path not written |
| FM VOX | `VOX` | 504 | 1,036 | ● | ● | ● |  |
| Receiver battery saver | `BATTERY_SAVE` | 296 | 740 | ● | ● | ● |  |
| Air band AM receive (108-137 MHz) | `AM_RX` | 408* | — | ✕ | ✕ | ● | Needs a receiver that does AM; DM-32 board glue not written yet, so nothing runs |
| Broadcast FM receiver | `BCAST_FM` | ~0* | — | ✕ | ✕ | ● | Needs a second receiver chip (DM-32's RDA5807M); glue not written yet |
| DMR Tier II voice (modem, call stack, AMBE FEC, vocoder) | `DMR` | 175,652 | 133,956 | ● | ● | ● | Includes every DMR feature and everything on IP over DMR |
| Basic privacy (Motorola / Anytone BP) | `DMR_PRIVACY` | 1,936 | 1,992 | ● | ● | ● |  |
| Talker alias receive and send | `DMR_TALKER_ALIAS` | 984 | 1,316 | ● | ● | ● |  |
| ETSI GPS Info in the embedded LC | `DMR_GPS` | 2,208 | 2,168 | ● | ● | ● |  |
| DMR emergency (alarm, emergency call, hot mic) | `DMR_EMERGENCY` | 3,288 | 3,172 | ● | ● | ● |  |
| Repeater roaming | `DMR_ROAMING` | 11,936 | 12,352 | ● | ● | ● |  |
| CSBK signaling: call alert, radio check | `DMR_SIGNALING` | 600 | 804 | ● | ● | ● |  |
| VFO (hand-tuned frequency) | `VFO` | 520 | 496 | ● | ● | ● |  |
| FM VFO | `VFO_FM` | 504 | 484 | ● | ● | ● |  |
| DMR VFO (ID, color code, slot, talk target typed in) | `VFO_DMR` | 8 | 8 | ● | ● | ● | Switch exists; the feature isn't built yet |
| Scan, band scan, dual watch, nuisance delete | `SCAN` | 2,576 | 3,004 | ● | ● | ● |  |
| Beeps and alert tones | `BEEP` | 1,552 | 1,880 | ● | ● | ● |  |

## DMR data

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Text messages (ETSI IP text by default) | `DMR_SMS` | 2,632 | 5,084 | ● | ● | ● | Includes the TMS format. Individual ETSI IP texts go confirmed ("Delivered to") |
| Motorola TMS text format | `DMR_TMS` | 784 | 728 | ● | ● | ● | Optional format next to ETSI IP text |
| ARS registration | `DMR_ARS` | 1,112 | 1,284 | ● | ● | ● |  |
| LRRP location | `DMR_LRRP` | 8,540 | 1,936 | ● | ● | ● |  |
| Compressed UDP/IP headers on transmit (SAP 3) | `DMR_UDP_COMPRESS` | 408 | 372 | ● | ● | ● | Auto: not for ports 4000-4099 or ETSI IP text |
| Confirmed packet data | `DMR_CONFIRMED` | 2,880 | 2,744 | ● | ● | ● |  |
| Single frequency repeater (hotspot SFR mode) | `SFR` | 6,876 | — | ● | ✕ | ● | MD-380: HR_C5000 lacks what the SFR glue uses |
| APRS: AFSK positions, status, messages; DMR positions; KISS TNC | `APRS` | 26,568 | 26,644 | ● | ● | ● | Includes APRS receive, KISS over TCP and APRS-IS where built. MD-380: AFSK TX needs the tone path (not written), DMR positions work |
| APRS receive: demodulator, station list, digipeater | `APRS_RX` | 8,464* | — | ✕ | ✕ | ● | DM-1701: no ADC sees receive audio. MD-380: not written |
| APRS iGate rules (with tools/aprs_gate.py) | `APRS_IGATE` | 1,888* | — | ✕ | ✕ | ● | Needs APRS receive |

## Host link and USB

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| USB device | `USB` | 127,108 | 87,428 | ● | ● | ✕ | DM-32 has no USB (host link on the programming cable's UART). Includes everything on USB |
| Host link: hotspot, CPS, debug link on the host port | `HOSTLINK` | 115,456 | 77,248 | ● | ● | ● | Includes its children |
| MMDVM hotspot | `HOTSPOT` | 34,196 | ~12,400 | ● | ○ | ● | Includes split duplex and the SFR hotspot mode where built. MD-380: left out for flash (DMR_ROAMING off is the alternative); size estimated |
| Split duplex repeater (two radios, one duplex modem) | `HOTSPOT_SPLIT` | 12,576 | — | ● | ✕ | ● | MD-380: HR_C6000-only glue. Slot numbering (CACH) still open |
| Debug link | `DEBUGLINK` | 32,368 | 29,004 | ● | ● | ● |  |
| Legacy CPS protocol (qdmr) | `CPS_LEGACY` | 3,056 | 3,012 | ● | ● | ● |  |

## Network (USB network adapter and IP over DMR)

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| USB network adapter (CDC-NCM) | `USB_NETWORK` | 48,152 | 47,300 | ● | ● | ✕ | Includes everything below |
| IP over DMR gateway | `DMR_IP_GATEWAY` | 43,944 | 43,124 | ● | ● | ✕ | Includes the radio's own services below |
| Control station mode (boot as network, PC answers data, fixed link) | `DMR_CONTROL_STATION` | 296 | 492 | ● | ● | ✕ |  |
| Minimal TCP | `NET_TCP` | 16,168 | 15,616 | ● | ● | ✕ | Includes KISS, HTTP, rigctld, APRS-IS. About 4.5 KB RAM |
| KISS over TCP, port 8001 | `NET_KISS` | 684 | 692 | ● | ● | ✕ |  |
| HTTP status page and JSON, port 80 | `NET_HTTP` | 4,796 | 4,660 | ● | ● | ✕ |  |
| HTTP static files stored and served gzip compressed | `NET_HTTP_GZIP` | -80 | -60 | ● | ● | ✕ | Negative: saves flash |
| rigctld, port 4532 | `NET_RIGCTL` | 3,788 | 3,704 | ● | ● | ✕ |  |
| APRS-IS client | `NET_APRSIS` | 2,848 | 2,740 | ● | ● | ✕ | Needs NAT on the host |
| mDNS / DNS-SD (odr-<id>.local) | `MDNS` | 6,736 | 6,584 | ● | ● | ✕ |  |
| TFTP codeplug read / write | `TFTP` | 6,348 | 6,412 | ● | ● | ✕ |  |
| SNMP v2c agent and traps (USB and over the air) | `SNMP` | 8,380 | 7,988 | ● | ● | ✕ |  |

## User interface

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Settings menu on the radio | `UI_SETTINGS` | 5,304 | 5,660 | ● | ● | ● |  |
| Diagnostics page | `DIAG` | 3,752 | 3,536 | ● | ● | ● |  |
| 16x32 font | `FONT_XL` | 6,352* | — | ○ | ✕ | ○ | Only useful at 240 px and wider (MD-380 is 160 px); DM-32 runs at half resolution, so nothing gained yet |

## Bench

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Audio path probe (debug link 0x10) | `AUDIO_PROBE` | 1,176* | — | ○ | ○ | ○ | Bench only, never in a release |

## Whole images

| Build | DM-1701 | MD-380 |
|---|---:|---:|
| Default | 339,528 | 278,600 |
| Maximal (every compatible switch on) | 339,048 (LTO on) | 278,980 |
| FM only, minimal | 122,348 | 108,908 |

Turning on link time optimization (`LTO=1 LTO_3P=1`) on the DM-1701 default build gives 319,132 B (about 20 KB
less); it waits for hardware testing before it becomes that radio's default.
