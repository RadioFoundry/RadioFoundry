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

| Switch | Default | What it covers |
|---|---|---|
| `CONFIG_FM` | y | analog FM receive and transmit |
| `CONFIG_SUBTONE` | = FM | CTCSS / DCS encode and decode |
| `CONFIG_FM_TX_TONES` | = FM | roger beep and 1750 Hz burst sent over the air (`docs/spec/radio-features.md` 3) |
| `CONFIG_VOX` | = FM | voice-operated transmit on FM, from the microphone level (`docs/spec/radio-features.md` 3.4) |
| `CONFIG_BATTERY_SAVE` | = FM | receiver duty cycling on an idle FM channel (`docs/spec/radio-features.md` 5.4) |
| `CONFIG_APRS` | = FM | APRS (`core/aprs`, `docs/spec/aprs.md`): position, status and message packets as AX.25 / Bell 202 AFSK on FM through `board.afsk`, positions over DMR (LRRP / UDT), beacon triggers, a KISS TNC on the host port. Transmit stays off until the user turns it on. About 18 KB flash, 2.8 KB RAM on the DM-1701 |
| `CONFIG_APRS_RX` | = APRS (DM-1701: n) | APRS receive: the AFSK demodulator on the board's received audio, station list, messages to us with acks, the digipeater. Useful only where `board.afsk` can sample received audio (the DM-1701 can't, `docs/spec/aprs.md` 5.3). About 10 KB flash, 4 KB RAM more |
| `CONFIG_APRS_IGATE` | = APRS_RX and HOSTLINK | the iGate rules, RF to APRS-IS and messages back, with the host helper `tools/aprs_gate.py` holding the APRS-IS connection |
| `CONFIG_DMR` | y | DMR voice: modem driver, call stack, AMBE FEC, vocoder hosting, PCM audio |
| `CONFIG_DMR_PRIVACY` | = DMR | basic privacy (Motorola / Anytone BP) on the voice path, `core/dmr/dmrPrivacy.c` (`docs/spec/dmr-voice.md` 5) |
| `CONFIG_DMR_TALKER_ALIAS` | = DMR | talker alias decode and our own alias TX (`docs/spec/dmr-voice.md` 3) |
| `CONFIG_DMR_GPS` | = DMR | ETSI GPS Info in the embedded LC (FLCO 8): the caller's position on the receive screen (with distance and bearing when ours is known), in Recent and on the debug link; ours sent in the alias superframes when Settings > DMR > Send position is on and a position source has a fix (the APRS position: GPS, host, fixed; `docs/spec/dmr-voice.md`) |
| `CONFIG_DMR_SMS` | = DMR | text messages: Motorola TMS receive (inbox, ACK) and send, with retries (docs/spec/dmr-data.md) |
| `CONFIG_DMR_ARS` | = DMR | ARS registration with a presence server, at power-up and channel change, refreshed (off until a server ID is set) |
| `CONFIG_DMR_LRRP` | = DMR | LRRP location answers (a radio without GPS answers "no fix"); builds core/dmr/dmrLrrp.c into the image |
| `CONFIG_DMR_SIGNALING` | = DMR | CSBK signaling: call alert and radio check, answered and sent |
| `CONFIG_DMR_EMERGENCY` | = DMR | DMR emergency: the alarm CSBK, the emergency call with hot mic, the alert heard (`spec/dmr-emergency.md`) |
| `CONFIG_DMR_ROAMING` | = DMR | DMR repeater roaming: per-zone roaming between repeater sites with the same talkgroup, chosen by base station sync with our color code and signal, passive (an optional wake probe sends a standard BS_Dwn_Act); Tools > Roaming, the Home chip (`docs/spec/dmr-roaming.md`). About 5.2 KB flash, 1 KB RAM on the DM-1701 |
| `CONFIG_DMR_IP_GATEWAY` | = DMR and USB_NETWORK | IP over DMR packet data through the USB network adapter, carried by the data service (`docs/spec/usb-network.md`) |
| `CONFIG_SNMP` | = DMR_IP_GATEWAY | the SNMPv2c agent on UDP 161 over the USB network and over the air, the ODR MIB, notifications to the host and an RF manager (`core/snmp`, `docs/spec/snmp.md`). Management over the air read-only and RF notifications off by default. About 8.6 KB flash, 3.2 KB RAM on the DM-1701 |
| `CONFIG_SFR` | = DMR | single frequency repeater: the MMDVM hotspot's SFR mode (duplex with RX = TX); the stand-alone repeater and user roles on a channel are not wired yet (`docs/spec/sfr.md`) |
| `CONFIG_AM_RX` | n (DM-32: = FM) | air band AM receive: an FM channel or the VFO in 108-137 MHz is received in AM on a board with `CAP_AM_RX` and `rx_am`, transmit there refused, AM squelch on RSSI, 8.33 kHz steps (`docs/spec/airband-and-broadcast.md` 3); needs FM. The DM-32 glue isn't written yet |
| `CONFIG_BCAST_FM` | n (DM-32: y) | broadcast FM on a second receiver chip (`hal_bcast.h`): `core/bcast`, paused for channel traffic (`docs/spec/airband-and-broadcast.md` 4). The task, settings, screen and DM-32 glue aren't written yet |
| `CONFIG_USB` | y (DM-32: n) | USB device (CDC serial) |
| `CONFIG_HOSTLINK` | = USB (DM-32: y) | the host link (`core/hostlink`) on the board's host port: USB CDC, or the programming cable's UART on radios without USB (DM-32) |
| `CONFIG_USB_NETWORK` | = USB and HOSTLINK | USB network adapter personality: CDC-NCM, switched with the serial one at run time (debug link op 25) |
| `CONFIG_HOTSPOT` | = DMR and HOSTLINK | MMDVM modem / hotspot over the host port |
| `CONFIG_HOTSPOT_SPLIT` | = HOTSPOT | split duplex repeater: two radios, each an MMDVM modem, as one duplex repeater (the TX half sends the continuous BS outbound with the CACH, the RX half labels both inbound slots from the TX half's slot clock); the vendor command 0xD6 and the version suffix " SPLIT1", roles only after SET_ROLE, the HR_C6000 BS outbound engine, the debug link's SPLIT command (`docs/spec/split-duplex.md`). Needs `CONFIG_HOTSPOT`. Off, the image is unchanged. About 12.8 KB flash, 2.1 KB RAM on the DM-1701 |
| `CONFIG_DEBUGLINK` | = HOSTLINK | debug link for bench automation on the host port: diagnostics, screenshots, key / PTT injection, log ring (`docs/spec/debuglink.md`) |
| `CONFIG_CPS_LEGACY` | = HOSTLINK | legacy CPS protocol on the host port (USB serial, or the DM-32 programming cable UART; qdmr, the legacy CPS, `tools/cps_client.py`): codeplug read / write, radio info (`docs/spec/cps-protocol.md` 2). The settings store and the sector journal (`docs/spec/settings-store.md`) are always built |
| `CONFIG_VFO` | y | tuning a frequency by hand |
| `CONFIG_VFO_FM` | = VFO and FM | analog VFO |
| `CONFIG_VFO_DMR` | = VFO and DMR | digital VFO (not built yet) |
| `CONFIG_DIAG` | y | the diagnostics page |
| `CONFIG_SCAN` | y | zone, VFO band and dual watch scan (`docs/spec/radio-features.md` 2) |
| `CONFIG_BEEP` | y | key, TX permit / deny, call end, time-out warning, battery and scan beeps (`docs/spec/radio-features.md` 4) |
| `CONFIG_FONT_XL` | n | 16x32 font, for 240 px and wider displays (the DM-32 draws at 120x160, so it stays off there) |
| `CONFIG_AUDIO_PROBE` | n | diagnostics: debug link command 0x10 reads / writes the audio chip's registers and measures the PCM stream, for the USB audio bench tests (`docs/spec/usb-audio.md` 2.3, `docs/spec/debuglink.md` 3.14); needs DEBUGLINK and DMR; about 6 KB flash, 2 KB RAM on the DM-1701. Never for a release |
| `CONFIG_UI_SETTINGS` | y | the Settings menu on the radio; off: settings keep their stored values and are set with qdmr (`docs/spec/ui.md` 5) |

Derived (not set by hand): `CONFIG_DMR_DATA_CORE`, the shared packet data / CSBK code, on when any data feature is.

## Values

| Value | Default | Meaning |
|---|---|---|
| `CONFIG_DISPLAY_INSET` | 0 (DM-1701: 5) | pixels hidden by the bezel or lens at each edge; the UI draws inside them |
| `CONFIG_DISPLAY_INSET_TOP` / `_BOTTOM` / `_LEFT` / `_RIGHT` | = `CONFIG_DISPLAY_INSET` | one edge |

### Board build variables

Not feature switches: they pick the hardware variant or set bench knobs, and `print-config` doesn't list them.

| Variable | Board | Meaning |
|---|---|---|
| `MD380_BAND` | md380 | `uhf` (default, 400-480 MHz) or `vhf` (136-174 MHz): the band table, the frequency plan and the board name ("MD-380 VHF") |
| `MD380_DEFS` | md380 | extra defines for the bench (`docs/testing/md380-bringup.md`): `-DMD380_PANEL_TYPE=n` and `-DMD380_PANEL_MADCTL_XOR=0xNN` (display controller and orientation), `-DMD380_CH_GRAY=1` / `-DMD380_CH_ACTIVE_HIGH=1` (channel switch code), `-DMD380_BATT_COUNTS_X10=n` (battery divider), `-DMD380_KEEP_SWD` (leave PA13 / PA14 to the debugger; the microphone is then unpowered) |

The MD-380's defaults (`boards/md380/config.mk`) switch off `CONFIG_SUBTONE`, `CONFIG_FM_TX_TONES` and `CONFIG_APRS_RX`
(no sub-audio or tone path written for that board yet) and `CONFIG_SFR` / `CONFIG_HOTSPOT_SPLIT` (HR_C6000 only).

## Adding a feature

1. Add the switch with a comment and its default to `config/features.mk`, to `FEATURES`, and to the dependency checks.
2. Add the default and the checks to `core/config.h`.
3. Guard the code, and leave a directory out in the Makefile when the whole directory belongs to the feature.
4. Add a row here, and check the build links with the switch off (`make BOARD=<b> CONFIG_X=n BUILD=build/test-x`).
   `tools/ci.sh` builds every board with each default-on switch off alone, an FM-minimal and a maximal
   configuration, all with zero warnings, and reports the image sizes (`docs/testing/README.md`). The maximal
   configuration leaves out the switches a board lists in `BOARD_UNSUPPORTED` (its `config.mk`): features its
   hardware support doesn't have, such as the HR_C6000-only SFR glue on the MD-380.
