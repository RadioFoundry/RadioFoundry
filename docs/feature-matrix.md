<!-- SPDX-License-Identifier: MIT -->

# Feature matrix

Every feature, the build switch it sits behind (`CONFIG_<switch>` in `config/features.mk`,
[build-config.md](build-config.md)), what it costs in flash on the ARM radios, and where it's built by default.

- **Bytes** = default image minus the image with that switch off (`tools/ci.sh` builds each switch off), flash bytes.
  A parent switch's figure includes its children (turning `FM` off also drops VOX, APRS, ...).
- Figures from tools/ci.sh at 3ec14e3 (2026-10-11).
  - DM-1701: plain build (LTO off), default image 340,680 B of 344,064 B (3,384 free). Since USRP (2026-10-13) the
    default build uses link time optimization: 330,824 B (13,240 free). The per-feature figures stay the plain ones.
  - MD-380: link time optimization on (`LTO=1 LTO_3P=1`, its default), default image 279,444 B of 281,668 B, the
    region it had then. With LTO a switch also changes what gets inlined elsewhere, so small MD-380 figures are
    approximate. Since the codec moved (2026-10-19) the region is 464 KB and the switches that were off for flash are
    on; the per-feature figures stay the 2026-10-11 ones.
  - `*` = off by default; measured by turning it on in the DM-1701 build (main c0022b4).
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
| Feature | DM-32UV | DM-1701 | DM-1801 Pro | DB25-D PRO | MD-380 | MD-UV380 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| `fm` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded |
| `subtone` | pass (seed) | pass (seed) | pass (seed) | not recorded | pass (seed) | not recorded |
| `fm_tx_tones` | not recorded | pass (seed) | not recorded | not recorded | not written | not recorded |
| `dtmf` | not recorded | not recorded | not recorded | not recorded | not written | not recorded |
| `vox` | not written | not recorded | not recorded | not written | not recorded | not recorded |
| `battery_save` | pass (seed) | pass (seed) | not recorded | not recorded | not recorded | not recorded |
| `am_rx` | not recorded | ✕ | not recorded | ✕ | ✕ | ✕ |
| `bcast_fm` | not recorded | ✕ | not recorded | not written | ✕ | ✕ |
| `dmr_voice` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded |
| `dmr_privacy` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_talker_alias` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded |
| `dmr_gps` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded |
| `dmr_emergency` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded |
| `dmr_roaming` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_signaling` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded |
| `vfo` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `vfo_fm` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `vfo_dmr` | not recorded | not recorded | not recorded | pass (seed) | not recorded | not recorded |
| `channel_list` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `channel_edit` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `zone_edit` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `key_assign` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `scan` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded |
| `dual_watch` | not recorded | pass (seed) | not recorded | not recorded | not recorded | not recorded |
| `beep` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `vibrate` | off | ✕ | not recorded | ✕ | ✕ | ✕ |
| `gps` | pass (seed) | ✕ | ✕ | not recorded | not recorded | not recorded |
| `dmr_data_rx` | not recorded | pass (seed) | pass (seed) | not recorded | not recorded | not recorded |
| `dmr_sms` | not recorded | pass (seed) | pass (seed) | not recorded | pass (seed) | not recorded |
| `dmr_tms` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_ars` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_lrrp` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_udp_compress` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_confirmed` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `sfr` | not recorded | pass (seed) | pass (seed) | not recorded | ✕ | not recorded |
| `sfr_dualpath_rx` | not recorded | not recorded | not recorded | not recorded | ✕ | not recorded |
| `sfr_cwid` | not recorded | not recorded | not recorded | not recorded | ✕ | not recorded |
| `aprs` | pass (seed) | not recorded | not recorded | not recorded | not recorded | not recorded |
| `aprs_rx` | pass (seed) | not recorded | not recorded | not written | not written | not recorded |
| `aprs_igate` | not recorded | off | not recorded | not written | not written | off |
| `usb` | ✕ | pass (seed) | ✕ | ✕ | not recorded | not recorded |
| `hostlink` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `hotspot` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded | not recorded |
| `split_duplex` | pass (seed) | pass (seed) | pass (seed) | not written | not written | not recorded |
| `debuglink` | pass (seed) | pass (seed) | pass (seed) | pass (seed) | pass (seed) | not recorded |
| `task_stacks` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `cps_openfw` | not recorded | pass (seed) | not recorded | pass (seed) | pass (seed) | not recorded |
| `cps_fast` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `stub_entry` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `crash_log` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `crash_screen` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `crash_report` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `watchdog` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `fault_trace` | not recorded | ✕ | not recorded | not recorded | ✕ | ✕ |
| `usb_network` | ✕ | pass (seed) | ✕ | ✕ | not recorded | not recorded |
| `dmr_ip_gateway` | not recorded | not recorded | pass (seed) | not recorded | not recorded | not recorded |
| `dmr_tx_spacing` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `zone_share` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `dmr_control_station` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `net_tcp` | not recorded | not recorded | pass (seed) | pass (seed) | not recorded | not recorded |
| `net_kiss` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `net_http` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `net_http_gzip` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `net_webctl` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `net_rigctl` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `net_usrp` | not recorded | not recorded | not recorded | not written | not recorded | not recorded |
| `net_aprsis` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `mdns` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `tftp` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `tftp_iddb` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `snmp` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `net_ppp` | not recorded | off | pass (seed) | pass (seed) | off | off |
| `ui_navigation` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `channel_knob` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `ui_settings` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `diag` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `ui_color` | not recorded | not recorded | ✕ | not recorded | not recorded | not recorded |
| `font_xl` | not recorded | ✕ | ✕ | ✕ | ✕ | ✕ |
| `font_aa` | not recorded | off | off | off | off | off |
| `font_big` | not recorded | off | off | off | off | off |
| `radio_controls` | not recorded | not recorded | not recorded | pass (seed) | not recorded | not recorded |
| `settings_store` | not recorded | not recorded | not recorded | pass (seed) | not recorded | not recorded |
| `nand_storage` | ✕ | ✕ | ✕ | not recorded | ✕ | ✕ |
| `codec_check` | not recorded | not recorded | not recorded | not recorded | not recorded | not recorded |
| `audio_probe` | not written | off | not written | not written | not written | off |
<!-- end generated -->

