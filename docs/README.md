<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Documentation map

This page lists every RadioFoundry document and the topic it owns. Each fact has one home: other pages link to that
home instead of repeating it. When you add, merge, or remove a page, update its row here in the same commit.
`tools/check_doc_links.py` fails on a page that this map doesn't list. For writing rules, see
[Documentation style](dev/doc-style.md).

## Start here

| Page | Owns |
|---|---|
| [README](../README.md) | What RadioFoundry is, the supported radios, where to go next |
| [User manual](user/README.md) | Everything a radio user does, from installing to settings |
| [Contributing](dev/contributing.md) | The rules for every change, and how a pull request gets merged |
| [Forking](../FORKING.md) | Using RadioFoundry code in another project: license notices |
| [Agent notes](../AGENTS.md) | Rules for coding agents; points to the human pages for detail |

## User manual

The manual is built to the web site and to a PDF in file-name order.

| Page | Owns |
|---|---|
| [Contents](user/README.md) | Table of contents and a quick reference |
| [Introduction](user/01-introduction.md) | What the firmware does, the reading order, the AMBE codec in brief, and the conventions |
| [Installing](user/02-installing.md) | Installing, updating, backing up, and going back to the original firmware |
| [First boot](user/03-first-boot.md) | What the radio shows the first time it starts |
| [Programming](user/04-programming.md) | Programming channels and contacts from a computer |
| [Keys and navigation](user/05-keys-and-navigation.md) | Keys, menus, and text entry |
| [Home screen](user/06-home-screen.md) | What the home screen shows |
| [Channels, zones, and VFO](user/07-channels-zones-vfo.md) | Selecting and editing channels, zones, and the VFO |
| [FM](user/08-fm.md) | FM operation: squelch, tones, monitor |
| [DMR](user/09-dmr.md) | DMR calls, talkgroups, contacts, time slots |
| [Messages and signaling](user/10-messages-and-signaling.md) | Text messages, call alert, radio check |
| [Emergency](user/11-emergency.md) | Sending and receiving an emergency |
| [Scan](user/12-scan.md) | Scanning channels and zones |
| [APRS](user/13-aprs.md) | Sending and receiving positions |
| [Hotspot](user/14-hotspot.md) | Using the radio as an MMDVM hotspot or single-frequency repeater |
| [USB network](user/15-usb-network.md) | Connecting the radio to a computer as a network device |
| [Battery and power](user/16-battery-and-power.md) | Battery, transmit power, and power saving |
| [Settings](user/17-settings.md) | Every setting with its default |
| [Moving from the original firmware](user/18-moving-from-stock.md) | Moving a radio and its codeplug from the manufacturer's firmware |
| [Troubleshooting](user/20-troubleshooting.md) | Every symptom and its fix, including installer errors |
| [FAQ](user/21-faq.md) | Common questions about the project |
| [Glossary](user/22-glossary.md) | Terms used in the manual (generated from `docs/glossary.json`) |
| [Radio reference](user/23-radios.md) | Each radio: install steps, keys, power levels, what it lacks |

## Contributor guide

| Page | Owns |
|---|---|
| [Contributor index](dev/README.md) | Where to start for each kind of change |
| [Contributing](dev/contributing.md) | Rules, branches, pull requests, license of contributions |
| [Clean-room rules](CLEANROOM.md) | What sources you can and can't use, and how to show it |
| [Documentation style](dev/doc-style.md) | How to write and organize these documents |
| [Architecture](dev/architecture.md) | The layers, the tasks, and the source tree |
| [Building](dev/building.md) | Toolchains, make targets, the firmware version, flashing your own build |
| [Build configuration](build-config.md) | Every build switch, value, variant, hardware build option, and board default |
| [Feature matrix](feature-matrix.md) | Each feature's flash cost, its default on each radio, and what was checked on each radio |
| [Board registry](dev/board-registry.md) | The fields of `boards/<id>/board.toml` |
| [Adding a radio](dev/adding-a-radio.md) | Porting RadioFoundry to a new radio |
| [Adding a mode](dev/adding-a-mode.md) | Adding a digital mode beside DMR |
| [Testing](dev/testing.md) | Test levels, where a new test goes, host tests |
| [Continuous integration](dev/ci.md) | The workflows, the steps and options of `tools/ci.sh`, coverage, and sanitizers |
| [CI runners](dev/ci-runners.md) | Where the workflows run, and the toolchain images |
| [Stack check](dev/stack-check.md) | Static stack-depth analysis for each task |
| [Debug link tool](dev/debug-link.md) | Using `tools/rfoundry_debug.py` with a radio |
| [The AMBE codec](dev/codec.md) | How the codec reaches each radio: merge tools, codec maps, keep and merge |
| [Releases](dev/release.md) | Version names, the release checklist, and release artifacts |
| [Web flasher](dev/web-flasher.md) | How the browser installer works |
| [Codeplug editor](dev/codeplug-editor.md) | The ways to program a radio, and how the browser codeplug editor works |
| [Web site](dev/website.md) | The site generator, hosting, and error reports |
| [Manual PDF](dev/manual-pdf.md) | Building the user manual as a PDF |
| [Translating](dev/translating.md) | Translating the radio, the site, and the manual |
| [Developer glossary](dev/glossary.md) | Terms used in the code and specs (generated from `docs/glossary.json`) |

