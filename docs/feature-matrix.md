<!-- SPDX-License-Identifier: MIT -->

# Feature matrix

Every feature, the build switch it sits behind (`CONFIG_<switch>` in `config/features.mk`,
[build-config.md](build-config.md)), what it costs in flash on the ARM radios, and where it's built by default.

- **Bytes** = default image minus the image with that switch off (`tools/ci.sh` builds each switch off), flash bytes.
  A parent switch's figure includes its children (turning `FM` off also drops VOX, APRS, ...).
- Figures from `tools/ci.sh` at 7ba8949c, GCC 15, each radio's default build: link time optimization on (`LTO=1`;
  the MD-380 also `LTO_3P=1`) and Spanish on. With LTO a switch also changes what gets inlined elsewhere, so small
  figures are approximate.
  - DM-1701: default image 704,632 B of its 720 KB app region (sectors 3-9).
  - MD-380: default image 575,736 B of its 848 KB app region (sectors 3-10).
  - A switch's figure includes its text in every language the image carries: the language tables hold only the
    strings of the code a build compiles ([translating.md](dev/translating.md)).
  - `*` = off by default; measured by turning it on in that radio's default build.
  - The figures for switches these two radios don't build now (`AM_RX`, `BCAST_FM`, `VIBRATE`, `FONT_XL`; `P25` on the
    MD-380) and for `AUDIO_PROBE` are older, from before the language tables held only the compiled strings.
