<!-- SPDX-License-Identifier: MIT -->

# Build configuration

Features that cost flash, and values that differ per radio, are chosen at build time. Defaults live in
`config/features.mk`. A platform changes them for every radio on it in `platform/<p>/config.mk`, a board changes them
in `boards/<board>/config.mk`, and the make command line overrides all of them. The Makefile reads the board's file
before the platform's: both use `?=`, which keeps the first value set, so the board wins over its platform:

```
make BOARD=dm1701                        # the board's defaults
make BOARD=dm1701 CONFIG_DMR=n           # FM only
make BOARD=dm1701 CONFIG_VFO=n           # programmed channels only
make BOARD=dm1701 CONFIG_DISPLAY_INSET=3 # smaller screen margins
make BOARD=dm32                          # DM-32UV (csky-elf toolchain: tools/build-csky-toolchain.sh)
make BOARD=md380                         # MD-380 / MD-390 / RT3, UHF
make BOARD=md380 BAND=vhf                # the VHF radios (one band per radio, one image per band): build/md380-vhf
```

Each switch reaches C as `CONFIG_<NAME>` = 0 or 1, always defined: test it with `#if CONFIG_X`, never `#ifdef`.
`core/config.h` holds the same defaults (all on) for host tests and checks the dependencies. Switches can also leave
whole source directories out of the build (`Makefile`, `platform/*/platform.mk`). A sub-feature follows its parent
unless it is set: `CONFIG_DMR=n` turns off every DMR feature, and asking for a sub-feature without its parent is an
error. Changing a switch rebuilds everything (the build keeps the flags in `build/<board>/flags`, checked when make
starts). `make BOARD=<b> print-config` lists the resolved switches.

Every feature is on by default wherever the radio has what it needs. A board turns a feature off only for hardware
it lacks (or board code not written yet) or when the image doesn't fit, and then the most obscure
large features go first. Bench diagnostics such as `CONFIG_AUDIO_PROBE` stay off.

A feature that needs a part only some radios have (a vibration motor, a GPS receiver, SPI NAND) is a hardware switch:
`hardware = "..."` on its entry in `tests/hil/features.toml`. Each board then builds it, lists it in
`BOARD_UNSUPPORTED`, or names it in `board.toml` `[hardware] unconfirmed`, and `tools/boards.py check` enforces that.
The accelerometer is the next one: today its driver (`drivers/sensor/lis3dh`) only feeds a board's
diagnostics page under `CONFIG_DIAG`, so it has no switch. The first feature that uses motion brings a hardware
switch with it. A real-time clock driver is the same: none exists yet, and it arrives as `CONFIG_RTC`, a hardware
switch ([radio-features.md](spec/radio-features.md) 5.6).

## Variants

A variant is a named preset of switches for the prebuilt release images and the web flasher's build picker
([website.md](dev/website.md)): `config/variants/<name>.mk`, read after the board's `board.mk` and before its
`config.mk`, so it wins over the board's defaults and the command line wins over it. A variant only turns switches
off (`CONFIG_X ?= n`); the board's own limits still apply. Without `VARIANT` the build is the full default.

```
make BOARD=dm1701 VARIANT=amateur        # build/dm1701-amateur/
make BOARD=dm1701 VARIANT=commercial     # build/dm1701-commercial/
```

| Variant | Leaves out |
|---|---|
| (none: full) | nothing |
| `amateur` | basic privacy, Motorola TMS, ARS, LRRP, control station mode, SNMP |
| `commercial` | the MMDVM hotspot (and split duplex), APRS (and KISS, APRS-IS) |

Each variant file starts with `## title:` and `## summary:` lines, and the comment above each switch says why it is
left out; the site shows both. The lists are a draft for the project lead to settle.

The firmware version (`ODR_GIT`) is the commit plus `-dirty` when the tree differs from it (tracked changes, or
untracked sources in the built directories); only the few files that use it are rebuilt when it changes.

## Switches

