<!-- SPDX-License-Identifier: MIT -->

# Build configuration

Features that cost flash, and values that differ per radio, are chosen at build time. Defaults live in
`config/features.mk`, a board overrides them in `boards/<board>/config.mk`, and the make command line overrides both:

```
make BOARD=dm1701                        # the board's defaults
make BOARD=dm1701 CONFIG_DMR=n           # FM only
make BOARD=dm1701 CONFIG_VFO=n           # programmed channels only
make BOARD=dm1701 CONFIG_DISPLAY_INSET=3 # smaller screen margins
make BOARD=dm32                          # DM-32UV (csky-elf toolchain: tools/build-csky-toolchain.sh)
make BOARD=md380                         # MD-380 / MD-390 / RT3, UHF
make BOARD=md380 MD380_BAND=vhf          # the VHF radios (one band per radio, one image per band)
```

Each switch reaches C as `CONFIG_<NAME>` = 0 or 1, always defined: test it with `#if CONFIG_X`, never `#ifdef`.
`core/config.h` holds the same defaults (all on) for host tests and checks the dependencies. Switches can also leave
whole source directories out of the build (`Makefile`, `platform/*/platform.mk`). A sub-feature follows its parent
unless it is set: `CONFIG_DMR=n` turns off every DMR feature, and asking for a sub-feature without its parent is an
error. Changing a switch rebuilds everything (the build keeps the flags in `build/<board>/flags`, checked when make
starts). `make BOARD=<b> print-config` lists the resolved switches.

Every feature is on by default wherever the radio has what it needs. A board turns a feature off only for hardware
it lacks (the DM-1701 has no receive path for APRS receive) or when the image doesn't fit, and then the most obscure
large features go first. Bench diagnostics such as `CONFIG_AUDIO_PROBE` stay off.

The firmware version (`ODR_GIT`) is the commit plus `-dirty` when the tree differs from it (tracked changes, or
untracked sources in the built directories); only the few files that use it are rebuilt when it changes.

## Switches

What each switch costs in flash, and where it's on by default: [feature-matrix.md](feature-matrix.md). RAM figures
below are for the DM-1701.