- **Radios:** which switches each radio builds by default is in
  [Built by default on each radio](#built-by-default-on-each-radio), generated from the build. A — in a bytes column
  means not built there, so not measured.
- **A mark in the Built by default table is not a statement that the feature works.** ● means the switch is on in
  that radio's default image, nothing more. A feature can be built in and still do nothing on a radio, or never have been tried on
  it. What has actually been checked on each radio is in [Checked on each radio](#checked-on-each-radio) below.
- The DM-32UV builds with csky-elf GCC; its sizes aren't in the per-feature tables (ARM only for now); its whole images are
  at the end.


## Checked on each radio

Generated from the bring-up records in [testing/results/](testing/results/README.md), by the status rule of
`tools/bringup.py status`. "pass" means a recorded run (`tools/bringup.py run`) passed the feature's required tests.
"(seed)" means the evidence is the board's seed record: a check the docs recorded before the board's first run.
"needs re-run" means the feature's spec or source files changed since that check; the result shown is the old one.
"not recorded" means no record covers the feature on that radio: it is built in and host tested, and should be
treated as unverified. "off" is off in the default build, ✕ not available. A radio's checklist with the required tests
is `tools/bringup.py list --radio <id>`.

<!-- generated: tools/boards.py docs checked; edit docs/testing/results/<board>/ (tools/bringup.py run) -->
| Feature | DM-32UV | DM-1701 | DM-1801 Pro | DB25-D PRO | MD-380 | MD-UV380 | MD-9600 | RT10 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `fm` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded | partial, needs re-run | partial, needs re-run |
| `ref_trim` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded | pass, needs re-run | partial, needs re-run |
| `subtone` | pass (seed) | pass (seed) | pass (seed) | not recorded | pass (seed) | not recorded | pass, needs re-run | partial, needs re-run |
| `fm_tx_tones` | not recorded | pass (seed) | not recorded | not recorded | not written | not recorded | pass, needs re-run | partial, needs re-run |
| `tone_burst_key` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | partial, needs re-run |
| `dtmf` | not recorded | not recorded | not recorded | not recorded | not written | not recorded | pass, needs re-run | ✕ |
| `vox` | not written | not recorded | not recorded | not written | not recorded | not recorded | pass, needs re-run | not written |
| `battery_save` | pass (seed) | pass (seed) | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | partial, needs re-run |
| `am_rx` | not recorded | ✕ | not recorded | ✕ | ✕ | ✕ | ✕ | ✕ |
| `bcast_fm` | not recorded | ✕ | not recorded | not written | ✕ | ✕ | ✕ | ✕ |
| `rx_ext` | not recorded | not recorded | not recorded | not recorded | not written | not recorded | not written | not recorded |
| `p25` | not recorded | not recorded | not recorded | not recorded | not written | not recorded | not recorded | partial, needs re-run |
| `p25_trunk` | not recorded | not recorded | not recorded | not recorded | not written | not recorded | not recorded | not recorded |
| `nxdn` | not recorded | not recorded | not recorded | not recorded | not written | not recorded | not recorded | partial, needs re-run |
| `nxdn_scrambler` | not recorded | not recorded | not recorded | not recorded | not written | not recorded | not recorded | not recorded |
| `dmr_voice` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded | partial, needs re-run | partial, needs re-run |
| `dmr_privacy` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | partial, needs re-run |
| `dmr_aes` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_data_privacy` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_talker_alias` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `dmr_gps` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `dmr_emergency` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded | pass | pass |
| `dmr_roaming` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `dmr_trunk` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_trunk_tx` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_trunk_supl` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_trunk_gw` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `voice_capture` | off | off | off | off | off | off | off | off |
| `dmr_signaling` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `vfo` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | ✕ |
| `vfo_fm` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | partial, needs re-run | ✕ |
| `vfo_dmr` | not recorded | not recorded | not recorded | pass (seed) | not recorded | not recorded | pass, needs re-run | ✕ |
| `channel_list` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | ✕ |
| `channel_edit` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | ✕ |
| `zone_edit` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | ✕ |
| `contact_edit` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | ✕ |
| `volume_bar` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `volume_lock` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `key_assign` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `scan` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | partial, needs re-run |
| `dual_watch` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded | partial, needs re-run | ✕ |
| `beep` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | partial, needs re-run | partial, needs re-run |
| `signaling_tones` | not recorded | not recorded | not recorded | not recorded | off | not recorded | not recorded | not recorded |
| `vibrate` | off | ✕ | not recorded | ✕ | ✕ | ✕ | ✕ | ✕ |
| `gps` | pass (seed) | not recorded | ✕ | not recorded | not recorded | not recorded | not recorded | ✕ |
| `satellite_tle` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `gps_icon` | not recorded | not recorded | ✕ | not recorded | not recorded | not recorded | not recorded | ✕ |
| `position` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run |
| `dmr_data_rx` | not recorded | pass (seed) | pass (seed) | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `dmr_sms` | not recorded | pass (seed) | pass (seed) | not recorded | pass (seed) | not recorded | pass, needs re-run | ✕ |
| `dmr_tms` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | ✕ |
| `dmr_ars` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_lrrp` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `dmr_udp_compress` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | partial, needs re-run |
| `dmr_confirmed` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass | partial |
| `sfr` | not recorded | pass (seed) | pass (seed) | not recorded | ✕ | not recorded | ✕ | partial |
| `sfr_dualpath_rx` | not recorded | not recorded | not recorded | not recorded | ✕ | not recorded | not recorded | not recorded |
| `sfr_cwid` | not recorded | not recorded | not recorded | not recorded | ✕ | not recorded | pass, needs re-run | pass, needs re-run |
| `aprs` | pass (seed) | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | partial, needs re-run |
| `aprs_afsk` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | partial, needs re-run |
| `aprs_dmr` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | partial, needs re-run |
| `aprs_rx` | pass (seed) | not recorded | not recorded | not written | not written | not recorded | pass, needs re-run | partial, needs re-run |
| `aprs_mute` | not recorded | not recorded | not recorded | not written | not written | not recorded | not recorded | not recorded |
| `aprs_igate` | not recorded | off | not recorded | not written | not written | off | pass, needs re-run | partial, needs re-run |
| `gps_sky` | not recorded | not recorded | ✕ | not recorded | not recorded | not recorded | not recorded | ✕ |
| `usb` | ✕ | pass (seed) | ✕ | ✕ | not recorded | not recorded | not recorded | ✕ |
| `hostlink` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `hotspot` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded | not recorded | pass, needs re-run | off |
| `split_duplex` | pass (seed) | pass (seed) | pass (seed) | not written | not written | not recorded | partial, needs re-run | not written |
| `debuglink` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded | pass, needs re-run | partial, needs re-run |
| `task_stacks` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass | pass |
| `cps_openfw` | not recorded | pass (seed) | not recorded | pass (seed) | pass (seed) | not recorded | pass, needs re-run | pass, needs re-run |
| `cps_fast` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `stub_entry` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `crash_log` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `crash_screen` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | ✕ |
| `crash_report` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | ✕ |
| `watchdog` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass | pass |
| `support_info` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `fault_trace` | not recorded | ✕ | not recorded | not recorded | ✕ | ✕ | ✕ | not recorded |
| `usb_network` | ✕ | pass (seed) | ✕ | ✕ | not recorded | not recorded | pass, needs re-run | ✕ |
| `usb_serial_request` | off | not recorded | off | off | not recorded | not recorded | not recorded | off |
| `dmr_ip_gateway` | not recorded | not recorded | pass (seed) | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `dmr_tx_spacing` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | partial, needs re-run |
| `zone_share` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | partial, needs re-run | ✕ |
| `dmr_control_station` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `net_tcp` | not recorded | not recorded | pass (seed) | pass (seed) | not recorded | not recorded | pass, needs re-run | off |
| `net_kiss` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `net_http` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `net_http_gzip` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | off |
| `net_webctl` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | off |
| `net_radio_udp` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | off |
| `net_rigctl` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `net_usrp` | not written | not recorded | not written | not written | not recorded | not recorded | pass, needs re-run | not written |
| `net_aprsis` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `mdns` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `tftp` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `tftp_iddb` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | off |
| `dmr_iddb` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass | ✕ |
| `iddb_compact` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `snmp` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | off |
| `net_ppp` | not recorded | off | pass (seed) | pass (seed) | off | off | off | off |
| `ui_navigation` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `key_lock` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | ✕ |
| `channel_knob` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass | pass |
| `ui_settings` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | partial, needs re-run | ✕ |
| `diag` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | ✕ |
| `backlight_test` | off | off | off | off | off | off | off | ✕ |
| `ui_color` | not recorded | not recorded | ✕ | not recorded | not recorded | not recorded | ✕ | ✕ |
| `font_xl` | not recorded | ✕ | ✕ | ✕ | ✕ | ✕ | ✕ | ✕ |
| `font_aa` | not recorded | off | off | off | off | off | ✕ | ✕ |
| `font_big` | not recorded | ✕ | ✕ | ✕ | ✕ | ✕ | ✕ | ✕ |
| `lang_es` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | off | ✕ |
| `lang_it` | off | off | off | off | off | off | off | ✕ |
| `lang_fr` | off | off | off | off | off | off | off | off |
| `lang_zh` | off | off | ✕ | ✕ | ✕ | off | off | ✕ |
| `cjk_glyphs` | off | off | off | off | off | off | off | off |
| `lang_default_es` | off | off | off | off | off | off | off | ✕ |
| `lang_default_it` | off | off | off | off | off | off | off | ✕ |
| `lang_default_fr` | off | off | off | off | off | off | off | off |
| `lang_default_zh` | off | off | ✕ | ✕ | ✕ | off | off | ✕ |
| `radio_controls` | not recorded | not recorded | not recorded | pass (seed) | not recorded | not recorded | pass, needs re-run | partial, needs re-run |
| `settings_store` | not recorded | not recorded | not recorded | pass (seed) | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `codeplug_foreign` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `nand_storage` | ✕ | ✕ | ✕ | not recorded | ✕ | ✕ | ✕ | ✕ |
| `codec_check` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded | pass, needs re-run | pass, needs re-run |
| `air_raw` | off | off | off | off | off | off | off | off |
| `debuglink_dmrip` | off | off | off | off | off | off | off | off |
| `audio_probe` | not written | off | not written | not written | not written | off | off | not written |
<!-- end generated -->

What the records don't hold yet: limits and known problems from the bring-up pages.

| Radio | Known limits and problems | Source |
|---|---|---|
| DM-1701 | Not yet checked: split duplex with a real pair of radios, standalone SFR, roaming to a repeater, IP over DMR end to end, VOX with a voice, privacy scrambling against another radio | [on-air-coverage](testing/on-air-coverage.md) |
| DM-32UV | GPS data seen, no fix yet. The transmit half of a split duplex pair not checked. FM transmit deviation at 12.5 kHz, both slots through a duplex hotspot. A DMR receive fault after hours of uptime (2026-10-16, cause unknown). USB not available | [dm32-bringup](testing/dm32-bringup.md) |
| DM-1801 Pro | The receive half of a split duplex pair, VHF transmit, VOX and APRS receive not checked. Chinese is unavailable: the 5x8 words leave no 16 px line for a CJK glyph, so a hand-tuned small-height CJK face and a layout to match are needed, not a switch ([UI spec](spec/ui.md) 2.5) | [hardware-dm1801pro](spec/hardware-dm1801pro.md) |
| DB25-D PRO | The older FD6818 batches (D64, D66, D6T) are driven by the same image, untested on a radio. Split duplex not built. Standalone and MMDVM SFR checked on the SDR bench (experimental). DMR audio from the speaker, a GPS fix and settings kept across power off not confirmed | [hardware-db25d](spec/hardware-db25d.md) |
| MD-380 | Fully operational (UHF)¹. DCS, the VHF image, tone deviation and data from a DMR VFO not checked. Split duplex and SFR not available (HR_C5000). The Chinese image builds in ci but is untested on a radio | [md380-bringup](testing/md380-bringup.md) |

¹ Frequency accuracy depends on each radio's factory calibration: confirm it against an external reference (frequency counter, service monitor, or a hotspot's MMDVMCal BER test). Two MD-380s measured 2-2.5 kHz high, outside an MMDVM hotspot's decode window; Settings > Advanced > Freq trim corrects receive and transmit together ([radio-features](spec/radio-features.md) 6.1, [hardware-md380](spec/hardware-md380.md)). The `ref_trim` row above lists the radios that have the trim.

## Built by default on each radio

● on in the radio's default image · ○ off by default (it builds when switched on) · ✕ the radio's hardware can't do
it: the board or its platform lists it in `BOARD_UNSUPPORTED` with the reason (`config.mk`) · … the hardware can, but
the radio's driver or glue isn't written yet (`BOARD_NOT_WRITTEN`) · ? off, and no source says yet whether the radio
has the part it needs (`board.toml` `[hardware] unconfirmed`). The rule:
[build-config.md](build-config.md#unsupported-not-written-unconfirmed). From each board's `make print-config`. A switch
that needs hardware some radios lack (`hardware` in `tests/hil/features.toml`) is never ○. The last two rows are not
switches. Voltage reading: what the radio measures, its battery, its DC input (Supply) or none (`board_volts()` in
`hal/board.h`). Clock: whether the radio's real-time clock keeps time through power-off (`board.toml` `[hardware]
clock`; ? = not known yet). No driver reads the clock yet ([radio-features.md](spec/radio-features.md) 5.6).

<!-- generated: tools/boards.py docs feature-defaults; edit config/features.mk and boards/*/config.mk -->
| Feature switch | DM-32UV | DM-1701 | DM-1801 Pro | DB25-D PRO | MD-380 | MD-UV380 | MD-9600 | RT10 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| `USB_SERIAL_REQUEST` | ○ | ● | ○ | ○ | ● | ● | ● | ○ |
| `FM` | ● | ● | ● | ● | ● | ● | ● | ● |
| `SUBTONE` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_PRIVACY` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_AES` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_TALKER_ALIAS` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_GPS` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `DMR_SMS` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `DMR_TMS` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `DMR_ARS` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_LRRP` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `DMR_SIGNALING` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_EMERGENCY` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_ROAMING` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_TRUNK` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_TRUNK_TX` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_TRUNK_SUPL` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_TRUNK_GW` | ● | ● | ● | ● | ● | ● | ● | ● |
| `VOICE_CAPTURE` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| `DMR_IP_GATEWAY` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `ZONE_SHARE` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `DMR_UDP_COMPRESS` | ● | ● | ● | ● | ● | ● | ● | ● |
| `NET_TCP` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `NET_KISS` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `NET_HTTP` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `NET_HTTP_GZIP` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `NET_WEBCTL` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `NET_RADIO_UDP` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `NET_RIGCTL` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `NET_APRSIS` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `NET_USRP` | … | ● | … | … | ● | ● | ● | … |
| `MDNS` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `TFTP` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `TFTP_IDDB` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `IDDB_COMPACT` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DMR_CONTROL_STATION` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `SNMP` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `SFR` | ● | ● | ● | ● | ✕ | ● | ● | ● |
| `SFR_DUALPATH_RX` | ● | ● | ● | ● | ✕ | ● | ● | ● |
| `SFR_CWID` | ● | ● | ● | ● | ✕ | ● | ● | ● |
| `TASK_STACKS` | ● | ● | ● | ● | ● | ● | ● | ● |
| `USB` | ✕ | ● | ✕ | ✕ | ● | ● | ● | ✕ |
| `HOSTLINK` | ● | ● | ● | ● | ● | ● | ● | ● |
| `USB_NETWORK` | ✕ | ● | ✕ | ✕ | ● | ● | ● | ✕ |
| `NET_PPP` | ● | ○ | ● | ● | ○ | ○ | ○ | ○ |
| `HOTSPOT` | ● | ● | ● | ● | ● | ● | ● | ○ |
| `HOTSPOT_SPLIT` | ● | ● | ● | … | … | ● | ● | … |
| `DEBUGLINK` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DEBUGLINK_DMRIP` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| `CPS_OPENFW` | ● | ● | ● | ● | ● | ● | ● | ● |
| `CPS_FAST` | ● | ● | ● | ● | ● | ● | ● | ● |
| `VFO` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `VFO_FM` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `VFO_DMR` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `DIAG` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `BACKLIGHT_TEST` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ✕ |
| `FONT_XL` | ● | ✕ | ✕ | ✕ | ✕ | ✕ | ✕ | ✕ |
| `FONT_AA` | ● | ○ | ○ | ○ | ○ | ○ | ✕ | ✕ |
| `FONT_BIG` | ● | ✕ | ✕ | ✕ | ✕ | ✕ | ✕ | ✕ |
| `LANG_ES` | ● | ● | ● | ● | ● | ● | ○ | ✕ |
| `LANG_IT` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ✕ |
| `LANG_FR` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| `LANG_ZH` | ○ | ○ | ✕ | ✕ | ✕ | ○ | ○ | ✕ |
| `CJK_GLYPHS` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| `LANG_DEFAULT_ES` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ✕ |
| `LANG_DEFAULT_IT` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ✕ |
| `LANG_DEFAULT_FR` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| `LANG_DEFAULT_ZH` | ○ | ○ | ✕ | ✕ | ✕ | ○ | ○ | ✕ |
| `UI_COLOR` | ● | ● | ✕ | ● | ● | ● | ✕ | ✕ |
| `UI_SETTINGS` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `SCAN` | ● | ● | ● | ● | ● | ● | ● | ● |
| `DUAL_WATCH` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `BEEP` | ● | ● | ● | ● | ● | ● | ● | ● |
| `SIG_TONES` | ● | ● | ● | ● | ○ | ● | ● | ● |
| `FM_TX_TONES` | ● | ● | ● | ● | … | ● | ● | ● |
| `DTMF` | ● | ● | ● | ● | … | ● | ● | ✕ |
| `RX_EXT` | ● | ● | ● | ● | … | ● | … | ● |
| `VOX` | … | ● | ● | … | ● | ● | ● | … |
| `REF_TRIM` | ● | ● | ● | ● | ● | ● | ● | ● |
| `BATTERY_SAVE` | ● | ● | ● | ● | ● | ● | ● | ● |
| `VIBRATE` | ? | ✕ | ● | ✕ | ✕ | ✕ | ✕ | ✕ |
| `GPS` | ● | ● | ✕ | ● | ● | ● | ● | ✕ |
| `AUDIO_PROBE` | … | ○ | … | … | … | ○ | ○ | … |
| `STUB_ENTRY` | ● | ● | ● | ● | ● | ● | ● | ● |
| `NAND_STORAGE` | ✕ | ✕ | ✕ | ● | ✕ | ✕ | ✕ | ✕ |
| `CRASH_LOG` | ● | ● | ● | ● | ● | ● | ● | ● |
| `CRASH_SCREEN` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `CRASH_REPORT` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `WATCHDOG` | ● | ● | ● | ● | ● | ● | ● | ● |
| `SUPPORT_INFO` | ● | ● | ● | ● | ● | ● | ● | ● |
| `FAULT_TRACE` | ● | ✕ | ● | ● | ✕ | ✕ | ✕ | ● |
| `AM_RX` | ● | ✕ | ● | ✕ | ✕ | ✕ | ✕ | ✕ |
| `BCAST_FM` | ● | ✕ | ● | … | ✕ | ✕ | ✕ | ✕ |
| `APRS` | ● | ● | ● | ● | ● | ● | ● | ● |
| `APRS_AFSK` | ● | ● | ● | ● | ● | ● | ● | ● |
| `APRS_DMR` | ● | ● | ● | ● | ● | ● | ● | ● |
| `APRS_RX` | ● | ● | ● | … | … | ● | ● | ● |
| `APRS_IGATE` | ● | ○ | ● | … | … | ○ | ● | ● |
| `GPS_SKY` | ● | ● | ✕ | ● | ● | ● | ● | ✕ |
| `SATELLITE_TLE` | ● | ● | ● | ● | ● | ● | ● | ● |
| `KEY_ASSIGN` | ● | ● | ● | ● | ● | ● | ● | ● |
| `CHANNEL_LIST` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `CHANNEL_EDIT` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `ZONE_EDIT` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `CONTACT_EDIT` | ● | ● | ● | ● | ● | ● | ● | ✕ |
| `VOLUME_BAR` | ● | ● | ● | ● | ● | ● | ● | ● |
| `P25` | ● | ● | ● | ● | … | ● | ● | ● |
| `P25_TRUNK` | ● | ● | ● | ● | … | ● | ● | ● |
| `NXDN` | ● | ● | ● | ● | … | ● | ● | ● |
| `NXDN_SCRAMBLER` | ● | ● | ● | ● | … | ● | ● | ● |
| `AIR_RAW` | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| Voltage reading (board, no switch) | Battery | Battery | Battery | None | Battery | Battery | Supply | Battery |
| Clock kept through power-off (board, no switch) | Yes | ? | Yes | ? | ? | ? | ? | No (only while on) |
<!-- end generated -->

## Radio and voice

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Analog FM receive and transmit | `FM` | 36,252 | 24,324 | Includes its children (tones, VOX, APRS, FM VFO) |
| CTCSS / DCS encode and decode | `SUBTONE` | 1,104 | 3,724 | MD-380: software decode (ADC) and PWM encode; CTCSS 100.0 Hz works on air both ways, DCS not yet checked |
| FM TX tones: roger beep, 1750 Hz burst | `FM_TX_TONES` | 1,616 | — | MD-380: TX tone path not written |
| DTMF transmit from the keys | `DTMF` | 916 | — | MD-380: TX tone path not written |
| FM VOX | `VOX` | 352 | 356 |  |
| Receiver battery saver | `BATTERY_SAVE` | 272 | 228 |  |
| Frequency trim, Settings > Advanced | `REF_TRIM` | 2,012 | 2,356 | The hook on each radio, the page and the setting |
| Air band AM receive (108-137 MHz) | `AM_RX` | 408* | — | Needs a receiver that does AM; DM-32 board glue not written yet, so nothing runs |
| Broadcast FM receiver | `BCAST_FM` | ~0* | — | Needs a second receiver chip (DM-32's RDA5807M); glue not written yet |
| Receive past the rated bands, Settings > Advanced | `RX_EXT` | 560 | — | Receive only, FM, off by default; ranges from each receiver chip's documents. MD-380: no receiver range in its chip documents |
| DMR Tier II voice (modem, call stack, AMBE FEC, vocoder) | `DMR` | 377,332 | 275,716 | Includes every DMR feature and everything on IP over DMR |
| P25 Phase 1 conventional: the air interface (framer, soft modem, shared FEC), the engine, the P25 channel mode and the full-rate voice decoder on the MBE core | `P25` | 40,588 | 24,640* | Needs a board with an air interface provider (`board.air`); without one nothing runs ([air-interface.md](spec/air-interface.md)). Off on the MD-380 until receive is written ([hardware-md380.md](spec/hardware-md380.md) 4.11). Receive and transmit checked on air against the SDR bench on the DM-32UV and the DM-1701, receive, zone scan and dual watch on the MD-9600; the VFO's mode tile, the debug link or a stored P25 channel sets it ([p25.md](spec/p25.md)). RAM: 6,640 B on the DM-1701, 424 B on the MD-380 |
| NXDN conventional voice, 4800 and 9600 bit/s: the frame codec, receive, the call layer, the engine and the NXDN mode in the radio core | `NXDN` | 10,512 | — | Needs `DMR` (its voice goes through the DMR vocoder path) and a board with an air interface provider (`board.air`); where the provider only receives a rate, PTT is refused at that rate. The VFO's mode tile, the debug link or a stored NXDN channel sets it ([nxdn.md](spec/nxdn.md) 6, [nxdn-codeplug.md](spec/nxdn-codeplug.md)). On the DM-1701 and the DM-32UV transmit is checked off air at both rates and receive with a bench transmitter, speech frames through the vocoder included (DM-1701); on the HR_C6000 radios the speaker plays NXDN speech through the chip's prompt tone FIFO, checked by ear on the DM-1701 and the MD-9600 ([modem-hrc6000.md](spec/modem-hrc6000.md) 10.6). RAM: 2,184 B on the DM-1701; with `P25` also on it shares P25's engine memory (9,464 B flash, 256 B RAM on top of P25) |
| NXDN Scrambler (15-bit) | `NXDN_SCRAMBLER` | 448 | — | Needs `NXDN`; supports conventional voice calls. The NXDN air interface must support the requested direction. Checked on air against another radio running RadioFoundry and a commercial portable ([bring-up record](testing/dm32-bringup.md#nxdn-scrambler-interoperability)). |
| Basic privacy, 8-bit and 16-bit keys | `DMR_PRIVACY` | 6,260 | 7,340 |  |
| AES-256 DMR voice, PI header and late-entry signaling | `DMR_AES` | 3,484 | 4,692 | Requires `DMR_PRIVACY`. DM-1701: 480 B RAM. TX to a receiver of another make needs the modem driver's burst F field (`set_burst_f`, [dmr-voice.md](spec/dmr-voice.md) 5.1): only the HR_C7000 driver has it (DM-32UV); on the HR_C6000 and other radios our AES overs reach radios running RadioFoundry only (#864). |
| Talker alias receive and send | `DMR_TALKER_ALIAS` | 1,088 | 1,068 |  |
| ETSI GPS Info in the embedded LC | `DMR_GPS` | 992 | 892 |  |
| DMR emergency (alarm, emergency call, hot mic) | `DMR_EMERGENCY` | 3,564 | 3,456 |  |
| Repeater roaming | `DMR_ROAMING` | 12,136 | 12,196 |  |
| CSBK signaling: call alert, radio check | `DMR_SIGNALING` | 1,044 | 988 |  |
| Tier III trunked systems: control channel, registration, following calls | `DMR_TRUNK` | 37,800 | 42,208 | Includes its children. Off, a trunked channel is shown as a mode not in this build ([dmr-trunking](spec/dmr-trunking.md)). |
| Trunked calls and services: voice, status, short data, packet data, authentication | `DMR_TRUNK_TX` | 12,456 | 11,948 | Needs `DMR_TRUNK`. Includes its children. Off, the radio follows a system and never transmits there |
| Trunked supplementary data and the short data poll | `DMR_TRUNK_SUPL` | 708 | 808 | Needs `DMR_TRUNK_TX` |
| Trunked calls through a system's telephone, line and dispatcher gateways: dialed digits sent with the call | `DMR_TRUNK_GW` | 1,400 | 720 | Needs `DMR_TRUNK_TX` |
| P25 channels in the codeplug: the conventional codec | `P25` | 56 | — | The codec only: a P25 channel is shown as a mode not in this build ([p25-codeplug.md](spec/p25-codeplug.md)). A stored P25 channel runs on its own parameters ([p25.md](spec/p25.md) 7) |
| P25 trunked systems in the codeplug: the codec | `P25_TRUNK` | 13,888 | — | Needs `P25`; on top of it. Off on the MD-380 with `P25`. A trunked channel hunts its system's control channel and follows voice grants for its talk group; where the system allows transmit it registers, affiliates, asks for a voice channel on PTT and transmits on the grant ([p25-trunking.md](spec/p25-trunking.md)). Run on the DM-1701 and the DM-32UV against a control channel the SDR bench plays, not on a live system. RAM: 1,232 B on the DM-1701 |
| VFO (hand-tuned frequency) | `VFO` | 2,424 | 1,344 | Includes the DMR VFO and Copy to VFO ([dmr-voice.md](spec/dmr-voice.md) 2.2), which is about 750 B on the C-SKY radios |
| FM VFO | `VFO_FM` | 0 | -28 |  |
| DMR VFO (color code, slot, talk target typed in; mode change FM / DMR) | `VFO_DMR` | 1,072 | 276 | The VFO's mode, color code, slot and talk target are settings of the VFO, kept across power cycles ([dmr-voice.md](spec/dmr-voice.md) 2.1) |
| Channels list: every channel by number, a channel's settings (hub 5) | `CHANNEL_LIST` | 18,320 | 16,580 |  |
| Channel editor: edit a channel, New channel from current settings, text entry | `CHANNEL_EDIT` | 14,532 | 13,368 | Needs `CHANNEL_LIST`. Without it a channel's settings are read only |
| Zone editor: move, add, copy and remove channels in a zone; rename, copy and delete a zone | `ZONE_EDIT` | 5,188 | 5,480 | Needs `CHANNEL_EDIT`. Also about 1.3 KB of RAM |
| Contact editor: a new contact from Find or a call's actions (name, ID, type, timeslot) | `CONTACT_EDIT` | 1,728 | 1,892 | Needs `CHANNEL_EDIT` and `DMR` |
| Volume bar when a knob the MCU reads turns (#354) | `VOLUME_BAR` | 520 | 492 | Shows only on a radio with such a knob. Off, the volume screen keeps the segment drawing it now shares (about 90 bytes) |
| Key assignment (Settings > Keys, codeplug) | `KEY_ASSIGN` | 2,468 | 2,224 | Up to 9 keys |
| Scan, band scan, dual watch, nuisance delete | `SCAN` | 25,640 | 24,204 |  |
| Dual watch: A and B in any zone or the VFO, PTT on the selected one, its screens, talk back | `DUAL_WATCH` | 20,876 | 20,640 | Needs `SCAN`. Without it, only the debug link's dual watch (two channels of one zone). |
| Beeps and alert tones | `BEEP` | 8,036 | 2,700 |  |
| Signaling tone chains (Settings > Signaling tones) | `SIG_TONES` | 5,340 | 5,748* | Needs `BEEP` |
| Vibration alerts (Settings > Sound > Vibrate) | `VIBRATE` | 520* | — | Needs a vibration motor (`hal_power_t.vibrate`): none of these radios has one RadioFoundry drives. On for the DM-1801 Pro. Measured with `CONFIG_SNMP=n`; 236 B with LTO |
| GPS receiver: the module driver and its NMEA input (Settings > Location > GPS) | `GPS` | 9,568 | 9,556 | Needs a GPS receiver. Off, DMR GPS Info, LRRP and APRS send the fixed position when one is set ([position](spec/position.md)). The DM-1701 has none from the factory but listens for an owner-fitted one |

## DMR data

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Text messages (ETSI IP text by default) | `DMR_SMS` | 3,076 | 2,892 | Includes the TMS format. Individual ETSI IP texts go confirmed ("Delivered to") |
| TMS text format | `DMR_TMS` | 1,080 | 1,264 | Optional format next to ETSI IP text |
| ARS registration | `DMR_ARS` | 1,300 | 1,576 |  |
| LRRP location | `DMR_LRRP` | 2,496 | 2,044 |  |
| Compressed UDP/IP headers on transmit (SAP 3) | `DMR_UDP_COMPRESS` | 752 | 756 | Auto: not for ports 4000-4099 or ETSI IP text |
| Zone and channel sharing: send a zone or a channel, import an offered one | `ZONE_SHARE` | 17,276 | 16,860 | Includes its screens and the import; about 4 KB of RAM. Received shares are offered by default, imported only when the user says so ([zone-share.md](spec/zone-share.md)). |
| Single frequency repeater: MMDVM SFR, and the standalone SFR (experimental) | `SFR` | 12,232 | — | MD-380: HR_C5000 lacks what the SFR glue uses |
| Experimental: SFR user receive "dual-path RX": a voice frame whose FEC failed comes from the other copy | `SFR_DUALPATH_RX` | 624 | — | Needs `SFR` |
| Experimental: standalone SFR repeater's periodic CW ID (callsign in Morse, FM) | `SFR_CWID` | 808 | — | Needs `SFR` |
| APRS: AFSK positions, status, messages; DMR positions; KISS TNC | `APRS` | 31,872 | 19,664 | Includes APRS receive, KISS over TCP and APRS-IS where built. MD-380: AFSK TX needs the tone path (not written), DMR positions work |
| APRS on FM: AX.25 / AFSK packets, messages, KISS TNC | `APRS_AFSK` | 23,956 | 12,124 | Needs APRS and FM. Includes APRS receive, KISS over TCP and APRS-IS where built. Off, APRS sends positions over DMR only |
| APRS over DMR: LRRP or UDT positions to a gateway ID | `APRS_DMR` | 728 | 104 | Needs APRS and DMR. Off, APRS is FM only |
| APRS receive: demodulator, station list, digipeater | `APRS_RX` | 10,012 | — | DM-1701: FM receive audio on the I2S stream, squelch gated ([aprs](spec/aprs.md) 5.3). MD-380: not written |
| GPS screen: sky plot, signal bars, grid locator (Tools > Position) | `GPS_SKY` | 6,200 | 5,332 | Needs GPS. 176 B of RAM |
| APRS iGate rules (with tools/aprs_gate.py) | `APRS_IGATE` | 1,420* | — | Needs APRS receive. DM-1701: off by default |

## Host link and USB

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| USB device | `USB` | 212,740 | 182,860 | DM-32 has no USB (host link on the programming cable's UART). Includes everything on USB |
| Host link: hotspot, CPS, debug link on the host port | `HOSTLINK` | 199,640 | 171,960 | Includes its children |
| MMDVM hotspot | `HOTSPOT` | 33,868 | 12,700 | Includes split duplex and MMDVM SFR where built. MD-380: size estimated |
| Split duplex repeater (two radios, one duplex modem) | `HOTSPOT_SPLIT` | 18,356 | — | MD-380: HR_C6000-only glue. DM-1701, DM-32UV, DM-1801 Pro: each half checked alone on the SDR bench; one pair (DM-1801 Pro TX, DM-32UV RX) with a subscriber: 29 of 31 calls on the right slot |
| Debug link | `DEBUGLINK` | 88,944 | 80,128 |  |
| Task stacks in the debug link view | `TASK_STACKS` | 976 | 944 | Each task's least free stack, the `tasks` view section ([debuglink](spec/debuglink.md) 3.7) |
| Open-firmware CPS protocol (qdmr) | `CPS_OPENFW` | 6,696 | 6,840 |  |
| Fast codeplug protocol (debug link CODEPLUG) | `CPS_FAST` | 3,348 | 3,428 | Changed 4 KB blocks only, LZ4 both ways, staged commit ([cps-fast.md](spec/cps-fast.md)). |
| Loader hand-over (stub entry, command 0x1A) | `STUB_ENTRY` | 1,224 | 392 | Updates and backups over the radio's own link without the bootloader key press: the web flasher and `stub/stubtool.py`. On by default on every radio, with or without the debug link ([stub-entry.md](spec/stub-entry.md)); on the STM32 radios a RAM flash stub that also takes 8 KB of RAM ([stub-stm32f4.md](spec/stub-stm32f4.md)) (the DM-1701's RAM is 95 % used with it on) |
| Crash log | `CRASH_LOG` | 6,240 | 5,928 | A fault, stack overflow, heap exhaustion or failed assertion keeps a record across the restart; the debug link shows it ([crash-log.md](spec/crash-log.md)) |
| Hardware watchdog | `WATCHDOG` | 1,248 | 572 | Fed by the idle task; a hang or a starved system resets the radio. Timeout about 5 s on the STM32 radios, 4 s on the HR_C7000 radios ([watchdog.md](spec/watchdog.md)) |
| Fault trace on the host UART | `FAULT_TRACE` | — | — | A CPU fault's registers in three framed lines before the restart, about 600 B ([crash-log.md](spec/crash-log.md) 2.3). The STM32 radios' host port is USB |
| Crash log screen | `CRASH_SCREEN` | 3,384 | 3,176 | Settings > Advanced > Crash log: the record's summary and Clear |
| Crash report over DMR | `CRASH_REPORT` | 1,564 | 1,268 | Sends the crash record as one UDP datagram to a codeplug contact, from the menu or after the restart (Off, Ask, Auto) ([crash-log.md](spec/crash-log.md) 5). Needs the screen |

## Network (USB network adapter and IP over DMR)

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| USB network adapter (CDC-NCM) | `USB_NETWORK` | 72,808 | 72,804 | Includes everything below |
| Leave network mode from a web page | `USB_SERIAL_REQUEST` | 588 | 588 | The web flasher and editor take a radio in USB network mode back to serial mode, with no key press on the radio (endpoint 0 request, `/api/usb`, TCP at 12.255.255.254). Follows `USB_NETWORK` |
| IP over DMR gateway | `DMR_IP_GATEWAY` | 67,784 | 67,976 | Includes the radio's own services below |
| Control station mode (boot as network, PC answers data, fixed link) | `DMR_CONTROL_STATION` | 316 | 468 |  |
| Minimal TCP | `NET_TCP` | 31,552 | 31,188 | Includes KISS, HTTP, rigctld, APRS-IS. About 4.5 KB RAM |
| KISS over TCP, port 8001 | `NET_KISS` | 776 | 664 |  |
| HTTP status page and JSON, port 80 | `NET_HTTP` | 20,304 | 19,548 |  |
| HTTP static files stored and served gzip compressed | `NET_HTTP_GZIP` | -19,456 | 12,616 | Negative: saves flash, nearly all of it on web control's files. MD-380: web control follows this switch ([hardware-md380.md](spec/hardware-md380.md) 19) |
| Web control: the front panel page, keys, status and receive audio over `/ws` | `NET_WEBCTL` | 12,944 | 12,788 | The page and the radio's drawing included (about 6 KB gzip compressed); 1 KB RAM ([tcp-services.md](spec/tcp-services.md) 5.1) |
| Zone and channel control over UDP, port 4191: where the radio is, select and step, change notices | `NET_RADIO_UDP` | 2,364 | 2,412 | 112-120 B RAM; needs no TCP or HTTP; DM-32UV 2,392 ([tcp-services.md](spec/tcp-services.md) 8) |
| rigctld, port 4532 | `NET_RIGCTL` | 3,856 | 3,872 |  |
| Audio linking (USRP over UDP, port 34001) | `NET_USRP` | 3,112 | 3,660 | DM-1701: DMR and FM both ways. MD-380: DMR only (FM audio doesn't reach the processor yet). Off where the host link is the programming cable at 115200 baud (about 11.5 KB/s): received audio alone needs about 17.6 KB/s |
| APRS-IS client | `NET_APRSIS` | 2,824 | 2,816 | Needs NAT on the host |
| mDNS / DNS-SD (rfoundry-<id>.local) | `MDNS` | 6,656 | 6,728 |  |
| TFTP codeplug read / write | `TFTP` | 8,248 | 8,208 | Without the ID database below |
| DMR ID database over TFTP (`iddb.bin`, combined `codeplug.bin`) | `TFTP_IDDB` | 2,096 | 2,036 | Follows `TFTP` |
| Compact DMR ID database (format C) read beside format N | `IDDB_COMPACT` | 848 | 1,028 | Follows `DMR` ([iddb-compact.md](spec/iddb-compact.md)) |
| SNMP v2c agent and traps (USB and over the air) | `SNMP` | 7,824 | 7,728 |  |
| PPP on the serial host port (IPv4 into the gateway) | `NET_PPP` | 5,308* | 4,832* | Interop tested with pppd; checked on the DB25-D PRO and the DM-1801 Pro (`docs/spec/ppp.md` 8). DM-32: its only IP link, on by default with the gateway and every service (Whole images, DM-32UV) |

## User interface

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Settings menu on the radio | `UI_SETTINGS` | 13,984 | 11,000 |  |
| Diagnostics page | `DIAG` | 4,216 | 4,556 |  |
| Satellite TLE catalog, pass prediction and Doppler compensation | `SATELLITE_TLE` | 28,184 | 27,724 | Needs `DEBUGLINK`. RAM: 1,280 B on DM-1701 (656 B linked, 624 B from the existing heap), 1,272 B on MD-380 (648 B linked, 624 B heap), and 1,264 B on DM-32UV (656 B linked, 608 B heap). Heap figures include allocation overhead and are already reserved in linked RAM. The DM-1701 and MD-UV380 maximal builds keep it on; the MD-9600 maximal build leaves it off to fit app flash. Default builds keep it on ([satellite.md](spec/satellite.md)). |
| Support info on About and the debug link (build source, chips, image hash) | `SUPPORT_INFO` | 3,016 | 2,572 | About 515 bytes of RAM, most of it About's longer text. Read only when About opens or a host asks ([support-info.md](spec/support-info.md)) |
| Backlight test pages | `BACKLIGHT_TEST` | 1,320* | 1,196* | Off by default; bench work ([backlight-curve.md](testing/backlight-curve.md)) |
| Color screen settings (theme, highlight color, filled status bar) | `UI_COLOR` | 1,088 | 1,040 | Off on the DM-1801 Pro (monochrome screen) |
| 16x32 font | `FONT_XL` | 6,352* | — | Only useful at 240 px and wider (MD-380 is 160 px). Anti-aliased on the DM-32 (7.4 KB of glyphs) |
| Anti-aliased fonts | `FONT_AA` | 6,040* | 6,032* | Smooth text on color panels; the DM-32's image grew 9.5 KB with it and `FONT_XL` |
| Larger text | `FONT_BIG` | — | — | 9x18 lists and labels on 240 px color panels (needs `FONT_AA`); 3,588 B on the DM-32 |
| Spanish on the radio | `LANG_ES` | 28,616 | 27,068 | The first language brings the lookup and the accented letters of every font (about 7.5 KB); the rest is its text, only the strings the build compiles (a switch that is off takes its strings out of every language) |
| Italian on the radio | `LANG_IT` | 21,376* | 20,216* | Off by default; measured with Spanish on (its text, and the hashes of strings Spanish leaves untranslated). Alone it costs about as much as Spanish |
| French on the radio | `LANG_FR` | 18,088* | 17,024* | Off by default; measured with Spanish on, as Italian |
| Chinese on the radio | `LANG_ZH` | 48,052* | — | Off by default. Not the text alone: 25,664 B of 16x16 glyph bitmaps plus 1,604 B of code points (802 characters, `core/ui/cjk_font_data.c`: the catalog's characters plus a reviewed list of province/city names and repeater words in `tools/cjk_names_zh.txt`) and about 17 KB of UTF-8 text, with the text layer's decoder. Channel names that use a character outside the list draw a box. Needs a 16 px line height: a 240 px color radio draws its words in 8x16 and shows Chinese on every screen, the 160 px class moves its words up to 8x16 while a wide language is in use (2 rows fewer on a 160x120 panel, Home without its zone line), and a 128x64 panel has no room for it and must not build it. A board without room leaves `LANG_ZH` off in its `config.mk`. Measured with Spanish on |
| Chinese glyph table for channel names | `CJK_GLYPHS` | 27,336 | — | The 802 16x16 glyph bitmaps and code points (25,664 + 1,604 B) in `LANG_ZH`'s figure, split out so a board too small for Chinese keeps its translated menus without the table (Chinese then draws as boxes). Defaults to `LANG_ZH`, which it needs |
| Start in Spanish | `LANG_DEFAULT_ES` | 0 | 0 | The language the radio starts in until the user picks one (needs `LANG_ES`); the web flasher sets it from its Radio language choice |
| Start in Italian | `LANG_DEFAULT_IT` | 0 | 0 | As `LANG_DEFAULT_ES` (needs `LANG_IT`) |
| Start in French | `LANG_DEFAULT_FR` | 0 | 0 | As `LANG_DEFAULT_ES` (needs `LANG_FR`) |
| Start in Chinese | `LANG_DEFAULT_ZH` | 0 | 0 | As `LANG_DEFAULT_ES` (needs `LANG_ZH`) |

## Storage

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Settings and codeplug storage on SPI NAND | `NAND_STORAGE` | — | — | For radios whose data flash is SPI NAND: on for the DB25-D (about 6.6 KB flash, 1.5 KB RAM), not in these columns. These radios have SPI NOR and don't use it ([nand-sectors](spec/nand-sectors.md)) |

## Bench

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Audio path probe (debug link 0x10) | `AUDIO_PROBE` | 1,176* | — | Bench only, never in a release |
| Air interface bench: raw symbols and tap samples (debug link 0x24) | `AIR_RAW` | 3,560* | — | Bench only, never in a release ([debuglink.md](spec/debuglink.md) 3.28) |
| Debug link over the air (UDP 3335 over DMR IP) and the detour (debug link 0x27, 0x28) | `DEBUGLINK_DMRIP` | 6,836* | 6,524* | Bench only, never in a release. About 0.8 KB RAM ([debuglink.md](spec/debuglink.md) 7) |
| Voice capture for receive tests: the next 32 voice frames received, read over the debug link | `VOICE_CAPTURE` | 264* | 284* | Bench only, never in a release ([debuglink](spec/debuglink.md)) |

## Whole images

### DM-1701 and MD-380

| Build | DM-1701 | MD-380 |
|---|---:|---:|
| Default | 704,632 | 575,736 |
| Maximal (every compatible switch on) | 733,604 | 631,656 |
| FM only, minimal | 240,436 | 213,428 |

Link time optimization is on by default on both radios. On the DM-1701 `LTO_3P=1` would take another 3 KB or so off
by optimizing the third-party code too.

The MD-380's app region is sectors 3-10, 0x0800C000..0x080DFFFF, 868,352 bytes (848 KB). The codec runs in place in
sector 11 ([codec-md380.md](spec/codec-md380.md)). With link time optimization, its default:

| MD-380 build | Flash | Free |
|---|---:|---:|
| Default (UHF) | 575,736 | 292,616 |
| Maximal (every switch it supports, PPP and Italian included) | 631,656 | 236,696 |

Until the codec first moved, the region was 281,668 bytes and the default image fit only with link time optimization
and the hotspot off. Until every codec window moved into sector 11 it was 475,136 bytes (sectors 3-7).

### DM-32UV

The DM-32UV has 880 KB of code space, so RAM limits it: 165,612 bytes above the stock codec's RAM window and 27,868
below it (RAM_LO). Its network is PPP on the programming cable (`NET_PPP`), on by default with the IP gateway and
every service, `TCP_CONNS=3` (`boards/dm32/config.mk`). It fits because the state the core marks `RFOUNDRY_CPU_RAM` lives
in RAM_LO ([hardware-dm32.md](spec/hardware-dm32.md) 17.1). Measured with csky-elf GCC 14 (bytes of the image, RAM
of the linker's regions; the RTOS heap, 40 KB with every task stack, is inside the used part):

| DM-32UV build | Image | RAM (165,612 B) | RAM_LO (27,868 B) |
|---|---:|---:|---:|
| before the network (`RFOUNDRY_CPU_RAM` in main RAM) | 313,804 | 153,740 (11,872 free) | 8,800 |
| default (PPP, gateway, every service, 3 TCP connections) | 365,236 | 154,428 (11,184 free) | 24,392 (3,476 free) |
| default, `TCP_CONNS=4` | 365,236 | 155,956 (9,656 free) | 24,392 |
| default, `TCP_CONNS=2` | 365,276 | 152,892 (12,720 free) | 24,392 |
| maximal (also `AUDIO_PROBE`, `FONT_XL`) | 372,856 | 155,740 (9,872 free) | 24,392 |

The margin kept, about 11 KB of main RAM, is what the image had before the network went in. RAM_LO has 3.5 KB left
for the RAM code (`.ramfunc`) and the core's `RFOUNDRY_CPU_RAM` state to grow.

Flash each network switch costs on the DM-32UV (default minus the build with it off): the gateway with PPP and every
service 51,428 (`NET_PPP` or `DMR_IP_GATEWAY` off), TCP and what runs on it 17,028, SNMP 9,116, mDNS 7,220, TFTP
6,584, HTTP 5,076, rigctld 4,020, APRS-IS 2,964, KISS 688, control station 272. Each TCP connection is about 1.5 KB
of RAM (TCP, HTTP, rigctld).

### DB25-D PRO

Same SoC and glue as the DM-32UV, with 876 KB of code space and 165,424 bytes of main RAM above the codec window plus
52,912 in RAM_LO (`boards/db25d/memory.ld`). The network buffers stay in RAM_LO. Left off: split duplex, USRP (PCM does not fit the 115200 baud host port), SFR, APRS receive and
iGate, USB, broadcast FM, air band AM, `FONT_XL`, `AUDIO_PROBE` (`boards/db25d/config.mk`).

| DB25-D build | Image | RAM (165,424 B) | RAM_LO (52,912 B) |
|---|---:|---:|---:|
| DMR only (hotspot, VFO and network off) | 295,460 | 139,952 (25,472 free) | 19,024 (33,888 free) |
| default (hotspot, PPP, gateway, every service but USRP, 3 TCP connections; no split duplex) | 362,364 | 150,080 (15,344 free) | 28,584 (24,328 free) |