What the records don't hold yet: limits and known problems from the bring-up pages.

| Radio | Known limits and problems | Source |
|---|---|---|
| DM-1701 | Not yet checked: split duplex with a real pair of radios, standalone SFR, roaming to a repeater, IP over DMR end to end, VOX with a voice, privacy scrambling against another radio | [on-air-coverage](testing/on-air-coverage.md) |
| DM-32UV | GPS data seen, no fix yet. The transmit half of a split duplex pair not checked. FM transmit deviation at 12.5 kHz, both slots through a duplex hotspot. A DMR receive fault after hours of uptime (2026-10-16, cause unknown). USB not available | [dm32-bringup](testing/dm32-bringup.md) |
| DM-1801 Pro | The receive half of a split duplex pair, VHF transmit, VOX and APRS receive not checked | [hardware-dm1801pro](spec/hardware-dm1801pro.md) |
| DB25-D PRO | Split duplex not built. Standalone and MMDVM SFR checked on the SDR bench (experimental). DMR audio from the speaker, a GPS fix and settings kept across power off not confirmed | [hardware-db25d](spec/hardware-db25d.md) |
| MD-380 | Fully operational (UHF)¹. DCS, the VHF image, tone deviation and data from a DMR VFO not checked. Split duplex and SFR not available (HR_C5000) | [md380-bringup](testing/md380-bringup.md) |

