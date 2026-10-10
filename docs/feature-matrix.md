<!-- SPDX-License-Identifier: MIT -->

# Feature matrix

Every feature, the build switch it sits behind (`CONFIG_<switch>` in `config/features.mk`,
[build-config.md](build-config.md)), what it costs in flash on the ARM radios, and where it's built by default.

- **Bytes** = default image minus the image with that switch off (`tools/ci.sh` builds each switch off), flash bytes.
  A parent switch's figure includes its children (turning `FM` off also drops VOX, APRS, ...).
- Figures from tools/ci.sh at 3ec14e3 (2026-10-11).
  - DM-1701: plain build (LTO off), default image 340,680 B of 344,064 B (3,384 free).
  - MD-380: link time optimization on (`LTO=1 LTO_3P=1`, its default and the only way everything fits), default
    image 279,444 B of 281,668 B (2,224 free). With LTO a switch also changes what gets inlined elsewhere, so small MD-380 figures are approximate.
  - `*` = off by default; measured by turning it on in the DM-1701 build (main c0022b4).
- **Radios:** ● on by default · ○ optional (supported, off by default) · ✕ not compatible (the hardware lacks it, or
  the board code isn't written: the note says which) · — in a bytes column: not built there, so not measured.
- The DM-32UV builds with csky-elf GCC; its sizes aren't in the per-feature tables (ARM only for now); its whole images are
  at the end.


## Radio and voice

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Analog FM receive and transmit | `FM` | 29,296 | 28,056 | ● | ● | ● | Includes its children (tones, VOX, APRS, FM VFO) |
| CTCSS / DCS encode and decode | `SUBTONE` | 904 | — | ● | ✕ | ● | MD-380: software decode (ADC) and PWM encode not written |
| FM TX tones: roger beep, 1750 Hz burst | `FM_TX_TONES` | 568 | — | ● | ✕ | ● | MD-380: TX tone path not written |
| FM VOX | `VOX` | 496 | 1,064 | ● | ● | ● |  |
| Receiver battery saver | `BATTERY_SAVE` | 296 | 740 | ● | ● | ● |  |
| Air band AM receive (108-137 MHz) | `AM_RX` | 408* | — | ✕ | ✕ | ● | Needs a receiver that does AM; DM-32 board glue not written yet, so nothing runs |
| Broadcast FM receiver | `BCAST_FM` | ~0* | — | ✕ | ✕ | ● | Needs a second receiver chip (DM-32's RDA5807M); glue not written yet |
| DMR Tier II voice (modem, call stack, AMBE FEC, vocoder) | `DMR` | 175,856 | 134,308 | ● | ● | ● | Includes every DMR feature and everything on IP over DMR |
| Basic privacy (Motorola / Anytone BP) | `DMR_PRIVACY` | 1,936 | 1,992 | ● | ● | ● |  |
| Talker alias receive and send | `DMR_TALKER_ALIAS` | 984 | 1,300 | ● | ● | ● |  |
| ETSI GPS Info in the embedded LC | `DMR_GPS` | 2,208 | 2,152 | ● | ● | ● |  |
| DMR emergency (alarm, emergency call, hot mic) | `DMR_EMERGENCY` | 3,296 | 3,188 | ● | ● | ● |  |
| Repeater roaming | `DMR_ROAMING` | 11,952 | 12,380 | ● | ● | ● |  |
| CSBK signaling: call alert, radio check | `DMR_SIGNALING` | 592 | 752 | ● | ● | ● |  |
| VFO (hand-tuned frequency) | `VFO` | 520 | 464 | ● | ● | ● |  |
| FM VFO | `VFO_FM` | 504 | 452 | ● | ● | ● |  |
| DMR VFO (ID, color code, slot, talk target typed in) | `VFO_DMR` | 8 | 8 | ● | ● | ● | Switch exists; the feature isn't built yet |
| Scan, band scan, dual watch, nuisance delete | `SCAN` | 2,576 | 3,032 | ● | ● | ● |  |
| Beeps and alert tones | `BEEP` | 1,552 | 1,800 | ● | ● | ● |  |

## DMR data

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Text messages (ETSI IP text by default) | `DMR_SMS` | 2,640 | 5,072 | ● | ● | ● | Includes the TMS format. Individual ETSI IP texts go confirmed ("Delivered to") |
| Motorola TMS text format | `DMR_TMS` | 784 | 740 | ● | ● | ● | Optional format next to ETSI IP text |
| ARS registration | `DMR_ARS` | 1,112 | 1,324 | ● | ● | ● |  |
| LRRP location | `DMR_LRRP` | 8,548 | 2,048 | ● | ● | ● |  |
| Compressed UDP/IP headers on transmit (SAP 3) | `DMR_UDP_COMPRESS` | 408 | 356 | ● | ● | ● | Auto: not for ports 4000-4099 or ETSI IP text |
| Confirmed packet data | `DMR_CONFIRMED` | 2,880 | 2,920 | ● | ● | ● |  |
| Single frequency repeater (MMDVM SFR) | `SFR` | 6,884 | — | ● | ✕ | ● | MD-380: HR_C5000 lacks what the SFR glue uses |
| APRS: AFSK positions, status, messages; DMR positions; KISS TNC | `APRS` | 26,568 | 26,628 | ● | ● | ● | Includes APRS receive, KISS over TCP and APRS-IS where built. MD-380: AFSK TX needs the tone path (not written), DMR positions work |
| APRS receive: demodulator, station list, digipeater | `APRS_RX` | 8,464* | — | ✕ | ✕ | ● | DM-1701: the receive audio path the bench found isn't wired yet ([hardware-dm1701](spec/hardware-dm1701.md) 21). MD-380: not written |
| APRS iGate rules (with tools/aprs_gate.py) | `APRS_IGATE` | 1,888* | — | ✕ | ✕ | ● | Needs APRS receive |

## Host link and USB

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| USB device | `USB` | 127,300 | 87,848 | ● | ● | ✕ | DM-32 has no USB (host link on the programming cable's UART). Includes everything on USB |
| Host link: hotspot, CPS, debug link on the host port | `HOSTLINK` | 115,640 | 77,644 | ● | ● | ● | Includes its children |
| MMDVM hotspot | `HOTSPOT` | 34,196 | ~12,400 | ● | ○ | ● | Includes split duplex and MMDVM SFR where built. MD-380: left out for flash (DMR_ROAMING off is the alternative); size estimated |
| Split duplex repeater (two radios, one duplex modem) | `HOTSPOT_SPLIT` | 12,584 | — | ● | ✕ | ● | MD-380: HR_C6000-only glue |
| Debug link | `DEBUGLINK` | 32,376 | 29,084 | ● | ● | ● |  |
| Legacy CPS protocol (qdmr) | `CPS_LEGACY` | 3,064 | 3,008 | ● | ● | ● |  |

## Network (USB network adapter and IP over DMR)

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| USB network adapter (CDC-NCM) | `USB_NETWORK` | 48,344 | 47,468 | ● | ● | ✕ | Includes everything below |
| IP over DMR gateway | `DMR_IP_GATEWAY` | 44,128 | 43,220 | ● | ● | ● | Includes the radio's own services below |
| Control station mode (boot as network, PC answers data, fixed link) | `DMR_CONTROL_STATION` | 288 | 484 | ● | ● | ● |  |
| Minimal TCP | `NET_TCP` | 16,240 | 15,656 | ● | ● | ● | Includes KISS, HTTP, rigctld, APRS-IS. About 4.5 KB RAM |
| KISS over TCP, port 8001 | `NET_KISS` | 700 | 752 | ● | ● | ● |  |
| HTTP status page and JSON, port 80 | `NET_HTTP` | 4,796 | 4,860 | ● | ● | ● |  |
| HTTP static files stored and served gzip compressed | `NET_HTTP_GZIP` | -64 | -96 | ● | ● | ● | Negative: saves flash |
| rigctld, port 4532 | `NET_RIGCTL` | 3,852 | 3,728 | ● | ● | ● |  |
| APRS-IS client | `NET_APRSIS` | 2,856 | 2,720 | ● | ● | ● | Needs NAT on the host |
| mDNS / DNS-SD (odr-<id>.local) | `MDNS` | 6,744 | 6,504 | ● | ● | ● |  |
| TFTP codeplug read / write | `TFTP` | 6,356 | 6,412 | ● | ● | ● |  |
| SNMP v2c agent and traps (USB and over the air) | `SNMP` | 8,460 | 8,036 | ● | ● | ● |  |
| PPP on the serial host port (IPv4 into the gateway) | `NET_PPP` | 4,368* | 3,816* | ○ | ○ | ● | New, untested on hardware; interop tested with pppd. Fits neither default image as it is (DM-1701 688 B short, MD-380 1,588 B short; DM-1701 with LTO: +3,948, fits). DM-32: its only IP link, on by default with the gateway and every service (Whole images, DM-32UV) |

## User interface

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Settings menu on the radio | `UI_SETTINGS` | 5,304 | 5,640 | ● | ● | ● |  |
| Diagnostics page | `DIAG` | 3,752 | 3,532 | ● | ● | ● |  |
| 16x32 font | `FONT_XL` | 6,352* | — | ○ | ✕ | ○ | Only useful at 240 px and wider (MD-380 is 160 px); DM-32 runs at half resolution, so nothing gained yet |

## Bench

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | DM-1701 | MD-380 | DM-32UV | Notes |
|---|---|---:|---:|:-:|:-:|:-:|---|
| Audio path probe (debug link 0x10) | `AUDIO_PROBE` | 1,176* | — | ○ | ○ | ○ | Bench only, never in a release |

## Whole images

### DM-1701 and MD-380

| Build | DM-1701 | MD-380 |
|---|---:|---:|
| Default | 340,680 | 279,668 |
| Maximal (every compatible switch on) | 340,396 (LTO on) | 280,048 |
| FM only, minimal | 123,300 | 109,488 |

Turning on link time optimization (`LTO=1 LTO_3P=1`) on the DM-1701 default build gives 320,200 B (about 20 KB
less); it waits for hardware testing before it becomes that radio's default.

The MD-380's app region is 281,668 bytes (0x0800C000..0x08050C43; the codec runs in place after it). Its default
image (UHF; VHF within 30 bytes) fits only with link time optimization, 2026-10-11 at 3ec14e3:

| MD-380 build | Bytes used | Free |
|---|---|---|
| `LTO=0` | | overflows by 16,504 |
| `LTO=1 LTO_3P=0` | | overflows by 972 |
| `LTO=1 LTO_3P=1` (default) | 279,668 | 2,000 |
| maximal (default + `AM_RX BCAST_FM`), 2026-10-12 | 280,760 | 908 |

### DM-32UV

The DM-32UV has 880 KB of code space, so RAM limits it: 165,612 bytes above the stock codec's RAM window and 27,868
below it (RAM_LO). Its network is PPP on the programming cable (`NET_PPP`), on by default with the IP gateway and
every service, `TCP_CONNS=3` (`boards/dm32/config.mk`). It fits because the state the core marks `ODR_CPU_RAM` lives
in RAM_LO ([hardware-dm32.md](spec/hardware-dm32.md) 17.1). Measured with csky-elf GCC 14 (bytes of the image, RAM
of the linker's regions; the RTOS heap, 40 KB with every task stack, is inside the used part):

| DM-32UV build | Image | RAM (165,612 B) | RAM_LO (27,868 B) |
|---|---:|---:|---:|
| before the network (`ODR_CPU_RAM` in main RAM) | 313,804 | 153,740 (11,872 free) | 8,800 |
| default (PPP, gateway, every service, 3 TCP connections) | 365,236 | 154,428 (11,184 free) | 24,392 (3,476 free) |
| default, `TCP_CONNS=4` | 365,236 | 155,956 (9,656 free) | 24,392 |
| default, `TCP_CONNS=2` | 365,276 | 152,892 (12,720 free) | 24,392 |
| maximal (also `AUDIO_PROBE`, `FONT_XL`) | 372,856 | 155,740 (9,872 free) | 24,392 |

The margin kept, about 11 KB of main RAM, is what the image had before the network went in. RAM_LO has 3.5 KB left
for the RAM code (`.ramfunc`) and the core's `ODR_CPU_RAM` state to grow.

Flash each network switch costs on the DM-32UV (default minus the build with it off): the gateway with PPP and every
service 51,428 (`NET_PPP` or `DMR_IP_GATEWAY` off), TCP and what runs on it 17,028, SNMP 9,116, mDNS 7,220, TFTP
6,584, HTTP 5,076, rigctld 4,020, APRS-IS 2,964, KISS 688, control station 272. Each TCP connection is about 1.5 KB
of RAM (TCP, HTTP, rigctld).