| Switch | Default | What it covers |
|---|---|---|
| `CONFIG_FM` | y | analog FM receive and transmit |
| `CONFIG_SUBTONE` | = FM (MD-380: n) | CTCSS / DCS encode and decode |
| `CONFIG_FM_TX_TONES` | = FM (MD-380: n) | roger beep and 1750 Hz burst sent over the air (`docs/spec/radio-features.md` 3) |
| `CONFIG_VOX` | = FM | voice-operated transmit on FM, from the microphone level (`docs/spec/radio-features.md` 3.4) |
| `CONFIG_BATTERY_SAVE` | = FM | receiver duty cycling on an idle FM channel (`docs/spec/radio-features.md` 5.4) |
| `CONFIG_APRS` | = FM | APRS (`core/aprs`, `docs/spec/aprs.md`): position, status and message packets as AX.25 / Bell 202 AFSK on FM through `board.afsk`, positions over DMR (LRRP / UDT), beacon triggers, a KISS TNC on the host port. Transmit stays off until the user turns it on. About 2.8 KB RAM on the DM-1701 |
| `CONFIG_APRS_RX` | = APRS (DM-1701, MD-380: n) | APRS receive: the AFSK demodulator on the board's received audio, station list, messages to us with acks, the digipeater. Useful only where `board.afsk` can sample received audio (the DM-1701 can't, `docs/spec/aprs.md` 5.3). About 4 KB RAM more |
| `CONFIG_APRS_IGATE` | = APRS_RX and HOSTLINK | the iGate rules, RF to APRS-IS and messages back, with the host helper `tools/aprs_gate.py` holding the APRS-IS connection |
| `CONFIG_DMR` | y | DMR voice: modem driver, call stack, AMBE FEC, vocoder hosting, PCM audio |
| `CONFIG_DMR_PRIVACY` | = DMR | basic privacy (Motorola / Anytone BP) on the voice path, `core/dmr/dmrPrivacy.c` (`docs/spec/dmr-voice.md` 5) |
| `CONFIG_DMR_TALKER_ALIAS` | = DMR | talker alias decode and our own alias TX (`docs/spec/dmr-voice.md` 3) |
| `CONFIG_DMR_GPS` | = DMR | ETSI GPS Info in the embedded LC (FLCO 8): the caller's position on the receive screen (with distance and bearing when ours is known), in Recent and on the debug link; ours sent in the alias superframes when the position-in-DMR-calls setting under Tools > APRS is on and a position source has a fix (the APRS position: GPS, host, fixed; `docs/spec/dmr-voice.md`) |
| `CONFIG_DMR_SMS` | = DMR | text messages: ETSI IP text (the default format) and ETSI defined short data, received (inbox, ETSI response) and sent (docs/spec/dmr-data.md 5) |
| `CONFIG_DMR_TMS` | = DMR_SMS | Motorola TMS format support for text messages: TMS receive (inbox, the TMS ACK) and send with retries, the Motorola entry of the format setting. Off: a TMS heard is neither kept nor acknowledged (confirmed data still gets its ETSI response; the IP gateway still forwards it) and a stored TMS default sends ETSI IP text (docs/spec/dmr-data.md 5.1) |
| `CONFIG_DMR_ARS` | = DMR | ARS registration with a presence server, at power-up and channel change, refreshed (off until a server ID is set) |
| `CONFIG_DMR_LRRP` | = DMR | LRRP location answers (a radio without GPS answers "no fix"); builds core/dmr/dmrLrrp.c into the image |
| `CONFIG_DMR_SIGNALING` | = DMR | CSBK signaling: call alert and radio check, answered and sent |
| `CONFIG_DMR_EMERGENCY` | = DMR | DMR emergency: the alarm CSBK, the emergency call with hot mic, the alert heard (`spec/dmr-emergency.md`) |
| `CONFIG_DMR_ROAMING` | = DMR | DMR repeater roaming: per-zone roaming between repeater sites with the same talkgroup, chosen by base station sync with our color code and signal, passive (an optional wake probe sends a standard BS_Dwn_Act); Tools > Roaming, the Home chip (`docs/spec/dmr-roaming.md`). About 1 KB RAM on the DM-1701 |
| `CONFIG_DMR_IP_GATEWAY` | = DMR and (USB_NETWORK or NET_PPP) | IP over DMR packet data through a network link to the host, the USB network adapter or PPP on the serial host port, carried by the data service (`docs/spec/usb-network.md`, `docs/spec/ppp.md`). Needs one of the two links |
| `CONFIG_DMR_UDP_COMPRESS` | = DMR | compressed UDP/IPv4 headers (SAP 3, TS 102 361-3 5.6) on transmit: UDP packets go with a 5-9 octet header instead of 28 where the setting allows (default: ports outside 4000-4099; `docs/spec/dmr-data.md` 11) |
| `CONFIG_DMR_CONFIRMED` | = DMR | confirmed packet data for the IP gateway's packets per destination or UDP port (N(S), ACK / NACK / selective retry, tries), and for confirmed data to us the CRC-9 check, selective ACK, NACK and duplicate drop (`docs/spec/dmr-data.md` 12). Both together about 0.3 KB RAM on the DM-1701 |
| `CONFIG_NET_TCP` | = DMR_IP_GATEWAY | a minimal TCP for the radio's own services on the USB network link, never over RF (`core/net`, `docs/spec/tcp-services.md`). About 4.6 KB RAM (4 connections) on the DM-1701 |
| `CONFIG_NET_KISS` | = NET_TCP and APRS | KISS over TCP on port 8001 for APRS programs, bridged to the APRS KISS TNC (`docs/spec/tcp-services.md` 4). About 0.8 KB RAM |
| `CONFIG_NET_HTTP` | = NET_TCP | the HTTP status server on port 80: a page, the status and the debug link's view sections as JSON, the screen as a BMP (`docs/spec/tcp-services.md` 5). About 3.2 KB RAM |
| `CONFIG_NET_HTTP_GZIP` | = NET_HTTP | the status server's static files (`core/net/http_assets/`) stored gzip compressed at build time and served with `Content-Encoding: gzip` (`tools/gen_http_assets.py`, `docs/spec/tcp-services.md` 5); off: stored and served as they are |
| `CONFIG_NET_RIGCTL` | = NET_TCP | Hamlib rigctld on port 4532: frequency, mode, PTT (from the host only, rate limited), S meter for loggers and SDR programs (`docs/spec/tcp-services.md` 6). About 0.8 KB RAM |
| `CONFIG_NET_APRSIS` | = NET_TCP and APRS | the radio's own APRS-IS client out through the host (NAT on the host): our beacons and the iGate's lines, off until a server address is set (`docs/spec/tcp-services.md` 7). About 0.2 KB RAM |
| `CONFIG_MDNS` | = DMR_IP_GATEWAY | mDNS / DNS-SD responder on the USB network link: the radio answers for `odr-<DMR ID>.local` and advertises its debug link, MMDVM modem and TFTP server (`docs/spec/mdns.md`). About 0.4 KB RAM on the DM-1701 |
| `CONFIG_TFTP` | = DMR_IP_GATEWAY | TFTP server on the USB network link: `codeplug.bin` read and written (staged in `hal_flash_map_t.staging_*`, checked, then committed through the journal), `info.txt` (`docs/spec/tftp.md`). About 0.1 KB RAM on the DM-1701 |
| `CONFIG_DMR_CONTROL_STATION` | = DMR_IP_GATEWAY | control station mode for PC dispatch software: passthrough (the PC is the endpoint of the radio's own IP data while the link is up; the link layer stays on the radio) and the fixed link pair 192.168.10.1 / .2 instead of the address by DMR ID (`docs/spec/usb-network.md` 5). The power-on network setting belongs to `CONFIG_USB_NETWORK` |
| `CONFIG_SNMP` | = DMR_IP_GATEWAY | the SNMPv2c agent on UDP 161 over the USB network and over the air, the ODR MIB, notifications to the host and an RF manager (`core/snmp`, `docs/spec/snmp.md`). Management over the air read-only and RF notifications off by default. About 3.2 KB RAM on the DM-1701 |
| `CONFIG_SFR` | = DMR (MD-380: n) | single frequency repeater: the MMDVM hotspot's SFR mode (duplex with RX = TX); the stand-alone repeater and user roles on a channel are not wired yet (`docs/spec/sfr.md`) |
| `CONFIG_AM_RX` | n (DM-32: = FM) | air band AM receive: an FM channel or the VFO in 108-137 MHz is received in AM on a board with `CAP_AM_RX` and `rx_am`, transmit there refused, AM squelch on RSSI, 8.33 kHz steps (`docs/spec/airband-and-broadcast.md` 3); needs FM. The DM-32 glue isn't written yet |
| `CONFIG_BCAST_FM` | n (DM-32: y) | broadcast FM on a second receiver chip (`hal_bcast.h`): `core/bcast`, paused for channel traffic (`docs/spec/airband-and-broadcast.md` 4). The task, settings, screen and DM-32 glue aren't written yet |
| `CONFIG_USB` | y (DM-32: n) | USB device (CDC serial) |
| `CONFIG_HOSTLINK` | = USB (DM-32: y) | the host link (`core/hostlink`) on the board's host port: USB CDC, or the programming cable's UART on radios without USB (DM-32) |
| `CONFIG_USB_NETWORK` | = USB and HOSTLINK | USB network adapter personality: CDC-NCM, switched with the serial one at run time (debug link op 25) or chosen at power-on by the stored setting (`docs/spec/usb-network.md` 1) |
| `CONFIG_NET_PPP` | n (DM-32: = HOSTLINK and DMR; off with `CONFIG_DMR_IP_GATEWAY=n`) | IPv4 over PPP on the serial host port into the IP gateway and its services (`core/net/ppp.c`, `core/hostlink/hostlink_ppp.c`, `docs/spec/ppp.md`): for radios whose only host link is a USB-serial cable (DM-32UV, GD-88), and a fallback on the USB serial port. The host link enters it on pppd's first LCP frame or debug link radio op 28. Needs HOSTLINK and DMR_IP_GATEWAY, and turns the gateway (with its services) on by default. New, untested on hardware. About 3.9 KB flash and 90 B RAM on its own (it borrows the adapter's receive buffer); on the DM-32 with the gateway about 10.7 KB flash and 4.1 KB RAM (`docs/spec/ppp.md` 7). On by default on the DM-32UV, its only IP link (see "The DM-32UV's image" below) |
| `CONFIG_HOTSPOT` | = DMR and HOSTLINK (MD-380: n) | MMDVM modem / hotspot over the host port |
| `CONFIG_HOTSPOT_SPLIT` | = HOTSPOT (MD-380: n) | split duplex repeater: two radios, each an MMDVM modem, as one duplex repeater (the TX half sends the continuous BS outbound with the CACH, the RX half labels both inbound slots from the TX half's slot clock); the vendor command 0xD6 and the version suffix " SPLIT1", roles only after SET_ROLE, the HR_C6000 BS outbound engine, the debug link's SPLIT command (`docs/spec/split-duplex.md`). Needs `CONFIG_HOTSPOT`. Off, the image is unchanged. About 2.1 KB RAM on the DM-1701 |
| `CONFIG_DEBUGLINK` | = HOSTLINK | debug link for bench automation on the host port: diagnostics, screenshots, key / PTT injection, log ring (`docs/spec/debuglink.md`) |
| `CONFIG_CPS_LEGACY` | = HOSTLINK | legacy CPS protocol on the host port (USB serial, or the DM-32 programming cable UART; qdmr, the legacy CPS, `tools/cps_client.py`): codeplug read / write, radio info (`docs/spec/cps-protocol.md` 2). The settings store and the sector journal (`docs/spec/settings-store.md`) are always built |
| `CONFIG_VFO` | y | tuning a frequency by hand |
| `CONFIG_VFO_FM` | = VFO and FM | analog VFO |
| `CONFIG_VFO_DMR` | = VFO and DMR | digital VFO (not built yet) |
| `CONFIG_DIAG` | y | the diagnostics page |
| `CONFIG_SCAN` | y | zone, VFO band and dual watch scan (`docs/spec/radio-features.md` 2) |
| `CONFIG_BEEP` | y | key, TX permit / deny, call end, time-out warning, battery and scan beeps (`docs/spec/radio-features.md` 4) |
| `CONFIG_FONT_XL` | n | 16x32 font, for 240 px and wider displays (the DM-32 draws at 120x160, so it stays off there) |
| `CONFIG_AUDIO_PROBE` | n | diagnostics: debug link command 0x10 reads / writes the audio chip's registers and measures the PCM stream, for the USB audio bench tests (`docs/spec/usb-audio.md` 2.3, `docs/spec/debuglink.md` 3.14); needs DEBUGLINK and DMR; about 2 KB RAM and 2.2 KB flash on the DM-1701. Never for a release; CI builds it in the DM-1701's `audio-probe` configuration, not in maximal |
| `CONFIG_UI_SETTINGS` | y | the Settings menu on the radio; off: settings keep their stored values and are set with qdmr (`docs/spec/ui.md` 5) |

Derived (not set by hand): `CONFIG_DMR_DATA_CORE`, the shared packet data / CSBK code, on when any data feature is.

## Values

| Value | Default | Meaning |
|---|---|---|
| `CONFIG_DISPLAY_INSET` | 0 (DM-1701: 5) | pixels hidden by the bezel or lens at each edge; the UI draws inside them. The DM-1701 sets its top and bottom to 0 (only left and right are under the lens) |
| `CONFIG_DISPLAY_INSET_TOP` / `_BOTTOM` / `_LEFT` / `_RIGHT` | = `CONFIG_DISPLAY_INSET` | one edge |

### Board build variables

Not feature switches: they pick the hardware variant or set bench knobs, and `print-config` doesn't list them.

| Variable | Board | Meaning |
|---|---|---|
| `MD380_BAND` | md380 | `uhf` (default, 400-480 MHz) or `vhf` (136-174 MHz): the band table, the frequency plan and the board name ("MD-380 VHF") |
| `MD380_DEFS` | md380 | extra defines for the bench (`docs/testing/md380-bringup.md`): `-DMD380_PANEL_TYPE=n` and `-DMD380_PANEL_MADCTL_XOR=0xNN` (display controller and orientation), `-DMD380_CH_GRAY=1` / `-DMD380_CH_ACTIVE_HIGH=1` (channel switch code), `-DMD380_BATT_COUNTS_X10=n` (battery divider), `-DMD380_KEEP_SWD` (leave PA13 / PA14 to the debugger; the microphone is then unpowered) |
| `LTO` | 0 (MD-380: 1) | link time optimization (`-flto=auto` at compile and link): about 17 KB smaller on the DM-1701 (20 KB with `LTO_3P=1`). The DM-1701 image stays without it until it has been tested on the radio (`LTO=1` to try it); its CI maximal build uses it (`MAXIMAL_VARS`). The DM-32 (csky) doesn't use it |
| `LTO_3P` | 0 (MD-380: 1) | with `LTO=1`, the third-party code too (HAL, CMSIS, FreeRTOS, TinyUSB): about 3 KB more. FreeRTOS marks what its port's assembly reaches `portDONT_DISCARD`; the vectors are referenced from the assembly start-up. `LTO_EXCLUDE` lists files kept out (file-scope assembly: `platform/stm32f4/codec_call.c`) |
| `TCP_CONNS` | dm32 | TCP connections in all states (`core/net/tcp.h`, 4 elsewhere): 3 by default on the DM-32UV, about 1.5 KB RAM each across TCP, HTTP and rigctld |
| `MAXIMAL_VARS` | DM-1701: `LTO=1` | set in a board's `config.mk`: make variables `tools/ci.sh` adds to that board's maximal build (`make print-maximal-vars`) |
| `BOARD_UNSUPPORTED` | MD-380, DM-32: see their sections | set in a board's `config.mk`: switches the radio's hardware code doesn't cover; the maximal build leaves them off (`make print-unsupported`) |
| `MAXIMAL_SKIP` | DM-1701, MD-380: `NET_PPP` | set in a board's `config.mk`: switches that work on the radio but that it doesn't need, which the maximal build leaves off too (`make print-maximal-skip`). `NET_PPP` stands in for the USB network adapter on UART-only radios. The maximal build also leaves `CONFIG_AUDIO_PROBE` off on every board (`MAXIMAL_SKIP` in `tools/ci.sh`); the DM-1701's `audio-probe` build covers it |

### The MD-380's image

The MD-380's defaults (`boards/md380/config.mk`) switch off `CONFIG_SUBTONE`, `CONFIG_FM_TX_TONES` and `CONFIG_APRS_RX`
(no sub-audio or tone path written for that board yet) and `CONFIG_SFR` / `CONFIG_HOTSPOT_SPLIT` (HR_C6000 only): those
are missing hardware code, not space. Its app region is 281,668 bytes (0x0800C000..0x08050C43, the codec runs in place
after it). Everything else is on, all the IP services included, which fits only with link time optimization
(`LTO=1 LTO_3P=1`, the MD-380's default) and one feature off for flash: `CONFIG_HOTSPOT` (the MMDVM hotspot, 12 KB; it
needs a host running MMDVMHost, the MD-380's DMR is untested on air, and the same USB link carries IP over DMR and the
radio's services instead). `CONFIG_DMR_ROAMING` (also 12 KB) is the alternative, should the hotspot matter more there.
The maximal configuration also leaves out `CONFIG_FONT_XL` (a 160 px panel has no use for it) and `CONFIG_NET_PPP`.

Sizes of the default image (UHF; VHF within 30 bytes), 2026-10-11 at 3ec14e3:

| Build | Bytes used | Free |
|---|---|---|
| `LTO=0` | | overflows by 16,504 |
| `LTO=1 LTO_3P=0` | | overflows by 972 |
| `LTO=1 LTO_3P=1` (default) | 279,668 | 2,000 |
| maximal (default + `AM_RX BCAST_FM`), 2026-10-12 | 280,760 | 908 |

### The DM-32UV's image

The DM-32UV (`boards/dm32/config.mk`) has 880 KB of code space, so RAM is what limits it: 165,612 bytes above the stock
codec's RAM window and 27,868 below it (RAM_LO). The state the core marks `ODR_CPU_RAM` (the radio task, APRS, the
hotspot's rings, the DMR data buffers) goes to RAM_LO, the network's buffers stay in main RAM
(`docs/spec/hardware-dm32.md` 17.1). With that, everything is on: PPP on the programming cable and with it the IP
gateway and every service, `TCP_CONNS=3`. Off: `CONFIG_USB` and `CONFIG_USB_NETWORK` (no USB on the radio, so the
maximal configuration leaves them out too: `BOARD_UNSUPPORTED`), `CONFIG_FONT_XL` (the core draws at 120x160) and
`CONFIG_AUDIO_PROBE` (a bench diagnostic); the maximal configuration builds `CONFIG_FONT_XL`.

Sizes with csky-elf GCC 14 (RAM free is main RAM; the RTOS heap, 40 KB with every task stack in it, is inside the
used part):

| Build | Image bytes | RAM used | RAM free | RAM_LO used |
|---|---:|---:|---:|---:|
| default | 365,236 | 154,428 | 11,184 | 24,392 |
| default + `FONT_XL AUDIO_PROBE` | 372,856 | 155,740 | 9,872 | 24,392 |
| default, `TCP_CONNS=4` | 365,236 | 155,956 | 9,656 | 24,392 |
| default, `TCP_CONNS=2` | 365,276 | 152,892 | 12,720 | 24,392 |

The margin kept, about 11 KB of main RAM, is what the image had before the network went in. RAM_LO has 3.5 KB left for
the RAM code (`.ramfunc`) and the core's `ODR_CPU_RAM` state to grow.

## Adding a feature

1. Add the switch with a comment and its default to `config/features.mk`, to `FEATURES`, and to the dependency checks.
2. Add the default and the checks to `core/config.h`.
3. Guard the code, and leave a directory out in the Makefile when the whole directory belongs to the feature.
4. Add a row here and in [feature-matrix.md](feature-matrix.md), and check the build links with the switch off (`make BOARD=<b> CONFIG_X=n BUILD=build/test-x`).
   `tools/ci.sh` builds every board with each default-on switch off alone, an FM-minimal and a maximal
   configuration, all with zero warnings, and reports the image sizes (`docs/testing/README.md`). The maximal
   configuration leaves out the switches a board lists in `BOARD_UNSUPPORTED` (its `config.mk`): features its
   hardware support doesn't have, such as the HR_C6000-only SFR glue on the MD-380.