¹ TX frequency accuracy depends on each radio's factory calibration: confirm it against an external reference (frequency counter, service monitor, or a hotspot's MMDVMCal BER test). One MD-380 measured +1.9 kHz high, outside an MMDVM hotspot's decode window; `MD380_TX_OFFSET_HZ` corrects it at build time ([hardware-md380](spec/hardware-md380.md)).

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
| Feature switch | DM-32UV | DM-1701 | DM-1801 Pro | DB25-D PRO | MD-380 | MD-UV380 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| `FM` | ● | ● | ● | ● | ● | ● |
| `SUBTONE` | ● | ● | ● | ● | ● | ● |
| `DMR` | ● | ● | ● | ● | ● | ● |
| `DMR_PRIVACY` | ● | ● | ● | ● | ● | ● |
| `DMR_TALKER_ALIAS` | ● | ● | ● | ● | ● | ● |
| `DMR_GPS` | ● | ● | ● | ● | ● | ● |
| `DMR_SMS` | ● | ● | ● | ● | ● | ● |
| `DMR_TMS` | ● | ● | ● | ● | ● | ● |
| `DMR_ARS` | ● | ● | ● | ● | ● | ● |
| `DMR_LRRP` | ● | ● | ● | ● | ● | ● |
| `DMR_SIGNALING` | ● | ● | ● | ● | ● | ● |
| `DMR_EMERGENCY` | ● | ● | ● | ● | ● | ● |
| `DMR_ROAMING` | ● | ● | ● | ● | ● | ● |
| `DMR_IP_GATEWAY` | ● | ● | ● | ● | ● | ● |
| `ZONE_SHARE` | ● | ● | ● | ● | ● | ● |
| `DMR_UDP_COMPRESS` | ● | ● | ● | ● | ● | ● |
| `DMR_CONFIRMED` | ● | ● | ● | ● | ● | ● |
| `NET_TCP` | ● | ● | ● | ● | ● | ● |
| `NET_KISS` | ● | ● | ● | ● | ● | ● |
| `NET_HTTP` | ● | ● | ● | ● | ● | ● |
| `NET_HTTP_GZIP` | ● | ● | ● | ● | ● | ● |
| `NET_WEBCTL` | ● | ● | ● | ● | ● | ● |
| `NET_RIGCTL` | ● | ● | ● | ● | ● | ● |
| `NET_APRSIS` | ● | ● | ● | ● | ● | ● |
| `NET_USRP` | ● | ● | ● | … | ● | ● |
| `MDNS` | ● | ● | ● | ● | ● | ● |
| `TFTP` | ● | ● | ● | ● | ● | ● |
| `TFTP_IDDB` | ● | ● | ● | ● | ● | ● |
| `DMR_CONTROL_STATION` | ● | ● | ● | ● | ● | ● |
| `SNMP` | ● | ● | ● | ● | ● | ● |
| `SFR` | ● | ● | ● | ● | ✕ | ● |
| `SFR_DUALPATH_RX` | ● | ● | ● | ● | ✕ | ● |
| `SFR_CWID` | ● | ● | ● | ● | ✕ | ● |
| `TASK_STACKS` | ● | ● | ● | ● | ● | ● |
| `USB` | ✕ | ● | ✕ | ✕ | ● | ● |
| `HOSTLINK` | ● | ● | ● | ● | ● | ● |
| `USB_NETWORK` | ✕ | ● | ✕ | ✕ | ● | ● |
| `NET_PPP` | ● | ○ | ● | ● | ○ | ○ |
| `HOTSPOT` | ● | ● | ● | ● | ● | ● |
| `HOTSPOT_SPLIT` | ● | ● | ● | … | … | ● |
| `DEBUGLINK` | ● | ● | ● | ● | ● | ● |
| `CPS_OPENFW` | ● | ● | ● | ● | ● | ● |
| `CPS_FAST` | ● | ● | ● | ● | ● | ● |
| `VFO` | ● | ● | ● | ● | ● | ● |
| `VFO_FM` | ● | ● | ● | ● | ● | ● |
| `VFO_DMR` | ● | ● | ● | ● | ● | ● |
| `DIAG` | ● | ● | ● | ● | ● | ● |
| `FONT_XL` | ● | ✕ | ✕ | ✕ | ✕ | ✕ |
| `FONT_AA` | ● | ○ | ○ | ○ | ○ | ○ |
| `FONT_BIG` | ● | ○ | ○ | ○ | ○ | ○ |
| `UI_COLOR` | ● | ● | ✕ | ● | ● | ● |
| `UI_SETTINGS` | ● | ● | ● | ● | ● | ● |
| `SCAN` | ● | ● | ● | ● | ● | ● |
| `DUAL_WATCH` | ● | ● | ● | ● | ● | ● |
| `BEEP` | ● | ● | ● | ● | ● | ● |
| `FM_TX_TONES` | ● | ● | ● | ● | … | ● |
| `DTMF` | ● | ● | ● | ● | … | ● |
| `VOX` | … | ● | ● | … | ● | ● |
| `BATTERY_SAVE` | ● | ● | ● | ● | ● | ● |
| `VIBRATE` | ? | ✕ | ● | ✕ | ✕ | ✕ |
| `GPS` | ● | ✕ | ✕ | ● | ● | ● |
| `AUDIO_PROBE` | … | ○ | … | … | … | ○ |
| `STUB_ENTRY` | ● | ● | ● | ● | ● | ● |
| `NAND_STORAGE` | ✕ | ✕ | ✕ | ● | ✕ | ✕ |
| `CRASH_LOG` | ● | ● | ● | ● | ● | ● |
| `CRASH_SCREEN` | ● | ● | ● | ● | ● | ● |
| `CRASH_REPORT` | ● | ● | ● | ● | ● | ● |
| `WATCHDOG` | ● | ● | ● | ● | ● | ● |
| `FAULT_TRACE` | ● | ✕ | ● | ● | ✕ | ✕ |
| `AM_RX` | ● | ✕ | ● | ✕ | ✕ | ✕ |
| `BCAST_FM` | ● | ✕ | ● | … | ✕ | ✕ |
| `APRS` | ● | ● | ● | ● | ● | ● |
| `APRS_RX` | ● | ● | ● | … | … | ● |
| `APRS_IGATE` | ● | ○ | ● | … | … | ○ |
| `KEY_ASSIGN` | ● | ● | ● | ● | ● | ● |
| `CHANNEL_LIST` | ● | ● | ● | ● | ● | ● |
| `CHANNEL_EDIT` | ● | ● | ● | ● | ● | ● |
| `ZONE_EDIT` | ● | ● | ● | ● | ● | ● |
| Voltage reading (board, no switch) | Battery | Battery | Battery | None | Battery | Battery |
| Clock kept through power-off (board, no switch) | Yes | ? | Yes | ? | ? | ? |
<!-- end generated -->