## Specifications

Reference pages for what the firmware does. Code cites these pages by file name and section number. For the full
index, see [Specifications](spec/README.md).

| Page | Owns |
|---|---|
| [Specifications index](spec/README.md) | The specs grouped by area |
| [Radios](spec/radios.md) | The radio survey: chips and port status of each radio |
| [Capabilities](spec/capabilities.md) | What a radio can do, by area, with the switch and spec behind each |
| [Radio features](spec/radio-features.md) | Scan, FM transmit extras, beeps, battery, and power management |
| [Air band and broadcast](spec/airband-and-broadcast.md) | AM air band and broadcast FM receive |
| [APRS](spec/aprs.md) | APRS over FM and DMR |
| [Position](spec/position.md) | Position sources, settings, and consumers |
| [User interface](spec/ui.md) | Screens, keys, and the drawing layer as built |
| [Air interface](spec/air-interface.md) | The shared air-interface layer under each digital mode |
| [DMR voice](spec/dmr-voice.md) | DMR voice calls |
| [DMR data](spec/dmr-data.md) | Text messages, ARS, LRRP, call alert, radio check |
| [Zone sharing](spec/zone-share.md) | Sharing zones and channels over DMR data |
| [DMR emergency](spec/dmr-emergency.md) | DMR emergency alarms and calls |
| [DMR roaming](spec/dmr-roaming.md) | Repeater roaming |
| [DMR Tier III](spec/dmr-trunking.md) | Tier III trunking, mobile station |
| [DMR Tier III codeplug](spec/dmr-trunking-codeplug.md) | Tier III systems and channels in the codeplug |
| [P25](spec/p25.md) | P25 conventional |
| [P25 trunking](spec/p25-trunking.md) | P25 trunking |
| [P25 codeplug](spec/p25-codeplug.md) | P25 channels and systems in the codeplug |
| [NXDN](spec/nxdn.md) | NXDN conventional |
| [NXDN codeplug](spec/nxdn-codeplug.md) | NXDN channels in the codeplug |
| [Single-frequency repeater](spec/sfr.md) | Repeating on one frequency |
| [Split duplex](spec/split-duplex.md) | Two radios as one duplex MMDVM modem |
| [DMR modem](spec/dmr-modem.md) | The modem driver contract and the chips compared |
| [HR_C6000 modem](spec/modem-hrc6000.md) | The HR_C6000 as a DMR modem |
| [HR_C6000 registers](spec/hrc6000-registers.md) | HR_C6000 register reference |
| [HR_C7000 modem](spec/modem-hrc7000.md) | The HR_C7000 as a DMR modem |
| [PA key-up](spec/pa-keyup.md) | DMR power amplifier key-up per burst |
| [Second receiver link](spec/rx2-link.md) | The link to a second receiver on another SoC |
| [Debug link](spec/debuglink.md) | The debug link protocol: commands, the view, key injection |
| [CPS protocol](spec/cps-protocol.md) | The host port's protocols and the CPS protocol |
| [Fast codeplug protocol](spec/cps-fast.md) | Block sums, LZ4, staged commit |
| [Compact ID database](spec/iddb-compact.md) | The compact DMR ID database format |
| [MMDVM protocol](spec/mmdvm-protocol.md) | The MMDVM modem protocol in hotspot mode |
| [USB network](spec/usb-network.md) | The USB network personality, IP gateway, and service ports |
| [PPP](spec/ppp.md) | PPP on the serial host link |
| [TCP services](spec/tcp-services.md) | HTTP, rigctld, KISS over TCP, APRS-IS |
| [TFTP](spec/tftp.md) | Codeplug access over TFTP |
| [mDNS](spec/mdns.md) | mDNS and DNS-SD |
| [SNMP](spec/snmp.md) | The SNMP agent |
| [USB audio](spec/usb-audio.md) | FM and DMR voice to a computer over USB or the network |
| [Open-firmware codeplug](spec/open-firmware-cps.md) | The codeplug format qdmr writes |
| [qdmr YAML](spec/qdmr-yaml.md) | qdmr's YAML file and how the editor maps it |
| [Native codeplug](spec/codeplug-native.md) | The codeplug editing model |
| [Codeplug settings](spec/codeplug-settings.md) | Keyed settings the open-firmware layout has no field for |
| [Channel modes](spec/channel-modes.md) | How a channel of any digital mode is stored |
| [Codeplug conversion](spec/codeplug-port.md) | Converting a manufacturer codeplug to RadioFoundry |
| [Settings store](spec/settings-store.md) | The settings store and safe flash writes |
| [NAND sectors](spec/nand-sectors.md) | 4 KB sectors over NAND for the settings store |
| [Stub entry](spec/stub-entry.md) | Loading the flashing stub at run time |
| [STM32F4 stub](spec/stub-stm32f4.md) | The STM32F4 flash stub: protocol and safety rules |
| [Crash log](spec/crash-log.md) | Fault capture, the kept record, and the fault report |
| [Watchdog](spec/watchdog.md) | The hardware watchdog |
| [Gotchas](spec/gotchas.md) | Past bugs and traps, by subsystem |