What each switch costs in flash, and where it's on by default: [feature-matrix.md](feature-matrix.md). RAM figures
below are for the DM-1701.

| Switch | Default | What it covers |
|---|---|---|
| `CONFIG_FM` | y | analog FM receive and transmit |
| `CONFIG_SUBTONE` | = FM | CTCSS / DCS encode and decode. MD-380: software decode from the ADC and PWM encode, 3.3 KB |
| `CONFIG_FM_TX_TONES` | = FM (MD-380: n) | roger beep and 1750 Hz burst sent over the air (`docs/spec/radio-features.md` 3) |
| `CONFIG_VOX` | = FM | voice-operated transmit on FM, from the microphone level (`docs/spec/radio-features.md` 3.4) |
| `CONFIG_BATTERY_SAVE` | = FM | receiver duty cycling on an idle FM channel (`docs/spec/radio-features.md` 5.4) |
| `CONFIG_VIBRATE` | n (radios with a vibration motor: y) | vibration alerts on the board's motor (`hal_power_t.vibrate`) for messages, call alerts, private calls and emergencies, and Settings > Sound > Vibrate (`docs/spec/radio-features.md` 4.1). No radio on main has a motor yet; a board without one shows the row dimmed |
| `CONFIG_GPS` | y (radios without a GPS receiver: n) | the GPS receiver: the board's module driver and its NMEA input (`drivers/gps`, `platform/*/gps*`, `CAP_GPS`). Off, the radio has no GPS; DMR GPS, LRRP and APRS keep the fixed position and report no fix |
| `CONFIG_APRS` | = FM | APRS (`core/aprs`, `docs/spec/aprs.md`): position, status and message packets as AX.25 / Bell 202 AFSK on FM through `board.afsk`, positions over DMR (LRRP / UDT), beacon triggers, a KISS TNC on the host port. Transmit stays off until the user turns it on. About 2.8 KB RAM on the DM-1701 |
| `CONFIG_APRS_RX` | = APRS (MD-380: n) | APRS receive: the AFSK demodulator on the board's received audio, station list, messages to us with acks, the digipeater. Useful only where the board can sample received audio: its own AFSK receive path, or FM receive audio on the PCM stream (the DM-1701, `docs/spec/aprs.md` 5.3). About 6 KB RAM more |
| `CONFIG_APRS_IGATE` | = APRS_RX and HOSTLINK (DM-1701: n, flash) | the iGate rules, RF to APRS-IS and messages back, with the host helper `tools/aprs_gate.py` holding the APRS-IS connection |
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
| `CONFIG_NET_USRP` | = DMR_IP_GATEWAY | USRP voice over UDP on port 34001: the receiver's audio to AllStarLink / DVSwitch / a bridge with the squelch as keyup, the peer's keyup frames transmitted (FM through the board's audio tap, DMR through the encoder; `docs/spec/usb-audio.md` 9.1). About 5.6 KB RAM |
| `CONFIG_NET_APRSIS` | = NET_TCP and APRS | the radio's own APRS-IS client out through the host (NAT on the host): our beacons and the iGate's lines, off until a server address is set (`docs/spec/tcp-services.md` 7). About 0.2 KB RAM |
| `CONFIG_MDNS` | = DMR_IP_GATEWAY | mDNS / DNS-SD responder on the USB network link: the radio answers for `odr-<DMR ID>.local` and advertises its debug link, MMDVM modem and TFTP server (`docs/spec/mdns.md`). About 0.4 KB RAM on the DM-1701 |
| `CONFIG_TFTP` | = DMR_IP_GATEWAY | TFTP server on the USB network link: `codeplug.bin` read and written (staged in `hal_flash_map_t.staging_*`, checked, then committed through the journal), `info.txt` (`docs/spec/tftp.md`). About 0.1 KB RAM on the DM-1701 |
| `CONFIG_TFTP_IDDB` | = TFTP | the DMR ID database over TFTP: `iddb.bin` read and written in place (header erased first, programmed last), and the database part of a combined `codeplug.bin` (`docs/spec/tftp.md` 5). About 2.2 KB of flash |
| `CONFIG_DMR_CONTROL_STATION` | = DMR_IP_GATEWAY | control station mode for PC dispatch software: passthrough (the PC is the endpoint of the radio's own IP data while the link is up; the link layer stays on the radio) and the fixed link pair 192.168.10.1 / .2 instead of the address by DMR ID (`docs/spec/usb-network.md` 5). The power-on network setting belongs to `CONFIG_USB_NETWORK` |
| `CONFIG_SNMP` | = DMR_IP_GATEWAY | the SNMPv2c agent on UDP 161 over the USB network and over the air, the ODR MIB, notifications to the host and an RF manager (`core/snmp`, `docs/spec/snmp.md`). Management over the air read-only and RF notifications off by default. About 3.2 KB RAM on the DM-1701 |
| `CONFIG_SFR` | = DMR (MD-380: n) | single frequency repeater: MMDVM SFR (the hotspot, duplex with RX = TX); Standalone SFR (the repeater and user roles on a channel) is not wired yet (`docs/spec/sfr.md`) |
| `CONFIG_AM_RX` | n (DM-32: = FM) | air band AM receive: an FM channel or the VFO in 108-137 MHz is received in AM on a board with `CAP_AM_RX` and `rx_am`, transmit there refused, AM squelch on RSSI, 8.33 kHz steps (`docs/spec/airband-and-broadcast.md` 3); needs FM. The DM-32 glue isn't written yet |
| `CONFIG_BCAST_FM` | n (DM-32: y) | broadcast FM on a second receiver chip (`hal_bcast.h`): `core/bcast`, paused for channel traffic (`docs/spec/airband-and-broadcast.md` 4). The task, settings, screen and DM-32 glue aren't written yet |
| `CONFIG_USB` | y (DM-32: n) | USB device (CDC serial) |
| `CONFIG_HOSTLINK` | = USB (DM-32: y) | the host link (`core/hostlink`) on the board's host port: USB CDC, or the programming cable's UART on radios without USB (DM-32) |
| `CONFIG_USB_NETWORK` | = USB and HOSTLINK | USB network adapter personality: CDC-NCM, switched with the serial one at run time (debug link op 25) or chosen at power-on by the stored setting (`docs/spec/usb-network.md` 1) |
| `CONFIG_NET_PPP` | n (DM-32: = HOSTLINK and DMR; off with `CONFIG_DMR_IP_GATEWAY=n`) | IPv4 over PPP on the serial host port into the IP gateway and its services (`core/net/ppp.c`, `core/hostlink/hostlink_ppp.c`, `docs/spec/ppp.md`): for radios whose only host link is a USB-serial cable (DM-32UV, GD-88), and a fallback on the USB serial port. The host link enters it on pppd's first LCP frame or debug link radio op 28. Needs HOSTLINK and DMR_IP_GATEWAY, and turns the gateway (with its services) on by default. Checked on the DB25-D PRO and the DM-1801 Pro. About 3.9 KB flash and 90 B RAM on its own (it borrows the adapter's receive buffer); on the DM-32 with the gateway about 10.7 KB flash and 4.1 KB RAM (`docs/spec/ppp.md` 7). On by default on the DM-32UV, its only IP link ("Board defaults" below) |
| `CONFIG_HOTSPOT` | = DMR and HOSTLINK | MMDVM modem / hotspot over the host port |
| `CONFIG_HOTSPOT_SPLIT` | = HOTSPOT (MD-380: n) | split duplex repeater: two radios, each an MMDVM modem, as one duplex repeater (the TX half sends the continuous BS outbound with the CACH, the RX half labels both inbound slots from the TX half's slot clock); the vendor command 0xD6 and the version suffix " SPLIT1", roles only after SET_ROLE, the HR_C6000 BS outbound engine, the debug link's SPLIT command (`docs/spec/split-duplex.md`). Needs `CONFIG_HOTSPOT`. Off, the image is unchanged. About 2.1 KB RAM on the DM-1701 |
| `CONFIG_DEBUGLINK` | = HOSTLINK | debug link for bench automation on the host port: diagnostics, screenshots, key / PTT injection, log ring (`docs/spec/debuglink.md`) |
| `CONFIG_STUB_ENTRY` | = HOSTLINK | the stub entry: the flashing tools load their flash-access RAM monitor through the host link (command 0x1A) and the radio jumps to it, so updates need no key held at power-on (`docs/spec/stub-entry.md`). Its own feature: with `CONFIG_DEBUGLINK=n` it brings its own listener (PING, VERSION, STUB). Needs `CONFIG_HOSTLINK`. On the STM32 radios 8 KB of RAM (the staging buffer); with the debug link on, under 0.1 KB flash more than the debug link alone |
| `CONFIG_TASK_STACKS` | = DEBUGLINK | every task's least free stack in the debug link's `tasks` view section (`docs/spec/debuglink.md` 3.7): the kernel's task create / delete trace hooks keep a table of the tasks (`core/app/task_stack.c`). About 0.5 KB flash (0.7 KB on the HR_C7000 radios) and 72 bytes RAM |
| `CONFIG_CPS_OPENFW` | = HOSTLINK | open-firmware CPS protocol on the host port (USB serial, or the programming cable UART of the DM-32 and the DB25-D; qdmr, the open-firmware CPS, `tools/cps_client.py`): codeplug read / write, radio info (`docs/spec/cps-protocol.md` 2). The settings store and the sector journal (`docs/spec/settings-store.md`) are always built |
| `CONFIG_CPS_FAST` | = CPS_OPENFW and DEBUGLINK | the fast codeplug protocol, debug link command 0x1C (`docs/spec/cps-fast.md`): the same codeplug image in 4 KB blocks, blank blocks reported and not sent, CRC-32 sums so a host moves only the blocks that changed, LZ4 both ways, writes staged and committed under a power-cut safe record where the board has a staging area (through the sector journal otherwise), the codeplug re-read at the end. Needs `CONFIG_CPS_OPENFW` and `CONFIG_DEBUGLINK`. No buffer of its own (it streams through the storage sector buffer and the debug link's reply). About 3.9 KB flash and 0.2 KB RAM on the DM-1701 |
| `CONFIG_VFO` | y | tuning a frequency by hand, and Copy to VFO from a channel (about 0.6 to 0.7 KB, no switch of its own) |
| `CONFIG_VFO_FM` | = VFO and FM | analog VFO |
| `CONFIG_VFO_DMR` | = VFO and DMR | digital VFO: the VFO can be a DMR channel with its own color code, slot and talk target, and the Mode tile switches it FM / DMR (needs both VFO kinds; `docs/spec/dmr-voice.md` 2.1). About 140 B flash. Off, the VFO is analog and nothing about it changes |
| `CONFIG_DIAG` | y | the diagnostics page |
| `CONFIG_CRASH_LOG` | y | the crash log: a CPU fault, a task stack overflow, running out of heap or a failed assertion keeps a record (registers, fault status, task, stack words) where a reset doesn't clear it and restarts the radio; the next start takes it up, the debug link and (with `CONFIG_CRASH_SCREEN`) **Settings > Advanced > Crash log** show it (`core/crash`, `docs/spec/crash-log.md`). Needs no other feature. About 1.6 KB flash and 1.4 KB RAM (mostly the fault path's own stack) on the DM-1701, and 32 bytes more idle task stack. Off: a crash stops the radio until a power cycle |
| `CONFIG_CRASH_SCREEN` | = `CRASH_LOG` | **Settings > Advanced > Crash log**: the record's summary and Clear, and the controls of the crash report (`docs/spec/crash-log.md` 5.2). Needs `CONFIG_CRASH_LOG`. About 1 KB flash on the DM-1701. Off, the debug link still shows the record |
| `CONFIG_CRASH_REPORT` | = `CRASH_SCREEN` with DMR | sends the crash log's packed record as one UDP datagram (port 4090) over DMR to a codeplug contact, from the Crash log screen or after the restart (Off, Ask, Auto) (`docs/spec/crash-log.md` 5). Needs `CONFIG_CRASH_SCREEN` and `CONFIG_DMR`; brings in the DMR data core. About 1.4 KB flash on the DM-1701 |
| `CONFIG_WATCHDOG` | y | the hardware watchdog: `main()` starts it (the STM32 IWDG, about 5 s; the HR_C7000's, 4 s) and the idle task feeds it, so a hang or a starved system resets the radio (`docs/spec/watchdog.md`). With the debug link, command 0x1D and the `watchdog` view section test it on the bench. Needs no other feature. About 0.9 KB flash on the DM-1701, 1.4 KB on the HR_C7000 radios. Off: no watchdog runs |
| `CONFIG_FAULT_TRACE` | y (DM-1701, MD-380: n) | the fault trace: a CPU fault prints its registers on the host UART, three framed lines a second apart, then the record and the restart (`docs/spec/crash-log.md` 2.3, `tools/odr_fault_trace.py`); without the crash log the radio restarts too. Needs a host port on a UART: the HR_C7000 radios. About 600 B flash. Off: straight to the record and the restart |
| `CONFIG_NAND_STORAGE` | n (DB25-D: y) | the core's storage (settings, sector journal, codeplug areas) on SPI NAND: the NOR-like 4 KB sector layer over the board's own block range (`drivers/flash/nand_sectors`, `docs/spec/nand-sectors.md`) and the NAND driver's write side. Off, a NAND radio runs on the default settings and stores nothing. Radios with SPI NOR don't use it. On the DB25-D about 6.6 KB flash and 1.5 KB RAM (no page buffer) |
| `CONFIG_CHANNEL_LIST` | y | the hub's Channels list: every codeplug channel by number, and a channel's settings (`docs/spec/ui.md` 2) |
| `CONFIG_CHANNEL_EDIT` | = `CHANNEL_LIST` | editing a channel on the radio, New channel from current settings, keypad text entry (`docs/spec/ui.md` 2.1). Needs `CONFIG_CHANNEL_LIST`; without it the channel's settings are read only |
| `CONFIG_KEY_ASSIGN` | y | side key assignment: Settings > Keys and the codeplug's quick-key slots say what the board's side keys do (`docs/spec/ui.md` 3.1) |
| `CONFIG_SCAN` | y | zone, VFO band and dual watch scan (`docs/spec/radio-features.md` 2) |
| `CONFIG_DUAL_WATCH` | y (with `SCAN`) | the dual watch screens: Home with both channels, the channel picker and the dual watch actions ([dual-watch](ux/dual-watch.md)); in the radio core, channels of other zones and the VFO, and PTT on the selected channel without ending the watch (`docs/spec/radio-features.md` 2.1). Off: dual watch stays reachable only from the debug link (two channels of the current zone) |
| `CONFIG_BEEP` | y | key, TX permit / deny, call end, time-out warning, battery and scan beeps (`docs/spec/radio-features.md` 4) |
| `CONFIG_FONT_XL` | n (DM-32: y) | 16x32 font, for 240 px and wider displays |
| `CONFIG_FONT_AA` | n (DM-32: y) | the fonts from 6x12 up anti-aliased (JetBrains Mono Bold, 2 bits a pixel) on color panels instead of the bitmap ones; a mono panel would draw them as plain ink, so mono radios leave it off. Glyphs: 15.6 KB with `FONT_XL` (8.2 KB without) in place of 9.2 KB (5.3 KB) of bitmap ones (`core/ui/fonts.c`). On a 4 bpp panel (every color board) the edge grays share the 16 palette entries with the screen's own colors; a screen that already uses many colors loses some of them, and some font sizes or colors can then be slightly harder to read (`docs/spec/ui.md` 5) |
| `CONFIG_FONT_BIG` | n (DM-32: y) | larger text on 240 px and wider color panels: lists, labels, the status strip and hints in a 9x18 font instead of 8x16 (24 characters a line on the DM-32 instead of 27; the 8x16 one takes the small print). Needs `FONT_AA`; 3.5 KB of flash on the DM-32 |
| `CONFIG_UI_COLOR` | y (DM-1801 Pro: n) | color screen settings: Theme, Highlight color and Filled status bar in Settings > Display (`docs/spec/ui.md` 5, 8). Off: no such rows, the dark theme's colors. About 600 B; off for radios whose only screen is monochrome |
| `CONFIG_AUDIO_PROBE` | n | diagnostics: debug link command 0x10 reads / writes the audio chip's registers and measures the PCM stream, for the USB audio bench tests (`docs/spec/hardware-dm1701.md` 21.3, `docs/spec/debuglink.md` 3.14); needs DEBUGLINK and DMR; about 2 KB RAM and 2.2 KB flash on the DM-1701. Never for a release; CI builds it in the DM-1701's `audio-probe` configuration (with `LTO=1`: the default image has no room for it without), not in maximal. A bench image without LTO: add `CONFIG_SNMP=n` |
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
| `BAND` | md380 | A build option (`board.toml` `[[build_options]]`, `board.mk` `BUILD_OPTIONS`): `uhf` (default, 400-480 MHz) or `vhf` (136-174 MHz), the band table, the frequency plan and the board name ("MD-380 VHF"). Given on the command line, it adds its value to the build directory (`build/md380-vhf`) |
| `MD380_DEFS` | md380 | extra defines for the bench (`docs/testing/md380-bringup.md`): `-DMD380_PANEL_TYPE=n` and `-DMD380_PANEL_MADCTL_XOR=0xNN` (display controller and orientation), `-DMD380_CH_ACTIVE_HIGH=1` (channel switch polarity), `-DMD380_BATT_COUNTS_X10=n` (battery divider), `-DMD380_KEEP_SWD` (leave PA13 / PA14 to the debugger; the microphone is then unpowered) |
| `GD77_DEFS` | gd77 | extra defines for the bench ([hardware-gd77.md](spec/hardware-gd77.md) 4.1): `-DGD77_I2C_A_HW=1` / `-DGD77_I2C_B_HW=1` (an I2C bus on I2C0), `-DGD77_C6K_HW_CS=1` (the HR_C6000 port's chip select as SPI0 PCS0), `-DGD77_BACKLIGHT_PWM=1` (the backlight on an FTM channel), `-DGD77_CLOCK_TARGET=1` (the 95.232 MHz HSRUN clock first). All off by default until a radio has checked them |
| `LTO` | 0 (MD-380, DM-1701: 1) | link time optimization (`-flto=auto` at compile and link): about 17 KB smaller on the DM-1701 (20 KB with `LTO_3P=1`). The DM-1701 and MD-380 defaults needed it while their application areas were 336 KB and 275 KB; since the codec moved ([codec-placement.md](spec/codec-placement.md)) they have 720 KB and 464 KB, and it stays on for the smaller image. The DM-32 (csky) doesn't use it |
| `LTO_3P` | 0 (MD-380: 1) | with `LTO=1`, the third-party code too (HAL, CMSIS, FreeRTOS, TinyUSB): about 3 KB more. FreeRTOS marks what its port's assembly reaches `portDONT_DISCARD`; the vectors are referenced from the assembly start-up. `LTO_EXCLUDE` lists files kept out (file-scope assembly: `platform/stm32f4/codec_call.c`) |
| `CONFIG_GD77_TX` | gd77 | `n` (default) or `y`: the bands allow transmit and `tx()` may key, FM and DMR, still only with a valid calibration point ([hardware-gd77.md](spec/hardware-gd77.md) 5.4) |
| `TCP_CONNS` | dm32 | TCP connections in all states (`core/net/tcp.h`, 4 elsewhere): 3 by default on the DM-32UV, about 1.5 KB RAM each across TCP, HTTP and rigctld |
| `MAXIMAL_VARS` | DM-1701: `LTO=1` | set in a board's `config.mk`: make variables `tools/ci.sh` adds to that board's maximal build (`make print-maximal-vars`) |
| `BOARD_UNSUPPORTED` | DM-1701: `FONT_XL`; MD-380, DM-32: see "Board defaults" | set in a board's `config.mk`, one switch a line with the reason in its comment: switches the radio's hardware code doesn't cover; the maximal build leaves them off (`make print-unsupported`) |
| `BOARD_REQUIRED` | DB25-D: `FM` | set in a board's `config.mk`: switches the board can't build without; ci.sh builds no `no-` configuration for them (`make print-required`) |
| `MAXIMAL_SKIP` | DM-1701: `NET_PPP APRS_IGATE`; MD-380: `NET_PPP NAND_STORAGE` | set in a board's `config.mk`: switches that work on the radio but that it doesn't need, which the maximal build leaves off too (`make print-maximal-skip`). `NET_PPP` stands in for the USB network adapter on UART-only radios. The maximal build also leaves `CONFIG_AUDIO_PROBE` off on every board (`MAXIMAL_SKIP` in `tools/ci.sh`); the DM-1701's `audio-probe` build covers it |

### Board defaults

Where a board's `config.mk` differs from the defaults, and why. Image sizes: [feature-matrix.md](feature-matrix.md),
"Whole images".

| Board | Off by default | Why |
|---|---|---|
| md380 | `FM_TX_TONES`, `APRS_RX` (maximal build too: `BOARD_UNSUPPORTED`) | no tone path or APRS receive written for the board yet; `APRS_IGATE` follows `APRS_RX` |
| md380 | `SFR`, `HOTSPOT_SPLIT` (maximal build too) | HR_C6000 only |
| md380 | `FONT_XL`, `AM_RX`, `BCAST_FM`, `NET_PPP` (off on most boards; the maximal build leaves them off here too) | a 160 px panel has no use for the font; no air band or broadcast receiver; PPP stands in for USB |
| dm1701, md380 | `FAULT_TRACE` (maximal build too: `BOARD_UNSUPPORTED`) | the host port is USB, and the one UART is a GPS receive line: nothing a fault handler can print on |
| dm32 | `USB`, `USB_NETWORK` (maximal build too: `BOARD_UNSUPPORTED`) | no USB data port on the radio; PPP on the programming cable is its IP link |
| dm1801pro | `UI_COLOR` (maximal build too: `BOARD_UNSUPPORTED`) | a 128x64 monochrome screen: no colors to set |
| every board | `AUDIO_PROBE` | a bench diagnostic |

The DM-32UV puts the state the core marks `ODR_CPU_RAM` in its low RAM below the codec window, and keeps the
network's buffers in main RAM ([hardware-dm32.md](spec/hardware-dm32.md) 17.1). With that, everything else is on.

## Adding a feature

1. Add the switch with a comment and its default to `config/features.mk`, to `FEATURES`, and to the dependency checks.
2. Add the default and the checks to `core/config.h`.
3. Guard the code, and leave a directory out in the Makefile when the whole directory belongs to the feature.
4. Add a row here and in [feature-matrix.md](feature-matrix.md) (the web site takes the feature's group, title and
   size from it; `site/build.py --strict` fails without the row), and check the build links with the switch off (`make BOARD=<b> CONFIG_X=n BUILD=build/test-x`).
   `tools/ci.sh` builds every board with each default-on switch off alone, an FM-minimal and a maximal
   configuration, all with zero warnings, and reports the image sizes (`docs/testing/README.md`). The maximal
   configuration leaves out the switches a board lists in `BOARD_UNSUPPORTED` (its `config.mk`): features its
   hardware support doesn't have, such as the HR_C6000-only SFR glue on the MD-380.