## Radio and voice

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Analog FM receive and transmit | `FM` | 29,296 | 28,056 | Includes its children (tones, VOX, APRS, FM VFO) |
| CTCSS / DCS encode and decode | `SUBTONE` | 904 | 3,292* | MD-380: software decode (ADC) and PWM encode; CTCSS 100.0 Hz works on air both ways (2026-10-19), DCS not yet checked |
| FM TX tones: roger beep, 1750 Hz burst | `FM_TX_TONES` | 568 | — | MD-380: TX tone path not written |
| DTMF transmit from the keys | `DTMF` | 920 | — | MD-380: TX tone path not written |
| FM VOX | `VOX` | 496 | 1,064 |  |
| Receiver battery saver | `BATTERY_SAVE` | 296 | 740 |  |
| Air band AM receive (108-137 MHz) | `AM_RX` | 408* | — | Needs a receiver that does AM; DM-32 board glue not written yet, so nothing runs |
| Broadcast FM receiver | `BCAST_FM` | ~0* | — | Needs a second receiver chip (DM-32's RDA5807M); glue not written yet |
| DMR Tier II voice (modem, call stack, AMBE FEC, vocoder) | `DMR` | 175,856 | 134,308 | Includes every DMR feature and everything on IP over DMR |
| Basic privacy (Motorola / Anytone BP) | `DMR_PRIVACY` | 1,936 | 1,992 |  |
| Talker alias receive and send | `DMR_TALKER_ALIAS` | 984 | 1,300 |  |
| ETSI GPS Info in the embedded LC | `DMR_GPS` | 2,208 | 2,152 |  |
| DMR emergency (alarm, emergency call, hot mic) | `DMR_EMERGENCY` | 3,296 | 3,188 |  |
| Repeater roaming | `DMR_ROAMING` | 11,952 | 12,380 |  |
| CSBK signaling: call alert, radio check | `DMR_SIGNALING` | 592 | 752 |  |
| VFO (hand-tuned frequency) | `VFO` | 656 | 516 | Includes the DMR VFO; `VFO` and `VFO_DMR` re-measured 2026-10-17 (DM-1701 plain build, MD-380 with LTO). Copy to VFO ([dmr-voice.md](spec/dmr-voice.md) 2.2) is on top of that figure, behind the same switch: about 630 B on the DM-1701 and 730 B on the MD-380 (default builds, LTO as each has it), 750 B on the C-SKY radios |
| FM VFO | `VFO_FM` | 504 | 452 |  |
| DMR VFO (color code, slot, talk target typed in; mode change FM / DMR) | `VFO_DMR` | 144 | 104 | The VFO's mode, color code, slot and talk target are settings of the VFO, kept across power cycles ([dmr-voice.md](spec/dmr-voice.md) 2.1) |
| Channels list: every channel by number, a channel's settings (hub 5) | `CHANNEL_LIST` | 1,536 | 2,140 |  |
| Channel editor: edit a channel, New channel from current settings, text entry | `CHANNEL_EDIT` | 4,656 | 4,252 | Needs `CHANNEL_LIST`. Without it a channel's settings are read only. Measured 2026-10-17 |
| Zone editor: move, add, copy and remove channels in a zone; rename, copy and delete a zone | `ZONE_EDIT` | 6,580 | 6,928 | Needs `CHANNEL_EDIT`. Measured 2026-10-24 on the default builds, LTO as each has it; also about 1.3 KB of RAM |
| Key assignment (Settings > Keys, codeplug) | `KEY_ASSIGN` | 2,120 | 2,188 | Up to 9 keys. Measured 2026-10-24 |
| Scan, band scan, dual watch, nuisance delete | `SCAN` | 2,576 | 3,032 |  |
| Dual watch: A and B in any zone or the VFO, PTT on the selected one, its screens, talk back | `DUAL_WATCH` | 10,864 | 11,456 | Needs `SCAN`. Without it, only the debug link's dual watch (two channels of one zone). Measured 2026-10-21 on the dual watch branch, default builds |
| Beeps and alert tones | `BEEP` | 1,552 | 1,800 |  |
| Vibration alerts (Settings > Sound > Vibrate) | `VIBRATE` | 520* | — | Needs a vibration motor (`hal_power_t.vibrate`): none of these radios has one RadioFoundry drives. On for the DM-1801 Pro. Measured 2026-10-16 with `CONFIG_SNMP=n`; 236 B with LTO |
| GPS receiver: the module driver and its NMEA input (Settings > Location > GPS) | `GPS` | — | 1,196 | Needs a GPS receiver. Off, DMR GPS, LRRP and APRS keep the fixed position and report no fix. MD-380 measured 2026-10-22 (LTO, 58f2cebe); the DM-1701 has no receiver |

## DMR data

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Text messages (ETSI IP text by default) | `DMR_SMS` | 2,640 | 5,072 | Includes the TMS format. Individual ETSI IP texts go confirmed ("Delivered to") |
| Motorola TMS text format | `DMR_TMS` | 784 | 740 | Optional format next to ETSI IP text |
| ARS registration | `DMR_ARS` | 1,112 | 1,324 |  |
| LRRP location | `DMR_LRRP` | 8,548 | 2,048 |  |
| Compressed UDP/IP headers on transmit (SAP 3) | `DMR_UDP_COMPRESS` | 408 | 356 | Auto: not for ports 4000-4099 or ETSI IP text |
| Confirmed packet data | `DMR_CONFIRMED` | 2,880 | 2,920 |  |
| Zone and channel sharing: send a zone or a channel, import an offered one | `ZONE_SHARE` | 12,240 | 12,388 | Includes its screens and the import; about 4 KB of RAM. Receiving is off until the user sets Ask ([zone-share.md](spec/zone-share.md)). Measured 2026-10-24 with LTO, on the zone share branch |
| Single frequency repeater: MMDVM SFR, and the standalone SFR (experimental) | `SFR` | 6,884 | — | MD-380: HR_C5000 lacks what the SFR glue uses |
| Experimental: SFR user receive "dual-path RX": a voice frame whose FEC failed comes from the other copy | `SFR_DUALPATH_RX` | 696 | — | Needs `SFR`. DM-1701 figure with LTO (2026-10-22) |
| Experimental: standalone SFR repeater's periodic CW ID (callsign in Morse, FM) | `SFR_CWID` | 900 | — | Needs `SFR`. DM-1701 figure with LTO (2026-10-22) |
| APRS: AFSK positions, status, messages; DMR positions; KISS TNC | `APRS` | 26,568 | 26,628 | Includes APRS receive, KISS over TCP and APRS-IS where built. MD-380: AFSK TX needs the tone path (not written), DMR positions work |
| APRS receive: demodulator, station list, digipeater | `APRS_RX` | 8,892 | — | DM-1701: FM receive audio on the I2S stream, squelch gated ([aprs](spec/aprs.md) 5.3). MD-380: not written |
| APRS iGate rules (with tools/aprs_gate.py) | `APRS_IGATE` | 1,820* | — | Needs APRS receive. DM-1701: off by default |

## Host link and USB

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| USB device | `USB` | 127,300 | 87,848 | DM-32 has no USB (host link on the programming cable's UART). Includes everything on USB |
| Host link: hotspot, CPS, debug link on the host port | `HOSTLINK` | 115,640 | 77,644 | Includes its children |
| MMDVM hotspot | `HOTSPOT` | 34,196 | ~12,400 | Includes split duplex and MMDVM SFR where built. MD-380: size estimated |
| Split duplex repeater (two radios, one duplex modem) | `HOTSPOT_SPLIT` | 12,584 | — | MD-380: HR_C6000-only glue. DM-1701, DM-32UV, DM-1801 Pro: each half checked alone on the SDR bench; one pair (DM-1801 Pro TX, DM-32UV RX) with a subscriber: 29 of 31 calls on the right slot |
| Debug link | `DEBUGLINK` | 32,376 | 29,084 |  |
| Task stacks in the debug link view | `TASK_STACKS` | 536 | 560 | Each task's least free stack, the `tasks` view section ([debuglink](spec/debuglink.md) 3.7) |
| Open-firmware CPS protocol (qdmr) | `CPS_OPENFW` | 3,064 | 3,008 |  |
| Fast codeplug protocol (debug link CODEPLUG) | `CPS_FAST` | 3,732 | 3,948* | Changed 4 KB blocks only, LZ4 both ways, staged commit ([cps-fast.md](spec/cps-fast.md)). DM-1701 figure with LTO (2026-10-18) |
| Loader hand-over (stub entry, command 0x1A) | `STUB_ENTRY` | 768* | — | Updates and backups over the radio's own link without the bootloader key press: the web flasher and `stub/stubtool.py`. On by default on every radio, with or without the debug link ([stub-entry.md](spec/stub-entry.md)); on the STM32 radios a RAM flash stub that also takes 8 KB of RAM ([stub-stm32f4.md](spec/stub-stm32f4.md)) (the DM-1701's RAM is 95 % used with it on) |
| Crash log | `CRASH_LOG` | 1,572 | 1,756 | A fault, stack overflow, heap exhaustion or failed assertion keeps a record across the restart; the debug link shows it ([crash-log.md](spec/crash-log.md)) |
| Hardware watchdog | `WATCHDOG` | 932 | 856 | Fed by the idle task; a hang or a starved system resets the radio. Timeout about 5 s on the STM32 radios, 4 s on the HR_C7000 radios ([watchdog.md](spec/watchdog.md)) |
| Fault trace on the host UART | `FAULT_TRACE` | — | — | A CPU fault's registers in three framed lines before the restart, about 600 B ([crash-log.md](spec/crash-log.md) 2.3). The STM32 radios' host port is USB |
| Crash log screen | `CRASH_SCREEN` | 1,024 | 1,408* | Settings > Advanced > Crash log: the record's summary and Clear |
| Crash report over DMR | `CRASH_REPORT` | 1,440 | 1,712* | Sends the crash record as one UDP datagram to a codeplug contact, from the menu or after the restart (Off, Ask, Auto) ([crash-log.md](spec/crash-log.md) 5). Needs the screen |

## Network (USB network adapter and IP over DMR)

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| USB network adapter (CDC-NCM) | `USB_NETWORK` | 48,344 | 47,468 | Includes everything below |
| IP over DMR gateway | `DMR_IP_GATEWAY` | 44,128 | 43,220 | Includes the radio's own services below |
| Control station mode (boot as network, PC answers data, fixed link) | `DMR_CONTROL_STATION` | 288 | 484 |  |
| Minimal TCP | `NET_TCP` | 16,240 | 15,656 | Includes KISS, HTTP, rigctld, APRS-IS. About 4.5 KB RAM |
| KISS over TCP, port 8001 | `NET_KISS` | 700 | 752 |  |
| HTTP status page and JSON, port 80 | `NET_HTTP` | 4,796 | 4,860 |  |
| HTTP static files stored and served gzip compressed | `NET_HTTP_GZIP` | -64 | -96 | Negative: saves flash |
| Web control: the front panel page, keys, status and receive audio over `/ws` | `NET_WEBCTL` | 9,620 | 9,332 | The page and the radio's drawing included (about 6 KB gzip compressed); 1 KB RAM ([tcp-services.md](spec/tcp-services.md) 5.1) |
| rigctld, port 4532 | `NET_RIGCTL` | 3,852 | 3,728 |  |
| Audio linking (USRP over UDP, port 34001) | `NET_USRP` | 4,552 | 4,888 | DM-1701: DMR and FM both ways. MD-380: DMR only (FM audio doesn't reach the processor yet). DM-32UV: DMR only until its board code has the FM audio paths |
| APRS-IS client | `NET_APRSIS` | 2,856 | 2,720 | Needs NAT on the host |
| mDNS / DNS-SD (rfoundry-<id>.local) | `MDNS` | 6,744 | 6,504 |  |
| TFTP codeplug read / write | `TFTP` | 6,356 | 6,412 | Without the ID database below |
| DMR ID database over TFTP (`iddb.bin`, combined `codeplug.bin`) | `TFTP_IDDB` | 2,184 | 2,264 | Follows `TFTP` |
| SNMP v2c agent and traps (USB and over the air) | `SNMP` | 8,460 | 8,036 |  |
| PPP on the serial host port (IPv4 into the gateway) | `NET_PPP` | 4,368* | 3,816* | Interop tested with pppd; checked on the DB25-D PRO and the DM-1801 Pro (`docs/spec/ppp.md` 8). Fits neither default image as it is when measured (DM-1701 688 B short, MD-380 1,588 B short of its 275 KB then; DM-1701 with LTO: +3,948, fits). DM-32: its only IP link, on by default with the gateway and every service (Whole images, DM-32UV) |

## User interface

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Settings menu on the radio | `UI_SETTINGS` | 5,304 | 5,640 |  |
| Diagnostics page | `DIAG` | 3,752 | 3,532 |  |
| Color screen settings (theme, highlight color, filled status bar) | `UI_COLOR` | 672 | 608 | Off on the DM-1801 Pro (monochrome screen; 628 B there). Measured 2026-10-17; DM-1701 with LTO 612 B |
| 16x32 font | `FONT_XL` | 6,352* | — | Only useful at 240 px and wider (MD-380 is 160 px). Anti-aliased on the DM-32 (7.4 KB of glyphs) |
| Anti-aliased fonts | `FONT_AA` | 3,004* | — | Smooth text on color panels (measured 2026-10-17); the DM-32's image grew 9.5 KB with it and `FONT_XL` |
| Larger text | `FONT_BIG` | — | — | 9x18 lists and labels on 240 px color panels (needs `FONT_AA`); 3,588 B on the DM-32 (measured 2026-10-17) |

## Storage

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Settings and codeplug storage on SPI NAND | `NAND_STORAGE` | — | — | For radios whose data flash is SPI NAND: on for the DB25-D (about 6.6 KB flash, 1.5 KB RAM), not in these columns. These radios have SPI NOR and don't use it ([nand-sectors](spec/nand-sectors.md)) |

## Bench

| Feature | Switch | Bytes DM-1701 | Bytes MD-380 | Notes |
|---|---|---:|---:|---|
| Audio path probe (debug link 0x10) | `AUDIO_PROBE` | 1,176* | — | Bench only, never in a release |

## Whole images

### DM-1701 and MD-380

| Build | DM-1701 | MD-380 |
|---|---:|---:|
| Default | 332,532 (LTO on) | 320,236 (2026-10-19) |
| Maximal (every compatible switch on) | 340,396 (LTO on) | 327,064 (2026-10-19) |
| FM only, minimal | 123,300 | 109,488 |

Link time optimization is on by default on both radios. The DM-1701 has run with it since 2026-10-13 (the HIL suite
passed); `LTO_3P=1` would take another 3 KB or so off by optimizing the third-party code too.

The MD-380's app region is sectors 3-7, 0x0800C000..0x0807FFFF, 475,136 bytes (464 KB). The codec runs in place in
sectors 8, 10 and 11 ([codec-md380.md](spec/codec-md380.md)). Measured 2026-10-19 with link time optimization, its
default:

| MD-380 build | Flash | Free | RAM | CCM |
|---|---:|---:|---:|---:|
| Default (UHF) | 320,236 | 154,900 | 107,120 of 131,068 | 59,640 of 65,536 |
| Maximal | 327,064 | 148,072 | | |

Until the codec moved, the region was 281,668 bytes and the default image fit only with link time optimization and
the hotspot off.

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