## Testing

| Page | Owns |
|---|---|
| [Testing overview](testing/README.md) | How to run each test level, from host tests to the bench |
| [Hardware-in-the-loop tests](../tests/hil/README.md) | Running HIL tests: flags, rig configuration, the test list |
| [DMR bench cases](testing/dmr-bench.md) | Manual DMR cases on the SDR bench and their pass criteria |
| [Power calibration](testing/power-calibration.md) | Measuring and setting transmit power |
| [Backlight curve](testing/backlight-curve.md) | Measuring the backlight response |
| [Bring-up records](testing/results/README.md) | The format of the bring-up run records |

## User interface design

| Page | Owns |
|---|---|
| [Design principles](ux/principles.md) | The principles every screen follows |
| [Information architecture](ux/information-architecture.md) | Menu structure and key roles |
| [Task flows](ux/task-flows.md) | Key presses per task, and their budgets |
| [Dual watch](ux/dual-watch.md) | The dual-watch design |
| [Usability tests](ux/usability-tests.md) | How the UI is tested with people |
| [Survey](ux/survey.md) | How other radios handle the same tasks |
| [Wireframes](ux/wireframes/README.md) | Layouts for each display class |
| [128x64 wireframes](ux/wireframes/128x64.md) | Layouts for 128x64 displays |
| [160x128 wireframes](ux/wireframes/160x128.md) | Layouts for 160x128 displays |
| [240x320 wireframes](ux/wireframes/240x320.md) | Layouts for 240x320 displays |

## Tools and components

These pages sit beside their code and cover only how to use it.

| Page | Owns |
|---|---|
| [rfoundry](../tools/rfoundry/README.md) | The command-line installer |
| [Codec maps](../tools/rfoundry/rfoundry/codec_maps/README.md) | The codec map files the installer reads |
| [Flash stubs](../stub/README.md) | The flash stubs: files and how to build them |
| [Stub tool guide](../stub/USER-GUIDE.md) | Using `stub/stubtool.py` |
| [First stage](../boot/README.md) | The first-stage loader |
| [Codec input](../codecs/README.md) | Building with the codec on your own machine |
| [CI runner](../tools/ci-runner/README.md) | Setting up a self-hosted CI runner |
| [Toolchain images](../tools/docker/toolchains/README.md) | Building and using the toolchain container images |
